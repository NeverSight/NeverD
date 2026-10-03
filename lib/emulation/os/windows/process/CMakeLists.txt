# User process policy is available without the Windows driver environment.
add_neverd_component_library(NeverDEmulationWindowsProcess
  WindowsProcess.cpp
  WindowsProcessImage.cpp
  WindowsProcessModules.cpp
  WindowsProcessExports.cpp
  WindowsProcessLifetime.cpp
  WindowsProcessLoader.cpp
  WindowsProcessEnvironment.cpp
  WindowsProcessServices.cpp
  WindowsProcessMemory.cpp
  LINK_COMPONENTS Loader EmulationRuntime EmulationABI
  LLVM_COMPONENTS Object Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationWindowsProcess PRIVATE NOMINMAX)
endif()
