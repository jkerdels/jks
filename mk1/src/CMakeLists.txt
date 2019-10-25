cmake_minimum_required (VERSION 3.5)
project (mk1)
# The version number.
set (mk1_VERSION_MAJOR 1)
set (mk1_VERSION_MINOR 0)

set(CMAKE_CXX_FLAGS "-std=c++1z -O3")

include_directories(${PROJECT_SOURCE_DIR})

add_executable(Server main.cpp connectionhandler.cpp)



