cmake_minimum_required(VERSION 3.16)
project(sul_plain_index CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

include_directories(${CMAKE_SOURCE_DIR}/include)

set(SUL_SOURCES
    src/z_order.cpp
    src/art_tree.cpp
    src/gpl_builder.cpp
    src/sul_index.cpp
)

add_library(sul_index STATIC ${SUL_SOURCES})

add_executable(sul_demo main.cpp)
target_link_libraries(sul_demo PRIVATE sul_index)
