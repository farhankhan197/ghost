cmake_minimum_required(VERSION 3.20)
project(ghost VERSION 0.1.29 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++ -static")
endif()

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(libgit2 CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory(src/note)
add_subdirectory(src/git)

add_library(ghost_core STATIC
  src/cli/args.cpp
  src/cli/audit_commands.cpp
  src/cli/basic_commands.cpp
  src/cli/commands.cpp
  src/cli/doctor_command.cpp
  src/cli/governance_commands.cpp
  src/cli/hook_commands.cpp
  src/cli/inspection_commands.cpp
  src/cli/notes_command.cpp
  src/cli/post_commit_command.cpp
  src/cli/rewrite_commands.cpp
  src/cli/session_summary.cpp
  src/cli/status_commands.cpp
  src/commit/post_commit.cpp
  src/checkpoint/checkpoint_store.cpp
  src/checkpoint/hook_event.cpp
  src/checkpoint/snapshot.cpp
  src/checkpoint/session.cpp
  src/checkpoint/session_json.cpp
  src/persist/db.cpp
  src/rewrite/rewrite_log.cpp
  src/rewrite/processor.cpp
  src/rewrite/working_state.cpp
  src/commit/note_index.cpp
  src/hooks/installer.cpp
  src/hooks/agent_detector.cpp
  src/hooks/agent_hooks.cpp
  src/hooks/opencode_plugin.cpp
  src/audit/attribution_notes.cpp
  src/audit/attribution_resolver.cpp
  src/audit/blame_overlay.cpp
  src/audit/aggregator.cpp
  src/audit/policy.cpp
  src/audit/auditor.cpp
  src/output/layout.cpp
  src/output/report.cpp
  src/output/style.cpp
  src/output/ux.cpp
  src/output/interactive.cpp
  src/config/ghost_config.cpp
  src/config/ignore_matcher.cpp
  src/signing/ssh_signing.cpp
  src/util/files.cpp
  src/util/json.cpp
  src/util/process.cpp
  src/util/signature.cpp
  src/util/text.cpp
)

target_compile_definitions(ghost_core PUBLIC GHOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(ghost_core PUBLIC note git unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/checkpoint)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/hooks)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/util)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/persist)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/rewrite)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/commit)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/signing)
target_include_directories(ghost_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/note)

add_executable(ghost-checkpoint
  src/checkpoint/main.cpp
)

add_executable(ghost
  src/main.cpp
)

target_compile_definitions(ghost-checkpoint PRIVATE GHOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(ghost-checkpoint PRIVATE ghost_core)

target_compile_definitions(ghost PRIVATE GHOST_VERSION="${PROJECT_VERSION}")
target_link_libraries(ghost PRIVATE ghost_core)

add_subdirectory(tests)
