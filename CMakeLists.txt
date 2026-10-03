cmake_minimum_required(VERSION 3.21)

project(PackManager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Concurrent Multimedia MultimediaWidgets Network Widgets)

add_library(packmanager_format
    src/PackageFormat.cpp
    src/PackageFormat.h
    src/ProjectFile.cpp
    src/ProjectFile.h
)
target_link_libraries(packmanager_format PUBLIC Qt6::Core)
target_include_directories(packmanager_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(PackManager WIN32
    src/PackManager.rc
    src/main.cpp
    src/PackManagerWindow.cpp
    src/PackManagerWindow.h
    src/ProjectFile.cpp
    src/ProjectFile.h
    src/VideoListWidget.cpp
    src/VideoListWidget.h
)
target_link_libraries(PackManager PRIVATE
    packmanager_format
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::Widgets
)
if(WIN32)
    target_link_libraries(packmanager_format PUBLIC bcrypt crypt32)
endif()

if(MSVC)
    target_compile_options(packmanager_format PRIVATE /utf-8)
    target_compile_options(PackManager PRIVATE /utf-8)
endif()

target_compile_definitions(PackManager PRIVATE
    PACKMANAGER_VERSION="${PROJECT_VERSION}"
)

set_target_properties(PackManager PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/dist"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/dist"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(PackManagerFormatTest tests/format_test_main.cpp)
    target_link_libraries(PackManagerFormatTest PRIVATE packmanager_format)
endif()
