QT += core gui qml quick quickcontrols2 multimedia dbus

CONFIG += c++17 release
TARGET = omacut
TEMPLATE = app

HEADERS += \
    src/clipmodel.h \
    src/filepicker.h \
    src/portalfilepicker.h \
    src/ffmpeg.h \
    src/thumbworker.h \
    src/thumbprovider.h \
    src/backend.h

SOURCES += \
    src/clipmodel.cpp \
    src/main.cpp \
    src/portalfilepicker.cpp \
    src/ffmpeg.cpp \
    src/thumbworker.cpp \
    src/thumbprovider.cpp \
    src/backend.cpp

RESOURCES += src/resources.qrc
