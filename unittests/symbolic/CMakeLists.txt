add_neverd_unittest(NeverDSymbolicTests
  SymConcreteTests.cpp
  SymDispatchTests.cpp
  SymExecTests.cpp
  SymCallEffectTests.cpp
  SymMemoryKnowledgeTests.cpp
  SymLoadOriginBindingTests.cpp
  SymMBARewriteCacheTests.cpp
  SymBatchSubstitutionTests.cpp
  SymConditionalControlTests.cpp
  SymMBAKnownFactTests.cpp
  SymMBAObservedWindowTests.cpp
  SymIntegerIntrinsicTests.cpp
  SymSegmentedMemoryTests.cpp
  SymRegionWrapTests.cpp
  SymStorePreservationTests.cpp
  SymConditionalStoreTests.cpp
  SymExploreCLIContractTests.cpp
  SymExploreTests.cpp
  SymExprTests.cpp
  SymExprBooleanEqualityTests.cpp
  SymExprCancellationTests.cpp
  SymLowWordProjectionTests.cpp
  SymExprCompareTests.cpp
  SymExprConditionalEqualityTests.cpp
  SymExprComplementTests.cpp
  SymExprExtensionTests.cpp
  SymExprExtensionEqualityTests.cpp
  SymExprMaskTests.cpp
  SymKnownBitsTests.cpp
  SymMBAAffineResidualTests.cpp
  SymMBABooleanResidualTests.cpp
  SymMBAArithmeticTests.cpp
  SymMBAAffineFastPathTests.cpp
  SymMBACandidateSelectionTests.cpp
  SymMBACandidateRootTests.cpp
  SymMBAComplementArithmeticTests.cpp
  SymMBAMaskRelationTests.cpp
  SymMBAWeightedMaskTests.cpp
  SymMBABitwiseViewTests.cpp
  SymMBAMultiCarrierTests.cpp
  SymMBAQuadraticTests.cpp
  SymMBAQuadraticRowTests.cpp
  SymMBACubicTests.cpp
  SymMBAHigherProductTests.cpp
  SymMBAResidualBudgetTests.cpp
  SymMBAResidueTests.cpp
  SymMBAAffineResidueTests.cpp
  SymMBAPrefixResidueTests.cpp
  SymMBAWidePairTests.cpp
  SymMBAWitnessTests.cpp
  SymMBAResidueCacheTests.cpp
  SymMBAResiduePowerTests.cpp
  SymMBAResidueCoordinateTests.cpp
  SymMBALinearAdmissionTests.cpp
  SymMBALazyPolynomialTests.cpp
  SymMBAComplementBoundaryTests.cpp
  SymMBAConstantRegionTests.cpp
  SymMBAMeasureTests.cpp
  SymMBAOffsetTests.cpp
  SymMBAOutcomeTests.cpp
  SymMBAMinimumTests.cpp
  SymMBAParityTests.cpp
  SymMBAPolynomialTests.cpp
  SymMBARestoredTests.cpp
  SymMBASampleTests.cpp
  SymMBASelectorTests.cpp
  SymMBAStressTests.cpp
  SymMBAFactorCostTests.cpp
  SymMBAExtensionTests.cpp
  SymMBAStructuralFactorTests.cpp
  SymMBAWeightedTests.cpp
  SymMBAComplementTests.cpp
  SymMBATests.cpp
  SymParseTests.cpp
  SymParseLexerTests.cpp
  SymReadabilityTests.cpp
  SymSynthTests.cpp
  SymSynthAffineTests.cpp
  SymSynthQuadraticTests.cpp
  x86/SymX87Tests.cpp

  LINK_COMPONENTS Symbolic Solver
)

target_include_directories(NeverDSymbolicTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(NeverDSymbolicTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  SYMBOLIC_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_dependencies(NeverDSymbolicTests neverd)

add_neverd_unittest(NeverDSymSynthAffineTests
  SymSynthAffineTests.cpp
  SymSynthTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymSynthQuadraticTests
  SymSynthQuadraticTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymMBAMaskRelationTests
  SymMBAMaskRelationTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymMBAWeightedMaskTests
  SymMBAWeightedMaskTests.cpp
  SymMBABitwiseViewTests.cpp
  SymMBAMultiCarrierTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymMBAQuadraticTests
  SymMBAQuadraticTests.cpp
  SymMBAQuadraticRowTests.cpp
  SymMBACubicTests.cpp
  SymMBAHigherProductTests.cpp
  SymMBAResidualBudgetTests.cpp
  SymMBAResidueTests.cpp
  SymMBAAffineResidueTests.cpp
  SymMBAPrefixResidueTests.cpp
  SymMBAWidePairTests.cpp
  SymMBAWitnessTests.cpp
  SymMBAResidueCacheTests.cpp
  SymMBAResiduePowerTests.cpp
  SymMBAResidueCoordinateTests.cpp
  SymMBALinearAdmissionTests.cpp
  SymMBALazyPolynomialTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymbolicIntegerIntrinsicTests
  SymIntegerIntrinsicTests.cpp
  SymExecTests.cpp
  SymStorePreservationTests.cpp
  x86/SymX87Tests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymbolicCallEffectTests
  SymCallEffectTests.cpp
  SymExploreTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymbolicPredicateTests
  SymExprTests.cpp
  SymExprBooleanEqualityTests.cpp
  SymExprCompareTests.cpp
  SymExprExtensionTests.cpp
  SymExprExtensionEqualityTests.cpp
  SymExprConditionalEqualityTests.cpp
  SymKnownBitsTests.cpp
  SymLowWordProjectionTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymbolicSparseContextTests
  SymMBASparseContextTests.cpp
  LINK_COMPONENTS Symbolic
)

target_include_directories(NeverDSymbolicSparseContextTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

add_neverd_unittest(NeverDSymMBAObservedWindowTests
  SymMBAObservedWindowTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymMBAKnownFactTests
  SymMBAKnownFactTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymConditionalControlTests
  SymConditionalControlTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymBatchSubstitutionTests
  SymBatchSubstitutionTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymMBARewriteCacheTests
  SymMBARewriteCacheTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymLoadOriginBindingTests
  SymLoadOriginBindingTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymMemoryKnowledgeTests
  SymMemoryKnowledgeTests.cpp
  LINK_COMPONENTS Symbolic Solver
)

add_neverd_unittest(NeverDSymConditionalStoreTests
  SymConditionalStoreTests.cpp
  LINK_COMPONENTS Symbolic Solver
)
