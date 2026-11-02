add_neverd_unittest(NeverDSignatureTests
  ImageProfileTests.cpp
  LibraryFeatureTests.cpp
  MachOPatternGeneratorTests.cpp
  PatternGeneratorTests.cpp
  VTableEvidenceTests.cpp
  RuntimePointerEvidenceTests.cpp
  CodeLayoutEvidenceTests.cpp
  PatternParserTests.cpp
  RichHeaderTests.cpp
  SignatureCacheTests.cpp
  SignatureDBTests.cpp
  SignatureDataContextTests.cpp
  LocalDataEvidenceTests.cpp
  SignatureVTableContextTests.cpp
  SignatureRuntimePointerContextTests.cpp
  SignatureCodeLayoutContextTests.cpp
  SignatureMatcherTests.cpp
  SignatureTreeTests.cpp
  X86ReferenceBoundaryTests.cpp
  ThrowInfoReferenceTests.cpp
  SynthesizedRoutineTests.cpp

  LINK_COMPONENTS Sigs
)

target_include_directories(NeverDSignatureTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(NeverDSignatureTests PRIVATE
  NEVERD_LIBRARY_FEATURE_DIR="${CMAKE_SOURCE_DIR}/signatures/features/rules")
