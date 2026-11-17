foreach(server_name mrtc_server signaling_server)
    add_executable(${server_name} signaling_server.c meeting.c)
    target_link_libraries(${server_name} PRIVATE mrtc_signaling)
    mrtc_set_warnings(${server_name})
    if(TARGET mrtc_sfu)
        target_link_libraries(${server_name} PRIVATE mrtc_sfu)
        target_compile_definitions(${server_name} PRIVATE MRTC_SIGNALING_SFU=1)
    else()
        target_link_libraries(${server_name} PRIVATE mrtc)
    endif()
endforeach()

set_target_properties(signaling_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/signaling")

if(MRTC_BUILD_TESTS)
    add_test(NAME test_signaling_server_args COMMAND ${CMAKE_COMMAND}
        "-DSERVER=$<TARGET_FILE:mrtc_server>"
        -P "${PROJECT_SOURCE_DIR}/signaling/tests/test_server_args.cmake")
    set_tests_properties(test_signaling_server_args PROPERTIES TIMEOUT 30)
    add_test(NAME test_server_compat_args COMMAND ${CMAKE_COMMAND}
        "-DSERVER=$<TARGET_FILE:signaling_server>"
        -P "${PROJECT_SOURCE_DIR}/signaling/tests/test_server_args.cmake")
    set_tests_properties(test_server_compat_args PROPERTIES TIMEOUT 30)
    add_executable(test_topology test_topology.c meeting.c)
    target_include_directories(test_topology PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(test_topology PRIVATE mrtc_signaling)
    mrtc_set_warnings(test_topology)
    add_test(NAME test_topology COMMAND test_topology)
    if(TARGET mrtc_sfu)
        find_program(MRTC_NODE_EXECUTABLE node)
        if(MRTC_NODE_EXECUTABLE)
            execute_process(COMMAND ${MRTC_NODE_EXECUTABLE} --version
                OUTPUT_VARIABLE MRTC_NODE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(MRTC_NODE_VERSION MATCHES "^v([0-9]+)" AND CMAKE_MATCH_1 GREATER_EQUAL 22)
                add_test(NAME test_server_runtime COMMAND ${MRTC_NODE_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/test_server_runtime.mjs $<TARGET_FILE:mrtc_server>)
                set_tests_properties(test_server_runtime PROPERTIES TIMEOUT 30)
            endif()
        endif()
    endif()
endif()
