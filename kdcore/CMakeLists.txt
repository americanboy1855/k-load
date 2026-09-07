cmake_minimum_required(VERSION 3.21)
project(kdcore LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Универсальный бинарник: macOS первого релиза на двух архитектурах.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

find_package(CURL REQUIRED)

add_library(kdcore SHARED
    src/Engine.cpp
    src/VpnMonitor.cpp
    src/CAPI.cpp
    src/platform/BrowserDefault.mm
    third_party/dart/dart_api_dl.c)

target_include_directories(kdcore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

target_link_libraries(kdcore PRIVATE
    ${CURL_LIBRARIES}
    "-framework Foundation"
    "-framework AppKit"
    "-framework CoreServices")

set_target_properties(kdcore PROPERTIES
    OUTPUT_NAME "kdcore"
    PREFIX "lib"
    MACOSX_RPATH ON)

add_executable(kd_tests tests/kd_tests.cpp)
target_include_directories(kd_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(kd_tests PRIVATE kdcore)
