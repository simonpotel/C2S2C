# Example programs. They use the public API only, linked against the shared library like any user.
function(dx_example name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE duplex)
    dx_enable_warnings(${name})
endfunction()

dx_example(chat_server C/Server/chat_server.c)
dx_example(chat_client C/Client/chat_client.c)

# C++ examples use the header-only binding.
if(CMAKE_CXX_COMPILER)
    function(dx_cpp_example name source)
        add_executable(${name} ${source})
        target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/Bindings/Cpp ${CMAKE_SOURCE_DIR}/Generated/Cpp)
        target_link_libraries(${name} PRIVATE duplex)
        dx_enable_warnings(${name})
    endfunction()
    dx_cpp_example(chat_server_cpp Cpp/Server/chat_server.cpp)
    dx_cpp_example(chat_client_cpp Cpp/Client/chat_client.cpp)
endif()
