# app/ - Applications

add_subdirectory(server)
if(MRTC_ENABLE_CLIENT_API)
    add_subdirectory(chat)
    add_subdirectory(conf_sdl)
endif()
if(MRTC_ENABLE_ROUTING_API AND MRTC_ENABLE_CLIENT_API)
    add_executable(sfu_probe sfu_probe.c)
    target_link_libraries(sfu_probe PRIVATE mrtc_conference)
    mrtc_set_warnings(sfu_probe)
endif()
