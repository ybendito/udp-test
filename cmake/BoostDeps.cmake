set(BOOSTUDP_BOOST_ROOT "${CMAKE_SOURCE_DIR}/deps/boost" CACHE PATH "Vendored Boost root")

if(NOT EXISTS "${BOOSTUDP_BOOST_ROOT}/boost-build.jam")
    message(FATAL_ERROR
        "Vendored Boost not found. Run scripts/fetch-deps.ps1 (Windows) "
        "or linux/scripts/fetch-deps.sh (Linux).")
endif()

set(BOOSTUDP_BOOST_LIBS
    align
    asio
    assert
    config
    core
    integer
    io
    mpl
    optional
    predef
    preprocessor
    smart_ptr
    "static_assert"
    system
    throw_exception
    type_traits
    utility
    winapi
)

set(BOOSTUDP_BOOST_INCLUDE_DIRS "${BOOSTUDP_BOOST_ROOT}")
foreach(_lib ${BOOSTUDP_BOOST_LIBS})
    set(_inc "${BOOSTUDP_BOOST_ROOT}/libs/${_lib}/include")
    if(EXISTS "${_inc}")
        list(APPEND BOOSTUDP_BOOST_INCLUDE_DIRS "${_inc}")
    else()
        message(WARNING "Boost include path missing: ${_inc}")
    endif()
endforeach()

add_library(boostudp_boost INTERFACE)
target_include_directories(boostudp_boost SYSTEM INTERFACE ${BOOSTUDP_BOOST_INCLUDE_DIRS})
target_compile_definitions(boostudp_boost INTERFACE
    BOOST_ALL_NO_LIB
    BOOST_SYSTEM_NO_LIB
    BOOST_ASIO_NO_LIB
)
if(WIN32)
    target_compile_definitions(boostudp_boost INTERFACE _WIN32_WINNT=0x0601)
    target_link_libraries(boostudp_boost INTERFACE ws2_32 mswsock)
endif()
