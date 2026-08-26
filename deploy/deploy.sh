#!/bin/bash
#
# deploy.sh - one-shot deployment script
#
# Deploys the cross-compiled client binary and its shared libraries to the
# RK3568 board.
#
# Usage:
#   ./deploy.sh <board_ip> [--full]
#     --full  First-time deployment: create directory tree, copy shared
#             libraries, register the systemd service.
#     (no flag) Update the binary only (OTA-style).
#
# Prerequisites:
#   1. Client has been cross-compiled with aarch64-linux-gnu- on the host.
#   2. The board is reachable over the network (ping board_ip).
#   3. SSH (port 22) is enabled on the board.
#
# Target directory layout (on the board):
#   /opt/retail/
#   ├── app/              # executables
#   │   └── RetailClient
#   ├── lib/              # shared libraries (.so)
#   ├── config/           # configuration files
#   │   └── client.conf
#   ├── data/             # runtime data (SQLite DB, face images)
#   │   └── faces/
#   ├── log/              # logs
#   └── ota/              # OTA workspace
#
# OTA compatibility:
#   This layout is OTA-friendly:
#   - OTA updates only /opt/retail/app/RetailClient (and optionally .so files
#     under /opt/retail/lib/).
#   - After the update, `systemctl restart retail-client` picks up the new
#     binary.
#   - User data under /opt/retail/data/ is never touched by OTA.

set -e

TARGET_IP="${1:?Usage: $0 <board_ip> [--full]}"
FULL_DEPLOY="${2}"

# PROJECT_ROOT can be overridden via the environment; defaults to two levels
# above this script (i.e. the repository root).
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
BUILD_DIR="${PROJECT_ROOT}/build/client"
BINARY="${BUILD_DIR}/RetailClient"

SSH="ssh -o StrictHostKeyChecking=no root@${TARGET_IP}"
SCP="scp -o StrictHostKeyChecking=no"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# ============================================================
# Step 1: Verify the local build artifact
# ============================================================
info "Checking build artifact..."

if [ ! -f "${BINARY}" ]; then
    error "RetailClient not found. Build it first: cd build/client && qmake && make"
fi

# Verify it is an ARM64 binary
BIN_ARCH=$(file "${BINARY}" | grep -o 'ARM aarch64')
if [ -z "${BIN_ARCH}" ]; then
    error "RetailClient is not ARM64. Cross-compile with aarch64-linux-gnu-, not x86 gcc."
fi

info "RetailClient: ARM64 architecture OK"

# ============================================================
# Step 2: Collect runtime shared libraries
# ============================================================
collect_libs() {
    info "Collecting shared libraries..."

    # Staging directory for libraries to ship to the board
    LIB_DIR="${PROJECT_ROOT}/deploy/tmp_libs"
    rm -rf "${LIB_DIR}" && mkdir -p "${LIB_DIR}"

    # Pull Qt/OpenCV/cJSON/etc. from the cross-compiler sysroot.
    SYSROOT="/usr/aarch64-linux-gnu"

    # Qt core libraries (minimal set, only what we actually use)
    QT_LIBS=(
        libQt5Core.so.5
        libQt5Gui.so.5
        libQt5Widgets.so.5
        libQt5Network.so.5
    )

    for lib in "${QT_LIBS[@]}"; do
        find "${SYSROOT}" /usr/lib -name "${lib}" -exec cp -v {} "${LIB_DIR}"/ \; 2>/dev/null
    done

    # OpenCV libraries (used by the face recognition path)
    find "${SYSROOT}" /usr/lib -name "libopencv_core.so*" -exec cp -v {} "${LIB_DIR}"/ \; 2>/dev/null
    find "${SYSROOT}" /usr/lib -name "libopencv_imgproc.so*" -exec cp -v {} "${LIB_DIR}"/ \; 2>/dev/null
    find "${SYSROOT}" /usr/lib -name "libopencv_imgcodecs.so*" -exec cp -v {} "${LIB_DIR}"/ \; 2>/dev/null
    find "${SYSROOT}" /usr/lib -name "libopencv_objdetect.so*" -exec cp -v {} "${LIB_DIR}"/ \; 2>/dev/null

    # cJSON is vendored as source under common/cjson, no shared lib to ship.

    # libssl / libcrypto (used by the crypto module)
    find "${SYSROOT}" /usr/lib -name "libcrypto.so*" -exec cp -v {} "${LIB_DIR}"/ \; 2>/dev/null
    find "${SYSROOT}" /usr/lib -name "libssl.so*" -exec cp -v {} "${LIB_DIR}"/ \; 2>/dev/null

    info "Collected $(ls "${LIB_DIR}" | wc -l) library files"
}

