include(${CMAKE_SOURCE_DIR}/cmake/NeverDJson.cmake)
add_neverd_component_library(NeverDModel
  Artifact.cpp JSON.cpp GGUF.cpp Package.cpp Template.cpp ONNX.cpp Structure.cpp Comparison.cpp
  LINK_COMPONENTS Digest
  LLVM_COMPONENTS Support
  LINK_LIBS nlohmann_json::nlohmann_json)
