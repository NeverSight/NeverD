add_neverd_unittest(NeverDSBFEmitterTests
  SBFEmitterTests.cpp

  LINK_COMPONENTS
  SBF
)

target_include_directories(NeverDSBFEmitterTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFSourceDifferentialTests
  SBFSourceDifferentialTests.cpp

  LINK_COMPONENTS
  Loader
  Support
  SBF
)

target_compile_definitions(NeverDSBFSourceDifferentialTests PRIVATE
  NEVERD_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(NeverDSBFSourceDifferentialTests PRIVATE
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

add_neverd_unittest(NeverDSBFLLVMDifferentialTests
  SBFLLVMDifferentialTests.cpp

  LINK_COMPONENTS
  SBF

  LINK_LIBS
  ${NEVERD_LLVM_JIT_LIBS}
)

target_include_directories(NeverDSBFLLVMDifferentialTests PRIVATE
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

add_neverd_unittest(NeverDSBFProgramImageTests
  SBFProgramImageTests.cpp

  LINK_COMPONENTS
  Loader
  SBF
)

target_include_directories(NeverDSBFProgramImageTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFMalformedCorpusTests
  SBFMalformedCorpusTests.cpp

  LINK_COMPONENTS
  Loader
  Support
  SBF
)

target_include_directories(NeverDSBFMalformedCorpusTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFISAConformanceTests
  SBFISAConformanceTests.cpp

  LINK_COMPONENTS
  Loader
  SBF
)

target_include_directories(NeverDSBFISAConformanceTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
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

# The integration fixture also uses internal SBF and LLVM support helpers.
# ELF and Mach-O resolve those symbols from neverd_shared, but the Windows
# import library only exposes public DLL exports, so link the component archive
# explicitly on Windows.
if(WIN32)
  target_link_libraries(NeverDSBFIntegrationTests PRIVATE NeverDSBF)
endif()

target_include_directories(NeverDSBFIntegrationTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LLVM_INCLUDE_DIRS}
)

add_neverd_unittest(NeverDSBFUpstreamConformanceTests
  SBFUpstreamConformanceTests.cpp

  LINK_COMPONENTS
  Loader
  Support
  SBF
)

target_compile_definitions(NeverDSBFUpstreamConformanceTests PRIVATE
  NEVERD_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_include_directories(NeverDSBFUpstreamConformanceTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LLVM_INCLUDE_DIRS}
)
