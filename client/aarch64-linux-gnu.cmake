# CMake toolchain file for aarch64 (RK3568) cross-compilation
#
# Usage:
#   cmake -B build_arm64 -DCMAKE_TOOLCHAIN_FILE=aarch64-linux-gnu.cmake \
#         -DRK3568_SYSROOT=/path/to/arm64/rootfs
#
# Environment variables (fallback):
#   RK3568_SYSROOT  Path to Ubuntu 22.04 arm64 rootfs.
#
# Uses the wrapper scripts aarch64-sysroot-gcc / aarch64-sysroot-g++ to
# work around glibc ABI mismatches between the host cross-compiler
# (Ubuntu 20.04, glibc 2.31) and the rootfs (Ubuntu 22.04, glibc 2.35).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# sysroot: Ubuntu 22.04 arm64 rootfs
if(NOT DEFINED RK3568_SYSROOT)
    if(DEFINED ENV{RK3568_SYSROOT})
        set(RK3568_SYSROOT $ENV{RK3568_SYSROOT})
    else()
        message(FATAL_ERROR "RK3568_SYSROOT is not set. Point it to the arm64 rootfs.")
    endif()
endif()
set(SYSROOT_PATH "${RK3568_SYSROOT}")
set(CMAKE_SYSROOT ${SYSROOT_PATH})

# Cross-compiler wrapper: uses the original compiler for compilation and
# the rootfs crt/libstdc++ for linking.
set(TOOLCHAIN_WRAPPER_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_C_COMPILER   "${TOOLCHAIN_WRAPPER_DIR}/aarch64-sysroot-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_WRAPPER_DIR}/aarch64-sysroot-g++")

set(CMAKE_FIND_ROOT_PATH ${SYSROOT_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Qt5 CMake config inside the sysroot aarch64-linux-gnu subtree
set(QT5_CMAKE_DIR "${SYSROOT_PATH}/usr/lib/aarch64-linux-gnu/cmake")
set(CMAKE_PREFIX_PATH "${QT5_CMAKE_DIR}/Qt5")

# pkg-config cross-compilation support
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_LIBDIR} "${SYSROOT_PATH}/usr/lib/aarch64-linux-gnu/pkgconfig:${SYSROOT_PATH}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} ${SYSROOT_PATH})
