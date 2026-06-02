cmake_minimum_required(VERSION 3.14)
project(mrtc VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default build type for single-config generators (Make, Ninja)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Options
option(MRTC_BUILD_TESTS "Build test executables" ON)
option(MRTC_ENABLE_ASAN "Enable AddressSanitizer (Debug builds)" OFF)

# Feature toggles
option(MRTC_ENABLE_TWCC "Transport-wide CC + GCC bandwidth estimator" ON)
option(MRTC_ENABLE_RATE_CONTROL "RR-based AIMD sender rate control" ON)

# Compiler warning flags applied to all targets in this project
function(mrtc_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# AddressSanitizer
if(MRTC_ENABLE_ASAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# Platform detection
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    set(PLATFORM_LIBS ws2_32 iphlpapi)
else()
    find_package(Threads REQUIRED)
    set(PLATFORM_LIBS m Threads::Threads)
endif()

find_package(OpenSSL REQUIRED)

# Testing
if(MRTC_BUILD_TESTS)
    enable_testing()
endif()

# Libraries and applications
add_subdirectory(common)
add_subdirectory(rtc)
add_subdirectory(media)
add_subdirectory(signaling)
add_subdirectory(conference)
add_subdirectory(app)

# Optional: install
install(TARGETS mrtc_common mrtc mrtc_media mrtc_signaling ARCHIVE DESTINATION lib)
install(DIRECTORY common/include/ DESTINATION include)
install(DIRECTORY rtc/include/ DESTINATION include)
install(DIRECTORY media/include/ DESTINATION include)
install(DIRECTORY signaling/include/ DESTINATION include)
