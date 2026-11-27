add_neverd_component_library(NeverDBytecodeAnalysis
  BytecodeDecoder.cpp
  BytecodeProfile.cpp
  BytecodeSource.cpp
  LINK_COMPONENTS IR
  LLVM_COMPONENTS Support
)

add_neverd_component_library(NeverDLuaRecovery
  lua/Chunk.cpp
  lua/Decode.cpp
  lua/JitInstructions.cpp
  lua/LuaJit.cpp
  lua/Luau.cpp
  lua/LuauInstructions.cpp
  lua/Puc.cpp
  lua/PucInstructions.cpp
  lua/Recovery.cpp
  lua/Source.cpp
  LINK_COMPONENTS Digest
  LLVM_COMPONENTS Support
)
