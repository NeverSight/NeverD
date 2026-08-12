add_neverd_tool(neverd
  neverd.cpp
  NeverDCLIOptions.cpp
  NeverDCmdInfo.cpp
  NeverDCmdTables.cpp
  NeverDCmdDisasm.cpp
  NeverDCmdMarkup.cpp
  NeverDCmdSearch.cpp
  NeverDCmdExport.cpp
  NeverDCmdPipeline.cpp
  NeverDCmdSimplify.cpp

  LINK_LIBS
  neverd_shared
)
