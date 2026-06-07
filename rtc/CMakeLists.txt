# mrtc - core RTC transport library
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
    src/rtc_rtp_ext.c
    src/rtc_rtcp.c
    src/rtc_nack_buf.c
    src/rtc_rate_control.c
    src/rtc_turn.c
)

set(RTC_CLIENT_SOURCES
    src/rtc_track.c
    src/rtc_data_channel.c
    src/rtc_peer.c
    src/rtc_peer_packets.c
)

set(RTC_SFU_SOURCES
    src/rtc_worker.c
    src/rtc_listener.c
)

set(LIB_SOURCES ${RTC_CORE_SOURCES} ${RTC_PROTOCOL_SOURCES})

if(MRTC_ENABLE_CLIENT_API)
    list(APPEND LIB_SOURCES ${RTC_CLIENT_SOURCES})
endif()

if(MRTC_ENABLE_SFU_API)
    list(APPEND LIB_SOURCES ${RTC_SFU_SOURCES})
endif()

# Conditional sources
if(MRTC_ENABLE_TWCC)
    list(APPEND LIB_SOURCES src/rtc_twcc_sender.c src/rtc_twcc_receiver.c src/rtc_bwe.c)
endif()

add_library(mrtc STATIC ${LIB_SOURCES})
target_include_directories(mrtc
    PUBLIC  include              # Public: rtc_peer.h, rtc_track.h, etc.
    PRIVATE src include/rtc      # Private: rtc_ice.h, rtc_dtls.h, etc.
)
target_link_libraries(mrtc PUBLIC mrtc_common OpenSSL::SSL OpenSSL::Crypto ${PLATFORM_LIBS})
mrtc_set_warnings(mrtc)

# Feature compile definitions
if(MRTC_ENABLE_TWCC)
    target_compile_definitions(mrtc PUBLIC MRTC_ENABLE_TWCC)
endif()
if(MRTC_ENABLE_RATE_CONTROL)
    target_compile_definitions(mrtc PUBLIC MRTC_ENABLE_RATE_CONTROL)
endif()
if(MRTC_ENABLE_CLIENT_API)
    target_compile_definitions(mrtc PUBLIC MRTC_ENABLE_CLIENT_API)
endif()
if(MRTC_ENABLE_SFU_API)
    target_compile_definitions(mrtc PUBLIC MRTC_ENABLE_SFU_API)
endif()

# Tests (need access to internal headers for component-level testing)
if(MRTC_BUILD_TESTS)
    set(TEST_NAMES test_stun test_rtp test_srtp test_dtls test_ice test_sdp test_packet_io
                   test_sdp_video test_rtcp test_turn test_rate_control test_rtcp_feedback
                   test_nack_buf test_rtp_ext test_poller test_timer_sched)
    if(MRTC_ENABLE_CLIENT_API)
        list(APPEND TEST_NAMES test_peer test_data_channel)
    endif()
    if(MRTC_ENABLE_SFU_API)
        list(APPEND TEST_NAMES test_worker test_listener)
    endif()
    if(MRTC_ENABLE_TWCC)
        list(APPEND TEST_NAMES test_twcc test_bwe)
    endif()
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
