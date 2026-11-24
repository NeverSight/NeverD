# Keep the link libraries and runtime registration on one backend inventory.
# A prebuilt LLVM package can contain more targets than NeverD uses.
set(_neverd_llvm_target_table
  "${CMAKE_CURRENT_LIST_DIR}/../include/neverd/support/LLVMCodegenTargets.def")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_neverd_llvm_target_table}")
set(_neverd_llvm_target_pattern
  "^NEVERD_LLVM_CODEGEN_TARGET\\(([A-Za-z0-9]+), (Required|IfAvailable), .+\\)$")
file(STRINGS "${_neverd_llvm_target_table}" _neverd_llvm_target_rows
  REGEX "^NEVERD_LLVM_CODEGEN_TARGET\\(")
set(NEVERD_LLVM_CODEGEN_TARGETS)
set(NEVERD_LLVM_TARGET_COMPONENTS)
set(NEVERD_LLVM_TARGET_LIBRARIES)
set(_neverd_llvm_selected_rows "")
foreach(_row IN LISTS _neverd_llvm_target_rows)
  if(NOT _row MATCHES "${_neverd_llvm_target_pattern}")
    message(FATAL_ERROR "${_neverd_llvm_target_table}: ${_row}")
  endif()
  set(_target "${CMAKE_MATCH_1}")
  if(NEVERD_LLVM_PREBUILT AND CMAKE_MATCH_2 STREQUAL "IfAvailable"
     AND NOT _target IN_LIST LLVM_TARGETS_TO_BUILD
     AND NOT NEVERD_LLVM_PREBUILT_TAG STREQUAL NEVERD_LLVM_PREBUILT_PINNED_TAG)
    continue()
  endif()
  string(APPEND _neverd_llvm_selected_rows "${_row}\n")
  list(APPEND NEVERD_LLVM_CODEGEN_TARGETS "${_target}")
  string(TOLOWER "${_target}" _component)
  list(APPEND NEVERD_LLVM_TARGET_COMPONENTS
    "${_component}codegen" "${_component}asmparser"
    "${_component}desc" "${_component}info")
  list(APPEND NEVERD_LLVM_TARGET_LIBRARIES
    "LLVM${_target}CodeGen" "LLVM${_target}AsmParser"
    "LLVM${_target}Desc" "LLVM${_target}Info")
endforeach()

set(NEVERD_LLVM_TARGET_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/llvm-targets")
file(GENERATE OUTPUT "${NEVERD_LLVM_TARGET_INCLUDE_DIR}/LLVMCodegenTargets.inc"
  CONTENT "${_neverd_llvm_selected_rows}")
