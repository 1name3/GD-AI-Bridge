cmake_minimum_required(VERSION 3.21)

project(GDAIBridge VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    scr/*.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

# ------------------------------------------------------------
# Windows / Winsock compatibility
# ------------------------------------------------------------

if (WIN32)

    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        _WINSOCKAPI_
        NOMINMAX
    )

endif()

# ------------------------------------------------------------
# Disable Geode PCH
#
# This prevents winsock.h from being pulled in before
# winsock2.h through the generated precompiled header.
# ------------------------------------------------------------

set(
    GEODE_DISABLE_PRECOMPILED_HEADERS
    ON
    CACHE BOOL
    "Disable Geode precompiled headers for Winsock compatibility"
    FORCE
)

# ------------------------------------------------------------
# Geode SDK
# ------------------------------------------------------------

add_subdirectory(
    $ENV{GEODE_SDK}
    ${CMAKE_CURRENT_BINARY_DIR}/geode
)

setup_geode_mod(${PROJECT_NAME})
