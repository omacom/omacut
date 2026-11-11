cmake_minimum_required(VERSION 3.21)
project(omacut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Multimedia DBus Test)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB omacut_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM omacut_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(omacut_core STATIC ${omacut_sources})
target_include_directories(omacut_core PUBLIC src)
target_link_libraries(omacut_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Multimedia Qt6::DBus)
target_compile_options(omacut_core PUBLIC -Wall -Wextra)

add_executable(omacut src/main.cpp src/resources.qrc)
target_link_libraries(omacut PRIVATE omacut_core)

# The tests read the QML they drive straight from src/, beside this file.
enable_testing()
add_executable(omacut_tests tests/omacut_tests.cpp)
target_link_libraries(omacut_tests PRIVATE omacut_core Qt6::Test)
add_test(NAME omacut_tests COMMAND omacut_tests)
