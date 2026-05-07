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

# --- Tests ---
if(MRTC_BUILD_TESTS)
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
endif()
