include(${CMAKE_SOURCE_DIR}/cmake/NeverDZlib.cmake)
add_neverd_component_library(NeverDDalvik
  AndroidFramework.cpp
  AndroidEventLog.cpp
  AndroidParcel.cpp
  AndroidServiceManager.cpp
  AndroidStatLogger.cpp
  AndroidProperties.cpp
  AndroidVersion.cpp
  Budget.cpp
  Dex.cpp
  Heap.cpp
  Library.cpp
  Model.cpp
  Runtime.cpp
  Signature.cpp
  LLVM_COMPONENTS Support
  LINK_LIBS ZLIB::ZLIB)
