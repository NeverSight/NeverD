add_neverd_unittest(NeverDSymbolicTests
  SymDispatchTests.cpp
  SymExecTests.cpp
  SymExploreTests.cpp
  SymExprTests.cpp
  SymMBAOutcomeTests.cpp
  SymMBAPolynomialTests.cpp
  SymMBAStressTests.cpp
  SymMBATests.cpp
  SymParseTests.cpp

  LINK_COMPONENTS Symbolic
)

target_include_directories(NeverDSymbolicTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
