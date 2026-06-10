# routing/ - Server-side media routing primitives (libmrtc_routing)

add_library(mrtc_routing STATIC
    src/rtc_router.c
    src/rtc_producer.c
    src/rtc_consumer.c
)
target_include_directories(mrtc_routing
    PUBLIC  include
    PRIVATE src
)
target_link_libraries(mrtc_routing PUBLIC mrtc)
mrtc_set_warnings(mrtc_routing)

if(MRTC_BUILD_TESTS)
    set(ROUTING_TEST_NAMES
        test_routing_transport
        test_routing_media
        test_routing_timers
        test_routing_ice
    )
    foreach(test_name ${ROUTING_TEST_NAMES})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/rtc/src
        )
        target_link_libraries(${test_name} PRIVATE mrtc_routing)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
