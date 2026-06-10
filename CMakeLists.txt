cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 0.1.23 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -static")
endif()

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(libgit2 CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

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
  src/signing/ssh_signing.cpp
)


target_compile_definitions(ghost-checkpoint PRIVATE GHOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(ghost-checkpoint PRIVATE note git unofficial::sqlite3::sqlite3)
target_include_directories(ghost-checkpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(ghost-checkpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/note)
target_include_directories(ghost-checkpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/persist)

target_compile_definitions(ghost PRIVATE GHOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(ghost PRIVATE note git unofficial::sqlite3::sqlite3)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/checkpoint)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/util)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/persist)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/rewrite)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/commit)
target_include_directories(ghost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/signing)

add_subdirectory(tests)
