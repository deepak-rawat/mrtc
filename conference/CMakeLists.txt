# conference/ - High-level conferencing library (libmrtc_conference)

add_library(mrtc_conference STATIC
    src/conference.c
)
target_include_directories(mrtc_conference PUBLIC include)
target_link_libraries(mrtc_conference PUBLIC
    mrtc_client
    mrtc_media
    mrtc_signaling
)
mrtc_set_warnings(mrtc_conference)

if(MRTC_BUILD_TESTS)
    add_executable(test_conference tests/test_conference.c)
    target_include_directories(test_conference PRIVATE include ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(test_conference PRIVATE mrtc_media mrtc_signaling)
    mrtc_set_warnings(test_conference)
    add_test(NAME test_conference COMMAND test_conference)
    set_tests_properties(test_conference PROPERTIES TIMEOUT 30)
endif()
