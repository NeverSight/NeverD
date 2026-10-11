add_neverd_unittest(NeverDRawISATests
  RawISATests.cpp

  LINK_COMPONENTS
  Loader
  Support
)

add_neverd_unittest(NeverDNativeBufferTests
  NativeBufferTests.cpp
  LINK_COMPONENTS Loader Support
)
