add_neverd_unittest(NeverDSBFEmitterTests
  SBFEmitterTests.cpp

  LINK_COMPONENTS
  SBF
)

target_include_directories(NeverDSBFEmitterTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFLLVMEmitterTests
  SBFLLVMEmitterTests.cpp

  LINK_COMPONENTS
  SBF
)

target_include_directories(NeverDSBFLLVMEmitterTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFSemanticTests
  SBFSemanticTests.cpp

  LINK_COMPONENTS
  Loader
  SBF
)

target_include_directories(NeverDSBFSemanticTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFMetadataTests
  SBFMetadataTests.cpp

  LINK_COMPONENTS
  SBF
)

target_include_directories(NeverDSBFMetadataTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFAnalyzerTests
  SBFAnalyzerTests.cpp

  LINK_COMPONENTS
  Loader
  SBF
)

target_include_directories(NeverDSBFAnalyzerTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFLoaderTests
  SBFLoaderTests.cpp

  LINK_COMPONENTS
  Loader
  Support
  SBF
)

target_include_directories(NeverDSBFLoaderTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFIntegrationTests
  SBFIntegrationTests.cpp

  LINK_LIBS
  neverd_shared
)

target_include_directories(NeverDSBFIntegrationTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LLVM_INCLUDE_DIRS}
)
