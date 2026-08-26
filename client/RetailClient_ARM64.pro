#
# RetailClient_ARM64.pro - RK3568 ARM64 cross-compilation (zero-copy pipeline)
#
# Usage:
#   1. Prepare an aarch64 sysroot (scp board libs to /tmp/board_libs)
#   2. Build:
#      mkdir -p build/client_arm64 && cd build/client_arm64
#      qmake ../../client/RetailClient_ARM64.pro \
#            SYSROOT=/path/to/rk3568_sysroot \
#            RKNN_SDK_ROOT=/path/to/librknn_api \
#            BOARD_LIBS=/tmp/board_libs
#      make -j$(nproc)
#   3. Deploy:
#      scp ../build/client_arm64/RetailClient root@<board_ip>:/opt/retail/
#
# Required qmake variables (or environment variables of the same name):
#   SYSROOT          Path to the extracted RK3568 arm64 rootfs.
#   RKNN_SDK_ROOT    Path to librknn_api inside the rknpu2 SDK.
#   BOARD_LIBS       Directory with Qt5 .so files copied from the board
#                    (defaults to /tmp/board_libs).
#

# Do not use QT += (would pull in x86 Qt5); specify Qt5 paths manually.
QT       =
CONFIG   += c++11 warn_on

TARGET = RetailClient
TEMPLATE = app

# ===== Cross-compiler =====
QMAKE_CC        = aarch64-linux-gnu-gcc
QMAKE_CXX       = aarch64-linux-gnu-g++
QMAKE_LINK      = aarch64-linux-gnu-g++
QMAKE_AR        = aarch64-linux-gnu-ar cqs
QMAKE_OBJCOPY   = aarch64-linux-gnu-objcopy
QMAKE_STRIP     = aarch64-linux-gnu-strip

# ===== Resolve sysroot from qmake variable or environment =====
isEmpty(SYSROOT) {
    SYSROOT = $$(SYSROOT)
}
isEmpty(SYSROOT) {
    error("SYSROOT is not set. Pass -D SYSROOT=/path/to/arm64/rootfs or export SYSROOT.")
}

isEmpty(BOARD_LIBS) {
    BOARD_LIBS = $$(BOARD_LIBS)
}
isEmpty(BOARD_LIBS) {
    BOARD_LIBS = /tmp/board_libs
}

# Override qmake auto-detected x86 Qt5 paths, point to ARM64 libraries.
QMAKE_INCDIR_QT = $${SYSROOT}/usr/include/aarch64-linux-gnu/qt5/qt5
QMAKE_LIBDIR_QT = $$BOARD_LIBS
QMAKE_MOC       = /usr/lib/qt5/bin/moc
QMAKE_UIC       = /usr/lib/qt5/bin/uic
QMAKE_RCC       = /usr/lib/qt5/bin/rcc

exists($${SYSROOT}) {
    message("Using sysroot: $$SYSROOT")
    INCLUDEPATH += $${SYSROOT}/usr/include
    INCLUDEPATH += $${SYSROOT}/usr/include/rga
    INCLUDEPATH += $${SYSROOT}/usr/include/aarch64-linux-gnu/qt5/qt5
    INCLUDEPATH += $${SYSROOT}/usr/include/aarch64-linux-gnu/qt5/qt5/QtWidgets
    INCLUDEPATH += $${SYSROOT}/usr/include/aarch64-linux-gnu/qt5/qt5/QtGui
    INCLUDEPATH += $${SYSROOT}/usr/include/aarch64-linux-gnu/qt5/qt5/QtNetwork
    INCLUDEPATH += $${SYSROOT}/usr/include/aarch64-linux-gnu/qt5/qt5/QtCore
    INCLUDEPATH += $${SYSROOT}/usr/include/aarch64-linux-gnu/qt5/qt5/QtConcurrent
    INCLUDEPATH += $${SYSROOT}/usr/include/libdrm
    INCLUDEPATH += $${SYSROOT}/usr/include/aarch64-linux-gnu
    LIBS += -L$${SYSROOT}/usr/lib/aarch64-linux-gnu
    LIBS += -L$${SYSROOT}/opt/retail/lib
    LIBS += -L$$BOARD_LIBS
} else {
    error("SYSROOT $$SYSROOT does not exist.")
}

