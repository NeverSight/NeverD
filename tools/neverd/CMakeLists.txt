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
    FetchContent_Declare(neverd_mobile_zlib
      URL https://zlib.net/fossils/zlib-1.3.2.tar.gz
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

# The CLI uses internal support helpers that are not part of libneverd's C ABI.
# ELF shared-library links only retain archive objects needed by the library
# itself, and Windows does not auto-export them.  Link the implementation into
# the CLI on both platforms.  macOS instead resolves support helpers through
# the dylib to preserve its single LLVM command-line registry.
if(NOT APPLE)
  target_link_libraries(neverd PRIVATE NeverDSupport)
endif()
