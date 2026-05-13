# common/ - Shared platform utilities (libmrtc_common)
#
# Provides: error codes, socket/addr types, threading, logging, random, time,
#           and generic data structures (dynamic array, hash maps).
# No protocol dependencies (no STUN, ICE, DTLS, etc.).

add_library(mrtc_common STATIC
    src/rtc_common.c
    src/rtc_vec.c
    src/rtc_u32_map.c
    src/rtc_str_map.c
)

# PUBLIC include paths:
#   include       → allows #include "rtc/rtc_common.h"
#   include/rtc   → allows #include "rtc_common.h" (for sibling headers in rtc/)
target_include_directories(mrtc_common PUBLIC include include/rtc)

target_link_libraries(mrtc_common PUBLIC OpenSSL::Crypto ${PLATFORM_LIBS})
mrtc_set_warnings(mrtc_common)

# Tests
if(MRTC_BUILD_TESTS)
    set(COMMON_TEST_NAMES test_vec test_u32_map test_str_map)
    foreach(test_name ${COMMON_TEST_NAMES})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
        )
        target_link_libraries(${test_name} PRIVATE mrtc_common)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
