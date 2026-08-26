#!/bin/bash
# ============================================================
# AI model download script for the retail system
# Usage: ./download_models.sh [--all|--landmark|--antispoof|--detect|--feature]
# ============================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ---------- Model URLs ----------
# UltraFace face detection
DETECT_ONNX="version-RFB-320.onnx"
DETECT_URL="https://github.com/Linzaer/Ultra-Light-Fast-Generic-Face-Detector-1MB/raw/master/models/onnx/version-RFB-320.onnx"

# MobileFaceNet feature extraction
FEATURE_ONNX="mobilefacenet.onnx"
FEATURE_URL="https://github.com/deepinsight/insightface/raw/master/model_zoo/mobilefacenet/mobilefacenet.onnx"

# PFLD 106-point face landmark
LANDMARK_ONNX="pfld_106.onnx"
LANDMARK_URL="https://github.com/Hsintao/pfld_106_face_landmarks/raw/master/onnx/pfld_106.onnx"

# SilentFace anti-spoofing (MiniFASNet)
ANTISPOOF_ONNX="miniFASNet.onnx"
ANTISPOOF_URL="https://github.com/minivision-ai/Silent-Face-Anti-Spoofing/raw/master/resources/anti_spoof_models/miniFASNet.onnx"

# Silero VAD (voice activity detection)
VAD_ONNX="silero_vad.onnx"
VAD_URL="https://github.com/snakers4/silero-vad/raw/master/files/silero_vad.onnx"

download_if_missing() {
    local name="$1" url="$2" file="$3"
    if [ -f "$file" ]; then
        echo "[SKIP] $name already exists: $file"
        return 0
    fi
    echo "[DL]  Downloading $name -> $file"
    echo "      URL: $url"
    if command -v wget &>/dev/null; then
        wget -q --show-progress -O "$file" "$url" || { rm -f "$file"; echo "[FAIL] Download failed: $name"; return 1; }
    elif command -v curl &>/dev/null; then
        curl -fSL -o "$file" "$url" || { rm -f "$file"; echo "[FAIL] Download failed: $name"; return 1; }
    else
        echo "[FAIL] wget or curl not found, please download manually"
        return 1
    fi
    local size=$(stat -c%s "$file" 2>/dev/null || echo "0")
    echo "[OK]  $name downloaded (${size} bytes)"
}

# ---------- Argument parsing ----------
DL_DETECT=0 DL_FEATURE=0 DL_LANDMARK=0 DL_ANTISPOOF=0 DL_VAD=0

if [ $# -eq 0 ]; then
    echo "Usage: $0 [--all|--detect|--feature|--landmark|--antispoof|--vad]"
    echo ""
    echo "Models:"
    echo "  --detect     UltraFace-RFB-320 face detection"
    echo "  --feature    MobileFaceNet feature extraction"
    echo "  --landmark   PFLD 106-point face landmark"
    echo "  --antispoof  MiniFASNet anti-spoofing"
    echo "  --vad        Silero VAD voice activity detection"
    echo "  --all        Download all models"
    exit 0
fi

for arg in "$@"; do
    case "$arg" in
        --all)       DL_DETECT=1; DL_FEATURE=1; DL_LANDMARK=1; DL_ANTISPOOF=1; DL_VAD=1 ;;
        --detect)    DL_DETECT=1 ;;
        --feature)   DL_FEATURE=1 ;;
        --landmark)  DL_LANDMARK=1 ;;
        --antispoof) DL_ANTISPOOF=1 ;;
        --vad)       DL_VAD=1 ;;
        *)           echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

echo "========================================"
echo "  Retail System AI Model Download"
echo "========================================"

[ $DL_DETECT -eq 1 ]    && download_if_missing "UltraFace detection"   "$DETECT_URL"    "$DETECT_ONNX"
[ $DL_FEATURE -eq 1 ]   && download_if_missing "MobileFaceNet feature"  "$FEATURE_URL"   "$FEATURE_ONNX"
[ $DL_LANDMARK -eq 1 ]  && download_if_missing "PFLD landmark"          "$LANDMARK_URL"  "$LANDMARK_ONNX"
[ $DL_ANTISPOOF -eq 1 ] && download_if_missing "MiniFASNet anti-spoof"  "$ANTISPOOF_URL" "$ANTISPOOF_ONNX"
[ $DL_VAD -eq 1 ]       && download_if_missing "Silero VAD"             "$VAD_URL"       "$VAD_ONNX"

echo ""
echo "========================================"
echo "  Done! Convert to RKNN with:"
echo "  python3 convert_to_rknn.py --all"
echo "========================================"
