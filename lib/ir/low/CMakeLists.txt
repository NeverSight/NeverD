add_neverd_component_library(NeverDIRLow
  CFGBuilder.cpp
  CFGBuilderException.cpp
  CFGBuilderX86Fpu.cpp
  CircleRange.cpp
  FuncDetector.cpp
  JumpTableResolver.cpp
  JumpTableResolverARM.cpp
  JumpTableResolverEmu.cpp
  JumpTableResolverGuards.cpp
  JumpTableResolverShapes.cpp
  JumpTableResolverSource.cpp
  JumpTableResolverStack.cpp
  NdOpEmulator.cpp

  LINK_COMPONENTS
  Symbolic
)
