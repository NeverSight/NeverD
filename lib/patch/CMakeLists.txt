include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDPerformance.cmake)
neverd_add_simdjson()

add_neverd_component_library(NeverDPatch
  PatchOverlay.cpp
  PatchJSON.cpp
  PatchExport.cpp
  PatchActions.cpp
  PatchAssembler.cpp
  LINK_COMPONENTS Loader Support Decode
  LLVM_COMPONENTS Object Support MC MCParser X86AsmParser X86Desc X86Info
                  ARMAsmParser ARMDesc ARMInfo AArch64AsmParser AArch64Desc AArch64Info
)

if(NEVERD_ENABLE_SIMDJSON)
  target_link_libraries(NeverDPatch PRIVATE simdjson)
  target_compile_definitions(NeverDPatch PRIVATE NEVERD_USE_SIMDJSON=1)
endif()
