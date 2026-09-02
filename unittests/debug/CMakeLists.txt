add_neverd_unittest(NeverDDebugInfoTests
  DebugInfoDiscoveryTests.cpp
  PDBIdentityTests.cpp

  LINK_COMPONENTS
  DebugInfo
  Loader
  Support
)

target_include_directories(NeverDDebugInfoTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

# Keeps fail-closed PE/PDB identity authentication independently runnable from
# the broader debug-info discovery suite while exact PDB extents remain gated.
add_neverd_unittest(NeverDPDBIdentityTests
  PDBIdentityTests.cpp

  LINK_COMPONENTS
  DebugInfo
  Loader
  Support
)

target_include_directories(NeverDPDBIdentityTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
