cmake_minimum_required(VERSION 3.16)
project(dbg-viewtype)

set(UBIN_PATH "$ENV{ubinpath}")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories($ENV{uincpath})

set(ENV{PKG_CONFIG} "pkg-config --static")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBELF REQUIRED libelf)
pkg_check_modules(LIBDWARF REQUIRED libdwarf)

add_executable(dbg-viewtype dbg-viewtype.cpp)

target_compile_definitions(dbg-viewtype PRIVATE _Linux64)

if(UBIN_PATH)
    target_link_directories(dbg-viewtype PRIVATE ${UBIN_PATH})
endif()

target_include_directories(dbg-viewtype PRIVATE ${LIBELF_INCLUDE_DIRS} ${LIBDWARF_INCLUDE_DIRS})
target_link_libraries(dbg-viewtype ${LIBELF_LIBRARIES} ${LIBDWARF_LIBRARIES} l64d)