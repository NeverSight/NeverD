add_neverd_component_library(NeverDDebugInfo
  COFFLLDMapParser.cpp
  DWARFLoader.cpp
  ELFMapParser.cpp
  GNUldMapParser.cpp
  LLDMapLoader.cpp
  MachOMapParser.cpp
  MapDebugContextBase.cpp
  MSVCMapLoader.cpp
  PDBLoader.cpp

  LINK_COMPONENTS
  IR
)
