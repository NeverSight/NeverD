add_neverd_component_library(NeverDPassIR
  HelloWorldPass.cpp
  InstSubstitutionPass.cpp
  ConstantEncryptionPass.cpp
  OpaquePredicatePass.cpp
  ControlFlowFlatteningPass.cpp
  BogusControlFlowPass.cpp
  IndirectBranchPass.cpp
  IndirectCallPass.cpp
  MBAPass.cpp
  IndirectGlobalPass.cpp
  ValueLaunderingPass.cpp
  ConstantPoolingPass.cpp
  BitMaskingPass.cpp

  LINK_COMPONENTS
  IR
)
