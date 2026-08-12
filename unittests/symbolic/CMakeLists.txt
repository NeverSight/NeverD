add_neverd_unittest(NeverDSymbolicTests
  SymExprTests.cpp
  SymMBATests.cpp
  SymParseTests.cpp
  LINK_COMPONENTS Symbolic
)

target_include_directories(NeverDSymbolicTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
