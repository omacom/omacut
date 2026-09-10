QT += core gui quick quickcontrols2 multimedia testlib dbus
CONFIG += c++17 testcase
TARGET = backend_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += \
    ../src/clipmodel.h \
    ../src/backend.h \
    ../src/ffmpeg.h \
    ../src/filepicker.h \
    ../src/portalfilepicker.h \
    ../src/thumbprovider.h \
    ../src/thumbworker.h

SOURCES += \
    ../src/clipmodel.cpp \
    backend_tests.cpp \
    ../src/backend.cpp \
    ../src/ffmpeg.cpp \
    ../src/portalfilepicker.cpp \
    ../src/thumbprovider.cpp \
    ../src/thumbworker.cpp
