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

# Internal C++ helpers are ordinary dylib exports on ELF/Mach-O.  Windows does
# not auto-export component-library symbols from neverd_shared, so link the
# shared atomic-output implementation's component archive into the CLI there.
if(WIN32)
  target_link_libraries(neverd PRIVATE NeverDSupport)
endif()
