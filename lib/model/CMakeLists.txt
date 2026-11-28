include(${CMAKE_SOURCE_DIR}/cmake/NeverDJson.cmake)
add_neverd_component_library(NeverDModel
  Artifact.cpp JSON.cpp GGUF.cpp Rebuild.cpp Package.cpp Checkpoint.cpp Security.cpp Template.cpp ONNX.cpp Structure.cpp Comparison.cpp
  LINK_COMPONENTS Digest Zip
  LLVM_COMPONENTS BinaryFormat Support
  LINK_LIBS nlohmann_json::nlohmann_json)
