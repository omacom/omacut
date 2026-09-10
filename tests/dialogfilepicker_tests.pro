QT += core gui widgets testlib
CONFIG += c++17 testcase
# qmake builds a GUI-subsystem binary on Windows by default, which detaches
# stdout -- QtTest's results would go nowhere.
win32: CONFIG += console
# and an .app bundle on macOS, which bin/test would not find.
macx: CONFIG -= app_bundle
TARGET = dialogfilepicker_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/filepicker.h ../src/dialogfilepicker.h
SOURCES += dialogfilepicker_tests.cpp ../src/dialogfilepicker.cpp
