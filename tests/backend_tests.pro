QT += core gui quick quickcontrols2 multimedia testlib
CONFIG += c++17 testcase
# qmake builds a GUI-subsystem binary on Windows by default, which detaches
# stdout -- QtTest's results would go nowhere.
win32: CONFIG += console
# and an .app bundle on macOS, which bin/test would not find.
macx: CONFIG -= app_bundle
TARGET = backend_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    ../src/backend.h \
    ../src/ffmpeg.h \
    ../src/filepicker.h \
    ../src/thumbprovider.h \
    ../src/thumbworker.h

SOURCES += \
    backend_tests.cpp \
    ../src/backend.cpp \
    ../src/ffmpeg.cpp \
    ../src/thumbprovider.cpp \
    ../src/thumbworker.cpp

# Match omacut.pro: the picker implementation is platform-bound.
linux {
    QT += dbus
    DEFINES += OMACUT_PORTAL_FILE_PICKER
    HEADERS += ../src/portalfilepicker.h
    SOURCES += ../src/portalfilepicker.cpp
} else {
    QT += widgets
    HEADERS += ../src/dialogfilepicker.h
    SOURCES += ../src/dialogfilepicker.cpp
}
