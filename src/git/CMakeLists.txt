add_library(git OBJECT
  notes.cpp
  repo.cpp
  blame.cpp
  diff.cpp
  engine.cpp
  path.cpp
  ref.cpp
)

target_include_directories(git PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(git PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(git PUBLIC ${CMAKE_SOURCE_DIR}/src/note)
target_link_libraries(git PUBLIC libgit2::libgit2package)
