# client/ - WebRTC-style peer connection facade (libmrtc_client)

add_library(mrtc_client STATIC
    src/rtc_client_runtime.c
    src/rtc_track.c
    src/rtc_peer.c
)
target_include_directories(mrtc_client
    PUBLIC  include
    PRIVATE
        src
        include/rtc
        ${PROJECT_SOURCE_DIR}/rtc/src
)
target_link_libraries(mrtc_client PUBLIC mrtc mrtc_sctp)
mrtc_set_warnings(mrtc_client)

if(MRTC_BUILD_TESTS)
    set(CLIENT_TEST_NAMES test_peer test_data_channel test_rtp_sender_loopback test_peer_integration)
    foreach(test_name ${CLIENT_TEST_NAMES})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src               # client internals
            ${CMAKE_CURRENT_SOURCE_DIR}/include/rtc
            ${PROJECT_SOURCE_DIR}/rtc/src
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
            foreach(mode IN ITEMS tcp tls udp-identities tls-identities
                    udp-ipv6 tcp-ipv6 tls-ipv6 udp-v4-v6 udp-v6-v4
                    udp-restart tcp-restart tls-restart udp-remote-restart)
                add_test(NAME test_peer_turn_${mode}
                    COMMAND sh ${PROJECT_SOURCE_DIR}/rtc/tests/run_turn_runtime.sh
                        ${MRTC_CLIENT_TURNSERVER} $<TARGET_FILE:test_peer_integration> ${mode})
                set_tests_properties(test_peer_turn_${mode} PROPERTIES TIMEOUT 60)
            endforeach()
        endif()
    endif()
endif()
