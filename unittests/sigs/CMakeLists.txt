add_neverd_unittest(NeverDSignatureTests
  PatternParserTests.cpp
  SignatureDBTests.cpp
  SignatureMatcherTests.cpp

  LINK_COMPONENTS Sigs
)

target_include_directories(NeverDSignatureTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
