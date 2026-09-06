cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBSCHRO_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_dirac.c
        ${CMAKE_CURRENT_SOURCE_DIR}/schro_stubs.c
)

SET(LIBSCHRO_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libschroedinger.a
)

SET(LIBSCHRO_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libschro
        "${LIBSCHRO_SRC}"
        "${LIBSCHRO_LIBS}"
        []
        ""
        "${LIBSCHRO_INC}"
        ""
        "1")
