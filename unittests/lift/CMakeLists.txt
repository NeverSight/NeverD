add_neverd_unittest(NeverDLiftTests
  aarch64/AArch64_ArithTests.cpp
  aarch64/AArch64_AtomicTests.cpp
  aarch64/AArch64_BitShiftTests.cpp
  aarch64/AArch64_CarryTests.cpp
  aarch64/AArch64_CallAbiTests.cpp
  aarch64/AArch64_CondTests.cpp
  aarch64/AArch64_CSelTests.cpp
  aarch64/AArch64_ExtendTests.cpp
  aarch64/AArch64_FPTests.cpp
  aarch64/AArch64_IntrinsicsTests.cpp
  aarch64/AArch64_LogicTests.cpp
  aarch64/AArch64_MemTests.cpp
  aarch64/AArch64_MopsTests.cpp
  aarch64/AArch64_MulDivTests.cpp
  aarch64/AArch64_PipelineE2ETests.cpp
  aarch64/AArch64_FastClassifyParityTests.cpp
  aarch64/AArch64_NativeDecodeParityTests.cpp
  aarch64/AArch64_NativeDecodeParityControlTests.cpp
  aarch64/AArch64_NativeDecodeParityFPTests.cpp
  aarch64/AArch64_NativeDecodeParityLoadStoreTests.cpp
  aarch64/AArch64_NativeDecodeParitySweepTests.cpp
  arm32/ARM32_ArithTests.cpp
  arm32/ARM32_CarryTests.cpp
  arm32/ARM32_ControlTests.cpp
  arm32/ARM32_ExtendTests.cpp
  arm32/ARM32_IntrinsicsTests.cpp
  arm32/ARM32_LogicTests.cpp
  arm32/ARM32_MemTests.cpp
  arm32/ARM32_MulDivTests.cpp
  arm32/ARM32_PipelineE2ETests.cpp
  arm32/ARM32_ShiftTests.cpp
  x86_32/X86_32_ArithTests.cpp
  x86_32/X86_32_BitOpsTests.cpp
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
  x86_32/X86_32_PipelineE2ETests.cpp
  x86_32/X86_32_RotateTests.cpp
  x86_32/X86_32_SetccTests.cpp
  x86_32/X86_32_SignExtTests.cpp
  x86_32/X86_32_StackTests.cpp
  x86_32/X86_32_StringTests.cpp
  x86_32/X86_32_X87FPUTests.cpp
  x86_64/X86_64_AdxMiscTests.cpp
  x86_64/X86_64_ArithTests.cpp
  x86_64/X86_64_BitOpsTests.cpp
  x86_64/X86_64_BMITests.cpp
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
  x86_64/X86_64_StringOpsTests.cpp
  x86_64/X86_64_X87FPUTests.cpp
  x86_64/X86_64_SwitchTests.cpp
  x86_64/X86_64_RoundTripTests.cpp
  aarch64/AArch64_SwitchTests.cpp
  aarch64/AArch64_RoundTripTests.cpp
  arm32/ARM32_SwitchTests.cpp
  arm32/ARM32_RoundTripTests.cpp
  core/InstructionModeTests.cpp
  core/LowInstructionBoundaryTests.cpp
  core/LiftCommonTests.cpp
  format/MachOI386RelocationTests.cpp
  format/MachOI386RelocationExternalTests.cpp
  format/MachOI386RelocationFixtureTests.cpp
  format/MachOI386RelocationPipelineTests.cpp
  format/MachOI386RelocationScatteredTests.cpp
  core/MedCallingConvTests.cpp
  eh/COFFExceptionTests.cpp
  eh/COFFExceptionARM32UnwindTests.cpp
  eh/COFFExceptionARM64UnwindTests.cpp
  eh/COFFExceptionDelphiTests.cpp
  eh/COFFExceptionFH3Tests.cpp
  eh/COFFExceptionFH4Tests.cpp
  eh/COFFExceptionGSTests.cpp
  eh/COFFExceptionIRTests.cpp
  eh/COFFExceptionNativeIRTests.cpp
  eh/COFFExceptionPatchTests.cpp
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
  format/PatchFormatTests.cpp
  format/PatchFormatCOFFARMTests.cpp
  format/PatchFormatCOFFTests.cpp
  format/PatchFormatSectionTests.cpp
  format/PatchFormatXCallTests.cpp
  format/RuntimeMetadataTests.cpp
  format/TextSectionDetectionTests.cpp
)

