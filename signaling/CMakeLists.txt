# signaling/ - Signaling library (libmrtc_signaling) + server executable

find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED IMPORTED_TARGET libwebsockets)
find_package(cJSON REQUIRED)

# --- Library: shared message format + WS client ---
add_library(mrtc_signaling STATIC
    src/signaling_msg.c
    src/signaling_client.c
)
target_include_directories(mrtc_signaling PUBLIC include)
target_link_libraries(mrtc_signaling PUBLIC
    mrtc_common
    cjson
    PkgConfig::LWS
)
mrtc_set_warnings(mrtc_signaling)

# --- Server executable ---
add_executable(signaling_server
    server/signaling_server.c
    server/meeting.c
)
target_include_directories(signaling_server PRIVATE server)
target_link_libraries(signaling_server PRIVATE mrtc_signaling)
mrtc_set_warnings(signaling_server)
if(MRTC_ENABLE_ROUTING_API AND MRTC_ENABLE_CLIENT_API)
    target_sources(signaling_server PRIVATE server/sfu.c)
    target_link_libraries(signaling_server PRIVATE mrtc_routing mrtc_client)
    target_compile_definitions(signaling_server PRIVATE MRTC_SIGNALING_SFU=1)
endif()

# --- Tests ---
if(MRTC_BUILD_TESTS)
    add_test(NAME test_signaling_server_args COMMAND ${CMAKE_COMMAND}
        "-DSERVER=$<TARGET_FILE:signaling_server>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_server_args.cmake")
    set_tests_properties(test_signaling_server_args PROPERTIES TIMEOUT 15)
    set(SIG_TEST_NAMES test_signaling_msg test_signaling)
    foreach(test_name ${SIG_TEST_NAMES})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
            server
        )
        target_link_libraries(${test_name} PRIVATE mrtc_signaling)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    # test_signaling needs meeting.c (server logic under test)
    target_sources(test_signaling PRIVATE server/meeting.c)
    target_include_directories(test_signaling PRIVATE server)
    if(MRTC_ENABLE_ROUTING_API AND MRTC_ENABLE_CLIENT_API)
        add_executable(test_sfu_session tests/test_sfu_session.c server/meeting.c)
        target_include_directories(test_sfu_session PRIVATE ${PROJECT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/rtc/src ${PROJECT_SOURCE_DIR}/client/include/rtc server)
        target_link_libraries(test_sfu_session PRIVATE mrtc_signaling mrtc_routing mrtc_client)
        mrtc_set_warnings(test_sfu_session)
        add_test(NAME test_sfu_session COMMAND test_sfu_session)
        set_tests_properties(test_sfu_session PROPERTIES TIMEOUT 15)
    endif()
endif()
