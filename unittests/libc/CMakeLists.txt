add_neverd_unittest(NeverDLibCTests
  LibCNamesTests.cpp
  LINK_COMPONENTS LibC
)

target_include_directories(NeverDLibCTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
