include(${CMAKE_SOURCE_DIR}/cmake/NeverDZlib.cmake)
add_neverd_component_library(NeverDDebugInfo
  CompressedSections.cpp
  DWARFLoader.cpp
  DebugFileIdentity.cpp
  DebugInfoDiscovery.cpp
  PDBFunctionNameHint.cpp
  PDBLoader.cpp
  PDB20Loader.cpp
  SymbolStore.cpp
  map/COFFLLDMapParser.cpp
  map/ELFMapParser.cpp
  map/GNUldMapParser.cpp
  map/LLDMapLoader.cpp
  map/MSVCMapLoader.cpp
  map/MachOMapParser.cpp
  map/MapDebugContextBase.cpp
  map/MapFile.cpp

  LINK_COMPONENTS
  IR
  Loader
  LINK_LIBS ZLIB::ZLIB
)
# The prebuilt LLVM may read no zstd-compressed section; the pinned static SDK
# then expands them (CompressedSections.cpp), as the web reader does.
if(NOT LLVM_ENABLE_ZSTD)
  find_package(zstd 1.5.7 EXACT CONFIG QUIET)
  if(zstd_FOUND AND TARGET zstd::libzstd_static)
    target_link_libraries(NeverDDebugInfo PRIVATE zstd::libzstd_static)
    set_property(SOURCE CompressedSections.cpp APPEND PROPERTY
      COMPILE_DEFINITIONS NEVERD_DEBUG_ZSTD=1)
  endif()
endif()
