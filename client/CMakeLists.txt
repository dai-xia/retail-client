cmake_minimum_required(VERSION 3.14)
project(RetailClient LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ========== Cross-compilation detection ==========
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compiling for: ${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "Sysroot: ${CMAKE_SYSROOT}")
    # Prefer static linking when cross-compiling (when .a is available)
    option(USE_STATIC_LIBS "Link statically when possible" ON)
else()
    option(USE_STATIC_LIBS "Link statically when possible" OFF)
endif()

# ========== Qt (always dynamically linked; no static build available) ==========
find_package(Qt5 COMPONENTS Core Gui Widgets Network Concurrent REQUIRED)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

# When cross-compiling, the host moc/uic/rcc must be used (the target's
# aarch64 binaries cannot run on x86_64). Must be set after find_package(Qt5),
# otherwise the Qt5 CMake config overrides these.
if(CMAKE_CROSSCOMPILING)
    find_program(HOST_MOC moc-qt5 moc PATHS /usr/lib/qt5/bin NO_DEFAULT_PATH)
    find_program(HOST_UIC uic-qt5 uic PATHS /usr/lib/qt5/bin NO_DEFAULT_PATH)
    find_program(HOST_RCC rcc-qt5 rcc PATHS /usr/lib/qt5/bin NO_DEFAULT_PATH)
    if(HOST_MOC AND HOST_UIC AND HOST_RCC)
        message(STATUS "Using host Qt5 tools: moc=${HOST_MOC}, uic=${HOST_UIC}, rcc=${HOST_RCC}")
        # Override the target tool paths set by the Qt5 CMake config
        set(CMAKE_AUTOMOC_EXECUTABLE ${HOST_MOC})
        set(CMAKE_AUTOUIC_EXECUTABLE ${HOST_UIC})
        set(CMAKE_AUTORCC_EXECUTABLE ${HOST_RCC})
        # Also override the imported-target properties of Qt5Core
        set_property(TARGET Qt5::moc PROPERTY IMPORTED_LOCATION ${HOST_MOC})
        set_property(TARGET Qt5::uic PROPERTY IMPORTED_LOCATION ${HOST_UIC})
        set_property(TARGET Qt5::rcc PROPERTY IMPORTED_LOCATION ${HOST_RCC})
    else()
        message(FATAL_ERROR "Cross-compiling requires the host Qt5 moc/uic/rcc. Install qtbase5-dev-tools.")
    endif()
endif()

# ========== OpenSSL ==========
if(USE_STATIC_LIBS)
    # Prefer static libraries: libssl.a, libcrypto.a
    find_library(OPENSSL_SSL_LIBRARY NAMES ssl PATH_SUFFIXES aarch64-linux-gnu)
    get_filename_component(OPENSSL_LIB_DIR "${OPENSSL_SSL_LIBRARY}" DIRECTORY)
    find_library(OPENSSL_SSL_STATIC libssl.a PATHS "${OPENSSL_LIB_DIR}" NO_DEFAULT_PATH)
    find_library(OPENSSL_CRYPTO_STATIC libcrypto.a PATHS "${OPENSSL_LIB_DIR}" NO_DEFAULT_PATH)
    if(OPENSSL_SSL_STATIC AND OPENSSL_CRYPTO_STATIC)
        message(STATUS "OpenSSL static link: ${OPENSSL_SSL_STATIC}, ${OPENSSL_CRYPTO_STATIC}")
        set(OPENSSL_LIBS "${OPENSSL_SSL_STATIC};${OPENSSL_CRYPTO_STATIC}")
    else()
        message(STATUS "OpenSSL static lib not found, falling back to dynamic link")
        find_package(OpenSSL REQUIRED)
        set(OPENSSL_LIBS "OpenSSL::SSL;OpenSSL::Crypto")
    endif()
else()
    find_package(OpenSSL REQUIRED)
    set(OPENSSL_LIBS "OpenSSL::SSL;OpenSSL::Crypto")
endif()

# ========== libcurl ==========
if(USE_STATIC_LIBS)
    find_library(CURL_LIBRARY NAMES curl PATH_SUFFIXES aarch64-linux-gnu)
    get_filename_component(CURL_LIB_DIR "${CURL_LIBRARY}" DIRECTORY)
    find_library(CURL_STATIC libcurl.a PATHS "${CURL_LIB_DIR}" NO_DEFAULT_PATH)
    if(CURL_STATIC)
        message(STATUS "libcurl static link: ${CURL_STATIC}")
        # Statically linking curl needs extra dependencies
        find_library(Z_STATIC libz.a PATHS "${CURL_LIB_DIR}" NO_DEFAULT_PATH)
        set(CURL_LINK_LIBS "${CURL_STATIC}")
        if(Z_STATIC)
            list(APPEND CURL_LINK_LIBS "${Z_STATIC}")
        endif()
    else()
        message(STATUS "libcurl static lib not found, falling back to dynamic link")
        find_package(CURL REQUIRED)
        set(CURL_LINK_LIBS "CURL::libcurl")
    endif()
else()
    find_package(CURL REQUIRED)
    set(CURL_LINK_LIBS "CURL::libcurl")
endif()

# ========== SQLite3 ==========
if(USE_STATIC_LIBS)
    find_library(SQLITE3_LIBRARY NAMES sqlite3 PATH_SUFFIXES aarch64-linux-gnu)
    get_filename_component(SQLITE3_LIB_DIR "${SQLITE3_LIBRARY}" DIRECTORY)
    find_library(SQLITE3_STATIC libsqlite3.a PATHS "${SQLITE3_LIB_DIR}" NO_DEFAULT_PATH)
    if(SQLITE3_STATIC)
        message(STATUS "SQLite3 静态链接: ${SQLITE3_STATIC}")
        set(SQLITE3_LINK_LIBS "${SQLITE3_STATIC}")
    else()
        message(STATUS "SQLite3 静态库未找到, 回退动态链接")
        find_package(SQLite3 REQUIRED)
        set(SQLITE3_LINK_LIBS "SQLite::SQLite3")
    endif()
else()
    find_package(SQLite3 REQUIRED)
    set(SQLITE3_LINK_LIBS "SQLite::SQLite3")
endif()


set(CLIENT_SOURCES
    main.cpp
    config/client_config.cpp
    ui/mainwindow.cpp
    core/networkmanager.cpp
    ui/memberregister.cpp
    ui/goodsmanager.cpp
    ui/settlementwidget.cpp
    ui/facepaywidget.cpp
    ui/orderquery.cpp
    ui/rechargewidget.cpp
    services/voice_engine.cpp
    services/voice_assist.cpp
    core/localdbmanager.cpp
    services/crashhandler.cpp
    services/otaupdater.cpp
    services/clientservice.cpp
    core/hardwareservice.cpp
    core/hardware_api.c
    core/alsa_capture.c
    core/audio_preproc.c
    core/video_encoder.c
    core/audio_encoder.c
    core/rtsp_streamer.c
    core/v4l2_capture.c
    core/rkisp_camera.c
    core/rga_transform.c
    core/rga_osd.c
    core/face_image_utils.c
    core/face_landmark.c
    core/face_antispoof.c
    core/h264_parser.c
    core/mp4_recorder.c
    core/facemanager.cpp
    core/hardwarethread.cpp
    core/watchdog.c
    core/hw_watchdog.c
    core/ota.c
)

set(CLIENT_HEADERS
    config/client_config.h
    ui/mainwindow.h
    core/networkmanager.h
    ui/memberregister.h
    ui/goodsmanager.h
    ui/settlementwidget.h
    ui/facepaywidget.h
    ui/orderquery.h
    ui/rechargewidget.h
    config/payment_provider.h
    config/mock_payment_provider.h
    services/voice_engine.h
    services/voice_assist.h
    services/vosk_capi.h
    core/localdbmanager.h
    services/crashhandler.h
    services/otaupdater.h
    services/clientservice.h
    core/hardwareservice.h
    core/hardware_api.h
    core/alsa_capture.h
    core/video_encoder.h
    core/audio_encoder.h
    core/rtsp_streamer.h
    core/v4l2_capture.h
    core/rkisp_camera.h
    core/rga_transform.h
    core/rga_osd.h
    core/face_landmark.h
    core/face_antispoof.h
    core/h264_parser.h
    core/mp4_recorder.h
    core/facemanager.h
    core/hardwarethread.h
    core/watchdog.h
    core/hw_watchdog.h
    core/ota.h
)

set(CLIENT_UIS
    ui/mainwindow.ui
    ui/memberregister.ui
    ui/goodsmanager.ui
    ui/settlementwidget.ui
    ui/facepaywidget.ui
    ui/orderquery.ui
    ui/rechargewidget.ui
)


set(COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/logger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/cjson/cJSON.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/crypto.c
)

set(COMMON_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/logger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/cjson/cJSON.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/crypto.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/common.h
)

# ========== RKNN (optional, RK3568 NPU acceleration) ==========
# RKNN SDK path (installed from the Rockchip rknpu2 SDK on the build host)
set(RKNN_SDK_ROOT "" CACHE PATH "RKNN SDK root (contains include/rknn_api.h and lib)")
if(NOT RKNN_SDK_ROOT)
    # Auto-detect common SDK install locations
    file(GLOB RKNN_SDK_HINTS
        "/opt/rknn-sdk"
        "/usr/local/rknn-sdk"
        "$ENV{RKNN_SDK_ROOT}"
    )
    if(RKNN_SDK_HINTS)
        list(GET RKNN_SDK_HINTS 0 RKNN_SDK_ROOT)
        message(STATUS "Auto-detected RKNN SDK: ${RKNN_SDK_ROOT}")
    endif()
endif()

if(CMAKE_CROSSCOMPILING)
    find_library(RKNN_LIB rknnrt
        HINTS ${RKNN_SDK_ROOT}/aarch64
              ${RKNN_SDK_ROOT}/lib
              ${CMAKE_SYSROOT}/usr/lib
              ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu
    )
    find_path(RKNN_INCLUDE_DIR rknn_api.h
        HINTS ${RKNN_SDK_ROOT}/include
              ${CMAKE_SYSROOT}/usr/include
              ${CMAKE_SYSROOT}/usr/include/rknn
    )
else()
    find_library(RKNN_LIB rknnrt
        HINTS ${RKNN_SDK_ROOT}/aarch64
              ${RKNN_SDK_ROOT}/lib
              /usr/local/lib
              /usr/lib
              /usr/lib/aarch64-linux-gnu
    )
    find_path(RKNN_INCLUDE_DIR rknn_api.h
        HINTS ${RKNN_SDK_ROOT}/include
              /usr/local/include
              /usr/local/include/rknn
              /usr/include/rknn
    )
endif()
if(RKNN_LIB AND RKNN_INCLUDE_DIR)
    message(STATUS "Found RKNN: lib=${RKNN_LIB}, include=${RKNN_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "RKNN SDK required (librknnrt + rknn_api.h). Please install RKNN SDK for RK3568.")
endif()
add_definitions(-DUSE_RKNN)

# ========== rkaiq (MIPI camera 3A, ARM cross-compile only) ==========
# The MIPI sensor (OV5695) outputs RAW; rkisp ISP + rkaiq 3A are required to
# produce NV12. On x86 there is no rkaiq, so rkisp_camera.c compiles its stub
# branch (rkisp_camera_open returns NULL and we fall back to USB).
if(CMAKE_CROSSCOMPILING)
    find_library(RKAIQ_LIB rkaiq
        HINTS ${CMAKE_SYSROOT}/usr/lib
              ${CMAKE_SYSROOT}/usr/lib64
    )
    if(RKAIQ_LIB)
        set(RKAIQ_INCLUDE_DIRS
            ${CMAKE_SYSROOT}/usr/include/rkaiq
            ${CMAKE_SYSROOT}/usr/include/rkaiq/uAPI2
            ${CMAKE_SYSROOT}/usr/include/rkaiq/common
            ${CMAKE_SYSROOT}/usr/include/rkaiq/algos
            ${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser
            ${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser_v2
        )
        add_definitions(-DHAVE_RKAIQ)
        message(STATUS "Found rkaiq: ${RKAIQ_LIB}")
    else()
        message(WARNING "rkaiq not found, MIPI camera disabled (USB only)")
    endif()
else()
    message(STATUS "x86 build: MIPI camera (rkaiq) disabled")
endif()

# ========== RGA (mandatory, RK3568 2D hardware acceleration) ==========
if(CMAKE_CROSSCOMPILING)
    find_library(RGA_LIB rga
        HINTS ${CMAKE_SYSROOT}/usr/lib
              ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu
    )
    find_path(RGA_INCLUDE_DIR
        NAMES RgaApi.h
        HINTS ${CMAKE_SYSROOT}/usr/include
              ${CMAKE_SYSROOT}/usr/include/rga
    )
else()
    find_library(RGA_LIB rga
        HINTS /usr/local/lib
              /usr/lib
              /usr/lib/aarch64-linux-gnu
    )
    find_path(RGA_INCLUDE_DIR
        NAMES RgaApi.h
        HINTS /usr/local/include
              /usr/include/rga
    )
endif()
if(RGA_LIB AND RGA_INCLUDE_DIR)
    message(STATUS "Found RGA: ${RGA_LIB}, include: ${RGA_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "librga required for RK3568 2D hardware acceleration.")
endif()
add_definitions(-DUSE_RGA)

# ========== ALSA (mandatory, native audio capture) ==========
find_package(ALSA)
if(ALSA_FOUND)
    message(STATUS "Found ALSA: ${ALSA_LIBRARIES}")
    set(ALSA_LINK_LIBS "${ALSA_LIBRARIES}")
else()
    message(FATAL_ERROR "ALSA required for native audio capture.")
endif()
add_definitions(-DUSE_ALSA)


find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FFMPEG libavcodec libavformat libavutil libswscale libswresample)

    pkg_check_modules(AVDEVICE libavdevice)
endif()
if(NOT FFMPEG_FOUND)

    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
        HINTS /usr/include/x86_64-linux-gnu /usr/local/include)
    find_library(FFMPEG_AVCODEC avcodec)
    find_library(FFMPEG_AVFORMAT avformat)
    find_library(FFMPEG_AVUTIL avutil)
    find_library(FFMPEG_SWSCALE swscale)
    find_library(FFMPEG_SWRESAMPLE swresample)
    find_library(FFMPEG_AVDEVICE avdevice)
    if(FFMPEG_AVCODEC AND FFMPEG_AVFORMAT AND FFMPEG_AVUTIL AND FFMPEG_SWSCALE)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_LIBRARIES ${FFMPEG_AVCODEC} ${FFMPEG_AVFORMAT} ${FFMPEG_AVUTIL} ${FFMPEG_SWSCALE} ${FFMPEG_SWRESAMPLE})
        if(FFMPEG_AVDEVICE)
            list(APPEND FFMPEG_LIBRARIES ${FFMPEG_AVDEVICE})
        endif()
        set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIR})
    endif()
