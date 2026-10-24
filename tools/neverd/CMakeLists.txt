if(NOT TARGET LLVMSwiftDemangle)
  message(FATAL_ERROR
    "The selected LLVM lacks LLVMSwiftDemangle. Update the NeverD LLVM fork "
    "and its matching prebuilt package, or use -DNEVERD_LLVM_PREBUILT=OFF "
    "with the current fork sources.")
endif()

include(${CMAKE_SOURCE_DIR}/cmake/NeverDZlib.cmake)
add_library(NeverDMobile STATIC
  mobile/MobileCommon.cpp
  mobile/MobileArchive.cpp
  mobile/MobileProcess.cpp
  mobile/MobileRecovery.cpp
  mobile/MobileAndroid.cpp
  mobile/MobileAndroidInventory.cpp
  mobile/MobileAndroidReferences.cpp
  mobile/MobileDalvikSmali.cpp
  mobile/MobileDalvikJava.cpp
  mobile/MobileIOS.cpp
  mobile/MobileIOSMetadata.cpp
  mobile/MobileIOSPlist.cpp
  mobile/MobileIOSObjC.cpp
  mobile/MobileIOSSwiftSignatures.cpp
  mobile/MobileIOSSwift.cpp
)
target_compile_features(NeverDMobile PUBLIC cxx_std_20)
target_include_directories(NeverDMobile PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/mobile
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS})
target_include_directories(NeverDMobile PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/sdk)
target_link_libraries(NeverDMobile PUBLIC ZLIB::ZLIB neverd_shared)
if(APPLE)
  # Resolve LLVM Support through libneverd's existing registry. Linking the
  # component's archive directly avoids pulling a second static Support into
  # the CLI through the LLVM component's transitive link requirements.
  target_link_libraries(NeverDMobile PRIVATE "$<TARGET_FILE:LLVMSwiftDemangle>"
    "$<TARGET_FILE:NeverDDalvik>")
  add_dependencies(NeverDMobile NeverDDalvik)
  add_dependencies(NeverDMobile LLVMSwiftDemangle)
else()
  target_link_libraries(NeverDMobile PRIVATE LLVMSwiftDemangle NeverDDalvik)
endif()
if(NOT APPLE)
  # Internal loader symbols are not exported by the Windows C ABI DLL.
  # macOS resolves these through libneverd, preserving one LLVM registry.
  target_link_libraries(NeverDMobile PUBLIC NeverDLoader NeverDSupport)
endif()

add_neverd_tool(neverd
  neverd.cpp
  NeverDCLIOptions.cpp
  NeverDCLIFunctions.cpp
  NeverDSymbolDownload.cpp
  NeverDTranslateOutput.cpp
  ExportTable.cpp
  cmd/NeverDCmdInfo.cpp
  cmd/NeverDCmdTables.cpp
  cmd/NeverDCmdTypes.cpp
  cmd/NeverDCmdTypeFit.cpp
  cmd/NeverDCmdStrings.cpp
  cmd/NeverDCmdPaths.cpp
  cmd/NeverDCmdXrefs.cpp
  cmd/NeverDCmdDisasm.cpp
  cmd/NeverDCmdMarkup.cpp
  cmd/NeverDCmdSearch.cpp
  cmd/NeverDCmdExport.cpp
  cmd/NeverDCmdDiff.cpp
  cmd/NeverDCmdPipeline.cpp
  cmd/NeverDCmdSimplify.cpp
  cmd/NeverDCmdProject.cpp
  cmd/NeverDCmdOptimizeIR.cpp
  cmd/NeverDCmdTranslate.cpp
  cmd/NeverDCmdEmulation.cpp
  cmd/NeverDCmdCPU.cpp
  cmd/NeverDCmdModules.cpp
  cmd/NeverDCmdExecution.cpp
  cmd/NeverDCmdProcess.cpp
  cmd/NeverDCmdUnpack.cpp
  cmd/NeverDCmdSafety.cpp
  cmd/NeverDCmdConcolic.cpp
  cmd/NeverDCmdMobile.cpp
  cmd/NeverDCmdWeb.cpp
  cmd/NeverDCmdWebRewrite.cpp
  cmd/NeverDCmdWebBehavior.cpp
  cmd/NeverDCmdWebBehaviorDiff.cpp
  cmd/NeverDCmdWebPackages.cpp
  cmd/NeverDCmdWebArchive.cpp
  cmd/NeverDCmdWebDesktop.cpp
  cmd/NeverDCmdWebIntegrity.cpp
  cmd/NeverDCmdWebAdvisories.cpp
  cmd/NeverDCmdWebAttestation.cpp
  cmd/NeverDCmdWebAssessment.cpp
  cmd/NeverDCmdWebInterfaces.cpp
  cmd/NeverDCmdWebStreams.cpp
  cmd/NeverDCmdWebSEA.cpp
  cmd/NeverDCmdWebNexe.cpp
  cmd/NeverDCmdWebPkg.cpp
  cmd/NeverDCmdWebDataPack.cpp
  cmd/NeverDCmdWebWails.cpp
  cmd/NeverDCmdWebTauri.cpp
  cmd/NeverDCmdWebReadableExport.cpp

  LINK_LIBS
  neverd_shared
  NeverDMobile
  neverd_tool_json
)
neverd_stage_json_notice(neverd)

