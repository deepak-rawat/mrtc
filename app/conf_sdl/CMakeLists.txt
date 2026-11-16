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

if(APPLE)
    set_target_properties(conf_sdl PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "mrtc Conference"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.mrtc.conf-sdl"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    )
endif()

target_include_directories(conf_sdl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MINGW)
    target_link_options(conf_sdl PRIVATE -mconsole)
endif()
target_link_libraries(conf_sdl PRIVATE
    mrtc_conference
    PkgConfig::SDL3
)
mrtc_set_warnings(conf_sdl)

if(MRTC_BUILD_TESTS)
    add_executable(test_ui_sdl3 test_ui_sdl3.c ui_sdl3.c)
    target_include_directories(test_ui_sdl3 PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(test_ui_sdl3 PRIVATE mrtc_media PkgConfig::SDL3)
    mrtc_set_warnings(test_ui_sdl3)
    add_test(NAME test_ui_sdl3 COMMAND test_ui_sdl3)
endif()
