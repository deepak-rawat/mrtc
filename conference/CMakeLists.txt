# conference/ - High-level conferencing library (libmrtc_conference)

add_library(mrtc_conference STATIC
    src/conference.c
)
target_include_directories(mrtc_conference PUBLIC include)
target_link_libraries(mrtc_conference PUBLIC
    mrtc_client
    mrtc_media
    mrtc_signaling
)
