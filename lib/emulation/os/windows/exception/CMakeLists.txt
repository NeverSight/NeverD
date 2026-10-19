# Windows exception semantics are shared by kernel and user workloads.
add_neverd_component_library(NeverDEmulationWindowsException
  X64SEH.cpp
  X64SEHMetadata.cpp
  X64SEHGS.cpp
  X64SEHEpilogue.cpp
  X64SIMDException.cpp
  WindowsX64Context.cpp
  LINK_COMPONENTS Loader EmulationCore EmulationArch
  LLVM_COMPONENTS Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationWindowsException PRIVATE NOMINMAX)
endif()
