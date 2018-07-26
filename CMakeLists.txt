cmake_minimum_required(VERSION 3.4)
project(proxy)

option(BUILD_EXE "build executable or library" ON)

add_compile_options(-std=c++11)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME ON)

if(MSVC)
add_definitions(-D_WIN32_WINNT=0x0501 -D_CRT_SECURE_NO_WARNINGS)
endif()

find_package(Boost REQUIRED)

include_directories( ${Boost_INCLUDE_DIR} )
link_directories( ${Boost_LIBRARY_DIR_DEBUG} ${Boost_LIBRARY_DIR_RELEASE})

if(BUILD_EXE)
add_definitions(-DFOR_MAIN)
add_executable(${PROJECT_NAME} tcpproxy_server.cpp )
else()
add_library(${PROJECT_NAME} STATIC tcpproxy_server.cpp )
endif()

if(MSVC)
target_compile_options( ${PROJECT_NAME} PUBLIC "/MT$<$<STREQUAL:$<CONFIGURATION>,Debug>:d>" )
endif()
target_link_libraries(${PROJECT_NAME} ${Boost_LIBRARIES})