QT += core testlib
QT -= gui
CONFIG += c++17 testcase console
TARGET = clipmodel_tests
TEMPLATE = app
INCLUDEPATH += ../src
HEADERS += ../src/clipmodel.h
SOURCES += clipmodel_tests.cpp ../src/clipmodel.cpp
