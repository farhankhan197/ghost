add_executable(ghost-tests
  unit/test_line_range.cpp
  unit/test_note_writer.cpp
  unit/test_note_reader.cpp
  unit/test_verified_writer.cpp
  unit/test_report.cpp
  unit/test_ghost_config.cpp
  unit/test_git_path.cpp
  unit/test_git_ref.cpp
  integration/test_checkpoint.cpp
  integration/test_post_commit.cpp
  integration/test_git_diff.cpp
  integration/test_check_cli.cpp
  integration/test_golden_flow.cpp
  integration/test_final_diff.cpp
  integration/test_blame_cli.cpp
  integration/test_audit.cpp
  integration/test_installer.cpp
  integration/test_governance_cli.cpp
  integration/test_rewrite.cpp
  ${CMAKE_SOURCE_DIR}/src/config/ghost_config.cpp
  ${CMAKE_SOURCE_DIR}/src/signing/ssh_signing.cpp
  ${CMAKE_SOURCE_DIR}/src/persist/db.cpp
  ${CMAKE_SOURCE_DIR}/src/rewrite/rewrite_log.cpp
  ${CMAKE_SOURCE_DIR}/src/rewrite/working_state.cpp
  ${CMAKE_SOURCE_DIR}/src/rewrite/processor.cpp
  ${CMAKE_SOURCE_DIR}/src/commit/note_index.cpp
  ${CMAKE_SOURCE_DIR}/src/output/report.cpp
  ${CMAKE_SOURCE_DIR}/src/output/style.cpp
)

target_link_libraries(ghost-tests PRIVATE note git unofficial::sqlite3::sqlite3 GTest::gtest_main)

target_compile_definitions(ghost-tests PRIVATE GHOST_VERSION="${PROJECT_VERSION}")
target_include_directories(ghost-tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/note
  ${CMAKE_SOURCE_DIR}/src/git
  ${CMAKE_SOURCE_DIR}/src/checkpoint
  ${CMAKE_SOURCE_DIR}/src/commit
  ${CMAKE_SOURCE_DIR}/src/audit
  ${CMAKE_SOURCE_DIR}/src/output
  ${CMAKE_SOURCE_DIR}/src/config
  ${CMAKE_SOURCE_DIR}/src/signing
  ${CMAKE_SOURCE_DIR}/src/hooks
  ${CMAKE_SOURCE_DIR}/src/persist
  ${CMAKE_SOURCE_DIR}/src/rewrite
)

include(GoogleTest)
gtest_discover_tests(ghost-tests)
