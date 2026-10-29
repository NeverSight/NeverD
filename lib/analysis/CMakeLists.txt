add_subdirectory(core)
add_subdirectory(bytecode)

add_neverd_component_library(NeverDLLVMMemoryAnalysis
  llvm/LLVMMemoryAnalysis.cpp
  llvm/LLVMPrivateFrameProjection.cpp
  LLVM_COMPONENTS Analysis Core Support
)

add_neverd_component_library(NeverDBinaryInterpreterSpecialization
  arch/x86_64/BinaryInterpreterSpecialization.cpp
  arch/x86_64/X64ContinuationSource.cpp
  LINK_COMPONENTS InterpreterSpecialization Decode Loader
)

add_neverd_component_library(NeverDLLVMInterpreterModel
  llvm/LLVMInterpreterModel.cpp
  arch/x86_64/LLVMInterpreterMachineState.cpp
  llvm/LLVMInterpreterModelContract.cpp
  llvm/LLVMInterpreterModelControl.cpp
  llvm/LLVMInterpreterModelInitialization.cpp
  llvm/LLVMInterpreterModelMemory.cpp
  llvm/LLVMInterpreterModelScalar.cpp
  llvm/LLVMScalarEquivalence.cpp
  LINK_COMPONENTS InterpreterSpecialization
  LLVM_COMPONENTS Core Support
)

add_neverd_component_library(NeverDLLVMScalarLoopRecovery
  llvm/LLVMScalarLoopRecovery.cpp
  llvm/LLVMScalarSourceRecovery.cpp
  llvm/LLVMScalarLoopPredicates.cpp
  llvm/LLVMScalarLoopBounds.cpp
  llvm/LLVMScalarLoopUnpeeling.cpp
  llvm/LLVMScalarLoopRegions.cpp
  llvm/LLVMScalarLoopAffine.cpp
  llvm/LLVMScalarLoopReplacements.cpp
  llvm/LLVMScalarLoopSeeds.cpp
  llvm/LLVMScalarLoopMasks.cpp
  llvm/LLVMScalarLoopWidths.cpp
  LINK_COMPONENTS LLVMInterpreterModel PassIR
  LLVM_COMPONENTS Analysis Core Support TransformUtils Passes InstCombine
)

add_neverd_component_library(NeverDLLVMScalarResultProjection
  llvm/LLVMScalarResultProjection.cpp
  LINK_COMPONENTS LLVMInterpreterModel
  LLVM_COMPONENTS Core Support TransformUtils
)

add_neverd_component_library(NeverDLLVMScalarStateProjection
  arch/x86_64/LLVMScalarStateProjection.cpp
  llvm/LLVMScalarStateProjection.cpp
  LINK_COMPONENTS LLVMScalarResultProjection
  LLVM_COMPONENTS Analysis Core Support TransformUtils
)

add_neverd_component_library(NeverDLLVMScalarInputProjection
  llvm/LLVMScalarInputProjection.cpp
  LINK_COMPONENTS LLVMScalarResultProjection
  LLVM_COMPONENTS Core Support
)

add_neverd_component_library(NeverDInterpreterLLVMRefinement
  arch/x86_64/InterpreterLLVMPreparation.cpp
  arch/x86_64/InterpreterLLVMRefinement.cpp
  LINK_COMPONENTS BinaryInterpreterSpecialization LLVMInterpreterModel
  LLVM_COMPONENTS AsmParser Core Support
)