target_compile_definitions(NeverDLiftTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  TEST_OBJ_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

target_include_directories(NeverDLiftTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/loader/COFF/unwind
  ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(NeverDLiftTests PRIVATE
  NeverDBackendC
  NeverDBackendLLVM
  NeverDCodeGen
  NeverDDecode
  NeverDIRLow
  NeverDLibC
  NeverDLoader
  NeverDLift
  NeverDPipeline
  NeverDSupport
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

target_include_directories(NeverDLLVMCVoidAnalysisTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Pins the source-EH-to-native-rewrite contract without relinking the large
# lift suite. The emitter creates the deliberately unlowerable source contract,
# and the two format installers are the public fail-closed boundary under test.
add_neverd_unittest(NeverDNativeExceptionRewriteContractTests
  eh/DwarfEHFrameTests.cpp
  eh/MachOCompactUnwindPatchTests.cpp
  eh/MachOExceptionPatchTests.cpp
  eh/NativeExceptionRewriteContractTests.cpp
  eh/RewriteFunctionProvenanceTests.cpp

  LINK_COMPONENTS
  BackendLLVM
  CodeGen
  LibC
  Lift
)

target_include_directories(NeverDNativeExceptionRewriteContractTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Guards the obfuscate-then-patch barrier in isolation.  A tiny TU that links
# the prebuilt NeverDPipeline archive (which carries SymSimplifyPass through its
# PassIR component), so the stamp/skip contract can be pinned without rebuilding
# or relinking the ~100-TU NeverDLiftTests binary.
add_neverd_unittest(NeverDSymSimplifyGuardTests
  pipeline/SymSimplifyGuardTests.cpp
  LINK_COMPONENTS
  Pipeline
)
target_include_directories(NeverDSymSimplifyGuardTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/pipeline
  ${LLVM_INCLUDE_DIRS}
)

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
    corpus/windows/WindowsEHCorpusManifest.cpp
    TIMEOUT 300
  )

  target_compile_definitions(NeverDWindowsEHCorpusTests PRIVATE
    NEVERD_BINARY_CORPUS_ROOT="${NEVERD_BINARY_CORPUS_ROOT}"
  )

  target_include_directories(NeverDWindowsEHCorpusTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
  )

  target_link_libraries(NeverDWindowsEHCorpusTests PRIVATE
    NeverDLoader
    NeverDSupport
  )
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
endif()

# --- Cross-compile test .c sources into .o objects at build time ---
find_program(CLANG_EXEC clang)

set(X86_64_TEST_SOURCES
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
  x86_64/test_jumptable_enhanced.c
  x86_64/test_twolevel_switch.c
  x86_64/test_jumptable_masked_abs.c
  x86_64/test_jumptable_worldclass.c
  x86_64/test_jumptable_cgoto.c
  x86_64/test_global_volatile.c
)

set(X86_32_TEST_SOURCES
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
  aarch64/test_param_phi_a64.s
  aarch64/test_arith_a64.c
  aarch64/test_cond_a64.c
  aarch64/test_mem_a64.c
  aarch64/test_bitshift_a64.c
  aarch64/test_fp_a64.c
  aarch64/test_logic_a64.c
  aarch64/test_muldiv_a64.c
  aarch64/test_csel_a64.c
  aarch64/test_carry_a64.c
  aarch64/test_extend_a64.c
  aarch64/test_atomic_a64.c
  aarch64/test_mops_a64.c
  aarch64/test_intrinsics_a64.c
  aarch64/test_switch_a64.c
  aarch64/test_roundtrip_a64.c
  aarch64/test_jumptable_enhanced_a64.c
  aarch64/test_jumptable_worldclass_a64.c
)

set(ARM32_TEST_SOURCES
  arm32/test_frame_pointer_void_arm.s
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
  list(APPEND TEST_OBJECTS ${call_abi_a64_macho_exe})
endif()

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
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(x86_32_extra_flags)
  if(src MATCHES "test_fp32")
    set(x86_32_extra_flags -msse2)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target i386-linux-gnu -m32 -nostdlib -static
            -O0 -fno-stack-protector -fno-pie ${x86_32_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (i386 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

foreach(src ${AARCH64_TEST_SOURCES})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(a64_extra_flags)
  if(src MATCHES "test_switch|test_jumptable")
    set(a64_extra_flags -O1)
  elseif(src MATCHES "test_atomic_a64")
    set(a64_extra_flags -march=armv9.4-a+lse128)
  elseif(src MATCHES "test_mops_a64")
    set(a64_extra_flags -march=armv8.8-a+mops)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target aarch64-linux-gnu -nostdlib -static
            -O0 -fno-stack-protector ${a64_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
    COMMENT "Compiling ${src} -> ${base}.o (AArch64 ELF)"
  )
  list(APPEND TEST_OBJECTS ${obj})
endforeach()

foreach(src ${ARM32_TEST_SOURCES})
  get_filename_component(base ${src} NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${base}.o")
  set(arm_extra_flags)
  if(src MATCHES "test_switch|test_jumptable")
    set(arm_extra_flags -O1)
  endif()
  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CLANG_EXEC} -target arm-linux-gnueabi -march=armv7-a -nostdlib -static
            -O0 -fno-stack-protector -marm ${arm_extra_flags} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
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

  set(coff_arm_obj "${CMAKE_CURRENT_BINARY_DIR}/test_patch_coff_arm.obj")
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
    OUTPUT ${coff_arm_exe}
    COMMAND ${LLD_LINK_EXEC} /entry:mainCRTStartup /subsystem:console
            /nodefaultlib /export:pe_leaf /export:pe_stacky /export:pe_tiny
            /export:pe_after_tiny ${coff_arm_obj} ${coff_arm_tiny_obj}
            /out:${coff_arm_exe}
    DEPENDS ${coff_arm_obj} ${coff_arm_tiny_obj}
    COMMENT "Linking ARM32 COFF objects -> test_patch_coff_arm.exe")

  list(APPEND TEST_OBJECTS
    ${coff_a64_obj} ${coff_a64_exe}
    ${coff_arm_obj} ${coff_arm_tiny_obj} ${coff_arm_exe})
endif()

# --- Link into ELF executables for patch testing ---
find_program(LLD_EXEC ld.lld)
set(ELF_EXECUTABLES "")
if(LLD_EXEC)
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
