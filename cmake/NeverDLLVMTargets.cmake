# Keep the link libraries and runtime registration on one backend inventory.
# A prebuilt LLVM package can contain more targets than NeverD uses.
set(_neverd_llvm_target_table
  "${CMAKE_CURRENT_LIST_DIR}/../include/neverd/support/LLVMCodegenTargets.def")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_neverd_llvm_target_table}")
file(STRINGS "${_neverd_llvm_target_table}" _neverd_llvm_target_rows
  REGEX "^NEVERD_LLVM_CODEGEN_TARGET\\([A-Za-z0-9]+\\)$")
set(NEVERD_LLVM_CODEGEN_TARGETS)
set(NEVERD_LLVM_TARGET_COMPONENTS)
set(NEVERD_LLVM_TARGET_LIBRARIES)
foreach(_row IN LISTS _neverd_llvm_target_rows)
  string(REGEX REPLACE "^NEVERD_LLVM_CODEGEN_TARGET\\(([A-Za-z0-9]+)\\)$"
    "\\1" _target "${_row}")
  list(APPEND NEVERD_LLVM_CODEGEN_TARGETS "${_target}")
  string(TOLOWER "${_target}" _component)
  list(APPEND NEVERD_LLVM_TARGET_COMPONENTS
    "${_component}codegen" "${_component}asmparser"
    "${_component}desc" "${_component}info")
  list(APPEND NEVERD_LLVM_TARGET_LIBRARIES
    "LLVM${_target}CodeGen" "LLVM${_target}AsmParser"
    "LLVM${_target}Desc" "LLVM${_target}Info")
endforeach()
