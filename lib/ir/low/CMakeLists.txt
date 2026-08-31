add_neverd_component_library(NeverDIRLowValidation
  LowIR.cpp
)

add_neverd_component_library(NeverDIRLow
  CFGBuilder.cpp
  CFGBuilderBlocks.cpp
  CFGBuilderException.cpp
  CFGBuilderInsn.cpp
  CFGBuilderX86Fpu.cpp
  CircleRange.cpp
  FuncDetector.cpp
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
  X86ApproxReference.c
  X86ApproxReference28.c
  jumptable/JumpTableResolver.cpp
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
  IRLowValidation
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
