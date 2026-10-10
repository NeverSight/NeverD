# Hermes is used as an embedded C++ parser/AST library, never as a tool or VM.
# Its own top-level project also builds a VM, tools and another gtest. Select
# only the reviewed parser dependency graph in our isolated CMake directory.
include(FetchContent)

function(neverd_add_javascript_parser)
  if(TARGET neverd_js_parser)
    return()
  endif()
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  FetchContent_Declare(neverd_hermes
    URL https://codeload.github.com/facebook/hermes/tar.gz/602befee340da188ea1560cde7a9cc33e6b180dc
    URL_HASH SHA256=6c615757374850ccb99991c4536aaf86ac0973661b1ae18aac45d2ca85143153
    SOURCE_SUBDIR neverd-no-automatic-subdirectory
    TIMEOUT 120)
  FetchContent_MakeAvailable(neverd_hermes)
  set(NEVERD_HERMES_SOURCE_DIR "${neverd_hermes_SOURCE_DIR}")
  add_subdirectory("${CMAKE_SOURCE_DIR}/cmake/hermes-parser"
                   "${neverd_hermes_BINARY_DIR}" EXCLUDE_FROM_ALL)
endfunction()
