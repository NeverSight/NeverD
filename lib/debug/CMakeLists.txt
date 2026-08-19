add_neverd_component_library(NeverDDebugInfo
  DWARFLoader.cpp
  DebugInfoDiscovery.cpp
  PDBLoader.cpp
  map/COFFLLDMapParser.cpp
  map/ELFMapParser.cpp
  map/GNUldMapParser.cpp
  map/LLDMapLoader.cpp
  map/MSVCMapLoader.cpp
  map/MachOMapParser.cpp
  map/MapDebugContextBase.cpp

  LINK_COMPONENTS
  IR
)
