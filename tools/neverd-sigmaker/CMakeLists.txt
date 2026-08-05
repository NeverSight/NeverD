add_neverd_tool(neverd-sigmaker
  neverd-sigmaker.cpp

  LINK_COMPONENTS
  Sigs
)

# neverd-sigmaker uses llvm::object::* (Archive, ObjectFile, etc.)
# which lives in LLVMObject + its deps.
if(TARGET LLVMObject)
  target_link_libraries(neverd-sigmaker PRIVATE LLVMObject LLVMMCParser
    LLVMMC LLVMBinaryFormat LLVMTargetParser LLVMCore LLVMBitReader
    LLVMTextAPI)
else()
  llvm_map_components_to_libnames(_sigmaker_llvm_libs object mcparser mc
    binaryformat targetparser core bitreader textapi)
  target_link_libraries(neverd-sigmaker PRIVATE ${_sigmaker_llvm_libs})
endif()
