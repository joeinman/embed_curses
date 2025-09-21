cmake_minimum_required(VERSION 3.28)

project(embed_curses)

add_library(embed_curses INTERFACE)

target_include_directories(embed_curses SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
