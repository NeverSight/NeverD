add_neverd_unittest(NeverDBytecodeAnalysisTests
  BytecodeDecoderTests.cpp
  BytecodeCLITests.cpp
  LINK_COMPONENTS BytecodeAnalysis IRMed IRHigh BackendC BackendLLVM Support
)
target_compile_definitions(NeverDBytecodeAnalysisTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_BYTECODE_BINARY="$<TARGET_FILE:neverd-bytecode>"
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
)
add_dependencies(NeverDBytecodeAnalysisTests neverd-bytecode)

add_neverd_unittest(NeverDBytecodeCAPITests
  BytecodeCAPITests.cpp
  LINK_LIBS neverd_shared
)
target_compile_definitions(NeverDBytecodeCAPITests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
)
if(NOT APPLE)
  target_link_libraries(NeverDBytecodeCAPITests PRIVATE LLVMSupport)
endif()

add_neverd_unittest(NeverDLowIRUndefinedIndependenceTests
  LowIRUndefinedIndependenceTests.cpp
  BinaryUndefinedIndependenceTests.cpp
  LINK_COMPONENTS InterpreterSpecialization Decode Lift
)

add_neverd_unittest(NeverDOriginalBinaryUndefinedIndependenceTests
  OriginalBinaryUndefinedIndependenceTests.cpp
  LINK_COMPONENTS BinaryInterpreterSpecialization
)

add_neverd_unittest(NeverDPEFixedImageTests
  PEFixedImageTests.cpp
  LINK_COMPONENTS InterpreterLLVMRefinement Loader
)

if(TARGET neverd_shared)
  add_neverd_unittest(NeverDPEBrowseCompatibilityTests
    PEBrowseCompatibilityTests.cpp
    LINK_COMPONENTS Loader CodeGen
    LINK_LIBS neverd_shared
  )
endif()

add_neverd_unittest(NeverDLowIRRefinementTests
  LowIRRefinementTests.cpp
  CompleteModelTests.cpp
  CompletedTargetFactsTests.cpp
  ConditionalImplicationTests.cpp
  DomainCoverageTests.cpp
  LowIRLoopAlignmentTests.cpp
  InterpreterMachineStateModelTests.cpp
  BinaryLowIRRefinementTests.cpp
  LINK_COMPONENTS BinaryInterpreterSpecialization
)

add_neverd_unittest(NeverDLLVMInterpreterModelTests
  LLVMInterpreterModelTests.cpp
  LLVMInterpreterModelCompileTests.cpp
  LLVMInterpreterModelGuardTests.cpp
  LLVMInterpreterModelInitializationTests.cpp
  LLVMInterpreterModelLoopTests.cpp
  LLVMInterpreterModelRejectionTests.cpp
  LLVMInterpreterModelShiftTests.cpp
  LLVMInterpreterModelFunnelTests.cpp
  LINK_COMPONENTS LLVMInterpreterModel
  LINK_LIBS LLVMAsmParser LLVMIRReader
)
target_compile_definitions(NeverDLLVMInterpreterModelTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDLLVMScalarEquivalenceTests
  LLVMScalarAssumeTests.cpp
  LLVMScalarEquivalenceTests.cpp
  LLVMScalarDecisionTests.cpp
  LLVMScalarDemandTests.cpp
  LLVMScalarModelTests.cpp
  LLVMScalarCompileTests.cpp
  LINK_COMPONENTS LLVMInterpreterModel
  LINK_LIBS LLVMAsmParser LLVMIRReader
)
target_compile_definitions(NeverDLLVMScalarEquivalenceTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDLLVMScalarResultProjectionTests
  LLVMScalarResultProjectionTests.cpp
  LINK_COMPONENTS LLVMScalarResultProjection
  LINK_LIBS LLVMAsmParser LLVMIRReader
)
target_compile_definitions(NeverDLLVMScalarResultProjectionTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDLLVMScalarStateProjectionTests
  LLVMScalarStateProjectionTests.cpp
  LINK_COMPONENTS LLVMScalarStateProjection
  LINK_LIBS LLVMAsmParser LLVMIRReader LLVMTransformUtils
)
target_compile_definitions(NeverDLLVMScalarStateProjectionTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDLLVMScalarInputProjectionTests
  LLVMScalarInputProjectionTests.cpp
  LINK_COMPONENTS LLVMScalarInputProjection
  LINK_LIBS LLVMAsmParser LLVMIRReader LLVMTransformUtils
)
target_compile_definitions(NeverDLLVMScalarInputProjectionTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDInterpreterLLVMRefinementTests
  InterpreterLLVMRefinementTests.cpp
  InterpreterLLVMRefinementPreservationTests.cpp
  InterpreterLLVMRefinementCollectionTests.cpp
  InterpreterLLVMRefinementLoopTests.cpp
  InterpreterLLVMRefinementCompileTests.cpp
  LINK_COMPONENTS InterpreterLLVMRefinement
)
target_compile_definitions(NeverDInterpreterLLVMRefinementTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDInterpreterSpecializationTests
  InterpreterSpecializationTests.cpp
  InterpreterTransferChainTests.cpp
  NativeStackSpecializationTests.cpp
  NativeStackControlTests.cpp
  NativeUndefinedIndependenceTests.cpp
  MemoryCallSpecializationTests.cpp
  ControlStateRecoveryTests.cpp
  MaskedControlRelationTests.cpp
  CanonicalControlFieldsTests.cpp
  ControlDiscoveryTests.cpp
  FiniteValuesTests.cpp
  FiniteQueryCacheTests.cpp
  CompletedQueryCacheTests.cpp
  OrderedQueryTests.cpp
  FrameOffsetsTests.cpp
  StringTransferTests.cpp
  LINK_COMPONENTS InterpreterSpecialization
)

add_neverd_unittest(NeverDDevirtualizationSourceTests
  DevirtualizationSourceTests.cpp
  VMShapeSourceTests.cpp
  RecoverySourcePublicationTests.cpp
  PrivateSourceFrameTests.cpp
  NativeRegisterRecoveryTests.cpp
  LINK_COMPONENTS Pipeline BackendC Support
)
target_compile_definitions(NeverDDevirtualizationSourceTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
add_dependencies(NeverDDevirtualizationSourceTests neverd)

add_neverd_unittest(NeverDInterpreterMachineStateTests
  InterpreterMachineStateTests.cpp
  MachineControlSourceTests.cpp
  MachineMemoryCallSourceTests.cpp
  MachineStringSourceTests.cpp
  LINK_COMPONENTS Pipeline BackendC Support
)
target_compile_definitions(NeverDInterpreterMachineStateTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)
add_dependencies(NeverDInterpreterMachineStateTests neverd)

add_neverd_unittest(NeverDLLVMScalarLoopRecoveryTests
  LLVMScalarLoopRecoveryTests.cpp
  LLVMScalarLoopPredicateTests.cpp
  LLVMScalarLoopBoundTests.cpp
  LLVMScalarSourceRecoveryTests.cpp
  LLVMScalarLoopMaskTests.cpp
  LINK_COMPONENTS LLVMScalarLoopRecovery
  LINK_LIBS LLVMAsmParser LLVMIRReader
)

add_neverd_unittest(NeverDLLVMPrivateFrameTests
  LLVMPrivateFrameProjectionTests.cpp
  LLVMPrivateFrameCompileTests.cpp
  LINK_COMPONENTS LLVMMemoryAnalysis
  LINK_LIBS LLVMAsmParser
)
target_compile_definitions(NeverDLLVMPrivateFrameTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)
target_compile_definitions(NeverDLLVMScalarLoopRecoveryTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)
