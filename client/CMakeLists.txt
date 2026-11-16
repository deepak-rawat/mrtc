# client/ - WebRTC-style peer connection facade (libmrtc_client)

find_package(PkgConfig REQUIRED)
pkg_check_modules(USRSCTP REQUIRED usrsctp>=0.9.5)
find_path(USRSCTP_HEADER_DIR usrsctp.h
    HINTS ${USRSCTP_INCLUDE_DIRS} ${USRSCTP_PREFIX}/include
    PATH_SUFFIXES usrsctp)
if(NOT USRSCTP_HEADER_DIR)
    message(FATAL_ERROR "usrsctp.h not found; install the usrsctp development package")
endif()

add_library(mrtc_client STATIC
    src/rtc_client_runtime.c
    src/rtc_track.c
    src/rtc_data_channel.c
    src/rtc_peer.c
)
target_include_directories(mrtc_client
    PUBLIC  include
    PRIVATE
        src
        include/rtc
        ${USRSCTP_HEADER_DIR}
)
target_link_libraries(mrtc_client PUBLIC mrtc PRIVATE ${USRSCTP_LINK_LIBRARIES})
target_compile_options(mrtc_client PRIVATE ${USRSCTP_CFLAGS_OTHER})
mrtc_set_warnings(mrtc_client)

if(MRTC_BUILD_TESTS)
    set(CLIENT_TEST_NAMES test_peer test_data_channel test_rtp_sender_loopback test_peer_integration)
    foreach(test_name ${CLIENT_TEST_NAMES})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src               # client internals
            ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc
        )
        target_link_libraries(${test_name} PRIVATE mrtc_client)
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
    endforeach()
    if(UNIX)
        find_program(MRTC_CLIENT_TURNSERVER turnserver HINTS /opt/homebrew/bin)
        if(MRTC_CLIENT_TURNSERVER)
            add_test(NAME test_peer_turn
                COMMAND sh ${PROJECT_SOURCE_DIR}/rtc/tests/run_turn_runtime.sh
                    ${MRTC_CLIENT_TURNSERVER} $<TARGET_FILE:test_peer_integration>)
            set_tests_properties(test_peer_turn PROPERTIES TIMEOUT 60)
        endif()
    endif()
endif()
