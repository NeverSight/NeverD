add_neverd_unittest(NeverDRawISATests
  RawISATests.cpp

  LINK_COMPONENTS
  Loader
  Support
)

add_neverd_unittest(NeverDNativeBufferTests
  NativeBufferTests.cpp
  FileBufferTests.cpp
  LoadCancellationTests.cpp
  LINK_COMPONENTS Loader Support
)

add_neverd_unittest(NeverDLinkerThunkTests
  LinkerThunkTests.cpp
  LINK_COMPONENTS Loader Support
)

add_neverd_unittest(NeverDStaticSymbolTableTests
  StaticSymbolTableTests.cpp
  LINK_COMPONENTS Loader Support
)

add_neverd_unittest(NeverDReadOnlyBytesTests
  ReadOnlyBytesTests.cpp
  LINK_COMPONENTS Loader Support
)

add_neverd_unittest(NeverDPEHeaderMappingTests
  PEHeaderMappingTests.cpp
  LINK_COMPONENTS Loader Support
)
