# client/ - WebRTC-style peer connection facade (libmrtc_client)

add_library(mrtc_client STATIC
    src/rtc_client_runtime.c
    src/rtc_track.c
    src/rtc_data_channel.c
    src/rtc_peer.c
    src/rtc_peer_packets.c
)
target_include_directories(mrtc_client
    PUBLIC  include
    PRIVATE
        src
        include/rtc
        ${PROJECT_SOURCE_DIR}/rtc/src      # client internals depend on runtime privates
        ${PROJECT_SOURCE_DIR}/rtc/include/rtc
)
target_link_libraries(mrtc_client PUBLIC mrtc)
mrtc_set_warnings(mrtc_client)

if(MRTC_BUILD_TESTS)
    set(CLIENT_TEST_NAMES test_peer test_data_channel test_rtp_sender_loopback)
    foreach(test_name ${CLIENT_TEST_NAMES})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src               # client internals
            ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc
            ${PROJECT_SOURCE_DIR}/rtc/src                 # mrtc internals (rtc_sdp.h, rtc_dtls.h, ...)
        )
        target_link_libraries(${test_name} PRIVATE mrtc_client)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
