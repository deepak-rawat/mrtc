# app/conf_sdl/ - SDL3 video conference application

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

add_executable(conf_sdl
    main.c
    ui_sdl3.c
)

target_include_directories(conf_sdl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_options(conf_sdl PRIVATE -mconsole)
target_link_libraries(conf_sdl PRIVATE
    mrtc_conference
    PkgConfig::SDL3
)
