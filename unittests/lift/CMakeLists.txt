add_neverd_unittest(NeverDPEProgramExportsTests
  format/PEProgramExportsTests.cpp
  LINK_COMPONENTS Loader
)

add_neverd_unittest(NeverDMedEntryFrameTests
  core/MedSSAMultiRootTests.cpp
  LINK_COMPONENTS Pipeline BackendC CodeGen)
target_include_directories(NeverDMedEntryFrameTests PRIVATE
  ${CMAKE_SOURCE_DIR}/unittests)
if(NEVERD_ENABLE_BINARY_CORPUS_TESTS)
  target_compile_definitions(NeverDMedEntryFrameTests PRIVATE
    NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}")
endif()

add_neverd_unittest(NeverDLiftTests
  core/X64MemoryCallTargetTests.cpp
  aarch64/AArch64_ArithTests.cpp
  aarch64/AArch64_AtomicTests.cpp
  aarch64/AArch64_BitShiftTests.cpp
  aarch64/AArch64_CarryTests.cpp
  aarch64/AArch64_CallAbiTests.cpp
  aarch64/AArch64_CondTests.cpp
  aarch64/AArch64_CSelTests.cpp
  aarch64/AArch64_ExtendTests.cpp
  aarch64/AArch64_FAMinMaxTests.cpp
  aarch64/AArch64_FPTests.cpp
  aarch64/AArch64_FunctionDiscoveryTests.cpp
  aarch64/AArch64_IntrinsicsTests.cpp
  aarch64/AArch64_LogicTests.cpp
  aarch64/AArch64_MemTests.cpp
  aarch64/AArch64_MopsTests.cpp
  aarch64/AArch64_MTETests.cpp
  aarch64/AArch64_PAuthTests.cpp
  aarch64/AArch64_RETTests.cpp
  aarch64/AArch64_SVELengthTests.cpp
  aarch64/AArch64_SVEStateTests.cpp
  aarch64/AArch64_SVEStoreTests.cpp
  aarch64/AArch64_WSPTests.cpp
  aarch64/AArch64_MulDivTests.cpp
  aarch64/AArch64DivisionSemanticTests.cpp
  aarch64/AArch64_PipelineE2ETests.cpp
  aarch64/AArch64_FastClassifyParityTests.cpp
  aarch64/AArch64_NativeDecodeParityTests.cpp
  aarch64/AArch64_NativeDecodeParityControlTests.cpp
  aarch64/AArch64_NativeDecodeParityFPTests.cpp
  aarch64/AArch64_NativeDecodeParityLoadStoreTests.cpp
  aarch64/AArch64_NativeDecodeParitySweepTests.cpp
  arm32/ARM32_ArithTests.cpp
  arm32/ARM32_CallAbiTests.cpp
  arm32/ARM32_CarryTests.cpp
  arm32/ARM32_ControlTests.cpp
  arm32/ARM32_ExtendTests.cpp
  arm32/ARM32_IntrinsicsTests.cpp
  arm32/ARM32_LogicTests.cpp
  arm32/ARM32_MemTests.cpp
  arm32/ARM32_MulDivTests.cpp
  arm32/ARM32_PipelineE2ETests.cpp
  arm32/ARM32_PredicatedStackTests.cpp
  arm32/ARM32_ShiftTests.cpp
  x86_32/X86_32_ArithTests.cpp
  x86_32/X86_32_BitOpsTests.cpp
  x86_32/X86_32_CallAbiTests.cpp
  x86_32/X86_32_CarryTests.cpp
  x86_32/X86_32_CmovTests.cpp
  x86_32/X86_32_CmpxchgTests.cpp
  x86_32/X86_32_ControlTests.cpp
  x86_32/X86_32_DShiftTests.cpp
  x86_32/X86_32_FlagsTests.cpp
  x86_32/X86_32_FPTests.cpp
  x86_32/X86_32_LodsMiscTests.cpp
  x86_32/X86_32_LoopTests.cpp
  x86_32/X86_32_MiscTests.cpp
  x86_32/X86_32_MulDivTests.cpp
  x86_32/X86_32_NativeHostModeTests.cpp
  x86_32/X86_32_PipelineE2ETests.cpp
  x86_32/X86_32_PICLibraryTests.cpp
  x86_32/X86_32_GetPcThunkSourceTests.cpp
  x86_32/X86_32_RotateTests.cpp
  x86_32/X86_32_SetccTests.cpp
  x86_32/X86_32_SignExtTests.cpp
  x86_32/X86_32_StackTests.cpp
  x86_32/X86_32_StringTests.cpp
  x86_32/X86_32_X87FPUTests.cpp
  x86_32/X86_32_DebugHighCTests.cpp
  x86_32/X86_32_FunctionDiscoveryTests.cpp
  x86_64/X86_64_AdxMiscTests.cpp
  x86_64/X86_64_ArithTests.cpp
  x86_64/X86_64_BitOpsTests.cpp
  x86_64/X86_64_BMITests.cpp
  x86_64/X86_64_CallAbiTests.cpp
  x86_64/X86_64_CarryOpsTests.cpp
  x86_64/X86_64_CmpxchgTests.cpp
  x86_64/X86_64_CFGEntryTests.cpp
  x86_64/X86_64_ControlFlowTests.cpp
  x86_64/X86_64_ControlSemanticsTests.cpp
  x86_64/X86_64_DoubleShiftTests.cpp
  x86_64/X86_64_ExtensionInvariantTests.cpp
  x86_64/X86_64_FlagOpsTests.cpp
  x86_64/X86_64_FlagsMiscTests.cpp
  x86_64/X86_64_FmaMiscTests.cpp
  x86_64/X86_64_FPConvertTests.cpp
  x86_64/X86_64_IntrinsicsTests.cpp
  x86_64/X86_64_LodsMiscTests.cpp
  x86_64/X86_64_LoopBitTests.cpp
  x86_64/X86_64_MemAtomicTests.cpp
  x86_64/X86_64_MiscTests.cpp
  x86_64/X86_64_MulDivTests.cpp
  x86_64/X86_64_PipelineE2ETests.cpp
  x86_64/X86_64_RclRcrTests.cpp
  x86_64/X86_64_RotateShiftTests.cpp
  x86_64/X86_64_SemanticE2ETests.cpp
  x86_64/X86_64_SetccCmovTests.cpp
  x86_64/X86_64_SignConvTests.cpp
  x86_64/X86_64_SignExtTests.cpp
  x86_64/X86_64_SSEAVXTests.cpp
  x86_64/X86_64_SSEFPTests.cpp
  x86_64/X86_64_SSEMiscTests.cpp
  x86_64/X86_64_StackMovTests.cpp
  x86_64/X86_64_StackMemorySemanticTests.cpp
  x86_64/X86_64_NativeHostModeTests.cpp
  x86_64/X86_64_StringOpsTests.cpp
  x86_64/X86_64_X87FPUTests.cpp
  x86_64/X86_64_EVEXCompareTests.cpp
  x86_64/X86_64_WideISAStateTests.cpp
  x86_64/X86_64_TerminatorTests.cpp
  x86_64/X86_64_SwitchTests.cpp
  x86_64/X86_64_RoundTripTests.cpp
  x86_64/X86_64_DebugRecordTests.cpp
  x86_64/X86_64_DebugParamTests.cpp
  aarch64/AArch64_SwitchTests.cpp
  aarch64/AArch64_RoundTripTests.cpp
  arm32/ARM32_SwitchTests.cpp
  arm32/ARM32_RoundTripTests.cpp
  core/InstructionModeTests.cpp
  core/ExecutableCodeOwnerIndexTests.cpp
  core/LowInstructionBoundaryTests.cpp
  core/LiftCommonTests.cpp
  core/TargetRegInfoSubRegTests.cpp
  core/MedSSAMultiRootTests.cpp
  core/ObjCSourceTypeHintTests.cpp
  core/ObjCProtocolTests.cpp
  core/ObjCPropertyTests.cpp
  core/ReturnContractTests.cpp
  core/SourceABITests.cpp
  core/AdjacentCallTests.cpp
  core/MedConstantPropagationTests.cpp
  core/MedImmutableTableScanTests.cpp
  core/JumpTableTargetOwnerTests.cpp
  core/SourceAggregateTests.cpp
  core/SourceCallReturnTests.cpp
  core/HighSSADefinitionTests.cpp
  core/HighSourceRangeTests.cpp
  core/ObjCCallHintTests.cpp
  core/SwiftFieldReceiverTests.cpp
  core/ObjCScrollViewCallTests.cpp
  core/ObjCViewControllerCallTests.cpp
  core/ObjCIOSSDKCallTests.cpp
  core/SwiftTypeNameCallTests.cpp
  core/SwiftNSCoderCallTests.cpp
  core/SwiftCombineCallTests.cpp
  core/SwiftConcurrencyCallTests.cpp
  core/SwiftCoreGraphicsCallTests.cpp
  core/SwiftStringValueCallTests.cpp
  core/SwiftStaticStringCallTests.cpp
  core/ObjCSentinelCallTests.cpp
  core/DarwinAccelerateSourceCallTests.cpp
  core/DarwinUIKitSourceCallTests.cpp
  core/ObjCCategoryTests.cpp
  core/ObjCStorageTests.cpp
  core/HighCSourceCallTests.cpp
  core/NativeSourceHintsTests.cpp
  core/SwiftFunctionSymbolTests.cpp
  core/SourceRegisterCopyTests.cpp
  core/SourceFrameAnalysisTests.cpp
  core/NativeFloatingReturnProofTests.cpp
  core/NativeBooleanResultProofTests.cpp
  core/SwiftBooleanRuntimeCandidateTests.cpp
  core/ObjCBlocksTests.cpp
  core/ObjCBlockCallHintTests.cpp
  core/HighFrameTemporaryTests.cpp
  core/HighControlFlowSemanticsTests.cpp
  core/LowZeroIdiomTests.cpp
  ../backend/HighSwiftEmitterTests.cpp
  ../loader/SwiftMetadataTests.cpp
  ../loader/SwiftOpaqueValueEffectTests.cpp
  ../loader/SwiftValueWitnessCallTests.cpp
  ../loader/SwiftVirtualCallTests.cpp
  ../loader/SwiftRuntimeSourceTests.cpp
  core/HighCPointerAddressTests.cpp
  core/SourceParameterPlacementTests.cpp
  core/HighCIntegerWidthTests.cpp
  core/CSourceDefinitionTests.cpp
  core/CStringCommentTests.cpp
  core/CImageObjectTests.cpp
  core/CSymbolNameTests.cpp
  sdk/ObjCSourceProjectionTests.cpp
  sdk/ObjCSourceBindingTests.cpp
  sdk/ObjCSwiftOnceSourceTests.cpp
  sdk/ObjCSuperGetterSourceTests.cpp
  sdk/ObjCMetadataFactorySourceTests.cpp
  sdk/ObjCMergedSetterSourceTests.cpp
  sdk/ObjCImmutableStringCallbackSourceTests.cpp
  sdk/ObjCSwiftBooleanSourceTests.cpp
  sdk/ObjCBlockSourceTests.cpp
  sdk/SwiftSourceNamespaceTests.cpp
  sdk/SwiftSourceSignatureTests.cpp
  sdk/SwiftABIProjectionPlanTests.cpp
  sdk/SwiftSourcePropertyTests.cpp
  sdk/SwiftRuntimeProjectionTests.cpp
  format/MachOARMHalfRelocationTests.cpp
  format/ARMObjectBranchTests.cpp
  format/DataSymbolBindingTests.cpp
  format/MachOARM64RelocationTests.cpp
  format/MachOI386RelocationTests.cpp
  format/MachOI386RelocationExternalTests.cpp
  format/MachOI386RelocationFixtureTests.cpp
  format/MachOI386RelocationPipelineTests.cpp
  format/MachOI386RelocationScatteredTests.cpp
  format/MachOFunctionListingTests.cpp
  format/FunctionDiscoveryAlignmentTests.cpp
  format/ELFFunctionListingTests.cpp
  format/ELFWithoutSectionsTests.cpp
  format/ELFARM32ModeTests.cpp
  format/COFFFunctionListingTests.cpp
  format/InputDigestTests.cpp
  format/MachOLLVMNameBoundaryTests.cpp
  format/COFFImportStorageBoundaryTests.cpp
  format/LLVMImportVeneerBoundaryTests.cpp
  format/MachOPointerRelocationBoundaryTests.cpp
  format/ImmutableChainedValueTests.cpp
  format/COFFRelocatableAbsoluteRelocationTests.cpp
  format/NativePointerRelocationBoundaryTests.cpp
  format/PointerRelocationBatchTests.cpp
  core/MedCallingConvTests.cpp
  core/MedDceImplicitSpTests.cpp
  core/MedVariadicTests.cpp
  eh/COFFExceptionTests.cpp
  eh/COFFExceptionARM32UnwindTests.cpp
  eh/COFFExceptionARM64UnwindTests.cpp
  eh/COFFExceptionDelphiTests.cpp
  eh/COFFExceptionFH3Tests.cpp
  eh/COFFExceptionFH4Tests.cpp
  eh/COFFFH4EncodingTests.cpp
  eh/COFFExceptionGSTests.cpp
  eh/COFFExceptionIRTests.cpp
  eh/COFFExceptionNativeIRTests.cpp
  eh/COFFExceptionSEHTests.cpp
  eh/ELFARMEHABIPatchTests.cpp
  eh/ELFExceptionTests.cpp
  eh/ExceptionCFGSeedTests.cpp
  eh/ItaniumEHEmitterTests.cpp
  eh/ItaniumEHStructurerTests.cpp
  eh/MachOARM32TransactionTests.cpp
  eh/MachOCompactUnwindPatchTests.cpp
  eh/MachOExceptionPatchTests.cpp
  core/HighSymSimplifyTests.cpp
  core/HighFrameStoreForwardingTests.cpp
  core/HighSwitchFollowTests.cpp
  core/MedNoReturnTests.cpp
  core/InternalNoReturnTests.cpp
  core/ObjectExternTests.cpp
  core/HighCReturnJoinTests.cpp
  eh/RegistrationTryLevelTests.cpp
  format/COFFARMFormatTests.cpp
  format/COFFARMFormatFragmentTests.cpp
  format/COFFARMFormatMalformedTests.cpp
  format/COFFARMHighCTests.cpp
  format/COFFARMPipelineTests.cpp
  format/COFFARMStackAnalysisTests.cpp
  arm32/FunctionDiscoveryThumbTests.cpp
  core/NdOpEmulatorTests.cpp
  core/NdOpEmulatorBitOpsTests.cpp
  core/NdOpEmulatorMemoryTests.cpp
  core/CircleRangeTests.cpp
  core/JumpTableEnhancedTests.cpp
  core/JumpTableFiniteSelectorTests.cpp
  core/JumpTableFixedAddressTests.cpp
  core/JumpTableSharedAbsoluteTests.cpp
  core/JumpTableProposalLFPTests.cpp
  format/PatchFormatTests.cpp
  format/PatchFormatCOFFARMTests.cpp
  format/PatchFormatCOFFTests.cpp
  format/PatchFormatSectionTests.cpp
  format/PatchFormatXCallTests.cpp
  format/RuntimeMetadataTests.cpp
  format/TextSectionDetectionTests.cpp
)

