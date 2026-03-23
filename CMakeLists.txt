cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_subdirectory(src/note)
add_subdirectory(src/git)

add_executable(ghost-checkpoint
  src/checkpoint/main.cpp
)

add_executable(ghost
  src/main.cpp
)

target_link_libraries(ghost-checkpoint PRIVATE note git)
target_link_libraries(ghost PRIVATE note git)