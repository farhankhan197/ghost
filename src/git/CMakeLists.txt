add_library(git OBJECT
  notes.cpp
  repo.cpp
  blame.cpp
  diff.cpp
  path.cpp
)

target_include_directories(git PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(git PUBLIC ${CMAKE_SOURCE_DIR}/src/note)
