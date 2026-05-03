# common/ - Shared platform utilities (libmrtc_common)
#
# Provides: error codes, socket/addr types, threading, logging, random, time.
# No protocol dependencies (no STUN, ICE, DTLS, etc.).

add_library(mrtc_common STATIC
    src/rtc_common.c
)

# PUBLIC include paths:
#   include       → allows #include "rtc/rtc_types.h"
#   include/rtc   → allows #include "rtc_types.h" (for sibling headers in rtc/)
target_include_directories(mrtc_common PUBLIC include include/rtc)

target_link_libraries(mrtc_common PUBLIC OpenSSL::Crypto ${PLATFORM_LIBS})
