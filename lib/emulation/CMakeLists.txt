add_neverd_component_library(NeverDEmulation
  DriverResult.cpp
  DriverScenario.cpp
  DriverSession.cpp
  GuestMemory.cpp
  X64ExecutionPolicy.cpp
  unicorn/UnicornBackend.cpp
  windows/DriverImage.cpp
  windows/KernelExportRegistry.cpp
  windows/GuardControlFlow.cpp
  windows/KernelFramework.cpp
  windows/KernelFrameworkControl.cpp
  windows/KernelFrameworkQueue.cpp
  windows/KernelFrameworkRequests.cpp
  windows/KernelFrameworkRequestAccessors.cpp
  windows/KernelModelFramework.cpp
  windows/KernelModelFrameworkRequests.cpp
  windows/KernelModelExports.cpp
  windows/KernelModel.cpp
  windows/KernelModelIO.cpp
  windows/KernelModelMDL.cpp
  windows/KernelModelIRPStack.cpp
  windows/KernelModelDeviceStack.cpp
  windows/KernelModelRuntime.cpp
  windows/KernelModelScheduling.cpp
  windows/KernelModelCancellation.cpp
  windows/KernelScheduler.cpp
  windows/KernelDispatcher.cpp
  windows/KernelAPIIRQL.cpp
  windows/DeviceLifecycle.cpp
  windows/KernelRegistry.cpp
  LINK_COMPONENTS Loader
  LINK_LIBS unicorn)
if(WIN32)
  target_compile_definitions(NeverDEmulation PRIVATE NOMINMAX)
endif()
