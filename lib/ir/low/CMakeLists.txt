add_neverd_component_library(NeverDIRLowValidation
  LowIR.cpp
  LowNoReturn.cpp
  LowPreservedState.cpp
  LowUndefinedEffects.cpp
  SEHFrameProof.cpp
  X87PartialRemainder.cpp

  LINK_COMPONENTS
  Digest
)

add_neverd_component_library(NeverDIRRegistrationState
  X86/RegistrationState.cpp
  X86/RegistrationStateCalls.cpp
  X86/RegistrationStateCxx.cpp
  X86/RegistrationStateCallback.cpp
  X86/RegistrationStateContracts.cpp
  X86/RegistrationStateCookies.cpp
  X86/RegistrationStateDispatch.cpp
  X86/RegistrationStateLocalUnwind.cpp
  X86/RegistrationStateOccurrences.cpp
  X86/RegistrationStateRealignment.cpp
  X86/RegistrationStateInstallation.cpp
  X86/RegistrationStateResult.cpp
  X86/RegistrationStateSearch.cpp
  X86/RegistrationStateTransfer.cpp
  X86/RegistrationStateUnwind.cpp
  X86/RegistrationFrame.cpp
  X86/RegistrationFrameCallback.cpp
  X86/RegistrationFrameRealignment.cpp
)

add_neverd_component_library(NeverDIRLow
  CFGBuilder.cpp
  CFGBuilderBlocks.cpp
  CFGBuilderException.cpp
  CFGBuilderInsn.cpp
  CFGBuilderX86Fpu.cpp
  CFGBuilderX86GetPc.cpp
  CallRegisterEffects.cpp
  CircleRange.cpp
  FuncDetector.cpp
  FuncDetectorX86.cpp
  FuncDetectorARM.cpp
  FuncDetectorAArch64.cpp
  IndirectTailFrame.cpp
  X86/RegistrationCalleeABI.cpp
  X86/RegistrationCalleeFrame.cpp
  X86/RegistrationCalleeIndex.cpp
  X86/RegistrationCalleeStack.cpp
  X86/RegistrationCallerABI.cpp
  NdOpEmulator.cpp
  NdOpEmulatorExec.cpp
  NdOpEmulatorFloatConvert.cpp
  NdOpEmulatorX86FPArith.cpp
  NdOpEmulatorX86FPConvert.cpp
  NdOpEmulatorX86FPRoundTransform.cpp
  NdOpEmulatorX86FPExtract.cpp
  NdOpEmulatorX86FPRange.cpp
  NdOpEmulatorX86FPFixup.cpp
  NdOpEmulatorX86FPScale.cpp
  NdOpEmulatorX86FPCompare.cpp
  NdOpEmulatorX87.cpp
  PlatformEvidence.cpp
  X86ApproxReference.c
  X86ApproxReference28.c
  jumptable/JumpTableResolver.cpp
  jumptable/JumpTableProofState.cpp
  jumptable/JumpTableResolverGroup.cpp
  jumptable/JumpTableResolverARM.cpp
  jumptable/JumpTableResolverBounds.cpp
  jumptable/JumpTableResolverEmu.cpp
  jumptable/JumpTableResolverExtract.cpp
  jumptable/JumpTableResolverFold.cpp
  jumptable/JumpTableResolverGuardAlias.cpp
  jumptable/JumpTableResolverGuardCFG.cpp
  jumptable/JumpTableResolverGuardRange.cpp
  jumptable/JumpTableResolverGuards.cpp
  jumptable/JumpTableResolverInterior.cpp
  jumptable/JumpTableResolverNorm.cpp
  jumptable/JumpTableResolverShapes.cpp
  jumptable/JumpTableResolverSlice.cpp
  jumptable/JumpTableResolverSource.cpp
  jumptable/JumpTableResolverStack.cpp
  jumptable/JumpTableResolverTargets.cpp
  jumptable/JumpTableResolverTwoLevel.cpp

  LINK_COMPONENTS
  Decode
  IRLowValidation
  IRRegistrationState
  LibC
  Loader
  Solver
  Symbolic
)

# The public approximation reference uses deliberate C pointer type-punning,
# and its documented intermediate rounding assumes separate operations.
if(MSVC)
  set_source_files_properties(X86ApproxReference.c X86ApproxReference28.c
    PROPERTIES
    COMPILE_OPTIONS "/fp:strict")
else()
  set_source_files_properties(X86ApproxReference.c X86ApproxReference28.c
    PROPERTIES
    COMPILE_OPTIONS "-fno-strict-aliasing;-ffp-contract=off")
endif()