INCLUDEPATH += ../common
INCLUDEPATH += ../common/cjson

# RKNN SDK
isEmpty(RKNN_SDK_ROOT) {
    RKNN_SDK_ROOT = $$(RKNN_SDK_ROOT)
}
isEmpty(RKNN_SDK_ROOT) {
    error("RKNN_SDK_ROOT is not set. Pass -D RKNN_SDK_ROOT=/path/to/librknn_api or export it.")
}
QMAKE_CXXFLAGS += -I$$RKNN_SDK_ROOT/include
QMAKE_CFLAGS   += -I$$RKNN_SDK_ROOT/include
LIBS += -L$$RKNN_SDK_ROOT/aarch64 -lrknnrt

# ===== Mandatory dependencies (no fallback path) =====
DEFINES += ARM64_PLATFORM
DEFINES += USE_RKNN
DEFINES += USE_FFMPEG
DEFINES += USE_ALSA
DEFINES += USE_SPEEXDSP
DEFINES += USE_RGA

LIBS += -lavcodec -lavformat -lavutil -lswscale -lswresample -lavdevice
LIBS += -lasound -lspeexdsp -lrga -ldrm
LIBS += -lpthread -lsqlite3 -lssl -lcrypto -lcurl -ldl
LIBS += -lQt5Widgets -lQt5Gui -lQt5Network -lQt5Core -lQt5Concurrent

# ===== Transitive dependencies (extracted from the board via ldd) =====
QMAKE_LFLAGS += -Wl,--no-as-needed
LIBS += -licuuc -licui18n -licudata
LIBS += -lpcre2-16 -lpcre2-8 -lpcre -lglib-2.0 -ldouble-conversion -lz
LIBS += -lpng16 -lharfbuzz -lgraphite2 -lfreetype -lfontconfig -lexpat
LIBS += -lxcb -lX11 -lXau -lXdmcp -lGL -lGLX -lGLdispatch -lxcb-shm
LIBS += -lvpx -lx264 -lx265 -lmp3lame -lvorbis -lvorbisenc -logg -lopus
LIBS += -ltheoradec -ltheoraenc -ltwolame -lspeex -lshout
LIBS += -lwebp -lwebpmux -lopenjp2 -lopenmpt -lcodec2 -laom
LIBS += -lva -lva-drm -lva-x11 -lvdpau -lOpenCL -lXext -lXfixes -lXrender
LIBS += -lcairo -lcairo-gobject -lpango-1.0 -lpangocairo-1.0 -lpangoft2-1.0 -lpixman-1 -lffi -lxcb-render
LIBS += -lvorbisfile -lmpg123
LIBS += -lrsvg-2 -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lgmodule-2.0
LIBS += -lxml2 -lssh-gcrypt -lgnutls -lsoxr -lsnappy -lshine -lchromaprint -lgme
LIBS += -lbluray -lzvbi -lxvidcore -lwavpack -lcodec2
LIBS += -lgmp -lhogweed -lnettle -lidn2 -lunistring -ltasn1 -lp11-kit -lgsm
LIBS += -lkrb5 -lk5crypto -lkrb5support -lcom_err -lkeyutils -lgssapi_krb5 -lresolv
LIBS += -lgcrypt -lgpg-error -lbz2 -llzma -lmount -lblkid -luuid -lselinux
LIBS += -ltbb -lgomp -lnuma -ldatrie -lthai -lfribidi -lbsd
QMAKE_LFLAGS += -Wl,--as-needed

# Vosk (no ARM64 build yet; stub-linked on the board for now)
# LIBS += -L$${SYSROOT}/opt/retail/lib -lvosk

# ===== Manual moc (QT= does not process moc automatically) =====
MOC = /usr/lib/qt5/bin/moc
MOC_DIR = ../build/client_arm64/.moc

