cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL file://C:/Users/farha/OneDrive/Desktop/Code/ghost/gtest.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_subdirectory(src/note)
add_subdirectory(src/git)

add_executable(ghost-checkpoint
  src/checkpoint/main.cpp
  src/checkpoint/working_log.cpp
  src/checkpoint/snapshot.cpp
  src/checkpoint/session.cpp
)

add_executable(ghost
  src/main.cpp
  src/cli/commands.cpp
  src/commit/post_commit.cpp
  src/checkpoint/working_log.cpp
  src/hooks/installer.cpp
  src/hooks/agent_detector.cpp
  src/hooks/agent_hooks.cpp
  src/audit/blame_overlay.cpp
  src/audit/aggregator.cpp
  src/audit/policy.cpp
  src/audit/auditor.cpp
  src/output/report.cpp
  src/output/style.cpp
  src/output/interactive.cpp
  src/config/ghost_config.cpp
)


target_link_libraries(ghost-checkpoint PRIVATE note git)
target_include_directories(ghost-checkpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/note)

target_link_libraries(ghost PRIVATE note git)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/checkpoint)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/util)

add_subdirectory(tests)
