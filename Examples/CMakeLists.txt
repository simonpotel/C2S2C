# Example programs. They use the public API only, linked against the shared library like any user.
function(dx_example name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE duplex)
    dx_enable_warnings(${name})
endfunction()

dx_example(chat_server C/Server/chat_server.c)
dx_example(chat_client C/Client/chat_client.c)
