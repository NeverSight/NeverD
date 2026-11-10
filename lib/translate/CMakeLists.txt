add_neverd_component_library(NeverDTranslate
  GuestMemoryRuntime.cpp
  GuestState.cpp
  NativeTranslationSession.cpp
  ResolvedHostTarget.cpp
  RuntimeABI.cpp
  RuntimeCodeMemory.cpp
  RuntimeGuestState.cpp
  RuntimeHelpers.cpp
  RuntimeSymbolRegistry.cpp
  X86/RuntimeX87GuestState.cpp
  X86/RuntimeX87Helpers.cpp
  TranslationOptions.cpp
  TranslationResult.cpp
  TranslationSession.cpp
  TranslationIRVerifier.cpp
  TranslationArtifactVerifier.cpp
  TranslationJITLinker.cpp
  TranslationLinkGraphVerifier.cpp
  TranslationCacheIdentity.cpp
  TranslationObjectCompiler.cpp
  TranslationObjectRequest.cpp
  TranslationTargetMachine.cpp
  X86/X86TranslationBlockBuilder.cpp
  X86/X86TranslationBlockLowerer.cpp
  X86/X86Paging.cpp
  X86/X86PagedMemory.cpp
  X86/X86PortIO.cpp
  X86/X86LegacyPortIO.cpp
  X86/X86LegacySegmentExecution.cpp
  X86/X86LegacyExecution.cpp
  X86/X86LegacyControlRegisters.cpp
  X86/X86LegacyTaskSwitch.cpp
  X86/X86LegacyMpx.cpp
  X86/X86LegacySourceExecution.cpp
  X86/X86LegacyScalarEncoding.cpp
  X86/X86LegacyBMI.cpp
  X86/X86LegacyDescriptorQuery.cpp
  X86/X86LegacySystemSelector.cpp
  X86/X86LegacyScalarExecution.cpp
  X86/X86LegacyStackExecution.cpp
  X86/X86LegacyString.cpp
  X86/X86LegacyEventDelivery.cpp
  X86/X86LegacyFrameReturn.cpp
  X86/X86LegacyFarTransfer.cpp
  X86/X87PortableRuntime.cpp

  LINK_COMPONENTS
  Decode
  PassIR

  LINK_LIBS
  ${NEVERD_LLVM_LINKGRAPH_LIBS}
)

# The generated-code ABI exports only its exact C helper table. Its C++ host
# context is an internal embedding contract and must match this source build.
add_library(neverd_x87_runtime SHARED X86/X87PortableExecution.cpp)
target_link_libraries(neverd_x87_runtime PRIVATE NeverDTranslate)
target_compile_definitions(neverd_x87_runtime PRIVATE NEVERD_X87_RUNTIME_EXPORTS)
target_compile_features(neverd_x87_runtime PRIVATE cxx_std_20)
set_target_properties(neverd_x87_runtime PROPERTIES
  OUTPUT_NAME neverd_x87_runtime_v1
  FOLDER "NeverD/Libraries"
  WINDOWS_EXPORT_ALL_SYMBOLS OFF
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

set(_x87_abi_table "${CMAKE_SOURCE_DIR}/include/neverd/ir/X86/X87PortableABI.def")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_x87_abi_table}")
file(READ "${_x87_abi_table}" _x87_abi_text)
string(REGEX MATCHALL "nvd_x87_v1_[a-z0-9_]+" _x87_exports "${_x87_abi_text}")
list(REMOVE_DUPLICATES _x87_exports)
if(APPLE)
  set(_x87_export_file "${CMAKE_CURRENT_BINARY_DIR}/x87-runtime.exports")
  set(_x87_export_text "")
  foreach(_name IN LISTS _x87_exports)
    string(APPEND _x87_export_text "_${_name}\n")
  endforeach()
  file(WRITE "${_x87_export_file}" "${_x87_export_text}")
  target_link_options(neverd_x87_runtime PRIVATE
    "LINKER:-exported_symbols_list,${_x87_export_file}")
elseif(UNIX)
  set(_x87_export_file "${CMAKE_CURRENT_BINARY_DIR}/x87-runtime.map")
  set(_x87_export_text "{ global:\n")
  foreach(_name IN LISTS _x87_exports)
    string(APPEND _x87_export_text "  ${_name};\n")
  endforeach()
  string(APPEND _x87_export_text "local: *; };\n")
  file(WRITE "${_x87_export_file}" "${_x87_export_text}")
  target_link_options(neverd_x87_runtime PRIVATE
    "LINKER:--version-script,${_x87_export_file}")
endif()
if(DEFINED _x87_export_file)
  set_property(TARGET neverd_x87_runtime APPEND PROPERTY LINK_DEPENDS
    "${_x87_export_file}")
endif()
