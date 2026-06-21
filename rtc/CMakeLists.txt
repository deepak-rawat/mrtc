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
        test_nack_buf test_rtp_ext test_rtp_demux test_media_session test_poller test_timer_sched
        test_worker test_listener test_interceptor
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
endif()