# Symbol downloads go through the system's HTTP client: WinHTTP on Windows,
# and elsewhere libcurl, loaded only when a download is asked for.
if(WIN32)
  target_sources(neverd PRIVATE NeverDSymbolTransportWinHTTP.cpp)
  target_link_libraries(neverd PRIVATE winhttp)
else()
  target_sources(neverd PRIVATE NeverDSymbolTransportCurl.cpp)
  target_link_libraries(neverd PRIVATE ${CMAKE_DL_LIBS})
endif()

# Keep notices with redistributable binaries in both source and package builds.
if(NEVERD_LLVM_PREBUILT)
  get_filename_component(_neverd_llvm_prefix "${LLVM_DIR}/../../.." ABSOLUTE)
  set(_neverd_swift_notices
    "${_neverd_llvm_prefix}/share/licenses/llvm/swift-demangling")
  set(_neverd_llvm_license "${_neverd_llvm_prefix}/share/licenses/llvm/LICENSE.TXT")
else()
  set(_neverd_swift_source "${CMAKE_SOURCE_DIR}/third_party/llvm-project/llvm/lib/SwiftDemangle")
  set(_neverd_swift_notices "${_neverd_swift_source}/vendor/swift")
  set(_neverd_llvm_license "${CMAKE_SOURCE_DIR}/third_party/llvm-project/llvm/LICENSE.TXT")
endif()
set(_neverd_swift_license_files
  "${_neverd_swift_notices}/LICENSE.txt"
  "${_neverd_swift_notices}/CONTRIBUTORS.txt")
if(NEVERD_LLVM_PREBUILT)
  list(APPEND _neverd_swift_license_files "${_neverd_swift_notices}/UPSTREAM.md")
else()
  list(APPEND _neverd_swift_license_files "${_neverd_swift_source}/UPSTREAM.md")
endif()
foreach(_notice IN LISTS _neverd_swift_license_files _neverd_llvm_license)
  if(NOT EXISTS "${_notice}")
    message(FATAL_ERROR "The selected LLVM is missing a required notice: ${_notice}")
  endif()
endforeach()
add_custom_command(TARGET neverd POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:neverd>/licenses/swift-demangling"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_neverd_swift_license_files}
    "$<TARGET_FILE_DIR:neverd>/licenses/swift-demangling"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${_neverd_llvm_license}" "$<TARGET_FILE_DIR:neverd>/licenses/LLVM-LICENSE.TXT"
  VERBATIM)

# The CLI uses internal support helpers that are not part of libneverd's C ABI.
# ELF shared-library links only retain archive objects needed by the library
# itself, and Windows does not auto-export them.  Link the implementation into
# the CLI on both platforms.  macOS instead resolves support helpers through
# the dylib to preserve its single LLVM command-line registry.
if(NOT APPLE)
  target_link_libraries(neverd PRIVATE NeverDSupport)
endif()

if(COMMAND neverd_sign_hypervisor)
  neverd_sign_hypervisor(neverd)
endif()
