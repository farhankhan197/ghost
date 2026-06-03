cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# SQLite amalgamation — bundled, no external dependency
add_library(sqlite3 STATIC src/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite)
if(WIN32)
  target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5)
else()
  target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5 SQLITE_OS_UNIX=1)
  target_link_libraries(sqlite3 PRIVATE pthread dl)
endif()

add_subdirectory(src/note)
add_subdirectory(src/git)

add_executable(ghost-checkpoint
  src/checkpoint/main.cpp
  src/checkpoint/working_log.cpp
  src/checkpoint/snapshot.cpp
  src/checkpoint/session.cpp
  src/persist/db.cpp
)

add_executable(ghost
  src/main.cpp
  src/cli/commands.cpp
  src/commit/post_commit.cpp
  src/checkpoint/working_log.cpp
  src/persist/db.cpp
  src/rewrite/rewrite_log.cpp
  src/rewrite/processor.cpp
  src/rewrite/working_state.cpp
  src/commit/note_index.cpp
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


target_link_libraries(ghost-checkpoint PRIVATE note git sqlite3)
target_include_directories(ghost-checkpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(ghost-checkpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/note)
target_include_directories(ghost-checkpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/persist)
target_include_directories(ghost-checkpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite)

target_link_libraries(ghost PRIVATE note git sqlite3)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/checkpoint)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/util)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/persist)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/rewrite)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/commit)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite)

add_subdirectory(tests)
