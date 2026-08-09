add_neverd_unittest(NeverDEVMOpcodeTests
  EVMOpcodeTests.cpp

  LINK_COMPONENTS
  EVM
)

add_neverd_unittest(NeverDEVMBytecodeTests
  EVMBytecodeTests.cpp

  LINK_COMPONENTS
  EVM
)

add_neverd_unittest(NeverDEVMLoaderTests
  EVMLoaderTests.cpp

  LINK_COMPONENTS
  Loader
  Support
)

add_neverd_unittest(NeverDEVMAnalyzerTests
  EVMAnalyzerTests.cpp

  LINK_COMPONENTS
  EVM
)

add_neverd_unittest(NeverDEVMSemanticTests
  EVMSemanticTests.cpp

  LINK_COMPONENTS
  EVM
)

add_neverd_unittest(NeverDEVMEmitterTests
  EVMEmitterTests.cpp

  LINK_COMPONENTS
  EVM
)

add_neverd_unittest(NeverDEVMIntegrationTests
  EVMIntegrationTests.cpp

  LINK_LIBS
  neverd_shared
)
