add_neverd_component_library(NeverDLLVMCallContract
  llvm/LLVMCallContract.cpp

  LINK_COMPONENTS
  IR
)

add_neverd_component_library(NeverDLLVMSourceTypeHints
  llvm/LLVMSourceTypeHints.cpp

  LINK_COMPONENTS
  IR
)

add_subdirectory(c)
add_subdirectory(codegen)
add_subdirectory(llvm)
add_subdirectory(swift)
