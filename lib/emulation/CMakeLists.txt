# CPU execution can be built independently of the Windows driver environment.
add_neverd_component_library(NeverDEmulationCore
  core/GuestMemory.cpp
  core/ExecutionBackend.cpp
  core/Registers.cpp
  core/CheckedBackend.cpp
  core/PhysicalMemory.cpp
  core/AddressSpace.cpp
  core/MemoryProjection.cpp
  core/RAMTransaction.cpp
  core/MemoryView.cpp
  LLVM_COMPONENTS Support)

add_neverd_component_library(NeverDEmulationArch
  arch/x86_64/X64FPState.cpp
  arch/x86_64/X64PageTables.cpp
  arch/x86_64/X64ExceptionMonitor.cpp
  arch/x86_64/X64MachineProbe.cpp
  arch/aarch64/AArch64PageTables.cpp
  arch/aarch64/AArch64State.cpp
  arch/aarch64/AArch64MachineProbe.cpp
  arch/aarch64/AArch64InstructionEffects.cpp
  arch/x86_64/CheckedX64Backend.cpp
  arch/x86_64/CheckedX64Memory.cpp
  arch/aarch64/CheckedAArch64Backend.cpp
  LINK_COMPONENTS EmulationCore
  LLVM_COMPONENTS Support)

add_neverd_component_library(NeverDEmulationNative
  backends/kvm/KvmMachine.cpp
  backends/kvm/KvmAArch64Machine.cpp
  backends/kvm/KvmRunControl.cpp
  backends/whp/WhpMachine.cpp
  backends/whp/WhpAArch64Machine.cpp
  LINK_COMPONENTS EmulationArch
  LLVM_COMPONENTS Support)

if(NEVERD_EMULATION_BACKEND_UNICORN)
  add_neverd_component_library(NeverDEmulationUnicorn
    backends/unicorn/UnicornBackend.cpp
    backends/unicorn/UnicornMachine.cpp
    backends/unicorn/UnicornArchitecture.cpp
    LINK_COMPONENTS EmulationArch
    LLVM_COMPONENTS Support
    LINK_LIBS unicorn)
else()
  add_neverd_component_library(NeverDEmulationUnicorn
    backends/unicorn/UnicornUnavailable.cpp
    LINK_COMPONENTS EmulationArch
    LLVM_COMPONENTS Support)
endif()

add_neverd_component_library(NeverDEmulationCPU
  runtime/BackendRegistry.cpp
  runtime/ExecutionConfiguration.cpp
  runtime/ExecutionConfigurationJSON.cpp
  LINK_COMPONENTS EmulationArch EmulationNative EmulationUnicorn
  LLVM_COMPONENTS Support)

add_neverd_component_library(NeverDEmulationABI
  abi/IntegerABI.cpp
  LINK_COMPONENTS EmulationCore
  LLVM_COMPONENTS Support)

add_neverd_component_library(NeverDEmulationRuntime
  runtime/ExecutionBudget.cpp
  runtime/ExecutionSession.cpp
  LINK_COMPONENTS EmulationCPU
  LLVM_COMPONENTS Support)

add_neverd_component_library(NeverDEmulationImage
  runtime/ImageMapping.cpp
  LINK_COMPONENTS Loader EmulationCore
  LLVM_COMPONENTS Support)

add_subdirectory(os/linux)
add_subdirectory(os/linux/android)
add_subdirectory(os/windows/process)
add_neverd_component_library(NeverDEmulationProcess
  runtime/ProcessSession.cpp
  runtime/ProcessReport.cpp
  runtime/ProcessAndroidJSON.cpp
  runtime/ProcessWindowsJSON.cpp
  LINK_COMPONENTS EmulationLinux EmulationAndroid EmulationWindowsProcess
  LLVM_COMPONENTS Support)

if(NEVERD_EMULATION_BACKEND_UNICORN)
  target_compile_definitions(NeverDEmulationCPU PRIVATE NEVERD_EMULATION_UNICORN=1)
endif()

if(WIN32)
  foreach(_component Core Arch Native Unicorn CPU ABI Runtime Image Process)
    target_compile_definitions(NeverDEmulation${_component} PRIVATE NOMINMAX)
  endforeach()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NEVERD_EMULATION_BACKEND_KVM)
  target_compile_definitions(NeverDEmulationNative PRIVATE NEVERD_EMULATION_KVM=1)
  target_compile_definitions(NeverDEmulationCPU PRIVATE NEVERD_EMULATION_KVM=1)
endif()
if(WIN32 AND NEVERD_EMULATION_BACKEND_WHP)
  target_compile_definitions(NeverDEmulationNative PRIVATE NEVERD_EMULATION_WHP=1)
  target_compile_definitions(NeverDEmulationCPU PRIVATE NEVERD_EMULATION_WHP=1)
endif()
if(NEVERD_ENABLE_DRIVER_EMULATION)
  add_subdirectory(os/windows)
endif()
