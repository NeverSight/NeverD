add_neverd_unittest(NeverDTranslationContractTests
  GuestMemoryRuntimeTests.cpp
  GuestStateTests.cpp
  ResolvedHostTargetTests.cpp
  RuntimeCodeMemoryTests.cpp
  RuntimeGuestStateTests.cpp
  RuntimeHelpersTests.cpp
  RuntimeSymbolRegistryTests.cpp
  TranslationIRVerifierTests.cpp
  TranslationArtifactVerifierTests.cpp
  TranslationObjectCompilerTests.cpp

  LINK_COMPONENTS
  Translate

  LINK_LIBS
  LLVMObjectYAML
)

target_include_directories(NeverDTranslationContractTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
