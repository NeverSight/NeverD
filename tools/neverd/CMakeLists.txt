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

  LINK_LIBS
  neverd_shared
)

# The CLI uses internal support helpers that are not part of libneverd's C ABI.
# ELF shared-library links only retain archive objects needed by the library
# itself, and Windows does not auto-export them.  Link the implementation into
# the CLI on both platforms.  macOS instead resolves support helpers through
# the dylib to preserve its single LLVM command-line registry.
if(NOT APPLE)
  target_link_libraries(neverd PRIVATE NeverDSupport)
endif()
