# mrtc - core WebRTC transport library
set(LIB_SOURCES
    src/rtc_utils.c
    src/rtc_poller.c
    src/rtc_transport.c
    src/rtc_stun.c
    src/rtc_ice.c
    src/rtc_dtls.c
    src/rtc_srtp.c
    src/rtc_sdp.c
    src/rtc_rtp.c
    src/rtc_rtcp.c
    src/rtc_nack_buf.c
    src/rtc_rate_control.c
    src/rtc_track.c
    src/rtc_data_channel.c
    src/rtc_peer.c
    src/rtc_turn.c
)

add_library(mrtc STATIC ${LIB_SOURCES})
target_include_directories(mrtc
    PUBLIC  include              # Public: rtc_peer.h, rtc_track.h, etc.
    PRIVATE src include/rtc      # Private: rtc_ice.h, rtc_dtls.h, etc.
)
target_link_libraries(mrtc PUBLIC mrtc_common OpenSSL::SSL OpenSSL::Crypto ${PLATFORM_LIBS})
mrtc_set_warnings(mrtc)

# Tests (need access to internal headers for component-level testing)
if(MRTC_BUILD_TESTS)
    set(TEST_NAMES test_stun test_rtp test_srtp test_dtls test_ice test_sdp test_transport test_peer
                   test_sdp_video test_rtcp test_data_channel test_turn test_rate_control
                   test_rtcp_feedback test_nack_buf)
    foreach(test_name ${TEST_NAMES})
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
