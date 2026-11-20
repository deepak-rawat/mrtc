# app/ - Applications

add_subdirectory(server)
if(MRTC_ENABLE_CLIENT_API)
    add_subdirectory(common)
    add_subdirectory(cli)
    if(MRTC_BUILD_DESKTOP)
        add_subdirectory(desktop)
    endif()
endif()
