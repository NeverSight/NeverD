if(NOT TARGET LLVMSwiftDemangle)
  message(FATAL_ERROR
    "The selected LLVM lacks LLVMSwiftDemangle. Update the NeverD LLVM fork "
    "and its matching prebuilt package, or use -DNEVERD_LLVM_PREBUILT=OFF "
    "with the current fork sources.")
endif()

# Prefer the platform's compression library. Windows source builds can use
# the pinned static fallback without a separately installed package manager.
find_package(ZLIB QUIET)
if(NOT TARGET ZLIB::ZLIB)
  include(FetchContent)
  function(neverd_mobile_fetch_zlib)
    if(POLICY CMP0135)
      cmake_policy(SET CMP0135 NEW)
    endif()
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_BUILD_STATIC ON)
    set(ZLIB_INSTALL OFF)
    # Both maintainer-hosted archives have the same pinned bytes. Prefer the
    # release asset so CI need not rely on the fossils endpoint being available.
    FetchContent_Declare(neverd_mobile_zlib
      URL
        https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
        https://zlib.net/fossils/zlib-1.3.2.tar.gz
      URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
      TIMEOUT 180)
    FetchContent_MakeAvailable(neverd_mobile_zlib)
  endfunction()
  neverd_mobile_fetch_zlib()
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
add_library(NeverDMobile STATIC
  mobile/MobileCommon.cpp
  mobile/MobileArchive.cpp
  mobile/MobileProcess.cpp
  mobile/MobileRecovery.cpp
  mobile/MobileAndroid.cpp
  mobile/MobileDalvikModel.cpp
  mobile/MobileDalvikDex.cpp
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
target_link_libraries(NeverDMobile PUBLIC ZLIB::ZLIB neverd_shared)
if(APPLE)
  # Resolve LLVM Support through libneverd's existing registry. Linking the
  # component's archive directly avoids pulling a second static Support into
  # the CLI through the LLVM component's transitive link requirements.
  target_link_libraries(NeverDMobile PRIVATE "$<TARGET_FILE:LLVMSwiftDemangle>")
  add_dependencies(NeverDMobile LLVMSwiftDemangle)
else()
  target_link_libraries(NeverDMobile PRIVATE LLVMSwiftDemangle)
endif()
if(NOT APPLE)
  # Internal loader symbols are not exported by the Windows C ABI DLL.
  # macOS resolves these through libneverd, preserving one LLVM registry.
  target_link_libraries(NeverDMobile PUBLIC NeverDLoader NeverDSupport)
endif()

add_neverd_tool(neverd
  neverd.cpp
  NeverDCLIOptions.cpp
  NeverDTranslateOutput.cpp
  cmd/NeverDCmdInfo.cpp
  cmd/NeverDCmdTables.cpp
  cmd/NeverDCmdDisasm.cpp
  cmd/NeverDCmdMarkup.cpp
  cmd/NeverDCmdSearch.cpp
  cmd/NeverDCmdExport.cpp
  cmd/NeverDCmdPipeline.cpp
  cmd/NeverDCmdSimplify.cpp
  cmd/NeverDCmdOptimizeIR.cpp
  cmd/NeverDCmdTranslate.cpp
  cmd/NeverDCmdSafety.cpp
  cmd/NeverDCmdConcolic.cpp
  cmd/NeverDCmdMobile.cpp

  LINK_LIBS
  neverd_shared
  NeverDMobile
)

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
