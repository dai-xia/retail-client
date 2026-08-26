# Retail client - x86 desktop build (NPU/RGA use stub functions, UI runnable)
#
# Configure SDK paths via one of:
#   1. client/local_config.pri  (copy local_config.pri.example and edit)
#   2. Environment variables: RKNN_SDK_ROOT, RGA_INCLUDE, VOSK_LIB_DIR
#   3. qmake -D RKNN_SDK_ROOT=... -D VOSK_LIB_DIR=...

QT       += core gui widgets network concurrent
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = RetailClient
TEMPLATE = app

DESTDIR = $$PWD/../build/client
OBJECTS_DIR = $$PWD/../build/client/.obj
MOC_DIR = $$PWD/../build/client/.moc
UI_DIR = $$PWD/../build/client/.ui

INCLUDEPATH += ../common
INCLUDEPATH += ../common/cjson
INCLUDEPATH += /usr/include/opencv4

# Local per-developer config (git-ignored). Copy local_config.pri.example.
exists(local_config.pri): include(local_config.pri)

# RKNN SDK headers (x86 build links against stubs)
isEmpty(RKNN_SDK_ROOT) {
    RKNN_SDK_ROOT = $$(RKNN_SDK_ROOT)
}
isEmpty(RKNN_SDK_ROOT) {
    error("RKNN_SDK_ROOT is not set. See client/local_config.pri.example.")
}
INCLUDEPATH += $$RKNN_SDK_ROOT/include

# RGA headers (extracted from RK3568 sysroot; x86 build links against stubs)
isEmpty(RGA_INCLUDE) {
    RGA_INCLUDE = $$(RGA_INCLUDE)
}
isEmpty(RGA_INCLUDE) {
    warning("RGA_INCLUDE not set. Defaulting to /usr/include/rga.")
    RGA_INCLUDE = /usr/include/rga
}
INCLUDEPATH += $$RGA_INCLUDE

# libdrm (xf86drm.h -> drm.h, DMA-BUF allocation)
INCLUDEPATH += /usr/include/libdrm
LIBS += -ldrm


SOURCES += \
    main.cpp \
    config/client_config.cpp \
    ui/mainwindow.cpp \
    core/networkmanager.cpp \
    ui/memberregister.cpp \
    ui/goodsmanager.cpp \
    ui/settlementwidget.cpp \
    ui/facepaywidget.cpp \
    ui/orderquery.cpp \
    ui/rechargewidget.cpp \
    services/voice_engine.cpp \
    services/voice_assist.cpp \
    core/localdbmanager.cpp \
    services/crashhandler.cpp \
    services/otaupdater.cpp \
    services/clientservice.cpp \
    core/hardwareservice.cpp \
    core/hardware_api.c \
    core/facemanager.cpp \
    core/hardwarethread.cpp \
    core/alsa_capture.c \
    core/audio_preproc.c \
    core/v4l2_capture.c \
    core/rkisp_camera.c \
    core/rga_transform.c \
    core/rga_osd.c \
    core/video_encoder.c \
    core/audio_encoder.c \
    core/rtsp_streamer.c \
    core/face_image_utils.c \
    core/face_landmark.c \
    core/face_antispoof.c \
    core/h264_parser.c \
    core/mp4_recorder.c \
    core/av_recorder.c \
    ../common/logger.c \
    core/watchdog.c \
    core/hw_watchdog.c \
    core/ota.c \
    ../common/cjson/cJSON.c \
    ../common/crypto.c \
    core/rknn_x86_stub.cpp \
    core/rga_x86_stub.cpp

HEADERS += \
    config/client_config.h \
    ui/mainwindow.h \
    core/networkmanager.h \
    ui/memberregister.h \
    ui/goodsmanager.h \
    ui/settlementwidget.h \
    ui/facepaywidget.h \
    ui/orderquery.h \
    ui/rechargewidget.h \
    config/payment_provider.h \
    config/mock_payment_provider.h \
    services/voice_engine.h \
    services/voice_assist.h \
    services/vosk_capi.h \
    core/localdbmanager.h \
    services/crashhandler.h \
    services/otaupdater.h \
    services/clientservice.h \
    core/hardwareservice.h \
    core/hardware_api.h \
    core/facemanager.h \
    core/hardwarethread.h \
    core/alsa_capture.h \
    core/audio_preproc.h \
    core/v4l2_capture.h \
    core/rkisp_camera.h \
    core/rga_transform.h \
    core/rga_osd.h \
    core/video_encoder.h \
    core/audio_encoder.h \
    core/rtsp_streamer.h \
    core/face_landmark.h \
    core/face_antispoof.h \
    core/h264_parser.h \
    core/mp4_recorder.h \
    core/av_recorder.h \
    ../common/logger.h \
    core/watchdog.h \
    core/hw_watchdog.h \
    core/ota.h \
    ../common/cjson/cJSON.h \
    ../common/crypto.h \
    ../common/common.h

FORMS += \
    ui/mainwindow.ui \
    ui/memberregister.ui \
    ui/goodsmanager.ui \
    ui/settlementwidget.ui \
    ui/facepaywidget.ui \
    ui/orderquery.ui \
    ui/rechargewidget.ui

# --- Mandatory dependencies (zero-copy pipeline, no fallback) ---

# Base libraries
LIBS += -lpthread -lsqlite3 -lssl -lcrypto -lcurl -ldl -lm

# OpenCV (face detection uses cv::Mat)
LIBS += $$system(pkg-config --libs opencv4)

# FFmpeg (video encoding / RTSP streaming / local recording)
DEFINES += USE_FFMPEG
LIBS += -lavcodec -lavformat -lavutil -lswscale -lswresample -lavdevice

# ALSA (native audio capture)
DEFINES += USE_ALSA
LIBS += -lasound

# SpeexDSP (audio front-end: AEC/NS/AGC/VAD, x86 system library)
DEFINES += USE_SPEEXDSP
LIBS += -lspeexdsp

# RKNN + RGA (x86 build links against stubs; UI is still runnable on desktop)
DEFINES += USE_RKNN USE_RGA

# Vosk (offline speech recognition)
isEmpty(VOSK_LIB_DIR) {
    VOSK_LIB_DIR = $$(VOSK_LIB_DIR)
}
isEmpty(VOSK_LIB_DIR) {
    error("VOSK_LIB_DIR is not set. See client/local_config.pri.example.")
}
LIBS += -L$$VOSK_LIB_DIR -lvosk
QMAKE_RPATHDIR += $$VOSK_LIB_DIR

CONFIG += c++11
DEFINES += QT_DEPRECATED_WARNINGS
