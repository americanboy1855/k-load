cmake_minimum_required(VERSION 3.21)
project(kdcore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Универсальный бинарник: macOS первого релиза на двух архитектурах.
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
    endif()
endif()

if(MSVC)
    # Исходники в UTF-8: без этого MSVC читает литералы в системной
    # кодировке и вся кириллица ядра (сообщения, имена, тире) портится.
    add_compile_options(/utf-8)
endif()
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

find_package(CURL REQUIRED)
message(STATUS "CURL_INCLUDE_DIRS: ${CURL_INCLUDE_DIRS}")
message(STATUS "CURL_LIBRARIES: ${CURL_LIBRARIES}")

add_library(kdcore SHARED
    src/Engine.cpp
    src/VpnMonitor.cpp
    src/CAPI.cpp
    third_party/dart/dart_api_dl.c)

# Cookie-цепочка: macOS читает браузеры (ObjC++), Windows — честный
# пустой стаб (cookie-базы браузеров на Windows непробиваемы).
if(APPLE)
    target_sources(kdcore PRIVATE src/platform/BrowserDefault.mm)
else()
    target_sources(kdcore PRIVATE src/platform/BrowserDefault.cpp)
endif()

target_include_directories(kdcore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(APPLE)
    target_link_libraries(kdcore PRIVATE
        ${CURL_LIBRARIES}
        "-framework Foundation"
        "-framework AppKit"
        "-framework CoreServices")
    set_target_properties(kdcore PROPERTIES
        OUTPUT_NAME "kdcore"
        PREFIX "lib"
        MACOSX_RPATH ON)
else()
    # iphlpapi — GetBestRoute/GetAdaptersAddresses для VpnMonitor.
    # Статический curl (vcpkg x64-windows-static-md) требует CURL_STATIC.
    target_compile_definitions(kdcore PRIVATE CURL_STATIC)
    if(TARGET CURL::libcurl)
        target_link_libraries(kdcore PRIVATE CURL::libcurl)
    else()
        target_include_directories(kdcore PRIVATE ${CURL_INCLUDE_DIRS})
        target_link_libraries(kdcore PRIVATE ${CURL_LIBRARIES})
    endif()
    target_link_libraries(kdcore PRIVATE iphlpapi)
    set_target_properties(kdcore PROPERTIES
        OUTPUT_NAME "kdcore"
        PREFIX ""
        # Экспорт всех символов: kd_* нужны Dart-FFI, Engine::* — тестам.
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

add_executable(kd_tests tests/kd_tests.cpp)
target_include_directories(kd_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(kd_tests PRIVATE kdcore)
