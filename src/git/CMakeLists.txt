add_library(git OBJECT
  notes.cpp
  repo.cpp
  blame.cpp
  diff.cpp
)

target_include_directories(git PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})