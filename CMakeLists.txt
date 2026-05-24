cmake_minimum_required(VERSION 3.16)
project(boostudp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT (/MT): no vcruntime140.dll / msvcp140.dll on the target machine.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(BoostDeps)

add_executable(boostudp_server server.cpp)
if(WIN32)
    target_sources(boostudp_server PRIVATE uso_recv_win.cpp)
    target_compile_definitions(boostudp_server PRIVATE _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(boostudp_server PRIVATE boostudp_boost)

add_executable(boostudp_client client.cpp uso_send_win.cpp)
target_compile_definitions(boostudp_client PRIVATE _WIN32_WINNT=0x0A00)
target_link_libraries(boostudp_client PRIVATE boostudp_boost)
if(WIN32)
    target_link_libraries(boostudp_client PRIVATE iphlpapi)
endif()
