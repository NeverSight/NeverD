add_neverd_unittest(NeverDSignatureTests
  ImageProfileTests.cpp
  LibraryFeatureTests.cpp
  MachOPatternGeneratorTests.cpp
  PatternGeneratorTests.cpp
  PatternParserTests.cpp
  RichHeaderTests.cpp
  SignatureCacheTests.cpp
  SignatureDBTests.cpp
  SignatureMatcherTests.cpp
  SignatureTreeTests.cpp
  SynthesizedRoutineTests.cpp

  LINK_COMPONENTS Sigs
)

target_include_directories(NeverDSignatureTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(NeverDSignatureTests PRIVATE
  NEVERD_LIBRARY_FEATURE_DIR="${CMAKE_SOURCE_DIR}/signatures/features/rules")
