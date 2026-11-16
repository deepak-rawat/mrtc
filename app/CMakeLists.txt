# app/ - Applications

add_subdirectory(chat)
add_subdirectory(conf_sdl)
if(MRTC_ENABLE_ROUTING_API)
    add_executable(sfu_probe sfu_probe.c)
    target_link_libraries(sfu_probe PRIVATE mrtc_conference)
    mrtc_set_warnings(sfu_probe)
endif()
