add_neverd_component_library(NeverDIRRegisters
  TargetRegInfo.cpp
  AArch64/TargetRegInfoAArch64.cpp
  ARM/TargetRegInfoARM.cpp
  MIPS/TargetRegInfoMIPS.cpp
  X86/TargetRegInfoX86.cpp
  ../lift/AArch64/AArch64Regs.cpp
  ../lift/ARM/ARMRegs.cpp
  ../lift/MIPS/MIPSRegs.cpp
  ../lift/X86/X86Regs.cpp

  LLVM_COMPONENTS
  Support
)

add_subdirectory(intrinsics)
add_subdirectory(low)
add_subdirectory(med)
add_subdirectory(high)

add_neverd_component_library(NeverDIR
  NdTypes.cpp
  IntegerCallArguments.cpp
  FloatingCallArguments.cpp
  RegisterFlow.cpp
  SourceABI.cpp
  SourceCallABI.cpp
  SourceStackFrame.cpp
  SourceVariableFlow.cpp
  SourceParameterPlacement.cpp
  MIPS/SourceParameterPlacementMIPS.cpp
  AArch64/SourceParameterPlacementAAPCS64.cpp
  X86/SourceParameterPlacementI386.cpp
  X86/SourceParameterPlacementSysV.cpp
  X86/SourceParameterPlacementWin64.cpp
  low/InternalNoReturn.cpp
  low/SourceFrameAnalysis.cpp
  low/SourceFrameBytes.cpp
  low/ImportCallee.cpp

  LINK_COMPONENTS
  IRRegisters
  IRHigh
  IRMed
  IRLow
  IRIntrinsics
)

# HighIRPrint calls NdType::str(), which is implemented by the aggregate
# NeverDIR archive.  Record that static-library cycle explicitly so CMake
# repeats the archives for one-pass linkers such as GNU ld.
target_link_libraries(NeverDIRHigh PUBLIC NeverDIR)

# IRHigh's selector-stub query can first extract Loader's call-hint object on
# the second scan. That object then needs SourceABI from the preceding IR
# archive, so one-pass archive linkers need a third scan of this component.
set_property(TARGET NeverDIR PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
