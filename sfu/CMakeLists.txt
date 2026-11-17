add_library(mrtc_sfu STATIC src/rtc_sfu.c)
target_include_directories(mrtc_sfu PUBLIC include)
target_link_libraries(mrtc_sfu PUBLIC mrtc_routing mrtc_sctp)
mrtc_set_warnings(mrtc_sfu)

if(MRTC_BUILD_TESTS AND MRTC_ENABLE_CLIENT_API)
    add_executable(test_sfu_session tests/test_sfu_session.c)
    target_include_directories(test_sfu_session PRIVATE ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/rtc/src ${PROJECT_SOURCE_DIR}/client/include/rtc)
    target_link_libraries(test_sfu_session PRIVATE mrtc_signaling mrtc_client mrtc_sfu)
    mrtc_set_warnings(test_sfu_session)
    add_test(NAME test_sfu_session COMMAND test_sfu_session)
    set_tests_properties(test_sfu_session PROPERTIES TIMEOUT 30)
endif()
