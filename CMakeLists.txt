cmake_minimum_required(VERSION 3.10)
project(mrtc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Platform detection
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    set(PLATFORM_LIBS ws2_32 iphlpapi)
else()
    find_package(Threads REQUIRED)
    set(PLATFORM_LIBS m Threads::Threads)
endif()

# Find OpenSSL (the only external dependency)
find_package(OpenSSL REQUIRED)

# Libraries and applications
add_subdirectory(common)
add_subdirectory(rtc)
add_subdirectory(media)
add_subdirectory(signaling)
add_subdirectory(turn)
add_subdirectory(conference)
add_subdirectory(app)

# Optional: install
install(TARGETS mrtc_common mrtc mrtc_media mrtc_signaling ARCHIVE DESTINATION lib)
install(DIRECTORY common/include/ DESTINATION include)
install(DIRECTORY rtc/include/ DESTINATION include)
install(DIRECTORY media/include/ DESTINATION include)
install(DIRECTORY signaling/include/ DESTINATION include)
