add_neverd_unittest(NeverDDebugInfoTests
  DebugInfoDiscoveryTests.cpp

  LINK_COMPONENTS
  DebugInfo
  Loader
  Support
)

target_include_directories(NeverDDebugInfoTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
