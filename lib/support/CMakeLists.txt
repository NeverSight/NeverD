include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NeverDPerformance.cmake)
neverd_add_text_acceleration()

add_neverd_component_library(NeverDDigest
  SHA256.cpp
  LLVM_COMPONENTS Support TargetParser
)

add_neverd_component_library(NeverDSupport
  AtomicOutput.cpp
  BinaryLoading.cpp
  HostMemory.cpp
  StringScan.cpp
  TextCodec.cpp
  TextEncoding.cpp
  TextEvidence.cpp

  LINK_COMPONENTS
  Loader

  LINK_LIBS neverd_text_acceleration
)
