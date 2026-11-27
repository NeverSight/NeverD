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
  LLVMTargetInitialization.cpp
  StringScan.cpp
  TextCodec.cpp
  TextEncoding.cpp
  TextEvidence.cpp

  LINK_COMPONENTS
  Loader

  LINK_LIBS neverd_text_acceleration
)
target_include_directories(NeverDSupport PRIVATE "${NEVERD_LLVM_TARGET_INCLUDE_DIR}")

# The language frontends and the optional web workbench share ZIP admission.
add_neverd_component_library(NeverDZip
  ../web/archives/Identity.cpp
  ../web/archives/PathIndex.cpp
  ../web/archives/Zip.cpp
  ../web/archives/ZipPayload.cpp
  ../web/BlobStore.cpp
  LINK_COMPONENTS Digest
  LLVM_COMPONENTS Support
  LINK_LIBS ZLIB::ZLIB
)
target_compile_definitions(NeverDZip PRIVATE NEVERD_WEB_ZLIB=1)
if(WIN32)
  target_sources(NeverDZip PRIVATE ../web/platform/WindowsFile.cpp)
  target_link_libraries(NeverDZip PRIVATE advapi32 bcrypt)
endif()
