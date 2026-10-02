add_neverd_unittest(NeverDSymbolicTests
  SymConcreteTests.cpp
  SymDispatchTests.cpp
  SymExecTests.cpp
  SymStorePreservationTests.cpp
  SymExploreCLIContractTests.cpp
  SymExploreTests.cpp
  SymExprTests.cpp
  SymExprCompareTests.cpp
  SymExprComplementTests.cpp
  SymExprMaskTests.cpp
  SymMBAAffineResidualTests.cpp
  SymMBABooleanResidualTests.cpp
  SymMBAArithmeticTests.cpp
  SymMBAAffineFastPathTests.cpp
  SymMBACandidateSelectionTests.cpp
  SymMBACandidateRootTests.cpp
  SymMBAComplementArithmeticTests.cpp
  SymMBAComplementBoundaryTests.cpp
  SymMBAConstantRegionTests.cpp
  SymMBAMeasureTests.cpp
  SymMBAOffsetTests.cpp
  SymMBAOutcomeTests.cpp
  SymMBAParityTests.cpp
  SymMBAPolynomialTests.cpp
  SymMBARestoredTests.cpp
  SymMBASampleTests.cpp
  SymMBASelectorTests.cpp
  SymMBAStressTests.cpp
  SymMBAFactorCostTests.cpp
  SymMBAStructuralFactorTests.cpp
  SymMBAWeightedTests.cpp
  SymMBAComplementTests.cpp
  SymMBATests.cpp
  SymParseTests.cpp
  SymReadabilityTests.cpp
  SymSynthTests.cpp
  SymX87Tests.cpp

  LINK_COMPONENTS Symbolic
)

target_include_directories(NeverDSymbolicTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(NeverDSymbolicTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  SYMBOLIC_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_dependencies(NeverDSymbolicTests neverd)

add_neverd_unittest(NeverDSymbolicSparseContextTests
  SymMBASparseContextTests.cpp
  LINK_COMPONENTS Symbolic
)

target_include_directories(NeverDSymbolicSparseContextTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
