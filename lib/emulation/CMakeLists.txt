add_neverd_component_library(NeverDEmulationCore
  GuestMemory.cpp
  core/ExecutionBackend.cpp
  core/PhysicalMemory.cpp
  arch/x86_64/CheckedX64Backend.cpp
  arch/x86_64/X64PageTables.cpp
  backends/kvm/KvmMachine.cpp
  backends/whp/WhpMachine.cpp)

add_neverd_component_library(NeverDEmulationUnicorn
  unicorn/UnicornBackend.cpp
  LINK_COMPONENTS EmulationCore
  LINK_LIBS unicorn)

add_neverd_component_library(NeverDEmulation
  core/BackendRegistry.cpp
  DriverResult.cpp
  DriverScenario.cpp
  DriverSession.cpp
  windows/WindowsX64ExecutionPolicy.cpp
  windows/DriverImage.cpp
  windows/KernelExportRegistry.cpp
  windows/KernelException.cpp
  windows/KernelSEH.cpp
  windows/KernelSEHGS.cpp
  windows/KernelSEHEpilogue.cpp
  windows/GuardControlFlow.cpp
  windows/KernelFramework.cpp
  windows/KernelFrameworkInterrupts.cpp
  windows/KernelFrameworkLocks.cpp
  windows/KernelModelFrameworkLocks.cpp
  windows/KernelModelFrameworkInterrupts.cpp
  windows/KernelFrameworkPower.cpp
  windows/KernelFrameworkPowerPolicy.cpp
  windows/KernelFrameworkPoFx.cpp
  windows/KernelModelPowerPolicy.cpp
  windows/KernelPoFx.cpp
  windows/KernelModelPoFx.cpp
  windows/KernelModelPoFxPolicy.cpp
  windows/KernelFrameworkControl.cpp
  windows/KernelFrameworkFiles.cpp
  windows/KernelFrameworkQueue.cpp
  windows/KernelFrameworkRequests.cpp
  windows/KernelFrameworkRequestAccessors.cpp
  windows/KernelModelFramework.cpp
  windows/KernelModelFrameworkRequests.cpp
  windows/KernelModelExports.cpp
  windows/KernelModel.cpp
  windows/KernelModelIO.cpp
  windows/KernelModelUserMemory.cpp
  windows/KernelModelPhysicalMemory.cpp
  windows/KernelModelMDL.cpp
  windows/KernelModelMDLUserMappings.cpp
  windows/KernelModelIRPStack.cpp
  windows/KernelModelDeviceStack.cpp
  windows/KernelModelPnpDevices.cpp
  windows/KernelModelPnpRequests.cpp
  windows/KernelModelPnpCompletion.cpp
  windows/KernelModelPowerRequests.cpp
  windows/KernelModelPowerCompletion.cpp
  windows/KernelModelResources.cpp
  windows/KernelInterrupts.cpp
  windows/KernelModelInterrupts.cpp
  windows/KernelModelInterruptEvents.cpp
  windows/KernelResources.cpp
  windows/KernelPhysicalMemory.cpp
  windows/KernelDMA.cpp
  windows/KernelDMAChannels.cpp
  windows/KernelDMAEvents.cpp
  windows/KernelModelDMA.cpp
  windows/KernelModelDMAChannels.cpp
  windows/KernelModelDMATransfers.cpp
  windows/KernelMMIO.cpp
  windows/KernelModelRemoveLocks.cpp
  windows/KernelRemoveLocks.cpp
  windows/KernelModelRuntime.cpp
  windows/KernelModelScheduling.cpp
  windows/KernelModelFrameworkSynchronization.cpp
  windows/KernelModelSpinLocks.cpp
  windows/KernelModelIRQL.cpp
  windows/KernelModelCancellation.cpp
  windows/KernelScheduler.cpp
  windows/KernelDispatcher.cpp
  windows/KernelAPIIRQL.cpp
  windows/DeviceLifecycle.cpp
  windows/KernelRegistry.cpp
  LINK_COMPONENTS Loader EmulationCore EmulationUnicorn)
if(WIN32)
  target_compile_definitions(NeverDEmulation PRIVATE NOMINMAX)
  target_compile_definitions(NeverDEmulationCore PRIVATE NOMINMAX)
  target_compile_definitions(NeverDEmulationUnicorn PRIVATE NOMINMAX)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NEVERD_EMULATION_BACKEND_KVM)
  target_compile_definitions(NeverDEmulationCore PRIVATE NEVERD_EMULATION_KVM=1)
endif()
if(WIN32 AND NEVERD_EMULATION_BACKEND_WHP)
  target_compile_definitions(NeverDEmulationCore PRIVATE NEVERD_EMULATION_WHP=1)
endif()
