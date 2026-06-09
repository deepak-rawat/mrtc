# mrtc_media - codec-specific media library

find_package(PkgConfig REQUIRED)
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

set(MEDIA_SOURCES
    src/vp8_packetizer.c
    src/video_codec.c
    src/codec_vp8.c
    src/audio_codec.c
    src/codec_opus.c
    src/jitter_buffer.c
    src/media_pipeline.c
    src/test_pattern.c
    src/test_tone.c
    src/video_stats.c
    src/video_dump.c
    src/video_debug.c
)

add_library(mrtc_media STATIC ${MEDIA_SOURCES})
target_include_directories(mrtc_media
    PUBLIC  include          # Public: media_pipeline.h, video_codec.h, etc.
    PRIVATE src              # Private: vp8_packetizer.h, jitter_buffer.h, etc.
)
target_link_libraries(mrtc_media PUBLIC mrtc_client PkgConfig::VPX PkgConfig::OPUS m)
mrtc_set_warnings(mrtc_media)

# Tests (need access to internal headers for component-level testing)
if(MRTC_BUILD_TESTS)
    set(MEDIA_TEST_NAMES test_vp8 test_video_codec test_audio_codec test_jitter_buffer test_media_pipeline test_video_stats test_video_dump test_video_debug)
    foreach(test_name ${MEDIA_TEST_NAMES})
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/src      # Access internal headers
        )
        target_link_libraries(${test_name} PRIVATE mrtc_media)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
