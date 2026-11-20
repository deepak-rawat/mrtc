# SDL3 desktop application.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
endif()

if(NOT TARGET PkgConfig::SDL3)
    message(STATUS "SDL3 not found — skipping desktop")
    return()
endif()

add_executable(mrtc_desktop main.c ui_sdl3.c)
if(APPLE)
    set_target_properties(mrtc_desktop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "mrtc Desktop"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.mrtc.desktop"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    )
endif()
target_include_directories(mrtc_desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MINGW)
    target_link_options(mrtc_desktop PRIVATE -mconsole)
endif()
target_link_libraries(mrtc_desktop PRIVATE mrtc_app_common PkgConfig::SDL3)
mrtc_set_warnings(mrtc_desktop)

if(MRTC_BUILD_TESTS)
    add_executable(test_ui_sdl3 test_ui_sdl3.c ui_sdl3.c)
    target_compile_definitions(test_ui_sdl3 PRIVATE UI_TEST_SNAPSHOTS)
    target_include_directories(test_ui_sdl3 PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(test_ui_sdl3 PRIVATE mrtc_media PkgConfig::SDL3)
    mrtc_set_warnings(test_ui_sdl3)
    add_test(NAME test_ui_sdl3 COMMAND test_ui_sdl3)
    if(TARGET mrtc_cli)
        find_program(MRTC_DESKTOP_NODE_EXECUTABLE node)
        if(MRTC_DESKTOP_NODE_EXECUTABLE)
            execute_process(COMMAND ${MRTC_DESKTOP_NODE_EXECUTABLE} --version
                OUTPUT_VARIABLE MRTC_DESKTOP_NODE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(MRTC_DESKTOP_NODE_VERSION MATCHES "^v([0-9]+)" AND CMAKE_MATCH_1 GREATER_EQUAL 22)
                add_test(NAME test_desktop_media COMMAND ${CMAKE_COMMAND} -E env
                    "MRTC_BUILD_DIR=${PROJECT_BINARY_DIR}"
                    ${MRTC_DESKTOP_NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_media_smoke.mjs)
                set_tests_properties(test_desktop_media PROPERTIES TIMEOUT 45)
            endif()
        endif()
    endif()
endif()
