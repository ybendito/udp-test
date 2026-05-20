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

# Standalone Asio (https://github.com/chriskohlhoff/asio) — available for reference/tools.
set(BOOSTUDP_ASIO_ROOT "${CMAKE_SOURCE_DIR}/deps/asio" CACHE PATH "Standalone Asio root")
if(EXISTS "${BOOSTUDP_ASIO_ROOT}/include/asio.hpp")
    message(STATUS "Standalone Asio found at ${BOOSTUDP_ASIO_ROOT}")
else()
    message(WARNING "Standalone Asio not found at ${BOOSTUDP_ASIO_ROOT}. Run scripts/fetch-deps.ps1")
endif()

add_executable(boostudp_server server.cpp)
if(WIN32)
    target_sources(boostudp_server PRIVATE uso_recv_win.cpp)
    target_compile_definitions(boostudp_server PRIVATE _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(boostudp_server PRIVATE boostudp_boost)

add_executable(boostudp_client client.cpp uso_send_win.cpp)
target_compile_definitions(boostudp_client PRIVATE _WIN32_WINNT=0x0A00)
target_link_libraries(boostudp_client PRIVATE boostudp_boost)
