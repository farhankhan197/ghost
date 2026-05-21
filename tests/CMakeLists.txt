add_executable(ghost-tests
  unit/test_line_range.cpp
  unit/test_note_writer.cpp
  unit/test_note_reader.cpp
  unit/test_verified_writer.cpp
  unit/test_ghost_config.cpp
  integration/test_checkpoint.cpp
  integration/test_post_commit.cpp
  integration/test_audit.cpp
  integration/test_installer.cpp
  ${CMAKE_SOURCE_DIR}/src/config/ghost_config.cpp
)

target_link_libraries(ghost-tests PRIVATE note git gtest_main)

target_include_directories(ghost-tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/note
  ${CMAKE_SOURCE_DIR}/src/git
  ${CMAKE_SOURCE_DIR}/src/checkpoint
  ${CMAKE_SOURCE_DIR}/src/commit
  ${CMAKE_SOURCE_DIR}/src/audit
  ${CMAKE_SOURCE_DIR}/src/output
  ${CMAKE_SOURCE_DIR}/src/config
  ${CMAKE_SOURCE_DIR}/src/hooks
)

include(GoogleTest)
gtest_discover_tests(ghost-tests)