# Keeps wide x86 architectural-state regressions independently runnable while
# AVX-512, AMX, and APX support is expanded in focused vertical slices.
add_neverd_unittest(NeverDX86WideISAStateTests
  x86_64/X86_64_WideISAStateTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86WideISAStateTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX MOVRS decode validation and architectural load semantics focused.
add_neverd_unittest(NeverDX86ApxMovrsTests
  x86_64/X86_64_APXMovrsTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86ApxMovrsTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX-EVEX KMOV raw/detail validation, extended addressing and concrete
# mask-register memory semantics independently runnable while preserving VEX.
add_neverd_unittest(NeverDX86APXKmovTests
  x86_64/X86_64_APXKmovTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXKmovTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps MPSADBW's lane-local immediate selection independently runnable
# through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86MPSADBWStrictTests
  x86_64/X86_64_MPSADBWStrictTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86MPSADBWStrictTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Exact EVEX VDBPSADBW decode, lift, immediate, lane and writemask semantics.
add_neverd_unittest(NeverDX86VDBPSADBWStrictTests
  x86_64/X86_64_VDBPSADBWStrictTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86VDBPSADBWStrictTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Safety boundary for AVX-512 floating-point control instructions whose
# immediate, masking, scalar merge, broadcast, or SAE semantics are not yet
# representable exactly in LowIR.
add_neverd_unittest(NeverDX86EVEXFPControlGuardTests
  x86_64/X86_64_EVEXFPControlGuardTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXFPControlGuardTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDX86FourFMAExactTests
  x86_64/X86_64_FourFMAExactTests.cpp
  LINK_COMPONENTS Decode Lift Loader
)
target_include_directories(NeverDX86FourFMAExactTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS}
)

# Exact AVX-512 4VNNIW VP4DPWSSD/SSDS tuple, source-block, mask and
# per-iteration integer accumulation semantics.
add_neverd_unittest(NeverDX86VP4DPWTests
  x86_64/X86_64_VP4DPWTests.cpp
  LINK_COMPONENTS Decode Lift Loader
)
target_include_directories(NeverDX86VP4DPWTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS}
)

# Runs MOVDIR64B through decode, lift, and strict memory-state emulation,
# including its read-before-write, alignment and 64-byte atomicity contract.
add_neverd_unittest(NeverDX86MOVDIR64BStateTests
  x86_64/X86_64_MOVDIR64BStateTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86MOVDIR64BStateTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps opaque system-memory side effects independently runnable through
# decode, lift and strict fail-closed emulation without executing host state.
add_neverd_unittest(NeverDX86SystemSideeffectTests
  x86_64/X86_64_SystemSideeffectTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86SystemSideeffectTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps restartable AMX tile-state semantics independently runnable through
# decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86AMXStateTests
  x86_64/X86_64_AMXStateTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86AMXStateTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Pins the LLVM boundary independently from concrete AMX emulation.  Until the
# versioned runtime owns tile state, restartable strided memory and fault
# continuation, every exact AMX MedIR shape must fail before emitting effects.
add_neverd_unittest(NeverDX86AMXLLVMGuardTests
  x86_64/X86_64_AMXLLVMGuardTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  LibC
  Lift
)

target_include_directories(NeverDX86AMXLLVMGuardTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Pins exact 128/256/512-bit memory transfer behavior independently of the
# aggregate lift suite, including fault atomicity and address wrap rejection.
add_neverd_unittest(NeverDNdOpEmulatorWideMemoryTests
  core/NdOpEmulatorWideMemoryTests.cpp

  LINK_COMPONENTS
  IRLow
  Lift
  Loader
)

target_include_directories(NeverDNdOpEmulatorWideMemoryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps legacy x86 memory RMW instructions fault-atomic through decode, lift,
# and strict concrete emulation, including the 128-bit CMPXCHG16B contract.
add_neverd_unittest(NeverDX86LegacyAtomicSemanticTests
  x86_64/X86_64_LegacyAtomicSemanticTests.cpp

  LINK_COMPONENTS
  Decode
  IRLow
  Lift
  Loader
)

target_include_directories(NeverDX86LegacyAtomicSemanticTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps EVEX compare semantics independently runnable while each predicate and
# masking rule is validated end to end through decode, lift, and emulation.
add_neverd_unittest(NeverDX86EVEXCompareTests
  x86_64/X86_64_EVEXCompareTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXCompareTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Exact EVEX floating-point compare predicate, K-result, masking, scalar and
# exception-state semantics, plus the compiled-backend safety boundary.
add_neverd_unittest(NeverDX86EVEXFPCompareTests
  x86_64/X86_64_EVEXFPCompareTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  Decode
  LibC
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXFPCompareTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps EVEX two-table permutation semantics independently runnable through
# decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86EVEXTablePermuteTests
  x86_64/X86_64_EVEXTablePermuteTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXTablePermuteTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps legal EVEX masked-memory and broadcast forms independently runnable
# through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86EVEXMemoryBroadcastTests
  x86_64/X86_64_EVEXMemoryBroadcastTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXMemoryBroadcastTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps EVEX pairwise integer multiply memory and writemask semantics
# independently runnable through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86EVEXPairwiseMulMemoryTests
  x86_64/X86_64_EVEXPairwiseMulMemoryTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXPairwiseMulMemoryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps scalar EVEX broadcast mask, memory-suppression, and register-source
# semantics independently runnable through decode, lift, and strict emulation.
add_neverd_unittest(NeverDX86EVEXScalarBroadcastTests
  x86_64/X86_64_EVEXScalarBroadcastTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXScalarBroadcastTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX extensions of existing EVEX GPR and ordinary address operands
# independently runnable through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86APXEVEXExistingGprTests
  x86_64/X86_64_APXEVEXExistingGprTests.cpp

  LINK_COMPONENTS
  BackendC
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXEVEXExistingGprTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps EVEX mask-producing test instructions independently runnable across
# register, fault-suppressed memory, and scalar-broadcast forms.
add_neverd_unittest(NeverDX86EVEXMaskTestMemoryTests
  x86_64/X86_64_EVEXMaskTestMemoryTests.cpp

  LINK_COMPONENTS
  Decode
  IRLow
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXMaskTestMemoryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Runs EVEX packed floating-point arithmetic through decode, lift, and concrete
# LowIR emulation without coupling the coverage to the broad lift test binary.
add_neverd_unittest(NeverDX86EVEXFloatArithTests
  x86_64/X86_64_EVEXFloatArithTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXFloatArithTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps legal EVEX packed floating-point memory and broadcast forms
# independently runnable through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86EVEXFloatMemoryTests
  x86_64/X86_64_EVEXFloatMemoryTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXFloatMemoryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Runs the FMA3 family through decode, lift, and concrete LowIR emulation so
# operand ordering, fused rounding, and EVEX destination rules stay observable.
add_neverd_unittest(NeverDX86EVEXFMATests
  x86_64/X86_64_EVEXFMATests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXFMATests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps mask/vector conversion semantics independently runnable across every
# AVX-512 vector length and element width.
add_neverd_unittest(NeverDX86EVEXMaskConversionTests
  x86_64/X86_64_EVEXMaskConversionTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXMaskConversionTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Runs every EVEX narrowing family through decode, lift, and concrete LowIR
# emulation across all legal source widths and register writemask modes.
add_neverd_unittest(NeverDX86EVEXNarrowTests
  x86_64/X86_64_EVEXNarrowTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXNarrowTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Runs EVEX VSIB gather/scatter through decode, lift, and concrete masked
# memory emulation, including suppression and restartable partial progress.
add_neverd_unittest(NeverDX86EVEXVSIBTests
  x86_64/X86_64_EVEXVSIBTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXVSIBTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Runs EVEX masked unaligned vector moves through decode, lift, and strict
# memory emulation without coupling fault-suppression coverage to broad tests.
add_neverd_unittest(NeverDX86EVEXMaskedMoveTests
  x86_64/X86_64_EVEXMaskedMoveTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXMaskedMoveTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps EVEX tuple broadcasts and lane insert/extract semantics independently
# runnable through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86EVEXLaneMoveTests
  x86_64/X86_64_EVEXLaneMoveTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXLaneMoveTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX stack encodings independently runnable through decode, lift, and
# strict concrete memory emulation.
add_neverd_unittest(NeverDX86APXStackTests
  x86_64/X86_64_APXStackTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXStackTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX SETcc/SETZUcc register-width and byte-memory semantics independently
# runnable through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86APXSetccTests
  x86_64/X86_64_APXSetccTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXSetccTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX conditional-faulting move semantics independently runnable through
# decode, lift, and strict concrete memory emulation.
add_neverd_unittest(NeverDX86APXCFCmovTests
  x86_64/X86_64_APXCFCmovTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXCFCmovTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX NDD CMOV's three-operand selection and ordinary faulting-load
# behavior independently runnable through decode, lift, and strict emulation.
add_neverd_unittest(NeverDX86APXNddCMovTests
  x86_64/X86_64_APXNddCMovTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXNddCMovTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX conditional TEST source-condition, default-flag, width and
# ordinary faulting-load semantics independently runnable through strict
# concrete emulation.
add_neverd_unittest(NeverDX86APXCTestTests
  x86_64/X86_64_APXCTestTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXCTestTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX conditional CMP source-condition, full subtraction flags, default
# flags, operand-order and ordinary faulting-load semantics independently
# runnable through strict concrete emulation.
add_neverd_unittest(NeverDX86APXCCmpTests
  x86_64/X86_64_APXCCmpTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXCCmpTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX-promoted BMI/BMI2/ADX operand roles, NF/NDD flags and ordinary
# memory-fault ordering independently runnable through strict emulation.
add_neverd_unittest(NeverDX86APXBMISemanticTests
  x86_64/X86_64_APXBMISemanticTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXBMISemanticTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps APX RAO and CMPccXADD atomic memory, condition, flag and fault-commit
# semantics independently runnable through decode, lift and strict emulation.
add_neverd_unittest(NeverDX86APXAtomicSemanticTests
  x86_64/X86_64_APXAtomicSemanticTests.cpp

  LINK_COMPONENTS
  Decode
  IRMed
  Lift
  Loader
)

target_include_directories(NeverDX86APXAtomicSemanticTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDX86CarryArithmeticFlagTests
  x86_64/X86_64_CarryArithmeticFlagTests.cpp
  LINK_COMPONENTS Decode Lift Loader
)

add_neverd_unittest(NeverDX86UndefinedEffectsTests
  x86_64/X86_64_UndefinedEffectsTests.cpp
  x86_64/X86_64_ShiftUndefinedEffectsTests.cpp
  x86_64/X86_64_DoubleShiftUndefinedEffectsTests.cpp
  x86_64/X86_64_BitTestUndefinedEffectsTests.cpp
  x86_64/X86_64_XaddAuditTests.cpp
  LINK_COMPONENTS Decode Lift Loader Symbolic
)

add_neverd_unittest(NeverDX86DecodeDetailTests
  x86_64/X86_64_DisplacementDetailTests.cpp
  LINK_COMPONENTS Decode Lift Loader
)

add_neverd_unittest(NeverDX86LogicIdentityTests
  x86_64/X86_64_LogicIdentityTests.cpp
  LINK_COMPONENTS Decode Lift Loader
)

add_neverd_unittest(NeverDX86NoIndexAddressTests
  x86_64/X86_64_NoIndexAddressTests.cpp
  LINK_COMPONENTS Decode Lift Loader
)

# Keeps APX promoted NDD dataflow and NF flag suppression independently
# runnable through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86APXNddNfSemanticTests
  x86_64/X86_64_APXNddNfSemanticTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86APXNddNfSemanticTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps EVEX packed integer/float conversions independently runnable through
# decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86EVEXPackedConvertTests
  x86_64/X86_64_EVEXPackedConvertTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXPackedConvertTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps same-width EVEX dword conversion memory and broadcast semantics
# independently runnable through decode, lift, and strict concrete emulation.
add_neverd_unittest(NeverDX86EVEXDwordConvertMemoryTests
  x86_64/X86_64_EVEXDwordConvertMemoryTests.cpp

  LINK_COMPONENTS
  Decode
  Lift
  Loader
)

target_include_directories(NeverDX86EVEXDwordConvertMemoryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps LLVM lowering of packed conversion scalar lanes independently runnable.
add_neverd_unittest(NeverDX86EVEXPackedConvertEmitterTests
  x86_64/X86_64_EVEXPackedConvertEmitterTests.cpp

  LINK_COMPONENTS
  LLVMFloatConvertLowering
)

target_include_directories(NeverDX86EVEXPackedConvertEmitterTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDX86EVEXCompressExpandMemoryTests
  x86_64/X86_64_EVEXCompressExpandMemoryTests.cpp
  LINK_COMPONENTS Decode Lift Loader
)
target_include_directories(NeverDX86EVEXCompressExpandMemoryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})

add_neverd_unittest(NeverDX86EVEXCryptoTests
  x86_64/X86_64_EVEXCryptoTests.cpp

  LINK_COMPONENTS
  BackendC
  BackendLLVM
  Decode
  LibC
  Lift
  Loader
)
target_include_directories(NeverDX86EVEXCryptoTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})

# Prebuilt LLVM packages ship libraries only.  Point HighC syntax checks at
# that tree's clang when it exists; otherwise use the host compiler that
# configured the build so CI does not try to exec `/clang`.
set(_neverd_bundled_clang
    "${LLVM_TOOLS_BINARY_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX}")
if(EXISTS "${_neverd_bundled_clang}")
  set(_neverd_test_clang "${_neverd_bundled_clang}")
else()
  find_program(_neverd_test_clang NAMES clang)
  if(NOT _neverd_test_clang AND CMAKE_C_COMPILER)
    set(_neverd_test_clang "${CMAKE_C_COMPILER}")
  endif()
endif()

target_compile_definitions(NeverDX86EVEXCryptoTests PRIVATE
  NEVERD_TEST_CLANG="${_neverd_test_clang}"
)

target_compile_definitions(NeverDLiftTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_RUNTIME_FIXTURE_COMPILER="${NEVERD_HOST_RUNTIME_FIXTURE_COMPILER_EXECUTABLE}"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  TEST_OBJ_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}"
)

target_include_directories(NeverDLiftTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/pipeline
  ${CMAKE_SOURCE_DIR}/lib/loader/COFF/unwind
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDJumpTableTests
  core/AffineFrameStateTests.cpp
  core/ResolverLaneViewTests.cpp
  core/IndirectTailFrameTests.cpp
  core/JumpTableEnhancedTests.cpp
  core/JumpTableFiniteSelectorTests.cpp
  core/JumpTableFixedAddressTests.cpp
  core/JumpTableSharedAbsoluteTests.cpp
  core/JumpTableProposalLFPTests.cpp
  LINK_COMPONENTS Pipeline Support
)
target_compile_definitions(NeverDJumpTableTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  TEST_OBJ_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
target_include_directories(NeverDJumpTableTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/ir/low
  ${CMAKE_SOURCE_DIR}/lib/pipeline ${LLVM_INCLUDE_DIRS})
add_dependencies(NeverDJumpTableTests neverd)

# Run source call ABI, binding, and emitted-code checks without rebuilding the
# complete fixture inventory during mobile decompiler development.
# MSVC's standard COFF object section limit is too small for this large test
# translation unit in Release; it is also part of NeverDLiftTests.
if(MSVC)
  set_source_files_properties(core/ObjCCallHintTests.cpp PROPERTIES
    COMPILE_OPTIONS /bigobj)
endif()
add_neverd_unittest(NeverDObjCSourceCallTests
  ../loader/SwiftOpaqueValueEffectTests.cpp
  ../loader/SwiftValueWitnessCallTests.cpp
  ../loader/SwiftVirtualCallTests.cpp
  core/ObjCCategoryTests.cpp
  core/SourceABITests.cpp
  core/AdjacentCallTests.cpp
  core/MedConstantPropagationTests.cpp
  core/JumpTableTargetOwnerTests.cpp
  core/SourceAggregateTests.cpp
  core/ObjCProtocolTests.cpp
  core/ObjCPropertyTests.cpp
  core/NativeSourceHintsTests.cpp
  core/SwiftFunctionSymbolTests.cpp
  core/SourceRegisterCopyTests.cpp
  core/SourceFrameAnalysisTests.cpp
  core/NativeFloatingReturnProofTests.cpp
  core/NativeBooleanResultProofTests.cpp
  core/SwiftBooleanRuntimeCandidateTests.cpp
  format/ImmutableChainedValueTests.cpp
  core/ObjCStorageTests.cpp
  core/ObjCBlocksTests.cpp
  core/ObjCCallHintTests.cpp
  core/SwiftFieldReceiverTests.cpp
  core/ObjCScrollViewCallTests.cpp
  core/ObjCViewControllerCallTests.cpp
  core/ObjCIOSSDKCallTests.cpp
  core/SwiftTypeNameCallTests.cpp
  core/SwiftNSCoderCallTests.cpp
  core/SwiftCombineCallTests.cpp
  core/SwiftConcurrencyCallTests.cpp
  core/SwiftCoreGraphicsCallTests.cpp
  core/SwiftStringValueCallTests.cpp
  core/SwiftStaticStringCallTests.cpp
  core/ObjCSentinelCallTests.cpp
  core/DarwinAccelerateSourceCallTests.cpp
  core/DarwinUIKitSourceCallTests.cpp
  core/HighCSourceCallTests.cpp
  sdk/ObjCBlockSourceTests.cpp
  sdk/ObjCSourceBindingTests.cpp
  sdk/ObjCSwiftOnceSourceTests.cpp
  sdk/ObjCSuperGetterSourceTests.cpp
  sdk/ObjCMetadataFactorySourceTests.cpp
  sdk/ObjCMergedSetterSourceTests.cpp
  sdk/ObjCImmutableStringCallbackSourceTests.cpp
  sdk/ObjCSwiftBooleanSourceTests.cpp
  sdk/ObjCSourceProjectionTests.cpp
  sdk/SwiftABIProjectionPlanTests.cpp
  LINK_COMPONENTS
  BackendC
  Pipeline
)
target_include_directories(NeverDObjCSourceCallTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(NeverDObjCSourceCallTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)
if(MSVC)
  target_compile_options(NeverDLiftTests PRIVATE /bigobj)
  target_compile_options(NeverDObjCSourceCallTests PRIVATE /bigobj)
  # JumpTableEnhancedTests.cpp outgrows the default section limit here too.
  target_compile_options(NeverDJumpTableTests PRIVATE /bigobj)
endif()

# Run instruction provenance and shared entry/frame checks without the full
# aggregate lift fixture inventory.
add_neverd_unittest(NeverDLowInstructionBoundaryTests
  core/LowInstructionBoundaryTests.cpp
  LINK_COMPONENTS BackendC BackendLLVM Pipeline
)
target_include_directories(NeverDLowInstructionBoundaryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NeverDLowInstructionBoundaryTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")

# Calls into a function's own body: push-and-jump lifting and the proof that a
# return never pops an address the function pushed itself.
add_neverd_unittest(NeverDOwnInteriorCallTests
  core/OwnInteriorCallTests.cpp
  LINK_COMPONENTS BackendC BackendLLVM Pipeline
)
target_include_directories(NeverDOwnInteriorCallTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NeverDOwnInteriorCallTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")

add_neverd_unittest(NeverDSysVCallContractTests
  core/SysVCallContractTests.cpp
  LINK_COMPONENTS BackendC BackendLLVM Pipeline
)
target_include_directories(NeverDSysVCallContractTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NeverDSysVCallContractTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")

add_neverd_unittest(NeverDWin64CallContractTests
  core/Win64CallContractTests.cpp
  LINK_COMPONENTS BackendC BackendLLVM Pipeline
)
target_include_directories(NeverDWin64CallContractTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})

add_neverd_unittest(NeverDI386CallContractTests
  core/I386CallContractTests.cpp
  LINK_COMPONENTS BackendC BackendLLVM Pipeline
)
target_include_directories(NeverDI386CallContractTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})

# Exercise shared source control-flow and SSA invariants independently of the
# aggregate lift inventory when fixing mobile source reconstruction.
add_neverd_unittest(NeverDHighControlFlowTests
  core/HighControlFlowSemanticsTests.cpp
  core/HighFrameStoreForwardingTests.cpp
  core/HighSwitchFollowTests.cpp
  core/HighIntegerSignednessTests.cpp
  core/HighNarrowLocalsTests.cpp
  core/HighValueForwardTests.cpp
  core/HighCIntegerConversionTests.cpp
  core/HighSSADefinitionTests.cpp
  core/HighSourceRangeTests.cpp
  core/HighFrameTemporaryTests.cpp
  LINK_COMPONENTS
  BackendC
)
target_include_directories(NeverDHighControlFlowTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps ARM64 Mach-O object relocation arithmetic and pair validation focused
# without relinking the aggregate lift suite during local iterations.
add_neverd_unittest(NeverDMachOARM64RelocationTests
  format/MachOARM64RelocationTests.cpp

  LINK_COMPONENTS
  Loader
)

target_include_directories(NeverDMachOARM64RelocationTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps strict embedded-signature parsing and automatic ad-hoc re-sign
# eligibility independently runnable.  The real arm64 fixture is a linker
# generated SHA-256 CodeDirectory and therefore exercises LLVM's Mach-O view
# in addition to the deliberately malformed synthetic containers.
add_neverd_unittest(NeverDMachOSignaturePolicyTests
  format/MachOSignaturePolicyTests.cpp

  LINK_COMPONENTS
  CodeGen
)

target_compile_definitions(NeverDMachOSignaturePolicyTests PRIVATE
  NEVERD_MACHO_SIGNATURE_FIXTURE="${CMAKE_SOURCE_DIR}/unittests/safety/fixtures/binaries/safety_cases_macho_arm64"
)

target_include_directories(NeverDMachOSignaturePolicyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDMachOSignerTests
  format/MachOSignerTests.cpp

  LINK_COMPONENTS
  CodeGen
)

target_include_directories(NeverDMachOSignerTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDPESignaturePolicyTests
  format/PESignaturePolicyTests.cpp

  LINK_COMPONENTS
  CodeGen
)

target_include_directories(NeverDPESignaturePolicyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(NeverDLiftTests PRIVATE
  NeverDBackendC
  NeverDBackendSwift
  NeverDBackendLLVM
  NeverDCodeGen
  NeverDDecode
  NeverDIRLow
  NeverDLibC
  NeverDLoader
  NeverDLift
  NeverDPipeline
  NeverDSupport
  NeverDDebugInfo
)

add_neverd_unittest(NeverDWindowsEHMetadataEncoderTests
  eh/WindowsEHMetadataEncoderTests.cpp

  LINK_COMPONENTS
  WindowsEHMetadata
)

add_neverd_unittest(NeverDNoReturnTests
  core/MedNoReturnTests.cpp
  core/InternalNoReturnTests.cpp

  LINK_COMPONENTS
  IRLow
  IRMed
)

add_neverd_unittest(NeverDRegistrationStateTests
  eh/RegistrationStateTests.cpp
  eh/RegistrationStateTestUtils.cpp
  eh/RegistrationCxxContinuationTests.cpp
  eh/RegistrationRealignedStateTests.cpp

  LINK_COMPONENTS
  IRRegistrationState
)

add_neverd_unittest(NeverDRegistrationEHTests
  eh/RegistrationTryLevelTests.cpp

  LINK_COMPONENTS
  IRLow
  IRMed
  IRHigh
  Pipeline
)

add_neverd_unittest(NeverDWindowsRegistrationFrameTests
  eh/WindowsRegistrationFrameTests.cpp
  eh/WindowsRegistrationCxxFrameTests.cpp

  LINK_COMPONENTS
  WindowsRegistrationFrame
)

add_neverd_unittest(NeverDWindowsRegistrationNativeTests
  eh/WindowsRegistrationNativeTests.cpp
  eh/RegistrationCxxContinuationTestUtils.cpp
  eh/RegistrationCallTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  CodeGen
)

target_include_directories(NeverDWindowsEHMetadataEncoderTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

if(TARGET neverd)
  add_dependencies(NeverDLiftTests neverd)
endif()

# Pins the format-neutral distinction between an explicitly returned semantic
# value producer and an incidental call result left in the native return
# register.  Keeping this focused avoids relinking the full lift suite.
add_neverd_unittest(NeverDLLVMCVoidAnalysisTests
  core/LLVMCVoidAnalysisTests.cpp

  LINK_COMPONENTS
  BackendC
)

add_neverd_unittest(NeverDImageAnalysisBoundaryTests
  core/MedConstantPropagationTests.cpp
  core/MedImmutableTableScanTests.cpp
  format/FunctionDiscoveryAlignmentTests.cpp
  LINK_COMPONENTS IR Loader
)

target_include_directories(NeverDLLVMCVoidAnalysisTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDLLVMCVoidAnalysisTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")
endif()

add_neverd_unittest(NeverDLLVMMemoryAlignmentTests
  core/MedLLVMMemoryAlignmentTests.cpp
  LINK_COMPONENTS BackendLLVM
)

target_include_directories(NeverDLLVMMemoryAlignmentTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDMedMutableSourceTests
  core/MedMutableSourceTests.cpp
  LINK_COMPONENTS Pipeline BackendLLVM BackendC IRMed
)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDMedMutableSourceTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")
endif()

add_neverd_unittest(NeverDLLVMCVectorTests
  core/LLVMCVectorTests.cpp
  LINK_COMPONENTS BackendC
)

target_include_directories(NeverDLLVMCVectorTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDLLVMCVectorTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")
endif()

add_neverd_unittest(NeverDLLVMCPhiTests
  core/LLVMCPhiTests.cpp
  core/LLVMCNonHeaderRegionTests.cpp
  LINK_COMPONENTS BackendC
)

target_include_directories(NeverDLLVMCPhiTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDLLVMCPhiTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")
endif()

add_neverd_unittest(NeverDLLVMCScalarLoopRecoveryTests
  core/LLVMCScalarLoopRecoveryTests.cpp
  LINK_COMPONENTS BackendC
)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDLLVMCScalarLoopRecoveryTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")
endif()

add_neverd_unittest(NeverDAArch64DivisionSemanticTests
  aarch64/AArch64DivisionSemanticTests.cpp
  LINK_COMPONENTS Decode IRLow Support
)
target_include_directories(NeverDAArch64DivisionSemanticTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDCMemoryCopyTests
  core/CMemoryCopyTests.cpp
  LINK_COMPONENTS BackendC
)
target_include_directories(NeverDCMemoryCopyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDCMemoryCopyTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")
endif()

add_neverd_unittest(NeverDLLVMCValueTests
  core/LLVMCValueTests.cpp
  LINK_COMPONENTS BackendC
)

target_include_directories(NeverDLLVMCValueTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDLLVMCValueTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}")
endif()

# Keeps HighIR store-to-load forwarding resource bounds independently runnable
# without relinking the aggregate lift suite during local RED/GREEN iterations.
add_neverd_unittest(NeverDHighCStoreForwardingTests
  core/HighCStoreForwardingTests.cpp

  LINK_COMPONENTS
  BackendC
)

target_include_directories(NeverDHighCStoreForwardingTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDLLVMCFrameMemoryTests
  core/LLVMCFrameMemoryTests.cpp
  LINK_COMPONENTS BackendC
)

target_include_directories(NeverDLLVMCFrameMemoryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps the COFF exception-metadata authentication contract independently
# runnable during RED/GREEN iterations of the patch planner.
add_neverd_unittest(NeverDCOFFExceptionPatchTests
  eh/COFFExceptionPatchTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  CodeGen
  LibC
  Lift
)

target_include_directories(NeverDCOFFExceptionPatchTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

if(UNIX AND NOT APPLE)
  target_link_libraries(NeverDCOFFExceptionPatchTests PRIVATE
    "-Wl,--start-group"
    "$<TARGET_FILE:NeverDIR>"
    "$<TARGET_FILE:NeverDLift>"
    "-Wl,--end-group"
  )
endif()

# Pins the source-EH-to-native-rewrite contract without relinking the large
# lift suite. The emitter creates the source contract, and the format installers
# are the public fail-closed boundary under test.
add_neverd_unittest(NeverDNativeExceptionRewriteContractTests
  eh/DwarfEHFrameTests.cpp
  eh/MachOCompactUnwindPatchTests.cpp
  eh/MachOExceptionPatchTests.cpp
  eh/NativeExceptionRewriteContractTests.cpp
  eh/RewriteFunctionProvenanceTests.cpp

  LINK_COMPONENTS
  BackendC
  BackendLLVM
  CodeGen
  LibC
  Lift
  Pipeline
)

target_include_directories(NeverDNativeExceptionRewriteContractTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# NeverDIR holds function pointers into register-name tables implemented by
# NeverDLift.  CMake may place or deduplicate a repeated NeverDLift before the
# transitive NeverDIR archive, so give one-pass Unix linkers an explicit rescan
# group containing the exact two archive files.
if(UNIX AND NOT APPLE)
  target_link_libraries(NeverDNativeExceptionRewriteContractTests PRIVATE
    "-Wl,--start-group"
    "$<TARGET_FILE:NeverDIR>"
    "$<TARGET_FILE:NeverDLift>"
    "-Wl,--end-group"
  )
endif()

# Keeps Mach-O patch symbol selection independent from the full lift suite.
add_neverd_unittest(NeverDMachOSymbolResolutionTests
  format/MachOSymbolResolutionTests.cpp

  LINK_COMPONENTS
  CodeGen
  Loader
)

target_include_directories(NeverDMachOSymbolResolutionTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/backend/codegen/MachO
  ${LLVM_INCLUDE_DIRS}
)

# Pins the format-native Mach-O name to canonical LLVM-global boundary without
# relinking the full lift suite during local RED/GREEN iterations.  The same TU
# is also listed in NeverDLiftTests above so the aggregate inventory owns it.
add_neverd_unittest(NeverDMachOLLVMNameBoundaryTests
  format/MachOLLVMNameBoundaryTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  LibC
  Lift
)

target_include_directories(NeverDMachOLLVMNameBoundaryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Pins persistent memory-operation callsite identity at the public LLVM
# emitter boundary without relinking the aggregate lift suite during TDD.
add_neverd_unittest(NeverDSafetyCallsiteMetadataTests
  core/SafetyCallsiteMetadataTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  LibC
  Lift
)

target_include_directories(NeverDSafetyCallsiteMetadataTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Pins exact import-veneer identity at the loader-to-LLVM code-pointer
# boundary without relinking the aggregate lift suite during local iterations.
add_neverd_unittest(NeverDLLVMImportVeneerBoundaryTests
  format/COFFImportStorageBoundaryTests.cpp
  format/LLVMImportVeneerBoundaryTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  LibC
  Lift
  Loader
)

target_include_directories(NeverDLLVMImportVeneerBoundaryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Pins Mach-O bind/rebase pointer provenance and its LLVM relocation boundary
# without relinking the full lift suite during local RED/GREEN iterations.
add_neverd_unittest(NeverDMachOPointerRelocationBoundaryTests
  format/MachOPointerRelocationBoundaryTests.cpp
  format/ImmutableChainedValueTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  Decode
  LibC
  Lift
  Loader
  Pipeline
)

target_include_directories(NeverDMachOPointerRelocationBoundaryTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Pins the format-neutral full-pointer relocation contract at both public
# loader and end-to-end LLVM lift boundaries without relinking the aggregate
# lift suite during RED/GREEN iterations.
add_neverd_unittest(NeverDNativePointerRelocationBoundaryTests
  format/COFFRelocatableAbsoluteRelocationTests.cpp
  format/NativePointerRelocationBoundaryTests.cpp

  LINK_COMPONENTS
  Support
  BackendLLVM
)

target_compile_definitions(NeverDNativePointerRelocationBoundaryTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  TEST_OBJ_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(NeverDNativePointerRelocationBoundaryTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_dependencies(NeverDNativePointerRelocationBoundaryTests neverd)

add_neverd_unittest(NeverDMBASourceTests
  core/MBASourceTests.cpp
  LINK_LIBS LLVMSupport
)
target_include_directories(NeverDMBASourceTests PRIVATE ${LLVM_INCLUDE_DIRS})
add_neverd_unittest(NeverDMedStackAlignmentTests
  core/MedStackAlignmentTests.cpp
  LINK_COMPONENTS IRMed
)
target_compile_definitions(NeverDMBASourceTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
add_dependencies(NeverDMBASourceTests neverd)

# These focused targets have the same NeverDIR-to-NeverDLift register-name
# dependency as the native exception contract target above.  Rescan the exact
# two archives on one-pass Unix linkers without changing other platforms.
if(UNIX AND NOT APPLE)
  foreach(_neverd_macho_boundary_target
      NeverDMachOLLVMNameBoundaryTests
      NeverDMachOPointerRelocationBoundaryTests)
    target_link_libraries(${_neverd_macho_boundary_target} PRIVATE
      "-Wl,--start-group"
      "$<TARGET_FILE:NeverDIR>"
      "$<TARGET_FILE:NeverDLift>"
      "-Wl,--end-group"
    )
  endforeach()
endif()

# Exact cells for residual overlapping byte arrays after ordinary SROA.
add_neverd_unittest(NeverDByteCellScalarizationTests
  pipeline/ByteCellScalarizationTests.cpp
  pipeline/ByteCellScalarizationCompileTests.cpp
  LINK_COMPONENTS Pipeline
)
target_compile_definitions(NeverDByteCellScalarizationTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

# Exact overlapping byte accesses before ordinary scalar promotion.
add_neverd_unittest(NeverDByteMemoryForwardingTests
  pipeline/ByteMemoryForwardingTests.cpp
  LINK_COMPONENTS Pipeline
)
target_include_directories(NeverDByteMemoryForwardingTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDByteMemoryForwardingTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  )
endif()

# Guards the obfuscate-then-patch barrier in isolation.  A tiny TU that links
# the prebuilt NeverDPipeline archive (which carries SymSimplifyPass through its
# PassIR component), so the stamp/skip contract can be pinned without rebuilding
# or relinking the ~100-TU NeverDLiftTests binary.
add_neverd_unittest(NeverDSymSimplifyGuardTests
  pipeline/SymSimplifyGuardTests.cpp
  pipeline/SymSimplifyFiniteTests.cpp
  pipeline/SymSimplifyPredicateTests.cpp
  LINK_COMPONENTS
  Pipeline
)
target_include_directories(NeverDSymSimplifyGuardTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/pipeline
  ${LLVM_INCLUDE_DIRS}
)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  target_compile_definitions(NeverDSymSimplifyGuardTests PRIVATE
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  )
endif()

# Pins the recovery against the flattening it undoes.  Both passes reach it
# through the same prebuilt NeverDPipeline archive, so the closed loop costs a
# single small TU rather than a relink of the large lift test binary.
add_neverd_unittest(NeverDControlFlowRecoveryTests
  pipeline/ControlFlowRecoveryTests.cpp
  LINK_COMPONENTS
  Pipeline
)
target_include_directories(NeverDControlFlowRecoveryTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Pins the LLVM emission scheduler's logical C++ EH group boundary without
# relinking the aggregate lift suite during local RED/GREEN iterations.
add_neverd_unittest(NeverDPipelineLLVMShardingTests
  pipeline/PipelineLLVMShardingTests.cpp
  LINK_COMPONENTS
  Pipeline
)
target_include_directories(NeverDPipelineLLVMShardingTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/pipeline
  ${LLVM_INCLUDE_DIRS}
)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(NEVERD_SYNTHETIC_WINDOWS_EH_ROOT
  "${CMAKE_CURRENT_BINARY_DIR}/windows-eh-synthetic")
set(NEVERD_SYNTHETIC_WINDOWS_EH_MANIFEST
  "${NEVERD_SYNTHETIC_WINDOWS_EH_ROOT}/synthetic.json")
add_custom_command(
  OUTPUT "${NEVERD_SYNTHETIC_WINDOWS_EH_MANIFEST}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_windows_eh_corpus.py"
          --output "${NEVERD_SYNTHETIC_WINDOWS_EH_ROOT}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_windows_eh_corpus.py"
  VERBATIM
  COMMENT "Generating synthetic Windows PE architecture corpus")
add_custom_target(NeverDWindowsEHSyntheticCorpus
  DEPENDS "${NEVERD_SYNTHETIC_WINDOWS_EH_MANIFEST}")

add_neverd_unittest(NeverDLanguageEHTests
  language/LanguageEHTests.cpp
  language/LanguageEHDirectBranchTests.cpp
  language/DirectBranchDecodeTests.cpp
  language/LanguageEHItaniumSJLJTests.cpp
  language/LanguageEHItaniumTests.cpp
  language/LanguageEHRuntimeTests.cpp
  language/LanguageEHRustTests.cpp
  language/GoRuntimeEHTests.cpp
  language/GoRuntimeDeferTests.cpp
  language/GoRuntimeMalformedTests.cpp
  language/GoRuntimePclnTabTests.cpp
  language/GoRuntimeStackMapTests.cpp
  language/CompactUnwindTests.cpp
  language/CompactUnwindARM32Tests.cpp
  language/CompactUnwindARM64Tests.cpp
  language/CompactUnwindSectionTests.cpp
  language/ARMEHABITests.cpp
  language/ARMEHABIMalformedTests.cpp
  language/ARMEHABITypeTableTests.cpp
  language/ARMEHABIUnwindOpcodeTests.cpp
  language/ObjCEHTests.cpp
  language/ObjCEHGnuTests.cpp
  language/ObjCEHRuntimeCallTests.cpp
  language/ObjCEHRuntimeStateTests.cpp
  language/ScratchRealBinary.cpp
  language/SymbolSpellingTests.cpp
)
target_include_directories(NeverDLanguageEHTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(NeverDLanguageEHTests PRIVATE
  NeverDLoader
  NeverDSigs
  NeverDSupport
)

add_neverd_unittest(NeverDWindowsEHCorpusManifestTests
  corpus/windows/WindowsEHCorpusManifestTests.cpp
  corpus/windows/WindowsEHCorpusManifest.cpp
)
add_dependencies(NeverDWindowsEHCorpusManifestTests
  NeverDWindowsEHSyntheticCorpus)
target_compile_definitions(NeverDWindowsEHCorpusManifestTests PRIVATE
  NEVERD_SYNTHETIC_WINDOWS_EH_ROOT="${NEVERD_SYNTHETIC_WINDOWS_EH_ROOT}"
)
target_include_directories(NeverDWindowsEHCorpusManifestTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(NeverDWindowsEHCorpusManifestTests PRIVATE
  NeverDLoader
  NeverDSupport
)

add_neverd_unittest(NeverDRustEHCorpusManifestTests
  corpus/rust/RustEHCorpusManifestTests.cpp
  corpus/rust/RustEHCorpusManifest.cpp
)
target_include_directories(NeverDRustEHCorpusManifestTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(NeverDRustEHCorpusManifestTests PRIVATE
  NeverDLoader
  NeverDSupport
)

add_neverd_unittest(NeverDGoEHCorpusManifestTests
  corpus/go/GoEHCorpusManifestTests.cpp
  corpus/go/GoEHCorpusManifest.cpp
)
target_include_directories(NeverDGoEHCorpusManifestTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(NeverDGoEHCorpusManifestTests PRIVATE
  NeverDLoader
  NeverDSupport
)

add_neverd_unittest(NeverDAdaDEHCorpusManifestTests
  corpus/ada-d-eh/AdaDEHCorpusManifestTests.cpp
  corpus/ada-d-eh/AdaDEHCorpusManifest.cpp
)
target_include_directories(NeverDAdaDEHCorpusManifestTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(NeverDAdaDEHCorpusManifestTests PRIVATE
  NeverDLoader
  NeverDSupport
)

add_neverd_unittest(NeverDCxxItaniumEHCorpusManifestTests
  corpus/cxx/CxxItaniumEHCorpusManifestTests.cpp
  corpus/cxx/CxxItaniumEHCorpusManifest.cpp
)
target_include_directories(NeverDCxxItaniumEHCorpusManifestTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(NeverDCxxItaniumEHCorpusManifestTests PRIVATE
  NeverDLoader
  NeverDSupport
)

# Each corpus line is published by its own CI workflow, so the root probes them
# separately and a line the submodule does not yet carry simply has no target.
# The manifest parsers above stay unconditional: they read synthetic strings and
# are the only thing that can still be tested before a corpus exists.
if(NEVERD_HAVE_WINDOWS_EH_CORPUS)
  add_neverd_unittest(NeverDWindowsEHCorpusTests
    corpus/windows/WindowsEHCorpusTests.cpp
    corpus/windows/WindowsEHModesCorpusTests.cpp
    corpus/windows/WindowsEHCorpusManifest.cpp
    TIMEOUT 300

    LINK_COMPONENTS
    BackendLLVM
    CodeGen
    Decode
    IRLow
    LibC
    Lift
  )

  target_compile_definitions(NeverDWindowsEHCorpusTests PRIVATE
    NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}"
    NEVERD_BINARY="$<TARGET_FILE:neverd>"
    NEVERD_CMAKE_COMMAND="${CMAKE_COMMAND}"
  )

  target_include_directories(NeverDWindowsEHCorpusTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDWindowsEHCorpusTests PRIVATE
    NeverDLoader
    NeverDSupport
    NeverDWindowsEHMetadata
  )

  add_dependencies(NeverDWindowsEHCorpusTests neverd)
endif()

if(NEVERD_HAVE_RUST_EH_CORPUS)
  # A Rust `bin` statically links the standard library, so every artifact
  # carries the whole of `std`'s Itanium tables alongside the probe's.
  add_neverd_unittest(NeverDRustEHCorpusTests
    corpus/rust/RustEHCorpusTests.cpp
    corpus/rust/RustEHCorpusManifest.cpp
    TIMEOUT 600
  )

  target_compile_definitions(NeverDRustEHCorpusTests PRIVATE
    NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}"
  )

  target_include_directories(NeverDRustEHCorpusTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDRustEHCorpusTests PRIVATE
    NeverDLoader
    NeverDSupport
  )
endif()

if(NEVERD_HAVE_GO_EH_CORPUS)
  # Every Go image links the whole runtime, so decoding one means walking a
  # `pclntab` of a few thousand functions and the funcdata behind each of them.
  add_neverd_unittest(NeverDGoEHCorpusTests
    corpus/go/GoEHCorpusTests.cpp
    corpus/go/GoEHCorpusManifest.cpp
    TIMEOUT 600
  )

  target_compile_definitions(NeverDGoEHCorpusTests PRIVATE
    NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}"
  )

  target_include_directories(NeverDGoEHCorpusTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDGoEHCorpusTests PRIVATE
    NeverDLoader
    NeverDSupport
  )
endif()

if(NEVERD_HAVE_CXX_ITANIUM_EH_CORPUS)
  # Nine toolchain cells of eight variants, and a mingw image statically links
  # the whole C runtime, so this reads considerably more table than it emits.
  add_neverd_unittest(NeverDCxxItaniumEHCorpusTests
    corpus/cxx/CxxItaniumEHCorpusTests.cpp
    corpus/cxx/CxxItaniumEHCorpusGraphTests.cpp
    corpus/cxx/CxxItaniumEHCorpusManifest.cpp
    TIMEOUT 600
  )

  target_compile_definitions(NeverDCxxItaniumEHCorpusTests PRIVATE
    NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}"
    NEVERD_BINARY="$<TARGET_FILE:neverd>"
  )

  target_include_directories(NeverDCxxItaniumEHCorpusTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDCxxItaniumEHCorpusTests PRIVATE
    NeverDLoader
    NeverDSupport
  )
  add_dependencies(NeverDCxxItaniumEHCorpusTests neverd)
endif()

if(NEVERD_HAVE_ADA_D_EH_CORPUS)
  add_neverd_unittest(NeverDAdaDEHCorpusTests
    corpus/ada-d-eh/AdaDEHCorpusTests.cpp
    corpus/ada-d-eh/AdaDEHCorpusManifest.cpp
    TIMEOUT 600
  )

  target_compile_definitions(NeverDAdaDEHCorpusTests PRIVATE
    NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}"
  )

  target_include_directories(NeverDAdaDEHCorpusTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDAdaDEHCorpusTests PRIVATE
    NeverDLoader
    NeverDSupport
  )
endif()

if(NEVERD_HAVE_OBJC_EH_CORPUS)
  add_neverd_unittest(NeverDObjCEHCorpusTests
    corpus/objc/ObjCEHCorpusTests.cpp
    TIMEOUT 600
  )

  target_compile_definitions(NeverDObjCEHCorpusTests PRIVATE
    NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}"
    NEVERD_BINARY="$<TARGET_FILE:neverd>"
  )

  target_include_directories(NeverDObjCEHCorpusTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDObjCEHCorpusTests PRIVATE
    NeverDLoader
    NeverDSupport
  )
  add_dependencies(NeverDObjCEHCorpusTests neverd)
endif()

# This one exercises the C API through the shared library, so it exists only
# where that library does.  Naming a missing target in add_dependencies is a
# configure-time error rather than a link-time one, so an unguarded reference
# takes down every other test in the build as well.
if(TARGET neverd_shared)
  add_neverd_unittest(NeverDMetadataJSONTests
    sdk/MetadataJSONTests.cpp
    sdk/SwiftNominalContextCAPITests.cpp
    sdk/ELFARM32ModeCAPITests.cpp
  )
  target_compile_definitions(NeverDMetadataJSONTests PRIVATE
    NEVERD_BINARY="$<TARGET_FILE:neverd>"
    NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
    TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )

  target_include_directories(NeverDMetadataJSONTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDMetadataJSONTests PRIVATE
    neverd_shared
    LLVMSupport
  )

  add_dependencies(NeverDMetadataJSONTests neverd_shared)

  add_neverd_unittest(NeverDSemanticCAPITests
    sdk/SemanticCAPIHeaderC.c
    sdk/SemanticCAPITests.cpp
  )
  if(MSVC)
    # File-scope _Static_assert is C11.  MSVC compiles .c as C89 unless told.
    set_source_files_properties(sdk/SemanticCAPIHeaderC.c PROPERTIES
      COMPILE_OPTIONS /std:c11)
  endif()

  target_include_directories(NeverDSemanticCAPITests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDSemanticCAPITests PRIVATE
    neverd_shared
    LLVMSupport
  )

  add_dependencies(NeverDSemanticCAPITests neverd_shared)

  add_neverd_unittest(NeverDSafetyPatchCAPITests
    sdk/SafetyPatchCAPIHeaderC.c
    sdk/SafetyPatchCAPITests.cpp
  )
  if(MSVC)
    set_source_files_properties(sdk/SafetyPatchCAPIHeaderC.c PROPERTIES
      COMPILE_OPTIONS /std:c11)
  endif()

  target_include_directories(NeverDSafetyPatchCAPITests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/sdk/capi
    ${LLVM_INCLUDE_DIRS}
  )

  target_compile_definitions(NeverDSafetyPatchCAPITests PRIVATE
    NEVERD_SANITIZE_FIXTURE_ROOT="${CMAKE_SOURCE_DIR}/unittests/concolic/fixtures/binaries"
    NEVERD_SAFETY_FIXTURE_ROOT="${CMAKE_SOURCE_DIR}/unittests/safety/fixtures/binaries"
    NEVERD_SANITIZE_RUNTIME_SOURCE="${CMAKE_SOURCE_DIR}/unittests/safety/fixtures/runtime_sanitizer_cli.c"
    NEVERD_SANITIZE_EMPTY_SOURCE="${CMAKE_SOURCE_DIR}/unittests/safety/fixtures/runtime_sanitizer_empty.c"
    NEVERD_RUNTIME_FIXTURE_COMPILER="${NEVERD_HOST_RUNTIME_FIXTURE_COMPILER_EXECUTABLE}"
  )

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(sdk/SafetyPatchCAPIHeaderC.c PROPERTIES
      COMPILE_OPTIONS "-fshort-enums"
    )
  endif()

  target_link_libraries(NeverDSafetyPatchCAPITests PRIVATE
    neverd_shared
    LLVMSupport
  )

  add_dependencies(NeverDSafetyPatchCAPITests neverd_shared)
endif()

# --- Cross-compile test .c sources into .o objects at build time ---
set(CLANG_EXEC "${NEVERD_TEST_CLANG_EXECUTABLE}")
if(NOT CLANG_EXEC)
  message(FATAL_ERROR
    "Building NeverD cross-target test fixtures requires a GNU-style Clang "
    "driver. Install clang or provide the bundled LLVM tools before enabling "
    "the test suite.")
endif()

set(X86_64_TEST_SOURCES
  x86_64/test_jumptable_narrow_guard_x64.s
  x86_64/test_arith.c
  x86_64/test_muldiv.c
  x86_64/test_control_flow.c
  x86_64/test_backward_entry.s
  x86_64/test_extension_widths.s
  x86_64/test_stack_mov.c
  x86_64/test_sse_fp.c
  x86_64/test_carry_ops.c
  x86_64/test_bitops.c
  x86_64/test_setcc_cmov.c
  x86_64/test_cmpxchg_xadd.c
  x86_64/test_signext.c
  x86_64/test_string_ops.c
  x86_64/test_rotate_shift.c
  x86_64/test_misc.c
  x86_64/test_semantic_e2e.c
  x86_64/test_control_semantics.c
  x86_64/test_bmi.c
  x86_64/test_adx_misc.c
  x86_64/test_sse_avx.c
  x86_64/test_flag_ops.c
  x86_64/test_mem_atomic.c
  x86_64/test_double_shift.c
  x86_64/test_flags_misc.c
  x86_64/test_fp_convert.c
  x86_64/test_rcl_rcr.c
  x86_64/test_sign_conv.c
  x86_64/test_loop.c
  x86_64/test_fma_misc.c
  x86_64/test_x87_fpu.c
  x86_64/test_lods_misc.c
  x86_64/test_sse_misc.c
  x86_64/test_intrinsics_system.c
  x86_64/test_switch.c
  x86_64/test_roundtrip.c
  x86_64/test_debug_records.c
  x86_64/test_debug_params.c
  x86_64/test_jumptable_enhanced.c
  x86_64/test_jumptable_inclusive_postload_clobber.s
  x86_64/test_jumptable_inclusive_postload_unrelated.s
  x86_64/test_jumptable_inclusive_preload_alias.s
  x86_64/test_jumptable_inclusive_preload_reuse.s
  x86_64/test_jumptable_identity_cfg_lane.s
  x86_64/test_jumptable_modulo_domain.s
  x86_64/test_jumptable_relative_identity.s
  x86_64/test_jumptable_modulo_dead_producer.s
  x86_64/test_jumptable_proposal_lfp.s
  x86_64/test_jumptable_singleton_closed_world.s
  x86_64/test_jumptable_raw_relative_domain.s
  x86_64/test_jumptable_lost_published.s
  x86_64/test_jumptable_target_ownership.s
  x86_64/test_jumptable_selector_occurrence.s
  x86_64/test_jumptable_mask_equal_bound.s
  x86_64/test_jumptable_field_reload.s
  x86_64/test_module_address_owner_induction.s
  x86_64/test_module_readonly_table_writer.s
  x86_64/test_extern_calls.c
  x86_64/test_extern_calls_nopic.c
  x86_64/test_return_join.c
  x86_64/test_twolevel_switch.c
  x86_64/test_jumptable_masked_abs.c
  x86_64/test_jumptable_worldclass.c
  x86_64/test_jumptable_cgoto.c
  x86_64/test_jumptable_stack_memcpy.s
  x86_64/test_global_volatile.c
)

set(X86_32_TEST_SOURCES
  x86_32/test_jumptable_narrow_guard_i386.s
  x86_32/test_extern_calls_i386.c
  x86_32/test_extern_calls_nopic_i386.c
  x86_32/test_return_join_i386.c
  x86_32/test_i386_direct_gotoff_group.s
  x86_32/test_i386_gotpc_model.s
  x86_32/test_i386_gotoff_forcepeel.s
  x86_32/test_i386_gotoff_stack_group.s
  x86_32/test_i386_relocation_writer_footprint.s
  x86_32/test_jumptable_frame_lane32.s
  x86_32/test_arith32.c
  x86_32/test_control32.c
  x86_32/test_bitops32.c
  x86_32/test_signext32.c
  x86_32/test_muldiv32.c
  x86_32/test_stack32.c
  x86_32/test_flags32.c
  x86_32/test_misc32.c
  x86_32/test_fp32.c
  x86_32/test_cmov32.c
  x86_32/test_rotate32.c
  x86_32/test_carry32.c
  x86_32/test_setcc32.c
  x86_32/test_string32.c
  x86_32/test_cmpxchg32.c
  x86_32/test_loop32.c
  x86_32/test_dshift32.c
  x86_32/test_x87_fpu32.c
  x86_32/test_lods_misc32.c
)

set(AARCH64_TEST_SOURCES
  aarch64/test_jumptable_narrow_guard_a64.s
  aarch64/test_extern_calls_a64.c
  aarch64/test_extern_calls_nopic_a64.c
  aarch64/test_return_join_a64.c
  aarch64/test_param_phi_a64.s
  aarch64/test_jumptable_index_identity_a64.s
  aarch64/test_pageoff_wrong_base_a64.s
  aarch64/test_pageoff_reaching_a64.s
  aarch64/test_pageoff_alternatives_a64.s
  aarch64/test_pageoff_nominal_miss_a64.s
  aarch64/test_jumptable_width_mismatch_a64.s
  aarch64/test_arith_a64.c
  aarch64/test_cond_a64.c
  aarch64/test_hbc_a64.c
  aarch64/test_mem_a64.c
  aarch64/test_bitshift_a64.c
  aarch64/test_fp_a64.c
  aarch64/test_logic_a64.c
  aarch64/test_muldiv_a64.c
  aarch64/test_csel_a64.c
  aarch64/test_carry_a64.c
  aarch64/test_extend_a64.c
  aarch64/test_faminmax_a64.c
  aarch64/test_atomic_a64.c
  aarch64/test_lse_builtin_a64.c
  aarch64/test_mops_a64.c
  aarch64/test_mte_a64.c
  aarch64/test_pauth_a64.c
  aarch64/test_ret_a64.c
  aarch64/test_sve_length_a64.c
  aarch64/test_sve_state_a64.c
  aarch64/test_sve_store_a64.c
  aarch64/test_wsp_a64.c
  aarch64/test_intrinsics_a64.c
  aarch64/test_switch_a64.c
  aarch64/test_roundtrip_a64.c
  aarch64/test_jumptable_enhanced_a64.c
  aarch64/test_jumptable_worldclass_a64.c
)

set(ARM32_TEST_SOURCES
  arm32/test_extern_calls_arm.c
  arm32/test_extern_calls_nopic_arm.c
  arm32/test_return_join_arm.c
  arm32/test_predicated_pop_arm.s
  arm32/test_inline_state_domain_arm.s
  arm32/test_frame_pointer_void_arm.s
  arm32/test_arm_rel32_reaching.s
  arm32/test_arm_rel32_multiple_sources.s
  arm32/test_module_predicated_escape_arm.s
  format/test_elf_pointer_reloc_arm32.s
  arm32/test_arith_arm.c
  arm32/test_mem_arm.c
  arm32/test_control_arm.c
  arm32/test_logic_arm.c
  arm32/test_muldiv_arm.c
  arm32/test_carry_arm.c
  arm32/test_extend_arm.c
  arm32/test_shift_arm.c
  arm32/test_intrinsics_arm.c
  arm32/test_switch_arm.c
  arm32/test_roundtrip_arm.c
  arm32/test_jumptable_enhanced_arm.c
  arm32/test_jumptable_worldclass_arm.c
)

set(TEST_OBJECTS "")
set(COFF_NATIVE_EH_TEST_OBJECTS "")

# TBB/TBH are Thumb-2-only.  Build the focused identity fixture as ARMNT
# COFF so the loader has an unambiguous image-wide Thumb mode; ELF relocatable
# symbols do not by themselves establish a uniform execution mode.
set(thumb_jt_identity_src
    "${CMAKE_CURRENT_SOURCE_DIR}/arm32/test_jumptable_index_identity_arm.s")
set(thumb_jt_identity_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_jumptable_index_identity_arm.obj")
add_custom_command(
  OUTPUT ${thumb_jt_identity_obj}
  COMMAND ${CLANG_EXEC} -target thumbv7-windows-msvc -march=armv7-a
          -nostdlib -c ${thumb_jt_identity_src} -o ${thumb_jt_identity_obj}
  DEPENDS ${thumb_jt_identity_src}
  COMMENT "Compiling Thumb-2 jump-table identity fixture (ARMNT COFF)")
list(APPEND TEST_OBJECTS ${thumb_jt_identity_obj})

set(thumb_jt_incomplete_domain_src
    "${CMAKE_CURRENT_SOURCE_DIR}/arm32/test_jumptable_incomplete_domain_arm.s")
set(thumb_jt_incomplete_domain_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_jumptable_incomplete_domain_arm.obj")
add_custom_command(
  OUTPUT ${thumb_jt_incomplete_domain_obj}
  COMMAND ${CLANG_EXEC} -target thumbv7-windows-msvc -march=armv7-a
          -nostdlib -c ${thumb_jt_incomplete_domain_src}
          -o ${thumb_jt_incomplete_domain_obj}
  DEPENDS ${thumb_jt_incomplete_domain_src}
  COMMENT "Compiling Thumb-2 incomplete-domain fixture (ARMNT COFF)")
list(APPEND TEST_OBJECTS ${thumb_jt_incomplete_domain_obj})

# One source of undefined functions and data, an import library's function
# pointer and a common symbol, compiled as an x64, x86 and ARM64 COFF object.
set(coff_externs_src "${CMAKE_CURRENT_SOURCE_DIR}/format/test_coff_externs.c")
foreach(coff_target "x86_64-pc-windows-msvc" "i686-pc-windows-msvc"
                    "aarch64-pc-windows-msvc")
  string(REGEX MATCH "^[a-z0-9_]+" coff_machine "${coff_target}")
  set(coff_externs_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_coff_externs_${coff_machine}.obj")
  add_custom_command(
    OUTPUT ${coff_externs_obj}
    COMMAND ${CLANG_EXEC} -target ${coff_target} -O2 -fcommon
            -c ${coff_externs_src} -o ${coff_externs_obj}
    DEPENDS ${coff_externs_src}
    COMMENT "Compiling COFF extern fixture for ${coff_target}")
  list(APPEND TEST_OBJECTS ${coff_externs_obj})
endforeach()

# The ELF extern fixture as Mach-O objects, which reach data and some calls
# through GOT entries.
set(macho_externs_src "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/test_extern_calls.c")
foreach(macho_target "x86_64-apple-macos11" "arm64-apple-macos11")
  string(REGEX MATCH "^[a-z0-9_]+" macho_arch "${macho_target}")
  set(macho_externs_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_extern_calls_macho_${macho_arch}.o")
  add_custom_command(
    OUTPUT ${macho_externs_obj}
    COMMAND ${CLANG_EXEC} -target ${macho_target} -O2 -fcommon -c
            ${macho_externs_src} -o ${macho_externs_obj}
    DEPENDS ${macho_externs_src}
    COMMENT "Compiling Mach-O extern fixture for ${macho_target}")
  list(APPEND TEST_OBJECTS ${macho_externs_obj})
endforeach()

set(macho_i386_src
    "${CMAKE_CURRENT_SOURCE_DIR}/format/test_macho_i386.c")
set(macho_i386_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_macho_i386.o")
set(macho_i386_nopic_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_macho_i386_nopic.o")
add_custom_command(
  OUTPUT ${macho_i386_obj}
  COMMAND ${CLANG_EXEC} -target i386-apple-macos10.6 -O0 -ffreestanding
          -fno-stack-protector -c ${macho_i386_src} -o ${macho_i386_obj}
  DEPENDS ${macho_i386_src}
  COMMENT "Compiling test_macho_i386.c -> .o (PIC i386 Mach-O)")
add_custom_command(
  OUTPUT ${macho_i386_nopic_obj}
  COMMAND ${CLANG_EXEC} -target i386-apple-macos10.6 -O0 -ffreestanding
          -fno-stack-protector -fno-pic -c ${macho_i386_src}
          -o ${macho_i386_nopic_obj}
  DEPENDS ${macho_i386_src}
  COMMENT "Compiling test_macho_i386.c -> .o (no-PIC i386 Mach-O)")
list(APPEND TEST_OBJECTS ${macho_i386_obj} ${macho_i386_nopic_obj})

if(APPLE)
  set(call_abi_a64_macho_src
      "${CMAKE_CURRENT_SOURCE_DIR}/aarch64/test_call_abi_a64_macho.c")
  set(call_abi_a64_macho_provider_src
      "${CMAKE_CURRENT_SOURCE_DIR}/aarch64/test_call_abi_provider_a64_macho.c")
  set(call_abi_a64_macho_dylib
      "${CMAKE_CURRENT_BINARY_DIR}/libtest_call_abi_a64_macho.dylib")
  set(call_abi_a64_macho_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_call_abi_a64_macho")
  set(call_arg_phi_a64_macho_src
      "${CMAKE_CURRENT_SOURCE_DIR}/aarch64/test_call_arg_phi_a64_macho.s")
  set(call_arg_phi_a64_macho_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_call_arg_phi_a64_macho")
  set(reentrant_feasible_phi_a64_macho_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_reentrant_feasible_phi_a64_macho.s")
  set(reentrant_feasible_phi_a64_macho_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_reentrant_feasible_phi_a64_macho")
  set(bounded_mixed_pointer_table_a64_macho_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_bounded_mixed_pointer_table_a64_macho.s")
  set(bounded_mixed_pointer_table_a64_macho_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_bounded_mixed_pointer_table_a64_macho")
  set(scalar_index_cycle_a64_macho_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_scalar_index_cycle_a64_macho.s")
  set(scalar_index_cycle_a64_macho_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_scalar_index_cycle_a64_macho")
  set(writable_pointer_induction_a64_macho_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_writable_pointer_table_induction_a64_macho.s")
  set(writable_pointer_induction_a64_macho_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_a64_macho")
  set(writable_pointer_induction_x64_macho_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_writable_pointer_table_induction_x64_macho.s")
  set(writable_pointer_induction_x64_macho_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_x64_macho")
  add_custom_command(
    OUTPUT ${call_abi_a64_macho_dylib}
    COMMAND /usr/bin/clang -target arm64-apple-macos14 -dynamiclib
            -fno-stack-protector ${call_abi_a64_macho_provider_src}
            -Wl,-install_name,@rpath/libtest_call_abi_a64_macho.dylib
            -o ${call_abi_a64_macho_dylib}
    DEPENDS ${call_abi_a64_macho_provider_src}
    COMMENT "Linking AArch64 Mach-O call-ABI provider dylib")
  add_custom_command(
    OUTPUT ${call_abi_a64_macho_exe}
    COMMAND /usr/bin/clang -target arm64-apple-macos14
            -fno-stack-protector ${call_abi_a64_macho_src}
            ${call_abi_a64_macho_dylib} -Wl,-e,_indirect_double_call
            -o ${call_abi_a64_macho_exe}
    DEPENDS ${call_abi_a64_macho_src} ${call_abi_a64_macho_dylib}
    COMMENT "Linking AArch64 Mach-O call-ABI fixture")
  add_custom_command(
    OUTPUT ${call_arg_phi_a64_macho_exe}
    COMMAND /usr/bin/clang -target arm64-apple-macos14
            ${call_arg_phi_a64_macho_src} -o ${call_arg_phi_a64_macho_exe}
    DEPENDS ${call_arg_phi_a64_macho_src}
    COMMENT "Linking AArch64 Mach-O call-argument PHI fixture")
  add_custom_command(
    OUTPUT ${reentrant_feasible_phi_a64_macho_exe}
    COMMAND /usr/bin/clang -target arm64-apple-macos14
            ${reentrant_feasible_phi_a64_macho_src}
            -o ${reentrant_feasible_phi_a64_macho_exe}
    DEPENDS ${reentrant_feasible_phi_a64_macho_src}
    COMMENT "Linking AArch64 Mach-O reentrant feasible-edge PHI fixture")
  add_custom_command(
    OUTPUT ${bounded_mixed_pointer_table_a64_macho_exe}
    COMMAND /usr/bin/clang -target arm64-apple-macos14
            ${bounded_mixed_pointer_table_a64_macho_src}
            -o ${bounded_mixed_pointer_table_a64_macho_exe}
    DEPENDS ${bounded_mixed_pointer_table_a64_macho_src}
    COMMENT "Linking AArch64 Mach-O bounded mixed pointer-table fixture")
  add_custom_command(
    OUTPUT ${scalar_index_cycle_a64_macho_exe}
    COMMAND /usr/bin/clang -target arm64-apple-macos14
            ${scalar_index_cycle_a64_macho_src}
            -o ${scalar_index_cycle_a64_macho_exe}
    DEPENDS ${scalar_index_cycle_a64_macho_src}
    COMMENT "Linking AArch64 Mach-O scalar index-cycle table fixture")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_a64_macho_exe}
    COMMAND /usr/bin/clang -target arm64-apple-macos14
            ${writable_pointer_induction_a64_macho_src}
            -o ${writable_pointer_induction_a64_macho_exe}
    DEPENDS ${writable_pointer_induction_a64_macho_src}
    COMMENT "Linking AArch64 Mach-O writable pointer-table induction fixture")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_x64_macho_exe}
    COMMAND /usr/bin/clang -target x86_64-apple-macos14
            ${writable_pointer_induction_x64_macho_src}
            -o ${writable_pointer_induction_x64_macho_exe}
    DEPENDS ${writable_pointer_induction_x64_macho_src}
    COMMENT "Linking x64 Mach-O writable pointer-table induction fixture")
  list(APPEND TEST_OBJECTS ${call_abi_a64_macho_exe}
                           ${call_arg_phi_a64_macho_exe}
                           ${reentrant_feasible_phi_a64_macho_exe}
                           ${bounded_mixed_pointer_table_a64_macho_exe}
                           ${scalar_index_cycle_a64_macho_exe}
                           ${writable_pointer_induction_a64_macho_exe}
                           ${writable_pointer_induction_x64_macho_exe})
endif()

# A fixture for another architecture can include the x86-64 source of the
# same name; it is rebuilt when that source changes.
function(neverd_shared_fixture_source src out)
  set(shared)
  foreach(name test_extern_calls test_return_join)
    if(src MATCHES "/${name}_")
      set(shared ${CMAKE_CURRENT_SOURCE_DIR}/x86_64/${name}.c)
    endif()
  endforeach()
  set(${out} ${shared} PARENT_SCOPE)
endfunction()

foreach(src ${X86_64_TEST_SOURCES})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(extra_flags)
  if(src MATCHES "test_jumptable_cgoto")
    # -O0 keeps the computed-goto label table decoupled from the indirect branch
    # by a spill/reload relay (the shared multi-site dispatch the const-base
    # absolute resolver recovers); -O1 would inline the load into the branch.
    set(extra_flags -O0)
  elseif(src MATCHES "test_switch|test_jumptable")
    set(extra_flags -O1)
  elseif(src MATCHES "test_bmi")
    set(extra_flags -mbmi -mbmi2 -mlzcnt -mpopcnt -mcrc32)
  elseif(src MATCHES "test_adx")
    set(extra_flags -madx -mbmi -mlzcnt -mpopcnt -mmovbe)
  elseif(src MATCHES "test_sse_avx")
    set(extra_flags -msse4.2 -mavx)
  elseif(src MATCHES "test_flags_misc")
    set(extra_flags -mbmi -mlzcnt -mpopcnt)
  elseif(src MATCHES "test_debug_records|test_debug_params")
    # The records and signatures under test come from the DWARF description.
    set(extra_flags -g)
  elseif(src MATCHES "test_return_join")
    set(extra_flags -O2)
  elseif(src MATCHES "test_extern_calls_nopic")
    set(extra_flags -O2 -fno-pic -fcommon)
  elseif(src MATCHES "test_extern_calls")
    # Position-independent, so its data references go through the GOT.
    set(extra_flags -O2 -fPIC -fcommon)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target x86_64-linux-gnu -nostdlib -static
            -O0 -fno-stack-protector -fno-pie ${extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (x86_64 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

foreach(src ${X86_32_TEST_SOURCES})
  neverd_shared_fixture_source(${src} shared_src)
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(x86_32_extra_flags)
  if(src MATCHES "test_fp32")
    set(x86_32_extra_flags -msse2)
  elseif(src MATCHES "test_return_join")
    set(x86_32_extra_flags -O2)
  elseif(src MATCHES "test_extern_calls_nopic")
    set(x86_32_extra_flags -O2 -fno-pic -fcommon)
  elseif(src MATCHES "test_extern_calls")
    set(x86_32_extra_flags -O2 -fPIC -fcommon)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target i386-linux-gnu -m32 -nostdlib -static
            -O0 -fno-stack-protector -fno-pie ${x86_32_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${shared_src}
    COMMENT "Compiling ${src} -> ${base}.o (i386 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

set(stack_cgoto_i386_pic_src
    "${CMAKE_CURRENT_SOURCE_DIR}/x86_32/test_jumptable_stack_loc8_pic.c")
set(stack_cgoto_i386_pic_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_jumptable_stack_loc8_pic.o")
add_custom_command(
  OUTPUT ${stack_cgoto_i386_pic_obj}
  COMMAND ${CLANG_EXEC} -target i386-linux-gnu -nostdlib -c -O0
          -fno-stack-protector -fno-exceptions -fno-unwind-tables
          -fno-asynchronous-unwind-tables -march=pentium4
          ${stack_cgoto_i386_pic_src} -o ${stack_cgoto_i386_pic_obj}
  DEPENDS ${stack_cgoto_i386_pic_src}
  COMMENT "Compiling PIC i386 stack-materialized jump-table fixture")
list(APPEND TEST_OBJECTS ${stack_cgoto_i386_pic_obj})

# Keep exact adjacent table ownership probes independent of compiler loop
# peeling: two five-entry GOTOFF runs and two dispatches per run.
set(i386_adjacent_twotable_src
    "${CMAKE_CURRENT_SOURCE_DIR}/x86_32/test_i386_adjacent_gotoff.S")
set(i386_adjacent_twotable_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_i386_adjacent_twotable.o")
add_custom_command(
  OUTPUT ${i386_adjacent_twotable_obj}
  COMMAND ${CLANG_EXEC} -target i386-linux-gnu -nostdlib -nostdlibinc -c -O2
          -fno-stack-protector -fno-exceptions -fno-unwind-tables
          -fno-asynchronous-unwind-tables -march=pentium4
          ${i386_adjacent_twotable_src} -o ${i386_adjacent_twotable_obj}
  DEPENDS ${i386_adjacent_twotable_src}
  COMMENT "Assembling adjacent i386 GOTOFF tables")
list(APPEND TEST_OBJECTS ${i386_adjacent_twotable_obj})

# O0 uses an EBP-relative GOT spill across three independent switch loops.
set(i386_three_switch_loops_src
    "${CMAKE_CURRENT_SOURCE_DIR}/x86_32/test_i386_three_switch_loops.c")
set(i386_three_switch_loops_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_i386_three_switch_loops.o")
add_custom_command(
  OUTPUT ${i386_three_switch_loops_obj}
  COMMAND ${CLANG_EXEC} -target i386-linux-gnu -nostdlib -nostdlibinc -c -O0
          -fno-stack-protector -fno-exceptions -fno-unwind-tables
          -fno-asynchronous-unwind-tables -march=pentium4
          ${i386_three_switch_loops_src} -o ${i386_three_switch_loops_obj}
  DEPENDS ${i386_three_switch_loops_src}
  COMMENT "Compiling O0 i386 PIC switch loops with a shared GOT spill")
list(APPEND TEST_OBJECTS ${i386_three_switch_loops_obj})

# A third adjacent run must join the same all-or-nothing ownership proof.
set(i386_three_switch_loops_o2_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_i386_three_switch_loops_o2.o")
add_custom_command(
  OUTPUT ${i386_three_switch_loops_o2_obj}
  COMMAND ${CLANG_EXEC} -target i386-linux-gnu -nostdlib -nostdlibinc -c -O2
          -fno-stack-protector -fno-exceptions -fno-unwind-tables
          -fno-asynchronous-unwind-tables -march=pentium4
          -DNEVERD_GOTOFF_THREE_RUNS=1
          ${i386_adjacent_twotable_src} -o ${i386_three_switch_loops_o2_obj}
  DEPENDS ${i386_adjacent_twotable_src}
  COMMENT "Assembling three-run i386 GOTOFF tables")
list(APPEND TEST_OBJECTS ${i386_three_switch_loops_o2_obj})

set(i386_adjacent_function_pointer_src
    "${CMAKE_CURRENT_SOURCE_DIR}/x86_32/test_i386_adjacent_function_pointer_runs.S")
set(i386_adjacent_function_pointer_obj
    "${CMAKE_CURRENT_BINARY_DIR}/test_i386_adjacent_function_pointer_runs.o")
add_custom_command(
  OUTPUT ${i386_adjacent_function_pointer_obj}
  COMMAND ${CLANG_EXEC} -target i386-linux-gnu -nostdlib -nostdlibinc -c
          ${i386_adjacent_function_pointer_src}
          -o ${i386_adjacent_function_pointer_obj}
  DEPENDS ${i386_adjacent_function_pointer_src}
  COMMENT "Compiling adjacent i386 GOT-relative function-pointer runs")
list(APPEND TEST_OBJECTS ${i386_adjacent_function_pointer_obj})

foreach(src ${AARCH64_TEST_SOURCES})
  neverd_shared_fixture_source(${src} shared_src)
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(a64_extra_flags)
  if(src MATCHES "test_jumptable_index_identity_a64")
    set(a64_extra_flags -O1 -march=armv8.1-a+lse)
  elseif(src MATCHES "test_switch|test_jumptable")
    set(a64_extra_flags -O1)
  elseif(src MATCHES "test_fp_a64")
    set(a64_extra_flags -march=armv8.6-a+bf16)
  elseif(src MATCHES "test_hbc_a64")
    set(a64_extra_flags -march=armv8.8-a+hbc)
  elseif(src MATCHES "test_mem_a64")
    set(a64_extra_flags -march=armv8.4-a)
  elseif(src MATCHES "test_faminmax_a64")
    set(a64_extra_flags -march=armv9.4-a+faminmax)
  elseif(src MATCHES "test_atomic_a64")
    set(a64_extra_flags -march=armv9.4-a+lse128+the+d128+rcpc3)
  elseif(src MATCHES "test_lse_builtin_a64")
    set(a64_extra_flags -march=armv8.1-a+lse)
  elseif(src MATCHES "test_mops_a64")
    set(a64_extra_flags -march=armv8.8-a+mops)
  elseif(src MATCHES "test_mte_a64")
    set(a64_extra_flags -march=armv8.5-a+memtag)
  elseif(src MATCHES "test_pauth_a64")
    set(a64_extra_flags -march=armv8.3-a+pauth)
  elseif(src MATCHES "test_sve_(length|state|store)_a64")
    set(a64_extra_flags -march=armv8.2-a+sve -msve-vector-bits=256)
  elseif(src MATCHES "test_return_join")
    set(a64_extra_flags -O2)
  elseif(src MATCHES "test_extern_calls_nopic")
    set(a64_extra_flags -O2 -fno-pic -fcommon)
  elseif(src MATCHES "test_extern_calls")
    set(a64_extra_flags -O2 -fPIC -fcommon)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -nostdlib -static
            -O0 -fno-stack-protector ${a64_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${shared_src}
    COMMENT "Compiling ${src} -> ${base}.o (AArch64 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

foreach(src ${ARM32_TEST_SOURCES})
  neverd_shared_fixture_source(${src} shared_src)
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(arm_extra_flags)
  if(src MATCHES "test_switch|test_jumptable")
    set(arm_extra_flags -O1)
  elseif(src MATCHES "test_return_join")
    set(arm_extra_flags -O2)
  elseif(src MATCHES "test_extern_calls_nopic")
    set(arm_extra_flags -O2 -fno-pic -fcommon)
  elseif(src MATCHES "test_extern_calls")
    set(arm_extra_flags -O2 -fPIC -fcommon)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target arm-linux-gnueabi -march=armv7-a -nostdlib -static
            -O0 -fno-stack-protector -marm ${arm_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${shared_src}
    COMMENT "Compiling ${src} -> ${base}.o (ARM32 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

# --- Compile patch-specific test sources (no inline asm) ---
set(PATCH_TEST_SOURCES_X64 x86_64/test_patch_switch.c x86_64/test_patch_xcall.c)
set(PATCH_TEST_SOURCES_A64 aarch64/test_patch_switch_a64.c aarch64/test_patch_xcall_a64.c)
set(PATCH_TEST_SOURCES_ARM arm32/test_patch_switch_arm.c arm32/test_patch_xcall_arm.c)

foreach(src ${PATCH_TEST_SOURCES_X64})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target x86_64-linux-gnu -nostdlib -static
            -O1 -fno-stack-protector -fno-pie -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (x86_64 ELF)")
  list(APPEND TEST_OBJECTS ${obj})
endforeach()
foreach(src ${PATCH_TEST_SOURCES_A64})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -nostdlib -static
            -O1 -fno-stack-protector -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (AArch64 ELF)")
  list(APPEND TEST_OBJECTS ${obj})
endforeach()
foreach(src ${PATCH_TEST_SOURCES_ARM})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target arm-linux-gnueabi -march=armv7-a -nostdlib -static
            -O1 -fno-stack-protector -marm -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (ARM32 ELF)")
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

# --- Compile COFF/PE test binary ---
find_program(LLD_LINK_EXEC lld-link)
if(LLD_LINK_EXEC)
  set(call_arg_phi_a64_pe_src
      "${CMAKE_CURRENT_SOURCE_DIR}/aarch64/test_call_arg_phi_a64_pe.s")
  set(call_arg_phi_a64_pe_def
      "${CMAKE_CURRENT_SOURCE_DIR}/aarch64/test_call_arg_phi_a64_pe.def")
  set(call_arg_phi_a64_pe_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_call_arg_phi_a64_pe.obj")
  set(call_arg_phi_a64_pe_import_lib
      "${CMAKE_CURRENT_BINARY_DIR}/test_call_arg_phi_a64_pe.lib")
  set(call_arg_phi_a64_pe_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_call_arg_phi_a64_pe.exe")
  add_custom_command(
    OUTPUT ${call_arg_phi_a64_pe_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-pc-windows-msvc -c
            ${call_arg_phi_a64_pe_src} -o ${call_arg_phi_a64_pe_obj}
    DEPENDS ${call_arg_phi_a64_pe_src}
    COMMENT "Compiling AArch64 PE call-argument PHI fixture")
  add_custom_command(
    OUTPUT ${call_arg_phi_a64_pe_import_lib}
    COMMAND ${LLD_LINK_EXEC} /dll /noentry /machine:arm64
            /def:${call_arg_phi_a64_pe_def}
            /implib:${call_arg_phi_a64_pe_import_lib}
            /out:${CMAKE_CURRENT_BINARY_DIR}/test_call_arg_phi_provider_a64_pe.dll
    DEPENDS ${call_arg_phi_a64_pe_def}
    COMMENT "Generating AArch64 PE call-argument import library")
  add_custom_command(
    OUTPUT ${call_arg_phi_a64_pe_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:arm64 /entry:start /subsystem:console
            /nodefaultlib ${call_arg_phi_a64_pe_obj}
            ${call_arg_phi_a64_pe_import_lib}
            /out:${call_arg_phi_a64_pe_exe}
    DEPENDS ${call_arg_phi_a64_pe_obj} ${call_arg_phi_a64_pe_import_lib}
    COMMENT "Linking AArch64 PE call-argument PHI fixture")
  list(APPEND TEST_OBJECTS ${call_arg_phi_a64_pe_obj}
                           ${call_arg_phi_a64_pe_import_lib}
                           ${call_arg_phi_a64_pe_exe})

  set(native_pointer_pe_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_pe_pointer_reloc_x64.s")
  set(native_pointer_pe_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_pe_pointer_reloc_x64.obj")
  set(native_pointer_pe_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_pe_pointer_reloc_x64.exe")
  set(native_pointer_pe_x86_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_pe_pointer_reloc_x86.s")
  set(native_pointer_pe_x86_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_pe_pointer_reloc_x86.obj")
  set(native_pointer_pe_x86_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_pe_pointer_reloc_x86.exe")
  add_custom_command(
    OUTPUT ${native_pointer_pe_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -c
            ${native_pointer_pe_src} -o ${native_pointer_pe_obj}
    DEPENDS ${native_pointer_pe_src}
    COMMENT "Compiling x64 PE pointer-relocation fixture")
  add_custom_command(
    OUTPUT ${native_pointer_pe_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:main /subsystem:console /nodefaultlib
            /machine:x64 /dynamicbase /fixed:no ${native_pointer_pe_obj}
            /out:${native_pointer_pe_exe}
    DEPENDS ${native_pointer_pe_obj}
    COMMENT "Linking x64 PE pointer-relocation fixture")
  add_custom_command(
    OUTPUT ${native_pointer_pe_x86_obj}
    COMMAND ${CLANG_EXEC} -target i686-pc-windows-msvc -c
            ${native_pointer_pe_x86_src} -o ${native_pointer_pe_x86_obj}
    DEPENDS ${native_pointer_pe_x86_src}
    COMMENT "Compiling x86 PE pointer-relocation fixture")
  add_custom_command(
    OUTPUT ${native_pointer_pe_x86_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:start /subsystem:console /nodefaultlib
            /machine:x86 /safeseh:no /dynamicbase /fixed:no
            ${native_pointer_pe_x86_obj} /out:${native_pointer_pe_x86_exe}
    DEPENDS ${native_pointer_pe_x86_obj}
    COMMENT "Linking x86 PE pointer-relocation fixture")
  list(APPEND TEST_OBJECTS ${native_pointer_pe_obj} ${native_pointer_pe_exe}
                           ${native_pointer_pe_x86_obj}
                           ${native_pointer_pe_x86_exe})

  set(rematerialized_table_a64_pe_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_rematerialized_table_base_a64_pe.s")
  set(rematerialized_table_a64_pe_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_rematerialized_table_base_a64_pe.obj")
  set(rematerialized_table_a64_pe_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_rematerialized_table_base_a64_pe.exe")
  set(rematerialized_table_x64_pe_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_rematerialized_table_base_x64_pe.s")
  set(rematerialized_table_x64_pe_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_rematerialized_table_base_x64_pe.obj")
  set(rematerialized_table_x64_pe_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_rematerialized_table_base_x64_pe.exe")
  add_custom_command(
    OUTPUT ${rematerialized_table_a64_pe_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-pc-windows-msvc -c
            ${rematerialized_table_a64_pe_src}
            -o ${rematerialized_table_a64_pe_obj}
    DEPENDS ${rematerialized_table_a64_pe_src}
    COMMENT "Compiling AArch64 PE rematerialized table-base fixture")
  add_custom_command(
    OUTPUT ${rematerialized_table_a64_pe_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:arm64 /entry:start /subsystem:console
            /nodefaultlib /dynamicbase /fixed:no
            ${rematerialized_table_a64_pe_obj}
            /out:${rematerialized_table_a64_pe_exe}
    DEPENDS ${rematerialized_table_a64_pe_obj}
    COMMENT "Linking AArch64 PE rematerialized table-base fixture")
  add_custom_command(
    OUTPUT ${rematerialized_table_x64_pe_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -c
            ${rematerialized_table_x64_pe_src}
            -o ${rematerialized_table_x64_pe_obj}
    DEPENDS ${rematerialized_table_x64_pe_src}
    COMMENT "Compiling x64 PE rematerialized table-base fixture")
  add_custom_command(
    OUTPUT ${rematerialized_table_x64_pe_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:x64 /entry:main /subsystem:console
            /nodefaultlib /dynamicbase /fixed:no
            ${rematerialized_table_x64_pe_obj}
            /out:${rematerialized_table_x64_pe_exe}
    DEPENDS ${rematerialized_table_x64_pe_obj}
    COMMENT "Linking x64 PE rematerialized table-base fixture")
  list(APPEND TEST_OBJECTS
       ${rematerialized_table_a64_pe_obj}
       ${rematerialized_table_a64_pe_exe}
       ${rematerialized_table_x64_pe_obj}
       ${rematerialized_table_x64_pe_exe})

  set(reentrant_feasible_phi_a64_pe_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_reentrant_feasible_phi_a64_pe.s")
  set(reentrant_feasible_phi_a64_pe_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_reentrant_feasible_phi_a64_pe.obj")
  set(reentrant_feasible_phi_a64_pe_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_reentrant_feasible_phi_a64_pe.exe")
  add_custom_command(
    OUTPUT ${reentrant_feasible_phi_a64_pe_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-pc-windows-msvc -c
            ${reentrant_feasible_phi_a64_pe_src}
            -o ${reentrant_feasible_phi_a64_pe_obj}
    DEPENDS ${reentrant_feasible_phi_a64_pe_src}
    COMMENT "Compiling AArch64 PE reentrant feasible-edge PHI fixture")
  add_custom_command(
    OUTPUT ${reentrant_feasible_phi_a64_pe_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:arm64 /entry:start /subsystem:console
            /nodefaultlib /dynamicbase /fixed:no
            ${reentrant_feasible_phi_a64_pe_obj}
            /out:${reentrant_feasible_phi_a64_pe_exe}
    DEPENDS ${reentrant_feasible_phi_a64_pe_obj}
    COMMENT "Linking AArch64 PE reentrant feasible-edge PHI fixture")
  list(APPEND TEST_OBJECTS
       ${reentrant_feasible_phi_a64_pe_obj}
       ${reentrant_feasible_phi_a64_pe_exe})

  set(writable_pointer_induction_a64_pe_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_writable_pointer_table_induction_a64_pe.s")
  set(writable_pointer_induction_a64_pe_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_a64_pe.obj")
  set(writable_pointer_induction_a64_pe_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_a64_pe.exe")
  set(writable_pointer_induction_x64_pe_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_writable_pointer_table_induction_x64_pe.s")
  set(writable_pointer_induction_x64_pe_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_x64_pe.obj")
  set(writable_pointer_induction_x64_pe_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_x64_pe.exe")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_a64_pe_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-pc-windows-msvc -c
            ${writable_pointer_induction_a64_pe_src}
            -o ${writable_pointer_induction_a64_pe_obj}
    DEPENDS ${writable_pointer_induction_a64_pe_src}
    COMMENT "Compiling AArch64 PE writable pointer-table induction fixture")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_a64_pe_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:arm64 /entry:start /subsystem:console
            /nodefaultlib /dynamicbase /fixed:no
            ${writable_pointer_induction_a64_pe_obj}
            /out:${writable_pointer_induction_a64_pe_exe}
    DEPENDS ${writable_pointer_induction_a64_pe_obj}
    COMMENT "Linking AArch64 PE writable pointer-table induction fixture")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_x64_pe_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -c
            ${writable_pointer_induction_x64_pe_src}
            -o ${writable_pointer_induction_x64_pe_obj}
    DEPENDS ${writable_pointer_induction_x64_pe_src}
    COMMENT "Compiling x64 PE writable pointer-table induction fixture")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_x64_pe_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:x64 /entry:main /subsystem:console
            /nodefaultlib /dynamicbase /fixed:no
            ${writable_pointer_induction_x64_pe_obj}
            /out:${writable_pointer_induction_x64_pe_exe}
    DEPENDS ${writable_pointer_induction_x64_pe_obj}
    COMMENT "Linking x64 PE writable pointer-table induction fixture")
  list(APPEND TEST_OBJECTS
       ${writable_pointer_induction_a64_pe_obj}
       ${writable_pointer_induction_a64_pe_exe}
       ${writable_pointer_induction_x64_pe_obj}
       ${writable_pointer_induction_x64_pe_exe})

  set(coff_src "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/test_patch_coff.c")
  set(coff_obj "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff.obj")
  set(coff_exe "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff.exe")
  add_custom_command(
    OUTPUT ${coff_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -O1
            -fno-inline-functions -c ${coff_src} -o ${coff_obj}
    DEPENDS ${coff_src}
    COMMENT "Compiling test_patch_coff.c -> .obj (x86_64 COFF)")
  add_custom_command(
    OUTPUT ${coff_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:mainCRTStartup /subsystem:console /nodefaultlib ${coff_obj} /out:${coff_exe}
    DEPENDS ${coff_obj}
    COMMENT "Linking test_patch_coff.obj -> .exe (x86_64 PE)")
  list(APPEND TEST_OBJECTS ${coff_obj} ${coff_exe})

  set(coff_seh_guard_src
      "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/test_patch_coff_seh_guard.s")
  set(coff_seh_guard_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_seh_guard.obj")
  set(coff_seh_guard_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_seh_guard.exe")
  add_custom_command(
    OUTPUT ${coff_seh_guard_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -c
            ${coff_seh_guard_src} -o ${coff_seh_guard_obj}
    DEPENDS ${coff_seh_guard_src}
    COMMENT "Compiling guarded x64 SEH fixture")
  add_custom_command(
    OUTPUT ${coff_seh_guard_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:seh_fixture_entry /subsystem:console
            /nodefaultlib /guard:cf /guard:ehcont
            /export:guarded_main /export:may_throw_stub
            /export:__C_specific_handler ${coff_seh_guard_obj}
            /out:${coff_seh_guard_exe}
    DEPENDS ${coff_seh_guard_obj}
    COMMENT "Linking guarded x64 SEH fixture")
  list(APPEND TEST_OBJECTS ${coff_seh_guard_obj} ${coff_seh_guard_exe})

  set(coff_cxx_fh3_src
      "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/test_patch_coff_cxx_fh3.s")
  set(coff_cxx_fh3_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_cxx_fh3.obj")
  set(coff_cxx_fh3_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_cxx_fh3.exe")
  add_custom_command(
    OUTPUT ${coff_cxx_fh3_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -c
            ${coff_cxx_fh3_src} -o ${coff_cxx_fh3_obj}
    DEPENDS ${coff_cxx_fh3_src}
    COMMENT "Compiling x64 FH3 reconstruction fixture")
  add_custom_command(
    OUTPUT ${coff_cxx_fh3_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:cxx_fixture_entry /subsystem:console
            /nodefaultlib
            /export:cxx_main /export:cxx_may_throw
            /export:__CxxFrameHandler3 ${coff_cxx_fh3_obj}
            /out:${coff_cxx_fh3_exe}
    DEPENDS ${coff_cxx_fh3_obj}
    COMMENT "Linking x64 FH3 reconstruction fixture")
  list(APPEND TEST_OBJECTS ${coff_cxx_fh3_obj} ${coff_cxx_fh3_exe})

  set(coff_cxx_fh4_src
      "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/test_patch_coff_cxx_fh4.s")
  set(coff_cxx_fh4_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_cxx_fh4.obj")
  set(coff_cxx_fh4_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_cxx_fh4.exe")
  add_custom_command(
    OUTPUT ${coff_cxx_fh4_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -c
            ${coff_cxx_fh4_src} -o ${coff_cxx_fh4_obj}
    DEPENDS ${coff_cxx_fh4_src}
    COMMENT "Compiling x64 FH4 reconstruction fixture")
  add_custom_command(
    OUTPUT ${coff_cxx_fh4_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:cxx_fh4_fixture_entry /subsystem:console
            /nodefaultlib
            /export:cxx_fh4_main /export:cxx_fh4_may_throw
            /export:__CxxFrameHandler4 ${coff_cxx_fh4_obj}
            /out:${coff_cxx_fh4_exe}
    DEPENDS ${coff_cxx_fh4_obj}
    COMMENT "Linking x64 FH4 reconstruction fixture")
  list(APPEND TEST_OBJECTS ${coff_cxx_fh4_obj} ${coff_cxx_fh4_exe})

  set(coff_cxx_fh4_typed_src
      "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/test_patch_coff_cxx_fh4_typed.s")
  set(coff_cxx_fh4_typed_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_cxx_fh4_typed.obj")
  set(coff_cxx_fh4_typed_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_cxx_fh4_typed.exe")
  add_custom_command(
    OUTPUT ${coff_cxx_fh4_typed_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -c
            ${coff_cxx_fh4_typed_src} -o ${coff_cxx_fh4_typed_obj}
    DEPENDS ${coff_cxx_fh4_typed_src}
    COMMENT "Compiling x64 typed FH4 reconstruction fixture")
  add_custom_command(
    OUTPUT ${coff_cxx_fh4_typed_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:cxx_fh4_typed_fixture_entry
            /subsystem:console /nodefaultlib
            /export:cxx_fh4_typed_main /export:cxx_fh4_typed_may_throw
            /export:__CxxFrameHandler4 ${coff_cxx_fh4_typed_obj}
            /out:${coff_cxx_fh4_typed_exe}
    DEPENDS ${coff_cxx_fh4_typed_obj}
    COMMENT "Linking x64 typed FH4 reconstruction fixture")
  list(APPEND TEST_OBJECTS ${coff_cxx_fh4_typed_obj}
       ${coff_cxx_fh4_typed_exe})

  set(coff_cxx_fh4_gs_src
      "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/test_patch_coff_cxx_fh4_gs.s")
  set(coff_cxx_fh4_gs_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_cxx_fh4_gs.obj")
  set(coff_cxx_fh4_gs_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_cxx_fh4_gs.exe")
  add_custom_command(
    OUTPUT ${coff_cxx_fh4_gs_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-pc-windows-msvc -c
            ${coff_cxx_fh4_gs_src} -o ${coff_cxx_fh4_gs_obj}
    DEPENDS ${coff_cxx_fh4_gs_src}
    COMMENT "Compiling x64 GS-wrapped FH4 reconstruction fixture")
  add_custom_command(
    OUTPUT ${coff_cxx_fh4_gs_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:cxx_fh4_gs_fixture_entry
            /subsystem:console /nodefaultlib
            /export:cxx_fh4_gs_main /export:cxx_fh4_gs_may_throw
            /export:__GSHandlerCheck_EH4
            /export:__security_cookie /export:__security_check_cookie
            ${coff_cxx_fh4_gs_obj} /out:${coff_cxx_fh4_gs_exe}
    DEPENDS ${coff_cxx_fh4_gs_obj}
    COMMENT "Linking x64 GS-wrapped FH4 reconstruction fixture")
  list(APPEND TEST_OBJECTS ${coff_cxx_fh4_gs_obj} ${coff_cxx_fh4_gs_exe})

  set(coff_arm_src "${CMAKE_CURRENT_SOURCE_DIR}/format/test_pe_arm.c")

  set(coff_a64_obj "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_a64.obj")
  set(coff_a64_exe "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_a64.exe")
  add_custom_command(
    OUTPUT ${coff_a64_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-pc-windows-msvc -O0
            -ffreestanding -fno-stack-protector -funwind-tables -c
            ${coff_arm_src} -o ${coff_a64_obj}
    DEPENDS ${coff_arm_src}
    COMMENT "Compiling test_pe_arm.c -> .obj (AArch64 COFF)")
  add_custom_command(
    OUTPUT ${coff_a64_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:mainCRTStartup /subsystem:console
            /nodefaultlib /export:pe_leaf /export:pe_stacky
            ${coff_a64_obj} /out:${coff_a64_exe}
    DEPENDS ${coff_a64_obj}
    COMMENT "Linking test_patch_coff_a64.obj -> .exe (AArch64 PE)")

  set(coff_a64_seh_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_patch_coff_a64_seh.s")
  set(coff_a64_seh_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_a64_seh.obj")
  set(coff_a64_seh_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_a64_seh.exe")
  add_custom_command(
    OUTPUT ${coff_a64_seh_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-pc-windows-msvc -c
            ${coff_a64_seh_src} -o ${coff_a64_seh_obj}
    DEPENDS ${coff_a64_seh_src}
    COMMENT "Compiling AArch64 native SEH reconstruction fixture")
  add_custom_command(
    OUTPUT ${coff_a64_seh_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:arm64 /entry:a64_seh_fixture_entry
            /subsystem:console /nodefaultlib /export:a64_seh_main
            /export:a64_seh_may_throw /export:__C_specific_handler
            ${coff_a64_seh_obj} /out:${coff_a64_seh_exe}
    DEPENDS ${coff_a64_seh_obj}
    COMMENT "Linking AArch64 native SEH reconstruction fixture")

  set(coff_a64_cxx_fh3_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_patch_coff_a64_cxx_fh3.s")
  set(coff_a64_cxx_fh3_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_a64_cxx_fh3.obj")
  set(coff_a64_cxx_fh3_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_a64_cxx_fh3.exe")
  add_custom_command(
    OUTPUT ${coff_a64_cxx_fh3_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-pc-windows-msvc -c
            ${coff_a64_cxx_fh3_src} -o ${coff_a64_cxx_fh3_obj}
    DEPENDS ${coff_a64_cxx_fh3_src}
    COMMENT "Compiling AArch64 FH3 reconstruction fixture")
  add_custom_command(
    OUTPUT ${coff_a64_cxx_fh3_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:arm64
            /entry:a64_cxx_fh3_fixture_entry /subsystem:console /nodefaultlib
            /export:a64_cxx_fh3_main /export:a64_cxx_fh3_may_throw
            /export:__CxxFrameHandler3 ${coff_a64_cxx_fh3_obj}
            /out:${coff_a64_cxx_fh3_exe}
    DEPENDS ${coff_a64_cxx_fh3_obj}
    COMMENT "Linking AArch64 FH3 reconstruction fixture")

  set(coff_arm_obj "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm.obj")
  set(coff_arm_seh_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_patch_coff_arm_seh.s")
  set(coff_arm_seh_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm_seh.obj")
  set(coff_arm_seh_exe
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm_seh.exe")
  set(coff_arm_tiny_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_pe_tiny_thumb.s")
  set(coff_arm_tiny_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm_tiny.obj")
  set(coff_arm_exe "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm.exe")
  add_custom_command(
    OUTPUT ${coff_arm_obj}
    COMMAND ${CLANG_EXEC} -target thumbv7-pc-windows-msvc -O0
            -ffreestanding -fno-stack-protector -funwind-tables -c
            ${coff_arm_src} -o ${coff_arm_obj}
    DEPENDS ${coff_arm_src}
    COMMENT "Compiling test_pe_arm.c -> .obj (ARM32 COFF)")
  add_custom_command(
    OUTPUT ${coff_arm_tiny_obj}
    COMMAND ${CLANG_EXEC} -target thumbv7-pc-windows-msvc -c
            ${coff_arm_tiny_src} -o ${coff_arm_tiny_obj}
    DEPENDS ${coff_arm_tiny_src}
    COMMENT "Compiling test_pe_tiny_thumb.s -> .obj (ARM32 COFF)")
  add_custom_command(
    OUTPUT ${coff_arm_seh_obj}
    COMMAND ${CLANG_EXEC} -target thumbv7-pc-windows-msvc -c
            ${coff_arm_seh_src} -o ${coff_arm_seh_obj}
    DEPENDS ${coff_arm_seh_src}
    COMMENT "Compiling ARM32 native SEH reconstruction fixture")
  add_custom_command(
    OUTPUT ${coff_arm_seh_exe}
    COMMAND ${LLD_LINK_EXEC} /machine:arm
            /entry:arm32_seh_fixture_entry /subsystem:console /nodefaultlib
            /export:arm32_seh_main /export:arm32_seh_may_throw
            /export:__C_specific_handler ${coff_arm_seh_obj}
            /out:${coff_arm_seh_exe}
    DEPENDS ${coff_arm_seh_obj}
    COMMENT "Linking ARM32 native SEH reconstruction fixture")
  add_custom_command(
    OUTPUT ${coff_arm_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:mainCRTStartup /subsystem:console
            /nodefaultlib /export:pe_leaf /export:pe_stacky /export:pe_tiny
            /export:pe_after_tiny ${coff_arm_obj} ${coff_arm_tiny_obj}
            /out:${coff_arm_exe}
    DEPENDS ${coff_arm_obj} ${coff_arm_tiny_obj}
    COMMENT "Linking ARM32 COFF objects -> test_patch_coff_arm.exe")

  list(APPEND TEST_OBJECTS
    ${coff_a64_obj} ${coff_a64_exe}
    ${coff_arm_obj}
    ${coff_arm_tiny_obj} ${coff_arm_exe})
  list(APPEND COFF_NATIVE_EH_TEST_OBJECTS
    ${coff_a64_seh_obj} ${coff_a64_seh_exe}
    ${coff_a64_cxx_fh3_obj} ${coff_a64_cxx_fh3_exe}
    ${coff_arm_seh_obj} ${coff_arm_seh_exe})
endif()

# --- Link into ELF executables for patch testing ---
find_program(LLD_EXEC ld.lld)
set(ELF_EXECUTABLES "")
if(LLD_EXEC)
  set(call_arg_phi_a64_elf_src
      "${CMAKE_CURRENT_SOURCE_DIR}/aarch64/test_call_arg_phi_a64_elf.s")
  set(call_arg_phi_a64_elf_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_call_arg_phi_a64_elf.o")
  set(call_arg_phi_a64_elf_so
      "${CMAKE_CURRENT_BINARY_DIR}/test_call_arg_phi_a64_elf.so")
  add_custom_command(
    OUTPUT ${call_arg_phi_a64_elf_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -fPIC -c
            ${call_arg_phi_a64_elf_src} -o ${call_arg_phi_a64_elf_obj}
    DEPENDS ${call_arg_phi_a64_elf_src}
    COMMENT "Compiling AArch64 ELF call-argument PHI fixture")
  add_custom_command(
    OUTPUT ${call_arg_phi_a64_elf_so}
    COMMAND ${LLD_EXEC} -shared -m aarch64linux
            -o ${call_arg_phi_a64_elf_so} ${call_arg_phi_a64_elf_obj}
    DEPENDS ${call_arg_phi_a64_elf_obj}
    COMMENT "Linking AArch64 ELF call-argument PHI fixture")
  list(APPEND ELF_EXECUTABLES ${call_arg_phi_a64_elf_obj}
                              ${call_arg_phi_a64_elf_so})

  # An i386 shared library as GCC builds one: its PLT entry jumps through the
  # GOT by the address in EBX.
  set(pic_library_i386_src
      "${CMAKE_CURRENT_SOURCE_DIR}/x86_32/test_pic_library_i386.s")
  set(pic_library_i386_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_pic_library_i386.o")
  set(pic_library_i386_so
      "${CMAKE_CURRENT_BINARY_DIR}/test_pic_library_i386.so")
  add_custom_command(
    OUTPUT ${pic_library_i386_obj}
    COMMAND ${CLANG_EXEC} -target i386-linux-gnu -c ${pic_library_i386_src}
            -o ${pic_library_i386_obj}
    DEPENDS ${pic_library_i386_src}
    COMMENT "Compiling i386 PIC library fixture")
  add_custom_command(
    OUTPUT ${pic_library_i386_so}
    COMMAND ${LLD_EXEC} -shared -m elf_i386 --eh-frame-hdr
            -o ${pic_library_i386_so} ${pic_library_i386_obj}
    DEPENDS ${pic_library_i386_obj}
    COMMENT "Linking i386 PIC library fixture")
  list(APPEND ELF_EXECUTABLES ${pic_library_i386_obj} ${pic_library_i386_so})

  set(native_pointer_elf_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_elf_pointer_reloc_x64.s")
  set(native_pointer_elf_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_elf_pointer_reloc_x64.o")
  set(native_pointer_elf
      "${CMAKE_CURRENT_BINARY_DIR}/test_elf_pointer_reloc_x64")
  set(native_pointer_elf_relr
      "${CMAKE_CURRENT_BINARY_DIR}/test_elf_pointer_relr_x64")
  set(native_pointer_elf_android_relr
      "${CMAKE_CURRENT_BINARY_DIR}/test_elf_pointer_android_relr_x64")
  set(native_pointer_elf_android_packed
      "${CMAKE_CURRENT_BINARY_DIR}/test_elf_pointer_android_packed_x64")
  add_custom_command(
    OUTPUT ${native_pointer_elf_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-linux-gnu -c
            ${native_pointer_elf_src} -o ${native_pointer_elf_obj}
    DEPENDS ${native_pointer_elf_src}
    COMMENT "Compiling x64 ELF pointer-relocation fixture")
  add_custom_command(
    OUTPUT ${native_pointer_elf}
    COMMAND ${LLD_EXEC} -pie -z relro --entry=_start
            ${native_pointer_elf_obj} -o ${native_pointer_elf}
    DEPENDS ${native_pointer_elf_obj}
    COMMENT "Linking x64 ELF pointer-relocation fixture")
  add_custom_command(
    OUTPUT ${native_pointer_elf_relr}
    COMMAND ${LLD_EXEC} -pie -z relro --pack-dyn-relocs=relr
            --entry=_start ${native_pointer_elf_obj}
            -o ${native_pointer_elf_relr}
    DEPENDS ${native_pointer_elf_obj}
    COMMENT "Linking x64 ELF packed-pointer-relocation fixture")
  add_custom_command(
    OUTPUT ${native_pointer_elf_android_relr}
    COMMAND ${LLD_EXEC} -pie -z relro --pack-dyn-relocs=relr
            --use-android-relr-tags --entry=_start ${native_pointer_elf_obj}
            -o ${native_pointer_elf_android_relr}
    DEPENDS ${native_pointer_elf_obj}
    COMMENT "Linking x64 ELF Android-RELR pointer fixture")
  add_custom_command(
    OUTPUT ${native_pointer_elf_android_packed}
    COMMAND ${LLD_EXEC} -pie -z relro --pack-dyn-relocs=android
            --entry=_start ${native_pointer_elf_obj}
            -o ${native_pointer_elf_android_packed}
    DEPENDS ${native_pointer_elf_obj}
    COMMENT "Linking x64 ELF Android-packed pointer fixture")
  list(APPEND ELF_EXECUTABLES ${native_pointer_elf_obj} ${native_pointer_elf}
                              ${native_pointer_elf_relr}
                              ${native_pointer_elf_android_relr}
                              ${native_pointer_elf_android_packed})

  set(rematerialized_table_a64_elf_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_rematerialized_table_base_a64_elf.s")
  set(rematerialized_table_a64_elf_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_rematerialized_table_base_a64_elf.o")
  set(rematerialized_table_a64_elf
      "${CMAKE_CURRENT_BINARY_DIR}/test_rematerialized_table_base_a64_elf")
  set(rematerialized_table_x64_elf_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_rematerialized_table_base_x64_elf.s")
  set(rematerialized_table_x64_elf_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_rematerialized_table_base_x64_elf.o")
  set(rematerialized_table_x64_elf
      "${CMAKE_CURRENT_BINARY_DIR}/test_rematerialized_table_base_x64_elf")
  add_custom_command(
    OUTPUT ${rematerialized_table_a64_elf_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -fPIC -c
            ${rematerialized_table_a64_elf_src}
            -o ${rematerialized_table_a64_elf_obj}
    DEPENDS ${rematerialized_table_a64_elf_src}
    COMMENT "Compiling AArch64 ELF rematerialized table-base fixture")
  add_custom_command(
    OUTPUT ${rematerialized_table_a64_elf}
    COMMAND ${LLD_EXEC} -pie -z relro -m aarch64linux --entry=_start
            ${rematerialized_table_a64_elf_obj}
            -o ${rematerialized_table_a64_elf}
    DEPENDS ${rematerialized_table_a64_elf_obj}
    COMMENT "Linking AArch64 ELF rematerialized table-base fixture")
  add_custom_command(
    OUTPUT ${rematerialized_table_x64_elf_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-linux-gnu -fPIC -c
            ${rematerialized_table_x64_elf_src}
            -o ${rematerialized_table_x64_elf_obj}
    DEPENDS ${rematerialized_table_x64_elf_src}
    COMMENT "Compiling x64 ELF rematerialized table-base fixture")
  add_custom_command(
    OUTPUT ${rematerialized_table_x64_elf}
    COMMAND ${LLD_EXEC} -pie -z relro --entry=_start
            ${rematerialized_table_x64_elf_obj}
            -o ${rematerialized_table_x64_elf}
    DEPENDS ${rematerialized_table_x64_elf_obj}
    COMMENT "Linking x64 ELF rematerialized table-base fixture")
  list(APPEND ELF_EXECUTABLES
       ${rematerialized_table_a64_elf_obj}
       ${rematerialized_table_a64_elf}
       ${rematerialized_table_x64_elf_obj}
       ${rematerialized_table_x64_elf})

  set(reentrant_feasible_phi_a64_elf_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_reentrant_feasible_phi_a64_elf.s")
  set(reentrant_feasible_phi_a64_elf_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_reentrant_feasible_phi_a64_elf.o")
  set(reentrant_feasible_phi_a64_elf
      "${CMAKE_CURRENT_BINARY_DIR}/test_reentrant_feasible_phi_a64_elf")
  add_custom_command(
    OUTPUT ${reentrant_feasible_phi_a64_elf_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -fPIC -c
            ${reentrant_feasible_phi_a64_elf_src}
            -o ${reentrant_feasible_phi_a64_elf_obj}
    DEPENDS ${reentrant_feasible_phi_a64_elf_src}
    COMMENT "Compiling AArch64 ELF reentrant feasible-edge PHI fixture")
  add_custom_command(
    OUTPUT ${reentrant_feasible_phi_a64_elf}
    COMMAND ${LLD_EXEC} -pie -m aarch64linux --entry=_start
            ${reentrant_feasible_phi_a64_elf_obj}
            -o ${reentrant_feasible_phi_a64_elf}
    DEPENDS ${reentrant_feasible_phi_a64_elf_obj}
    COMMENT "Linking AArch64 ELF reentrant feasible-edge PHI fixture")
  list(APPEND ELF_EXECUTABLES
       ${reentrant_feasible_phi_a64_elf_obj}
       ${reentrant_feasible_phi_a64_elf})

  set(writable_pointer_induction_a64_elf_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_writable_pointer_table_induction_a64_elf.s")
  set(writable_pointer_induction_a64_elf_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_a64_elf.o")
  set(writable_pointer_induction_a64_elf
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_a64_elf")
  set(writable_pointer_induction_x64_elf_src
      "${CMAKE_CURRENT_SOURCE_DIR}/format/test_writable_pointer_table_induction_x64_elf.s")
  set(writable_pointer_induction_x64_elf_obj
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_x64_elf.o")
  set(writable_pointer_induction_x64_elf
      "${CMAKE_CURRENT_BINARY_DIR}/test_writable_pointer_table_induction_x64_elf")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_a64_elf_obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -fPIC -c
            ${writable_pointer_induction_a64_elf_src}
            -o ${writable_pointer_induction_a64_elf_obj}
    DEPENDS ${writable_pointer_induction_a64_elf_src}
    COMMENT "Compiling AArch64 ELF writable pointer-table induction fixture")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_a64_elf}
    COMMAND ${LLD_EXEC} -pie -m aarch64linux --entry=_start
            ${writable_pointer_induction_a64_elf_obj}
            -o ${writable_pointer_induction_a64_elf}
    DEPENDS ${writable_pointer_induction_a64_elf_obj}
    COMMENT "Linking AArch64 ELF writable pointer-table induction fixture")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_x64_elf_obj}
    COMMAND ${CLANG_EXEC} -target x86_64-linux-gnu -fPIC -c
            ${writable_pointer_induction_x64_elf_src}
            -o ${writable_pointer_induction_x64_elf_obj}
    DEPENDS ${writable_pointer_induction_x64_elf_src}
    COMMENT "Compiling x64 ELF writable pointer-table induction fixture")
  add_custom_command(
    OUTPUT ${writable_pointer_induction_x64_elf}
    COMMAND ${LLD_EXEC} -pie --entry=_start
            ${writable_pointer_induction_x64_elf_obj}
            -o ${writable_pointer_induction_x64_elf}
    DEPENDS ${writable_pointer_induction_x64_elf_obj}
    COMMENT "Linking x64 ELF writable pointer-table induction fixture")
  list(APPEND ELF_EXECUTABLES
       ${writable_pointer_induction_a64_elf_obj}
       ${writable_pointer_induction_a64_elf}
       ${writable_pointer_induction_x64_elf_obj}
       ${writable_pointer_induction_x64_elf})

  # Patch-success fixtures publish the native unwind index and leave a
  # deterministic file-backed gap after each growable unwind section.  The
  # low-level tests separately require byte-identical failure when a real image
  # has no such capacity.
  set(unwind_slack_src
      "${CMAKE_CURRENT_SOURCE_DIR}/eh/unwind_patch_slack.c")
  set(dwarf_unwind_slack_x64
      "${CMAKE_CURRENT_BINARY_DIR}/unwind_patch_slack_x64.o")
  set(dwarf_unwind_slack_a64
      "${CMAKE_CURRENT_BINARY_DIR}/unwind_patch_slack_a64.o")
  set(arm_unwind_slack
      "${CMAKE_CURRENT_BINARY_DIR}/unwind_patch_slack_arm.o")
  add_custom_command(
    OUTPUT ${dwarf_unwind_slack_x64}
    COMMAND ${CLANG_EXEC} -target x86_64-linux-gnu -nostdlib -static
            -DNEVERD_DWARF_EH_SLACK=1 -c ${unwind_slack_src}
            -o ${dwarf_unwind_slack_x64}
    DEPENDS ${unwind_slack_src}
    COMMENT "Compiling x86_64 unwind patch capacity fixture")
  add_custom_command(
    OUTPUT ${dwarf_unwind_slack_a64}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -nostdlib -static
            -DNEVERD_DWARF_EH_SLACK=1 -c ${unwind_slack_src}
            -o ${dwarf_unwind_slack_a64}
    DEPENDS ${unwind_slack_src}
    COMMENT "Compiling AArch64 unwind patch capacity fixture")
  add_custom_command(
    OUTPUT ${arm_unwind_slack}
    COMMAND ${CLANG_EXEC} -target arm-linux-gnueabi -march=armv7-a
            -nostdlib -static -marm -DNEVERD_ARM_EHABI_SLACK=1
            -c ${unwind_slack_src} -o ${arm_unwind_slack}
    DEPENDS ${unwind_slack_src}
    COMMENT "Compiling ARM EHABI patch capacity fixture")

  # x86_64 ELF executables
  foreach(base_name test_switch test_jumptable_enhanced test_patch_switch test_patch_xcall test_roundtrip)
    set(obj_file "${CMAKE_CURRENT_BINARY_DIR}/${base_name}.o")
    set(elf_file "${CMAKE_CURRENT_BINARY_DIR}/${base_name}_elf")
    add_custom_command(
      OUTPUT ${elf_file}
      COMMAND ${LLD_EXEC} -o ${elf_file} --image-base=0x400000
              --eh-frame-hdr --section-start=.eh_frame=0x400800
              --section-start=.neverd.eh_frame.limit=0x401000
              --section-start=.eh_frame_hdr=0x401800
              --section-start=.neverd.eh_frame_hdr.limit=0x401c00
              --section-start=.text=0x402000 -e _start ${obj_file}
              ${dwarf_unwind_slack_x64}
      DEPENDS ${obj_file} ${dwarf_unwind_slack_x64}
      COMMENT "Linking ${base_name} -> x86_64 ELF executable")
    list(APPEND ELF_EXECUTABLES ${elf_file})
  endforeach()

  # AArch64 ELF executables
  foreach(a64_base test_patch_switch_a64 test_patch_xcall_a64 test_switch_a64)
    set(a64_obj "${CMAKE_CURRENT_BINARY_DIR}/${a64_base}.o")
    set(a64_elf "${CMAKE_CURRENT_BINARY_DIR}/${a64_base}_elf")
    add_custom_command(
      OUTPUT ${a64_elf}
      COMMAND ${LLD_EXEC} -m aarch64linux -o ${a64_elf}
              --image-base=0x400000 --eh-frame-hdr
              --section-start=.eh_frame=0x400800
              --section-start=.neverd.eh_frame.limit=0x401000
              --section-start=.eh_frame_hdr=0x402000
              --section-start=.neverd.eh_frame_hdr.limit=0x403000
              --section-start=.text=0x410000 -e _start ${a64_obj}
              ${dwarf_unwind_slack_a64}
      DEPENDS ${a64_obj} ${dwarf_unwind_slack_a64}
      COMMENT "Linking ${a64_base} -> AArch64 ELF executable")
    list(APPEND ELF_EXECUTABLES ${a64_elf})
  endforeach()

  # ARM32 ELF executables
  foreach(arm_base test_patch_switch_arm test_patch_xcall_arm test_switch_arm)
    set(arm_obj "${CMAKE_CURRENT_BINARY_DIR}/${arm_base}.o")
    set(arm_elf "${CMAKE_CURRENT_BINARY_DIR}/${arm_base}_elf")
    add_custom_command(
      OUTPUT ${arm_elf}
      COMMAND ${LLD_EXEC} -m armelf -o ${arm_elf} --image-base=0x400000
              --section-start=.ARM.exidx=0x400800
              --section-start=.neverd.arm_exidx.limit=0x401800
              --section-start=.text=0x410000 -e _start ${arm_obj}
              ${arm_unwind_slack}
      DEPENDS ${arm_obj} ${arm_unwind_slack}
      COMMENT "Linking ${arm_base} -> ARM32 ELF executable")
    list(APPEND ELF_EXECUTABLES ${arm_elf})
  endforeach()
endif()

add_custom_target(lift-test-objects DEPENDS ${TEST_OBJECTS} ${ELF_EXECUTABLES})
add_dependencies(NeverDLiftTests lift-test-objects)
add_dependencies(NeverDJumpTableTests lift-test-objects)
add_dependencies(NeverDNativePointerRelocationBoundaryTests lift-test-objects)
if(COFF_NATIVE_EH_TEST_OBJECTS)
  add_custom_target(lift-coff-native-eh-test-objects
                    DEPENDS ${COFF_NATIVE_EH_TEST_OBJECTS})
  add_dependencies(NeverDLiftTests lift-coff-native-eh-test-objects)
endif()

add_neverd_unittest(NeverDLLVMCIntrinsicSemanticTests
  core/LLVMCIntrinsicSemanticTests.cpp
  core/LLVMCFunnelShiftTests.cpp
  LINK_COMPONENTS BackendC
)
target_include_directories(NeverDLLVMCIntrinsicSemanticTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# The feature repository retains compiler-produced objects and their manifests.
# Use those objects, rather than a hand-written stand-in for a standard library.
set(_library_feature_dir "${CMAKE_CURRENT_BINARY_DIR}/library-features/libcxx")
set(_library_feature_archive
  "${CMAKE_SOURCE_DIR}/signatures/features/evidence/libcxx-23git-arm64-macos-abi1-alternate-clang22.tar.gz")
add_custom_command(
  OUTPUT "${_library_feature_dir}/accessors-inline.o"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_library_feature_dir}"
  COMMAND ${CMAKE_COMMAND} -E chdir "${_library_feature_dir}"
    ${CMAKE_COMMAND} -E tar xzf "${_library_feature_archive}"
  DEPENDS "${_library_feature_archive}"
  VERBATIM)
add_custom_target(neverd-library-feature-fixtures
  DEPENDS "${_library_feature_dir}/accessors-inline.o")
add_neverd_unittest(NeverDLibraryRecognitionTests
  core/LibraryRecognitionTests.cpp
  LINK_COMPONENTS Pipeline Sigs)
add_dependencies(NeverDLibraryRecognitionTests neverd-library-feature-fixtures)
target_compile_definitions(NeverDLibraryRecognitionTests PRIVATE
  NEVERD_LIBRARY_FEATURE_DIR="${CMAKE_SOURCE_DIR}/signatures/features/rules"
  NEVERD_LIBRARY_FIXTURE_DIR="${_library_feature_dir}")

if(TARGET neverd-worker AND Python3_Interpreter_FOUND)
  add_test(NAME NeverDWorkerLibraryFeatures
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/neverd-worker/tests/library_features_test.py"
      "$<TARGET_FILE:neverd-worker>"
      "${_library_feature_dir}/accessors-inline.o"
      "${CMAKE_SOURCE_DIR}/signatures/features/rules/libcxx-23git-arm64-macos-abi1-alternate-clang22.json")
  set_tests_properties(NeverDWorkerLibraryFeatures PROPERTIES TIMEOUT 60 LABELS "unit;library-recognition;worker")
endif()

if(TARGET neverd AND Python3_Interpreter_FOUND AND CLANG_EXEC)
  add_test(NAME NeverDLibraryFeatureCLI
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/library_cli_test.py"
      "$<TARGET_FILE:neverd>"
      "${_library_feature_dir}/accessors-inline.o"
      "${CMAKE_SOURCE_DIR}/signatures/features/rules/libcxx-23git-arm64-macos-abi1-alternate-clang22.json"
      "${CLANG_EXEC}")
  set_tests_properties(NeverDLibraryFeatureCLI PROPERTIES
    TIMEOUT 120 LABELS "unit;library-recognition;cli")
endif()

# Link the original MSVC /Z7 objects locally so their CodeView locations can be
# authenticated to an actual PE/PDB pair on every host with lld-link. The ATL
# fixtures are analysis-only: unresolved platform calls are never executed.
if(LLD_LINK_EXEC AND CLANG_EXEC)
  set(_library_msvc_dir "${CMAKE_CURRENT_BINARY_DIR}/library-features/msvc")
  set(_library_msvc_archive "${CMAKE_SOURCE_DIR}/signatures/features/evidence/msvc-14.44.35207-x64-release.tar.gz")
  add_custom_command(
    OUTPUT "${_library_msvc_dir}/accessors-inline.obj"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_library_msvc_dir}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${_library_msvc_dir}"
      ${CMAKE_COMMAND} -E tar xzf "${_library_msvc_archive}"
    DEPENDS "${_library_msvc_archive}" VERBATIM)
  add_custom_command(
    OUTPUT "${_library_msvc_dir}/link-storage.obj"
    COMMAND "${CLANG_EXEC}" -target x86_64-pc-windows-msvc -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/library_link_storage.c"
      -o "${_library_msvc_dir}/link-storage.obj"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/library_link_storage.c"
      "${_library_msvc_dir}/accessors-inline.obj" VERBATIM)
  set(_library_msvc_outputs)
  # A real import thunk lets the PE loader authenticate the exception
  # personality without a Windows runtime implementation on the build host.
  foreach(_runtime vcruntime vcruntime_base)
    add_custom_command(
      OUTPUT "${_library_msvc_dir}/${_runtime}.lib"
      COMMAND "${LLD_LINK_EXEC}" /lib /machine:x64
        "/def:${CMAKE_CURRENT_SOURCE_DIR}/fixtures/library_${_runtime}.def"
        "/out:${_library_msvc_dir}/${_runtime}.lib"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/library_${_runtime}.def"
        "${_library_msvc_dir}/accessors-inline.obj" VERBATIM)
  endforeach()
  foreach(_profile accessors-inline accessors-negative-configuration atl_string-inline atl_com-inline)
    add_custom_command(
      OUTPUT "${_library_msvc_dir}/${_profile}.dll" "${_library_msvc_dir}/${_profile}.pdb"
      COMMAND "${LLD_LINK_EXEC}" /dll /noentry /nodefaultlib /force:unresolved
        /opt:noref /opt:noicf /debug:full "/out:${_profile}.dll" "/pdb:${_profile}.pdb" "${_profile}.obj" link-storage.obj vcruntime.lib vcruntime_base.lib
      WORKING_DIRECTORY "${_library_msvc_dir}"
      DEPENDS "${_library_msvc_dir}/accessors-inline.obj" "${_library_msvc_dir}/link-storage.obj" "${_library_msvc_dir}/vcruntime.lib" "${_library_msvc_dir}/vcruntime_base.lib" VERBATIM)
    list(APPEND _library_msvc_outputs "${_library_msvc_dir}/${_profile}.dll")
  endforeach()
  add_custom_target(neverd-library-msvc-fixtures DEPENDS ${_library_msvc_outputs})
  target_sources(NeverDLibraryRecognitionTests PRIVATE core/MsvcLibraryRecognitionTests.cpp)
  add_dependencies(NeverDLibraryRecognitionTests neverd-library-msvc-fixtures)
  target_compile_definitions(NeverDLibraryRecognitionTests PRIVATE
    NEVERD_LIBRARY_MSVC_FIXTURE_DIR="${_library_msvc_dir}")
  if(TARGET neverd AND Python3_Interpreter_FOUND)
    add_test(NAME NeverDLibraryFeatureMsvcCLI
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/library_cli_test.py"
        "$<TARGET_FILE:neverd>"
        "${_library_msvc_dir}/accessors-inline.dll"
        "${CMAKE_SOURCE_DIR}/signatures/features/rules/msvc-14.44.35207-x64-release-stl.json"
        "${CLANG_EXEC}" msvc-names)
    set_tests_properties(NeverDLibraryFeatureMsvcCLI PROPERTIES
      TIMEOUT 120 LABELS "unit;library-recognition;cli")
  endif()
else()
  message(STATUS "MSVC library consumer fixtures skipped: lld-link unavailable")
endif()

set(_library_libc_dir "${CMAKE_CURRENT_BINARY_DIR}/library-features/musl")
set(_library_libc_archive "${CMAKE_SOURCE_DIR}/signatures/features/evidence/musl-1.2.5-x64-clang22-memory.tar.gz")
add_custom_command(
  OUTPUT "${_library_libc_dir}/library-memory.elf" "${_library_libc_dir}/library-memory.truth.json"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_library_libc_dir}"
  COMMAND ${CMAKE_COMMAND} -E chdir "${_library_libc_dir}"
    ${CMAKE_COMMAND} -E tar xzf "${_library_libc_archive}"
  DEPENDS "${_library_libc_archive}" VERBATIM)
add_custom_target(neverd-library-libc-fixtures
  DEPENDS "${_library_libc_dir}/library-memory.elf" "${_library_libc_dir}/library-memory.truth.json")
target_sources(NeverDLibraryRecognitionTests PRIVATE core/LibcLibraryRecognitionTests.cpp)
add_dependencies(NeverDLibraryRecognitionTests neverd-library-libc-fixtures)
target_compile_definitions(NeverDLibraryRecognitionTests PRIVATE
  NEVERD_LIBRARY_LIBC_FIXTURE_DIR="${_library_libc_dir}")

add_neverd_unittest(NeverDX86FPStateAccuracyTests
  x86_64/X86_64_FPStateAccuracyTests.cpp
  LINK_COMPONENTS Pipeline BackendC CodeGen)
target_include_directories(NeverDX86FPStateAccuracyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/unittests)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/x86_64/X86FPStateTestExecution.cmake")

add_neverd_unittest(NeverDX86FPConversionAccuracyTests
  x86_64/X86_64_FPConversionAccuracyTests.cpp
  LINK_COMPONENTS Pipeline BackendC CodeGen)
target_include_directories(NeverDX86FPConversionAccuracyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/unittests)
add_neverd_unittest(NeverDX86EncodingAccuracyTests
  x86_64/X86_64_EncodingAccuracyTests.cpp
  LINK_COMPONENTS Decode Lift Loader)

add_neverd_unittest(NeverDX86SIMDAccuracyTests
  x86_64/X86_64_SIMDAccuracyTests.cpp
  LINK_COMPONENTS Pipeline BackendC CodeGen)
target_include_directories(NeverDX86SIMDAccuracyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/unittests)

add_neverd_unittest(NeverDX86SourceAccuracyTests
  x86_64/X86_64_SourceAccuracyTests.cpp
  LINK_COMPONENTS Pipeline BackendC CodeGen)
target_include_directories(NeverDX86SourceAccuracyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/unittests)

add_neverd_unittest(NeverDX86StateAccuracyTests
  x86_64/X86_64_StateAccuracyTests.cpp
  LINK_COMPONENTS Pipeline BackendC CodeGen)
target_include_directories(NeverDX86StateAccuracyTests PRIVATE
  ${CMAKE_SOURCE_DIR}/unittests)
