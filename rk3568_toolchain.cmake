# CMake toolchain for RK3568 aarch64 cross-compilation
#
# Override these paths via -D on the cmake command line, or by setting the
# corresponding environment variables before invoking cmake:
#
#   RK3568_TOOLCHAIN_ROOT  Path to Linaro aarch64-linux-gnu GCC toolchain
#   RK3568_SYSROOT         Path to extracted arm64 rootfs (libc, crti.o, ...)
#   RKNN_SDK_ROOT          Path to librknn_api inside the rknpu2 SDK
#
# Example:
#   cmake -B build_arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=rk3568_toolchain.cmake \
#         -DRK3568_TOOLCHAIN_ROOT=/opt/toolchains/gcc-linaro-7.5.0 \
#         -DRK3568_SYSROOT=/opt/rk3568_sysroot \
#         -DRKNN_SDK_ROOT=/opt/rknn-sdk

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Linaro GCC toolchain
if(NOT DEFINED RK3568_TOOLCHAIN_ROOT)
    if(DEFINED ENV{RK3568_TOOLCHAIN_ROOT})
        set(RK3568_TOOLCHAIN_ROOT $ENV{RK3568_TOOLCHAIN_ROOT})
    else()
        message(FATAL_ERROR "RK3568_TOOLCHAIN_ROOT is not set. Point it to the Linaro aarch64 GCC root.")
    endif()
endif()
set(TOOLCHAIN_ROOT ${RK3568_TOOLCHAIN_ROOT})
set(CMAKE_C_COMPILER   ${TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_ROOT}/bin/aarch64-linux-gnu-g++)

# Sysroot from extracted arm64 rootfs (provides libc, crti.o etc.)
if(NOT DEFINED RK3568_SYSROOT)
    if(DEFINED ENV{RK3568_SYSROOT})
        set(RK3568_SYSROOT $ENV{RK3568_SYSROOT})
    else()
        message(FATAL_ERROR "RK3568_SYSROOT is not set. Point it to the extracted arm64 rootfs.")
    endif()
endif()
set(CMAKE_SYSROOT ${RK3568_SYSROOT})

# RKNN SDK
if(NOT DEFINED RKNN_SDK_ROOT)
    if(DEFINED ENV{RKNN_SDK_ROOT})
        set(RKNN_SDK_ROOT $ENV{RKNN_SDK_ROOT})
    else()
        message(WARNING "RKNN_SDK_ROOT is not set; RKNN-dependent targets will fail to configure.")
    endif()
endif()

# Host include dirs for Qt/FFmpeg/ALSA headers (arch-independent)
include_directories(BEFORE SYSTEM
    /usr/include/x86_64-linux-gnu
    /usr/include
)

if(RKNN_SDK_ROOT)
    include_directories(BEFORE SYSTEM ${RKNN_SDK_ROOT}/include)
endif()

# Qt5
set(Qt5_DIR /usr/lib/x86_64-linux-gnu/cmake/Qt5)

# OpenCV
set(OpenCV_DIR /usr/lib/x86_64-linux-gnu/cmake/opencv4)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Workaround for x86 FFmpeg4 header intptr_t precision issue
# (not needed on aarch64 where long == pointer width)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fpermissive")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive")