endif()
if(FFMPEG_FOUND)
    message(STATUS "Found FFmpeg: ${FFMPEG_LIBRARIES}")
else()
    message(FATAL_ERROR "FFmpeg required for video encoding/RTSP streaming.")
endif()
add_definitions(-DUSE_FFMPEG)

# ========== Vosk (optional, always dynamically linked) ==========

# ========== SpeexDSP (optional, audio front-end AEC/NS/AGC/VAD) ==========
find_library(SPEEXDSP_LIB speexdsp)
find_path(SPEEXDSP_INCLUDE_DIR speex/speex_preprocess.h)
if(SPEEXDSP_LIB AND SPEEXDSP_INCLUDE_DIR)
    message(STATUS "Found SpeexDSP: ${SPEEXDSP_LIB}, include: ${SPEEXDSP_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "SpeexDSP required for audio preprocessing (AEC/NS/AGC).")
endif()
add_definitions(-DUSE_SPEEXDSP)
if(CMAKE_CROSSCOMPILING)
    find_library(VOSK_LIB vosk
        HINTS ${CMAKE_SYSROOT}/usr/lib
    )
else()
    find_library(VOSK_LIB vosk
        HINTS "$ENV{VOSK_LIB_DIR}"
              /usr/local/lib
              /usr/lib
    )
endif()
if(VOSK_LIB)
    message(STATUS "Found Vosk: ${VOSK_LIB}")
else()
    message(WARNING "Vosk not found, building without voice support")
    add_definitions(-DNO_VOSK)
    set(VOSK_LIB "")
endif()


add_executable(${PROJECT_NAME}
    ${CLIENT_SOURCES}
    ${CLIENT_HEADERS}
    ${CLIENT_UIS}
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/cjson
)
if(RGA_INCLUDE_DIR)
    target_include_directories(${PROJECT_NAME} PRIVATE ${RGA_INCLUDE_DIR})
endif()
if(RKNN_INCLUDE_DIR)
    target_include_directories(${PROJECT_NAME} PRIVATE ${RKNN_INCLUDE_DIR})
endif()
if(FFMPEG_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
endif()
if(SPEEXDSP_INCLUDE_DIR)
    target_include_directories(${PROJECT_NAME} PRIVATE ${SPEEXDSP_INCLUDE_DIR})
endif()
if(RKAIQ_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${RKAIQ_INCLUDE_DIRS})
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    QT_DEPRECATED_WARNINGS
)


target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network
    Qt5::Concurrent
    ${OPENSSL_LIBS}
    ${CURL_LINK_LIBS}
    ${SQLITE3_LINK_LIBS}
    pthread
    dl
    m
    ${RKNN_LIB}
    ${RGA_LIB}
    ${RKAIQ_LIB}
    ${ALSA_LINK_LIBS}
    ${FFMPEG_LIBRARIES}
    ${SPEEXDSP_LIB}
    ${VOSK_LIB}
)


if(CMAKE_CROSSCOMPILING)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH "/usr/lib/aarch64-linux-gnu:/usr/lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
else()

    if(VOSK_LIB)
        get_filename_component(VOSK_LIB_DIR ${VOSK_LIB} DIRECTORY)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            BUILD_RPATH "${VOSK_LIB_DIR}"
            INSTALL_RPATH "${VOSK_LIB_DIR}"
        )
    endif()
endif()
