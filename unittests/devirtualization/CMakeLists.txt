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

add_test(NAME NeverDBytecodeObservation
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/scripts/tests/test_bytecode_observer.py"
    "$<TARGET_FILE:neverd-bytecode-observe>")
set_tests_properties(NeverDBytecodeObservation PROPERTIES
  LABELS "NeverDBytecodeAnalysisTests;NeverDBytecodeObservation" TIMEOUT 120)
add_dependencies(NeverDBytecodeAnalysisTests neverd-bytecode-observe)

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

# Native encoding admission binds complete semantic spans without widening
# the execution context or the independent opaque-state preservation audit.
add_neverd_unittest(NeverDX86NativeStateAuditTests
  NativeStateAuditTests.cpp
  LINK_COMPONENTS InterpreterSpecialization Decode Lift IRLow Loader
)

add_neverd_unittest(NeverDX86LegacyGSRecoveryTests
  LegacyGSRecoveryTests.cpp
  LINK_COMPONENTS InterpreterSpecialization Decode Lift IRLow Loader
)

# A raw recovery declaration must not enable concrete execution. Exercise the
# production admission owner even when emulation backends are disabled.
add_neverd_unittest(NeverDLegacyGSExecutionPolicyTests
  LegacyGSExecutionPolicyTests.cpp
  ${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/driver/WindowsX64ExecutionPolicy.cpp
  LINK_LIBS capstone_static LLVMSupport)
target_include_directories(NeverDLegacyGSExecutionPolicyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/emulation
  ${LLVM_INCLUDE_DIRS})

add_neverd_unittest(NeverDX86ScalarMoveAuditTests
  ScalarMoveAuditTests.cpp
  LINK_COMPONENTS Decode Lift IRLow Loader
)

add_neverd_unittest(NeverDOriginalBinaryUndefinedIndependenceTests
  OriginalBinaryUndefinedIndependenceTests.cpp
  NativeQueryTraceTests.cpp
  LINK_COMPONENTS BinaryInterpreterSpecialization
)

add_neverd_unittest(NeverDPEFixedImageTests
  PEFixedImageTests.cpp
  LINK_COMPONENTS InterpreterLLVMRefinement Loader
)

if(TARGET neverd_shared)
  set(_pe_browse_components Loader CodeGen)
  if(APPLE)
    # Resolve the C++ helpers and LLVM from the same dylib as the C API.
    # Static components would register LLVM command-line options twice.
    set(_pe_browse_components "")
  endif()
  add_neverd_unittest(NeverDPEBrowseCompatibilityTests
    PEBrowseCompatibilityTests.cpp
    LINK_COMPONENTS ${_pe_browse_components}
    LINK_LIBS neverd_shared
  )
  target_include_directories(NeverDPEBrowseCompatibilityTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
  unset(_pe_browse_components)
endif()

add_neverd_unittest(NeverDLowIRRefinementTests
  LowIRRefinementTests.cpp
  LowIRReadTraceTests.cpp
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
  LLVMInterpreterModelAlignmentTests.cpp
  LLVMInterpreterModelBswapTests.cpp
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
  LLVMScalarBswapTests.cpp
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
  InterpreterLLVMRefinementAlignmentTests.cpp
  InterpreterLLVMRefinementBswapTests.cpp
  InterpreterLLVMRefinementLoopTests.cpp
  InterpreterLLVMRefinementCompileTests.cpp
  LINK_COMPONENTS InterpreterLLVMRefinement
)
target_compile_definitions(NeverDInterpreterLLVMRefinementTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDRawEnvironmentRequirementTests
  RawEnvironmentRequirementTests.cpp
  LINK_COMPONENTS BinaryInterpreterSpecialization BackendC BackendLLVM
)

add_neverd_unittest(NeverDInterpreterSpecializationTests
  InterpreterSpecializationTests.cpp
  InterpreterTransferChainTests.cpp
  NativeStackSpecializationTests.cpp
  NativeStackControlTests.cpp
  TerminalImportSpecializationTests.cpp
  NativeUndefinedIndependenceTests.cpp
  MemoryCallSpecializationTests.cpp
  ControlStateRecoveryTests.cpp
  MaskedControlRelationTests.cpp
  CanonicalControlFieldsTests.cpp
  ControlDiscoveryTests.cpp
  FiniteValuesTests.cpp
  FiniteQueryCacheTests.cpp
  CompletedQueryCacheTests.cpp
  FreshChoiceProjectionTests.cpp
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

add_neverd_unittest(NeverDX64ContinuationSourceTests
  X64ContinuationSourceTests.cpp
  X64ContinuationIntegerTests.cpp
  LINK_COMPONENTS BackendC BackendLLVM Pipeline BinaryInterpreterSpecialization
)
target_compile_definitions(NeverDX64ContinuationSourceTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
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

# The public continuation route owns source/manifest export and driver stops.
add_neverd_unittest(NeverDX64ContinuationExportTests
  X64ContinuationExportTests.cpp
  LINK_LIBS neverd_shared)
target_compile_definitions(NeverDX64ContinuationExportTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(NeverDX64ContinuationExportTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
if(NOT APPLE)
  target_link_libraries(NeverDX64ContinuationExportTests PRIVATE LLVMSupport)
endif()
add_dependencies(NeverDX64ContinuationExportTests neverd)
