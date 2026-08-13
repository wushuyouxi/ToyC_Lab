cmake_minimum_required(VERSION 3.15)
project(toyc CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

file(GLOB TOYC_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(toyc ${TOYC_SOURCES})