# ============================================================
# Step 3: Full first-time deployment
# (create directory tree + copy all files + register service)
# ============================================================
full_deploy() {
    info "=== Full deployment ==="

    collect_libs
    LIB_DIR="${PROJECT_ROOT}/deploy/tmp_libs"

    # Create directory tree
    info "Creating directory tree..."
    $SSH "mkdir -p /opt/retail/{app,lib,config,data/faces,log,ota}"

    # Copy the executable
    info "Deploying RetailClient..."
    $SCP "${BINARY}" root@${TARGET_IP}:/opt/retail/app/

    # Copy shared libraries
    if [ -d "${LIB_DIR}" ] && [ "$(ls -A ${LIB_DIR})" ]; then
        info "Deploying shared libraries..."
        $SCP "${LIB_DIR}"/* root@${TARGET_IP}:/opt/retail/lib/
    else
        warn "No libraries collected. OK if Qt is already installed on the board."
    fi

    # Optional: deploy configuration file
    if [ -f "${PROJECT_ROOT}/deploy/client.conf" ]; then
        info "Deploying configuration..."
        $SCP "${PROJECT_ROOT}/deploy/client.conf" root@${TARGET_IP}:/opt/retail/config/
    fi

    # Register the systemd service
    info "Registering systemd service..."
    $SCP "${PROJECT_ROOT}/deploy/retail-client.service" root@${TARGET_IP}:/etc/systemd/system/

    # Refresh the dynamic linker cache
    $SSH "echo '/opt/retail/lib' > /etc/ld.so.conf.d/retail.conf && ldconfig"

    # Enable and (do not yet start) the service
    $SSH "systemctl daemon-reload"
    $SSH "systemctl enable retail-client"

    info "=== Full deployment complete ==="
    info "Start with:    ssh root@${TARGET_IP} systemctl start retail-client"
    info "Check status:  ssh root@${TARGET_IP} systemctl status retail-client"
}

# ============================================================
# Step 4: Incremental update (OTA-style binary swap)
# ============================================================
incremental_deploy() {
    info "=== Incremental update (OTA mode) ==="

    # Stop the service first
    info "Stopping service..."
    $SSH "systemctl stop retail-client" || warn "Service may not be running"

    # Back up the previous binary for rollback
    $SSH "cp /opt/retail/app/RetailClient /opt/retail/ota/RetailClient.bak" || true

    # Push the new binary
    info "Updating binary..."
    $SCP "${BINARY}" root@${TARGET_IP}:/opt/retail/app/RetailClient

    # Restart
    info "Restarting service..."
    $SSH "systemctl start retail-client"

    # Wait briefly and verify
    sleep 3
    STATUS=$($SSH "systemctl is-active retail-client")
    if [ "${STATUS}" = "active" ]; then
        info "Service status: ${GREEN}active OK${NC}"
    else
        error "Service status: ${RED}${STATUS}${NC} - rolling back..."
        $SSH "cp /opt/retail/ota/RetailClient.bak /opt/retail/app/RetailClient"
        $SSH "systemctl start retail-client"
    fi

    info "=== Incremental update complete ==="
}

# ============================================================
# Entry point
# ============================================================

info "Target: ${TARGET_IP}"

# Connectivity check
if ! ${SSH} "echo OK" 2>/dev/null; then
    error "Unable to SSH to ${TARGET_IP}. Check network and SSH configuration."
fi

if [ "${FULL_DEPLOY}" = "--full" ]; then
    full_deploy
else
    incremental_deploy
fi

echo ""
info "Deployment finished."
echo "  View logs:    ssh root@${TARGET_IP} journalctl -u retail-client -f"
echo "  Restart:      ssh root@${TARGET_IP} systemctl restart retail-client"
