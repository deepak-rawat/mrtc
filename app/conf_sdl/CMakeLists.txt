# app/conf_sdl/ - SDL3 video conference application

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
endif()

if(NOT TARGET PkgConfig::SDL3)
    message(STATUS "SDL3 not found — skipping conf_sdl")
    return()
endif()

add_executable(conf_sdl
    main.c
    ui_sdl3.c
)

target_include_directories(conf_sdl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MINGW)
    target_link_options(conf_sdl PRIVATE -mconsole)
endif()
target_link_libraries(conf_sdl PRIVATE
    mrtc_conference
    PkgConfig::SDL3
)
mrtc_set_warnings(conf_sdl)
