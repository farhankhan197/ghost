add_library(note OBJECT
  line_range.cpp
  writer.cpp
  reader.cpp
  verified_writer.cpp
  verified_reader.cpp
  gitai_reader.cpp
)

target_include_directories(note PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})