# app/chat/ - Text chat over WebRTC data channels

add_executable(chat ../cli/main.c)
target_compile_definitions(chat PRIVATE APP_CHAT_COMPAT)
target_link_libraries(chat PRIVATE mrtc_app_control)
mrtc_set_warnings(chat)