# moc rule: generate moc_*.cpp for every Q_OBJECT header in HEADERS
moc_header.commands = $$MOC $$QMAKE_MOC_FLAGS ${QMAKE_FILE_IN} -o ${QMAKE_FILE_OUT}
moc_header.output = $$MOC_DIR/moc_${QMAKE_FILE_BASE}.cpp
moc_header.input = HEADERS
moc_header.variable_out = SOURCES
QMAKE_EXTRA_COMPILERS += moc_header
SOURCES += \
    ../client/main.cpp \
    ../client/config/client_config.cpp \
    ../client/ui/mainwindow.cpp \
    ../client/core/networkmanager.cpp \
    ../client/ui/memberregister.cpp \
    ../client/ui/goodsmanager.cpp \
    ../client/ui/settlementwidget.cpp \
    ../client/ui/facepaywidget.cpp \
    ../client/ui/orderquery.cpp \
    ../client/ui/rechargewidget.cpp \
    ../client/core/localdbmanager.cpp \
    ../client/services/voice_engine.cpp \
    ../client/services/voice_assist.cpp \
    ../client/services/crashhandler.cpp \
    ../client/services/otaupdater.cpp \
    ../client/services/clientservice.cpp \
    ../client/core/hardwareservice.cpp \
    ../client/core/hardware_api.c \
    ../client/core/facemanager.cpp \
    ../client/core/hardwarethread.cpp \
    ../client/core/alsa_capture.c \
    ../client/core/audio_preproc.c \
    ../client/core/v4l2_capture.c \
    ../client/core/rkisp_camera.c \
    ../client/core/rga_transform.c \
    ../client/core/rga_osd.c \
    ../client/core/video_encoder.c \
    ../client/core/audio_encoder.c \
    ../client/core/rtsp_streamer.c \
    ../client/core/face_image_utils.c \
    ../client/core/face_landmark.c \
    ../client/core/face_antispoof.c \
    ../client/core/h264_parser.c \
    ../client/core/mp4_recorder.c \
    ../common/logger.c \
    ../client/core/watchdog.c \
    ../client/core/hw_watchdog.c \
    ../client/core/ota.c \
    ../common/cjson/cJSON.c \
    ../common/crypto.c \
    ../client/services/vosk_stub.c

HEADERS += \
    ../client/config/client_config.h \
    ../client/ui/mainwindow.h \
    ../client/core/networkmanager.h \
    ../client/ui/memberregister.h \
    ../client/ui/goodsmanager.h \
    ../client/ui/settlementwidget.h \
    ../client/ui/facepaywidget.h \
    ../client/ui/orderquery.h \
    ../client/ui/rechargewidget.h \
    ../client/config/payment_provider.h \
    ../client/config/mock_payment_provider.h \
    ../client/services/voice_engine.h \
    ../client/services/voice_assist.h \
    ../client/services/vosk_capi.h \
    ../client/core/localdbmanager.h \
    ../client/services/crashhandler.h \
    ../client/services/otaupdater.h \
    ../client/services/clientservice.h \
    ../client/core/hardwareservice.h \
    ../client/core/hardware_api.h \
    ../client/core/facemanager.h \
    ../client/core/hardwarethread.h \
    ../client/core/alsa_capture.h \
    ../client/core/audio_preproc.h \
    ../client/core/v4l2_capture.h \
    ../client/core/rkisp_camera.h \
    ../client/core/rga_transform.h \
    ../client/core/rga_osd.h \
    ../client/core/video_encoder.h \
    ../client/core/audio_encoder.h \
    ../client/core/rtsp_streamer.h \
    ../client/core/face_landmark.h \
    ../client/core/face_antispoof.h \
    ../client/core/h264_parser.h \
    ../client/core/mp4_recorder.h \
    ../common/logger.h \
    ../client/core/watchdog.h \
    ../client/core/hw_watchdog.h \
    ../client/core/ota.h \
    ../common/cjson/cJSON.h \
    ../common/crypto.h \
    ../common/common.h

FORMS += \
    ../client/ui/mainwindow.ui \
    ../client/ui/memberregister.ui \
    ../client/ui/goodsmanager.ui \
    ../client/ui/settlementwidget.ui \
    ../client/ui/facepaywidget.ui \
    ../client/ui/orderquery.ui \
    ../client/ui/rechargewidget.ui

CONFIG += c++11
DEFINES += QT_DEPRECATED_WARNINGS

DESTDIR = ../build/client_arm64
OBJECTS_DIR = ../build/client_arm64/.obj
MOC_DIR = ../build/client_arm64/.moc
UI_DIR = ../build/client_arm64/.ui

# Ensure uic-generated ui_*.h are found
INCLUDEPATH += $$UI_DIR
