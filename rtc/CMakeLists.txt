# rtc/ - core RTC runtime library (libmrtc)
#
# Runtime + protocol primitives. Always built. The optional WebRTC-style
# peer-connection facade lives in ../client/ as libmrtc_client.

set(RTC_CORE_SOURCES
    src/rtc_utils.c
    src/rtc_poller.c
    src/rtc_packet_io.c
    src/rtc_timer_sched.c
)

set(RTC_PROTOCOL_SOURCES
    src/rtc_stun.c
    src/rtc_ice.c
    src/rtc_dtls.c
    src/rtc_srtp.c
    src/rtc_sdp.c
    src/rtc_rtp.c
    src/rtc_rtp_stream.c
    src/rtc_rtp_demux.c
    src/rtc_rtp_ext.c
    src/rtc_rtcp.c
    src/rtc_interceptor.c
    src/rtc_media_session.c
    src/rtc_nack_buf.c
    src/rtc_rate_control.c
    src/rtc_turn.c
)

set(RTC_RUNTIME_SOURCES
    src/rtc_worker.c
    src/rtc_listener.c
    src/rtc_transport.c
    src/rtc_turn_runtime.c
)

set(LIB_SOURCES ${RTC_CORE_SOURCES} ${RTC_PROTOCOL_SOURCES} ${RTC_RUNTIME_SOURCES})

if(MRTC_ENABLE_TWCC)
    list(APPEND LIB_SOURCES src/rtc_twcc_sender.c src/rtc_twcc_receiver.c src/rtc_bwe.c)
endif()

add_library(mrtc STATIC ${LIB_SOURCES})
target_include_directories(mrtc
    PUBLIC  include              # Public: rtc_worker.h, rtc_listener.h, etc.
    PRIVATE src include/rtc      # Private: rtc_ice.h, rtc_dtls.h, etc.
)
target_link_libraries(mrtc PUBLIC mrtc_common OpenSSL::SSL OpenSSL::Crypto ${PLATFORM_LIBS})
mrtc_set_warnings(mrtc)

if(MRTC_BUILD_FUZZERS)
    add_library(mrtc_fuzz STATIC ${LIB_SOURCES})
    target_include_directories(mrtc_fuzz PUBLIC include PRIVATE src include/rtc)
    target_link_libraries(mrtc_fuzz PUBLIC mrtc_common OpenSSL::SSL OpenSSL::Crypto ${PLATFORM_LIBS})
    target_compile_options(mrtc_fuzz PRIVATE -fsanitize=fuzzer-no-link)
    mrtc_set_warnings(mrtc_fuzz)
    if(MRTC_ENABLE_TWCC)
        target_compile_definitions(mrtc_fuzz PUBLIC MRTC_ENABLE_TWCC)
    endif()
    if(MRTC_ENABLE_RATE_CONTROL)
        target_compile_definitions(mrtc_fuzz PUBLIC MRTC_ENABLE_RATE_CONTROL)
    endif()
    add_executable(fuzz_packet_parsers tests/fuzz_packet_parsers.c)
    target_include_directories(fuzz_packet_parsers PRIVATE src include/rtc)
    target_compile_options(fuzz_packet_parsers PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_packet_parsers PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz_packet_parsers PRIVATE mrtc_fuzz)
    mrtc_set_warnings(fuzz_packet_parsers)
endif()

if(MRTC_ENABLE_CLIENT_API OR MRTC_ENABLE_ROUTING_API)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(USRSCTP REQUIRED usrsctp>=0.9.5)
    find_path(USRSCTP_HEADER_DIR usrsctp.h
        HINTS ${USRSCTP_INCLUDE_DIRS} ${USRSCTP_PREFIX}/include
        PATH_SUFFIXES usrsctp)
    if(NOT USRSCTP_HEADER_DIR)
        message(FATAL_ERROR "usrsctp.h not found")
    endif()
    add_library(mrtc_sctp STATIC src/rtc_data_channel.c)
    target_include_directories(mrtc_sctp PRIVATE src include/rtc ${USRSCTP_HEADER_DIR})
    target_link_libraries(mrtc_sctp PUBLIC mrtc PRIVATE ${USRSCTP_LINK_LIBRARIES})
    target_compile_options(mrtc_sctp PRIVATE ${USRSCTP_CFLAGS_OTHER})
    mrtc_set_warnings(mrtc_sctp)
endif()

if(MRTC_ENABLE_TWCC)
    target_compile_definitions(mrtc PUBLIC MRTC_ENABLE_TWCC)
endif()
if(MRTC_ENABLE_RATE_CONTROL)
    target_compile_definitions(mrtc PUBLIC MRTC_ENABLE_RATE_CONTROL)
endif()

# Tests (need access to internal headers for component-level testing)
if(MRTC_BUILD_TESTS)
    set(RUNTIME_TEST_NAMES
        test_stun test_rtp test_srtp test_dtls test_ice test_sdp test_packet_io
        test_sdp_video test_rtcp test_turn test_rate_control test_rtcp_feedback
        test_nack_buf test_rtp_ext test_rtp_demux test_media_session test_loopback test_poller
        test_timer_sched test_worker test_listener test_interceptor test_network_runtime
    )
    if(MRTC_ENABLE_TWCC)
        list(APPEND RUNTIME_TEST_NAMES test_twcc test_bwe)
    endif()
    foreach(test_name ${RUNTIME_TEST_NAMES})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src      # Access internal headers
            ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc  # Same-dir resolution
        )
        target_link_libraries(${test_name} PRIVATE mrtc)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
    if(UNIX)
        find_program(MRTC_COTURN_EXECUTABLE turnserver)
        add_test(NAME test_network_turn
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_turn_runtime.sh
                "${MRTC_COTURN_EXECUTABLE}" $<TARGET_FILE:test_network_runtime>)
        set_tests_properties(test_network_turn PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
        foreach(mode IN ITEMS tcp tls tls-untrusted tls-wrong-name
                udp-failover tcp-failover tls-failover tls-failover-untrusted
                tls-failover-wrong-name udp-exhausted tcp-stall-failover tls-stall-failover
                tls-dns udp-four-failover)
            string(REPLACE "-" "_" test_mode "${mode}")
            add_test(NAME test_network_turn_${test_mode}
                COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_turn_runtime.sh
                    "${MRTC_COTURN_EXECUTABLE}" $<TARGET_FILE:test_network_runtime> "${mode}")
            set_tests_properties(test_network_turn_${test_mode}
                PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
        endforeach()
    endif()
endif()
