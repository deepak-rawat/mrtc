# turn/ - Minimal TURN server executable

add_executable(turn_server
    turn_server.c
    turn_handler.c
)
target_include_directories(turn_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(turn_server PRIVATE mrtc)
