QT += core gui qml quick quickcontrols2 multimedia

CONFIG += c++17 release
TARGET = omacut
TEMPLATE = app

HEADERS += \
    src/filepicker.h \
    src/ffmpeg.h \
    src/thumbworker.h \
    src/thumbprovider.h \
    src/backend.h

SOURCES += \
    src/main.cpp \
    src/ffmpeg.cpp \
    src/thumbworker.cpp \
    src/thumbprovider.cpp \
    src/backend.cpp

# The file picker is the one genuinely platform-bound piece. Linux asks
# xdg-desktop-portal over D-Bus; everywhere else uses Qt's own QFileDialog,
# which is native on both Windows and macOS.
linux {
    QT += dbus
    DEFINES += OMACUT_PORTAL_FILE_PICKER
    HEADERS += src/portalfilepicker.h
    SOURCES += src/portalfilepicker.cpp
} else {
    QT += widgets
    HEADERS += src/dialogfilepicker.h
    SOURCES += src/dialogfilepicker.cpp
}

RESOURCES += src/resources.qrc
