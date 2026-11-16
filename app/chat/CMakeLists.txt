# app/chat/ - Text chat over WebRTC data channels

add_executable(chat main.c)
target_link_libraries(chat PRIVATE mrtc_conference)
mrtc_set_warnings(chat)
