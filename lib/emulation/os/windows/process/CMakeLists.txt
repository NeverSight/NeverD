# User process policy is available without the Windows driver environment.
add_neverd_component_library(NeverDEmulationWindowsProcess
  WindowsProcess.cpp
  WindowsProcessContext.cpp
  WindowsProcessExceptions.cpp
  WindowsProcessSEH.cpp
  WindowsProcessImage.cpp
  WindowsLibraryHost.cpp
  WindowsProcessModules.cpp
  WindowsSystemModules.cpp
  WindowsNativeServices.cpp
  WindowsProcessExports.cpp
  WindowsProcessLifetime.cpp
  WindowsProcessLoader.cpp
  WindowsProcessEnvironment.cpp
  WindowsProcessVariables.cpp
  WindowsProcessHeap.cpp
  WindowsProcessState.cpp
  WindowsProcessFiles.cpp
  WindowsProcessTime.cpp
  WindowsProcessServices.cpp
  WindowsProcessMemory.cpp
  WindowsProcessMemoryWrite.cpp
  LINK_COMPONENTS Loader EmulationRuntime EmulationABI EmulationWindowsException
  LLVM_COMPONENTS Object Support)
if(WIN32)
  target_compile_definitions(NeverDEmulationWindowsProcess PRIVATE NOMINMAX)
endif()
