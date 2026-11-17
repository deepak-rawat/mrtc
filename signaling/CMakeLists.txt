find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED IMPORTED_TARGET libwebsockets)
find_package(cJSON REQUIRED)

add_library(mrtc_signaling STATIC src/signaling_msg.c src/signaling_client.c)
target_include_directories(mrtc_signaling PUBLIC include)
target_link_libraries(mrtc_signaling PUBLIC mrtc_common cjson PkgConfig::LWS)
mrtc_set_warnings(mrtc_signaling)

if(MRTC_BUILD_TESTS)
    foreach(test_name test_signaling_msg test_signaling)
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE ${PROJECT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/app/server)
        target_link_libraries(${test_name} PRIVATE mrtc_signaling)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
    target_sources(test_signaling PRIVATE ${PROJECT_SOURCE_DIR}/app/server/meeting.c)
endif()
