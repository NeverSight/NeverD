add_neverd_unittest(NeverDHvfTests
  HvfTests.cpp
  HvfExecutorTests.cpp
  HvfIntelDeadlineTests.cpp
  HvfOwnerThreadTests.cpp
  HvfIntelOwnerTests.cpp
  TIMEOUT 20
  LINK_COMPONENTS EmulationCPU)

target_include_directories(NeverDHvfTests PRIVATE ${CMAKE_SOURCE_DIR}/lib/emulation)
if(APPLE AND NEVERD_EMULATION_BACKEND_HVF)
  target_compile_definitions(NeverDHvfTests PRIVATE NEVERD_EMULATION_HVF=1)
endif()

add_neverd_unittest(NeverDAArch64StateTests
  AArch64StateTests.cpp
  AArch64MachineProbeTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationArch)
target_include_directories(NeverDAArch64StateTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDAArch64FPTests
  AArch64FPStateTests.cpp
  AArch64SIMDTests.cpp
  AArch64ProbeExecutionTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDAArch64FPTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDAArch64PAuthTests
  AArch64PAuthTests.cpp
  TIMEOUT 20
  LINK_COMPONENTS EmulationCPU)

add_neverd_unittest(NeverDAArch64BTITests
  AArch64BTITests.cpp
  TIMEOUT 20
  LINK_COMPONENTS EmulationCPU EmulationRuntime)

add_neverd_unittest(NeverDAArch64CacheTests
  AArch64CacheTests.cpp
  TIMEOUT 20
  NO_PRETTY_VALUES
  LINK_COMPONENTS EmulationCPU EmulationRuntime)

add_neverd_unittest(NeverDAArch64NZCVTests
  AArch64NZCVTests.cpp
  TIMEOUT 20
  NO_PRETTY_VALUES
  LINK_COMPONENTS EmulationCPU EmulationRuntime)

add_neverd_unittest(NeverDAArch64MemoryTests
  AArch64CrossPageTests.cpp
  AArch64VectorMemoryTests.cpp
  AArch64StructureMemoryTests.cpp
  AArch64AcquireReleaseTests.cpp
  AArch64ExclusiveTests.cpp
  AArch64AtomicTests.cpp
  TIMEOUT 15
  NO_PRETTY_VALUES
  LINK_COMPONENTS EmulationCPU)

add_neverd_unittest(NeverDX64FPTests
  X64FPStateTests.cpp
  X64XsaveTests.cpp
  X64MachineProbeTests.cpp
  X64ProbeExecutionTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDX64FPTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDProjectionCacheTests
  ProjectionCacheTests.cpp
  LINK_COMPONENTS EmulationArch)
target_include_directories(NeverDProjectionCacheTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDMemoryLifecycleTests
  MemoryLifecycleTests.cpp
  UnicornObservationTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)

add_neverd_unittest(NeverDInstructionFetchTests
  InstructionFetchTests.cpp
  TIMEOUT 15
  NO_PRETTY_VALUES
  LINK_COMPONENTS EmulationCPU)

add_neverd_unittest(NeverDExecutionSessionTests
  ExecutionSessionTests.cpp
  ImageMappingTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationRuntime EmulationImage)

add_neverd_unittest(NeverDDirectX64Tests
  DirectX64Tests.cpp
  NO_PRETTY_VALUES
  TIMEOUT 15
  LINK_COMPONENTS EmulationRuntime)

add_neverd_unittest(NeverDX64MemoryUpdateTests
  X64MemoryUpdateTests.cpp
  X64BitStringTests.cpp
  X64StringTransferTests.cpp
  X64StringComparisonTests.cpp
  X64MXCSRTests.cpp
  X64DataTests.cpp
  X64PackedIntegerTests.cpp
  X64PackedShiftTests.cpp
  X64VectorMaskTests.cpp
  X64ShuffleTests.cpp
  X64PartialMoveTests.cpp
  X64WordLaneTests.cpp
  X64IntegerFloatTests.cpp
  X64FloatIntegerTests.cpp
  X64SSEComparisonTests.cpp
  X64SSEPredicateTests.cpp
  X64SSEPrecisionTests.cpp
  X64PackedFloatTests.cpp
  X64PackedFloatIntegerTests.cpp
  X64CrossPageTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDX64MemoryUpdateTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDRAMTransactionTests
  RAMTransactionTests.cpp
  RAMReservationTests.cpp
  X64AtomicTests.cpp
  X64WideAtomicTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDRAMTransactionTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDParallelExecutionTests
  ParallelExecutionTests.cpp
  TIMEOUT 30
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDParallelExecutionTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDMMIOAtomicTests
  MMIOAtomicTests.cpp TIMEOUT 60 LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDMMIOAtomicTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDThreadPointerTests
  ThreadPointerTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)

add_neverd_unittest(NeverDX64IntegerTests
  X64IntegerTests.cpp
  X64PauseTests.cpp
  X64PushFlagsTests.cpp
  X64PopFlagsTests.cpp
  X64StatusFlagsTests.cpp
  X64DoubleShiftTests.cpp
  X64ScalarShiftTests.cpp
  X64LoopTests.cpp
  X64BranchTests.cpp
  X64StackTests.cpp
  X64FrameExitTests.cpp
  X64FrameEntryTests.cpp
  X64ReturnPrefixTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDX64IntegerTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDX64ExceptionTests
  X64ExceptionTests.cpp
  X64SIMDExceptionTests.cpp
  X64AlignmentTests.cpp
  X64DivisionTests.cpp
  X64StateTransitionTests.cpp
  NO_PRETTY_VALUES
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDX64ExceptionTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDLinuxProcessTests
  LinuxProcessTests.cpp
  LinuxPriorityTests.cpp
  LinuxKernelAvailabilityTests.cpp
  LinuxPIDFDTests.cpp
  LinuxProcessLayoutTests.cpp
  LinuxMemoryTests.cpp
  LinuxClockTests.cpp
  LinuxOutputNativeTests.cpp
  ProcessReportTests.cpp
  TIMEOUT 20
  LINK_COMPONENTS EmulationProcess)
target_include_directories(NeverDLinuxProcessTests PRIVATE ${CMAKE_SOURCE_DIR}/lib/emulation)
if(TARGET neverd_shared AND TARGET neverd)
  add_neverd_unittest(NeverDProcessPublicTests
    ProcessPublicTests.cpp
    TIMEOUT 30
    LINK_LIBS neverd_shared)
  target_include_directories(NeverDProcessPublicTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(NeverDProcessPublicTests PRIVATE
    NEVERD_PROCESS_CLI="$<TARGET_FILE:neverd>")
  if(NOT APPLE)
    target_link_libraries(NeverDProcessPublicTests PRIVATE LLVMSupport)
  endif()
  add_dependencies(NeverDProcessPublicTests neverd)
  add_neverd_unittest(NeverDUnpackPublicTests
    UnpackPublicTests.cpp
    TIMEOUT 120
    LINK_LIBS neverd_shared)
  target_include_directories(NeverDUnpackPublicTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/unittests/unpack)
  target_compile_definitions(NeverDUnpackPublicTests PRIVATE
    NEVERD_UNPACK_CLI="$<TARGET_FILE:neverd>"
    NEVERD_UNPACK_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/unittests/unpack/fixtures/binaries")
  if(NOT APPLE)
    target_link_libraries(NeverDUnpackPublicTests PRIVATE LLVMSupport LLVMObject)
  endif()
  add_dependencies(NeverDUnpackPublicTests neverd)
endif()
include(ProcessFixtures.cmake)
add_neverd_unittest(NeverDDarwinProcessTests
  DarwinProcessTests.cpp
  DarwinImageTests.cpp
  DarwinMemoryTests.cpp
  DarwinFileTests.cpp
  DarwinVectorTests.cpp
  DarwinTimeTests.cpp
  DarwinSystemTests.cpp
  DarwinEntropyTests.cpp
  DarwinNativeTests.cpp
  TIMEOUT 30
  NO_PRETTY_VALUES
  LINK_COMPONENTS EmulationProcess)
target_include_directories(NeverDDarwinProcessTests PRIVATE ${CMAKE_SOURCE_DIR}/lib/emulation)
include(DarwinFixtures.cmake)
add_neverd_unittest(NeverDAndroidNativeTests
  AndroidNativeTests.cpp
  AndroidMemoryTests.cpp
  AndroidTokenTests.cpp
  AndroidSearchTests.cpp
  AndroidMutexTests.cpp
  AndroidThreadAttributeTests.cpp
  AndroidThreadTests.cpp
  AndroidTimeTests.cpp
  AndroidSleepTests.cpp
  AndroidSignalTests.cpp
  AndroidSnapshotTests.cpp
  AndroidResidencyTests.cpp
  AndroidFileTests.cpp
  AndroidFileStatusAtTests.cpp
  AndroidFileSystemStatusTests.cpp
  AndroidSyscallTests.cpp
  AndroidFormattingTests.cpp
  AndroidScanningTests.cpp
  AndroidFinalizerTests.cpp
  TIMEOUT 30
  LINK_COMPONENTS EmulationProcess)
target_include_directories(NeverDAndroidNativeTests PRIVATE ${CMAKE_SOURCE_DIR}/lib/emulation)
include(AndroidFixtures.cmake)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/AndroidTestExecution.cmake")

# Batch PE scenarios use the common harness timeout; each guest workload
# retains its own finite instruction, event and time budget.
add_neverd_unittest(NeverDWindowsProcessTests
  WindowsProcessTests.cpp
  WindowsProcessMemoryTests.cpp
  WindowsMemoryWriteTests.cpp
  WindowsModuleTests.cpp
  WindowsLifetimeTests.cpp
  WindowsExportTests.cpp
  WindowsDynamicTests.cpp
  WindowsEnvironmentTests.cpp
  WindowsHeapTests.cpp
  WindowsSystemTests.cpp
  WindowsExceptionTests.cpp
  WindowsAlignmentProcessTests.cpp
  WindowsExclusiveProcessTests.cpp
  WindowsAtomicProcessTests.cpp
  WindowsSIMDMappingTests.cpp
  WindowsSIMDExecutionTests.cpp
  WindowsContinuationTests.cpp
  WindowsDeferredTests.cpp
  WindowsSEHTests.cpp
  WindowsContextTests.cpp
  WindowsNativeTestSupport.cpp
  LINK_COMPONENTS EmulationProcess)
target_include_directories(NeverDWindowsProcessTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/WindowsTestExecution.cmake")
include(WindowsProcessFixtures.cmake)
include(WindowsModuleFixtures.cmake)
include(WindowsLifetimeFixtures.cmake)
include(WindowsExportFixtures.cmake)
include(WindowsDynamicFixtures.cmake)
include(WindowsEnvironmentFixtures.cmake)
include(WindowsHeapFixtures.cmake)
include(WindowsMemoryWriteFixtures.cmake)
include(WindowsSystemFixtures.cmake)
include(WindowsExceptionFixtures.cmake)
include(WindowsSEHFixtures.cmake)
include(WindowsContextFixtures.cmake)
include(WindowsDeferredFixtures.cmake)

# Recovery of packed images runs whole stubs on each backend. The protector
# fixtures are checked-in images, so they need no cross toolchain; the
# generated-code fixture is built for every guest instruction set.
add_neverd_unittest(NeverDUnpackExecutionTests
  UnpackExecutionTests.cpp
  UnpackGeneratedTests.cpp
  WindowsNativeFailure.cpp
  UnpackLibraryTests.cpp
  TIMEOUT 300
  LINK_COMPONENTS Unpack)
target_include_directories(NeverDUnpackExecutionTests PRIVATE
  ${CMAKE_SOURCE_DIR}/unittests/unpack)
target_compile_definitions(NeverDUnpackExecutionTests PRIVATE
  NEVERD_UNPACK_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/unittests/unpack/fixtures/binaries")
include(UnpackGeneratedFixtures.cmake)
include(UnpackLibraryFixtures.cmake)
if(TARGET NeverDWindowsDeferredFixtures)
  add_dependencies(NeverDUnpackExecutionTests NeverDWindowsDeferredFixtures)
  target_compile_definitions(NeverDUnpackExecutionTests PRIVATE
    NEVERD_WINDOWS_DEFERRED_FIXTURE_DIR="${_deferred_dir}")
endif()

add_neverd_unittest(NeverDExecutionBudgetTests
  ExecutionBudgetTests.cpp
  LINK_COMPONENTS EmulationRuntime)

add_neverd_unittest(NeverDIntegerABITests
  IntegerABITests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationABI EmulationCPU
  LINK_LIBS LLVMObject)
if(NEVERD_TEST_CLANG_EXECUTABLE)
  set(_abi_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/abi-fixtures")
  file(MAKE_DIRECTORY "${_abi_fixture_dir}")
  set(_abi_outputs)
  foreach(_abi_name Win64 SysVAMD64 AAPCS64)
    if(_abi_name STREQUAL "Win64")
      set(_abi_target x86_64-pc-windows-msvc)
    elseif(_abi_name STREQUAL "SysVAMD64")
      set(_abi_target x86_64-unknown-linux-gnu)
    else()
      set(_abi_target aarch64-unknown-linux-gnu)
    endif()
    set(_abi_object "${_abi_fixture_dir}/${_abi_name}.obj")
    add_custom_command(OUTPUT "${_abi_object}"
      COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}" "--target=${_abi_target}"
        -std=c11 -ffreestanding -fno-stack-protector -fno-vectorize
        -fno-slp-vectorize -O1 -c
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/integer_call.c" -o "${_abi_object}"
      DEPENDS fixtures/integer_call.c
      VERBATIM)
    list(APPEND _abi_outputs "${_abi_object}")
  endforeach()
  add_custom_target(NeverDIntegerABIFixtures DEPENDS ${_abi_outputs})
  add_dependencies(NeverDIntegerABITests NeverDIntegerABIFixtures)
  target_compile_definitions(NeverDIntegerABITests PRIVATE
    NEVERD_ABI_FIXTURE_DIR="${_abi_fixture_dir}")
endif()

add_neverd_unittest(NeverDServiceRequestTests
  ServiceRequestTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDServiceRequestTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

add_neverd_unittest(NeverDUserExecutionTests
  UserExecutionTests.cpp
  UserProjectionTests.cpp
  TIMEOUT 15
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDUserExecutionTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NEVERD_EMULATION_BACKEND_KVM)
  add_neverd_unittest(NeverDKvmRunTests
    KvmRunTests.cpp
    KvmHandoffTests.cpp
    TIMEOUT 10
    LINK_COMPONENTS EmulationNative)
  target_include_directories(NeverDKvmRunTests PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/emulation)
  target_link_options(NeverDKvmRunTests PRIVATE "-Wl,--wrap=ioctl")
  add_neverd_unittest(NeverDKvmStateTransferTests
    KvmStateTransferTests.cpp
    TIMEOUT 15
    LINK_COMPONENTS EmulationCPU)
  target_include_directories(NeverDKvmStateTransferTests PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/emulation)
  target_link_options(NeverDKvmStateTransferTests PRIVATE "-Wl,--wrap=ioctl")
  add_neverd_unittest(NeverDKvmCancellationTests
    KvmCancellationTests.cpp
    TIMEOUT 10
    LINK_COMPONENTS EmulationNative)
  target_include_directories(NeverDKvmCancellationTests PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/emulation)
endif()

add_neverd_unittest(NeverDRunControlTests
  RunControlTests.cpp
  NativeEntryTests.cpp
  WhpEntryControlTests.cpp
  WhpXsaveTests.cpp
  WhpStateTransferTests.cpp
  WhpMemoryTests.cpp
  WhpResourceTests.cpp
  MachineRunControlTests.cpp
  TIMEOUT 10
  LINK_COMPONENTS EmulationCPU)
target_include_directories(NeverDRunControlTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)
if(WIN32 AND NEVERD_EMULATION_BACKEND_WHP)
  target_compile_definitions(NeverDRunControlTests PRIVATE
    NEVERD_EMULATION_WHP=1 NOMINMAX)
endif()

add_neverd_unittest(NeverDExecutionConfigurationTests
  ExecutionConfigurationTests.cpp
  ExecutionReportTests.cpp
  LINK_COMPONENTS EmulationCPU)

if(TARGET neverd_shared AND TARGET neverd)
  add_neverd_unittest(NeverDExecutionCapabilitiesPublicTests
    ExecutionCapabilitiesPublicTests.cpp
    LINK_LIBS neverd_shared)
  target_include_directories(NeverDExecutionCapabilitiesPublicTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(NeverDExecutionCapabilitiesPublicTests PRIVATE
    NEVERD_CPU_CLI="$<TARGET_FILE:neverd>")
  if(NOT APPLE)
    target_link_libraries(NeverDExecutionCapabilitiesPublicTests PRIVATE LLVMSupport)
  endif()
  add_dependencies(NeverDExecutionCapabilitiesPublicTests neverd)
endif()

if(NEVERD_EMULATION_BACKEND_UNICORN)
  add_executable(NeverDUnicornDecodeTests
    ${CMAKE_SOURCE_DIR}/third_party/unicorn/tests/unit/test_x86_decode_boundaries.c)
  if(MSVC)
    target_include_directories(NeverDUnicornDecodeTests PRIVATE
      ${CMAKE_SOURCE_DIR}/third_party/unicorn/msvc)
  else()
    target_include_directories(NeverDUnicornDecodeTests PRIVATE ${CMAKE_BINARY_DIR})
  endif()
  target_link_libraries(NeverDUnicornDecodeTests PRIVATE unicorn)
  add_dependencies(NeverDUnitTests NeverDUnicornDecodeTests)
  add_test(NAME UnicornDecode.RegisterU COMMAND NeverDUnicornDecodeTests
    register_u_is_not_an_apx_index_bit)
  add_test(NAME UnicornDecode.RoundMemory COMMAND NeverDUnicornDecodeTests
    round_memory_fault_priority_and_recovery)
  set_tests_properties(UnicornDecode.RegisterU UnicornDecode.RoundMemory PROPERTIES
    LABELS NeverDUnicornDecodeTests TIMEOUT 15)
  target_compile_definitions(NeverDExecutionConfigurationTests PRIVATE NEVERD_TEST_UNICORN=1)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_neverd_unittest(NeverDUnicornStateTransferTests
      UnicornStateTransferTests.cpp
      TIMEOUT 15
      LINK_COMPONENTS EmulationCPU
      LINK_LIBS unicorn)
    target_include_directories(NeverDUnicornStateTransferTests PRIVATE
      ${CMAKE_SOURCE_DIR}/lib/emulation)
    target_link_options(NeverDUnicornStateTransferTests PRIVATE
      "-Wl,--wrap=uc_reg_read")
    add_neverd_unittest(NeverDUnicornMachineControlTests
      UnicornMachineControlTests.cpp
      TIMEOUT 15
      LINK_COMPONENTS EmulationCPU
      LINK_LIBS unicorn)
    target_include_directories(NeverDUnicornMachineControlTests PRIVATE
      ${CMAKE_SOURCE_DIR}/lib/emulation)
    target_link_options(NeverDUnicornMachineControlTests PRIVATE
      "-Wl,--wrap=uc_emu_start" "-Wl,--wrap=uc_reg_read")
    add_executable(NeverDUnicornDeadlineTests
      ${CMAKE_SOURCE_DIR}/third_party/unicorn/tests/unit/test_pre_entry_deadline.c)
    target_include_directories(NeverDUnicornDeadlineTests PRIVATE
      ${CMAKE_BINARY_DIR})
    get_target_property(_unicorn_test_options unicorn COMPILE_OPTIONS)
    target_compile_options(NeverDUnicornDeadlineTests PRIVATE
      ${_unicorn_test_options})
    target_link_libraries(NeverDUnicornDeadlineTests PRIVATE unicorn pthread)
    target_link_options(NeverDUnicornDeadlineTests PRIVATE
      "-Wl,--wrap=pthread_create")
    add_test(NAME UnicornDeadline.BeforeEntry COMMAND NeverDUnicornDeadlineTests)
    set_tests_properties(UnicornDeadline.BeforeEntry PROPERTIES
      LABELS NeverDUnicornDeadlineTests TIMEOUT 15)
  endif()
  add_neverd_unittest(NeverDCPUEmulationTests
    CPUArchitectureTests.cpp
    ExecutionExitTests.cpp
    AArch64ProjectionTests.cpp
    LINK_COMPONENTS EmulationCPU
    LINK_LIBS unicorn)
  target_include_directories(NeverDCPUEmulationTests PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/emulation)
endif()

if(NOT NEVERD_ENABLE_DRIVER_EMULATION)
  return()
endif()

find_program(NEVERD_DRIVER_LLD_LINK NAMES lld-link
  HINTS "${LLVM_TOOLS_BINARY_DIR}")
set(NEVERD_KMDF_FIXTURE "" CACHE FILEPATH
  "Optional original KMDF lifecycle fixture linked with genuine WDK libraries")
set(NEVERD_KMDF_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF lifecycle fixture linked with active CFG")
set(NEVERD_KMDF_CONTROL_FIXTURE "" CACHE FILEPATH
  "Optional original KMDF control-device fixture linked with genuine WDK libraries")
set(NEVERD_KMDF_CONTROL_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF control-device fixture linked with active CFG")
set(NEVERD_KMDF_PNP_FIXTURE "" CACHE FILEPATH
  "Optional original KMDF PnP fixture linked with genuine WDK libraries")
set(NEVERD_KMDF_PNP_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF PnP fixture linked with active CFG")
set(NEVERD_KMDF_LOCK_FIXTURE "" CACHE FILEPATH
  "Optional original KMDF lock fixture linked with genuine WDK libraries")
set(NEVERD_KMDF_LOCK_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF lock fixture linked with active CFG")
set(NEVERD_WDM_OWNED_IRP_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK caller-allocated internal IRP fixture")
set(NEVERD_WDM_OWNED_IRP_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK caller-allocated internal IRP fixture with active CFG")
set(NEVERD_WDM_WAIT_WAKE_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK native wait/wake fixture")
set(NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK native wait/wake fixture with active CFG")
set(NEVERD_WDM_MULTIPLE_WAIT_FIXTURE "" CACHE FILEPATH
  "Optional original genuine WDK multiple-object wait fixture")
set(NEVERD_WDM_MULTIPLE_WAIT_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK multiple-object wait fixture with active CFG")
set(NEVERD_WDM_USB_IDLE_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK native USB idle fixture")
set(NEVERD_WDM_USB_IDLE_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK native USB idle fixture with active CFG")
set(NEVERD_KMDF_USB_IDLE_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK framework USB idle fixture")
set(NEVERD_KMDF_USB_IDLE_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK framework USB idle fixture with active CFG")
set(NEVERD_WDM_STACK_FIXTURE "" CACHE FILEPATH
  "Optional original WDM device-stack fixture linked with genuine WDK libraries")
set(NEVERD_WDM_STACK_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK device-stack fixture linked with active CFG")
set(NEVERD_WDM_PNP_FIXTURE "" CACHE FILEPATH
  "Optional original WDM PnP fixture linked with genuine WDK libraries")
set(NEVERD_WDM_PNP_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK PnP fixture linked with active CFG")
set(NEVERD_WDM_POWER_FIXTURE "" CACHE FILEPATH
  "Optional original WDM power fixture linked with genuine WDK libraries")
set(NEVERD_WDM_POWER_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK power fixture linked with active CFG")
set(NEVERD_KMDF_POFX_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF custom component power fixture")
set(NEVERD_KMDF_POFX_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF custom component power fixture with active CFG")
set(NEVERD_KMDF_CHILD_WAKE_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF parent and child wake fixture")
set(NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK KMDF parent and child wake fixture with active CFG")
set(NEVERD_WDM_POFX_FIXTURE "" CACHE FILEPATH
  "Optional original WDM component power fixture linked with genuine WDK libraries")
set(NEVERD_WDM_POFX_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK component power fixture linked with active CFG")
set(NEVERD_WDM_RESOURCE_FIXTURE "" CACHE FILEPATH
  "Optional original WDM resource/MMIO fixture linked with genuine WDK libraries")
set(NEVERD_WDM_RESOURCE_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK resource/MMIO fixture linked with active CFG")
set(NEVERD_WDM_DMA_FIXTURE "" CACHE FILEPATH
  "Optional original WDM DMA fixture linked with genuine WDK libraries")
set(NEVERD_WDM_DMA_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK DMA fixture linked with active CFG")
set(NEVERD_WDM_SEH_FIXTURE "" CACHE FILEPATH
  "Optional original WDM API exception fixture linked with genuine WDK libraries")
set(NEVERD_WDM_SEH_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK API exception fixture linked with active CFG")
set(NEVERD_WDM_USER_MAPPING_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK user-mode MDL mapping driver")
set(NEVERD_WDM_USER_MAPPING_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK user-mode MDL mapping driver with active CFG")
set(NEVERD_WDM_NEITHER_FIXTURE "" CACHE FILEPATH
  "Optional original WDM user-buffer fixture linked with genuine WDK libraries")
set(NEVERD_WDM_NEITHER_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK user-buffer fixture linked with active CFG")
set(NEVERD_WDM_DMA_CHANNEL_FIXTURE "" CACHE FILEPATH
  "Optional original WDM DMA channel fixture linked with genuine WDK libraries")
set(NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK DMA channel fixture linked with active CFG")
set(NEVERD_WDM_INTERRUPT_FIXTURE "" CACHE FILEPATH
  "Optional original WDM interrupt fixture linked with genuine WDK libraries")
set(NEVERD_WDM_INTERRUPT_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK interrupt fixture linked with active CFG")
set(NEVERD_WDM_REMOVE_LOCK_FIXTURE "" CACHE FILEPATH
  "Optional original WDM remove-lock fixture linked with genuine WDK libraries")
set(NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK remove-lock fixture linked with active CFG")
set(NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK DBG remove-lock fixture")
set(NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE "" CACHE FILEPATH
  "Optional genuine WDK DBG remove-lock fixture linked with active CFG")
if(NOT NEVERD_TEST_CLANG_EXECUTABLE OR NOT NEVERD_DRIVER_LLD_LINK)
  message(STATUS "Driver emulation fixtures unavailable: Clang and lld-link are required")
  return()
endif()

set(_driver_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${_driver_fixture_dir}")
set(_driver_import_lib "${_driver_fixture_dir}/ntoskrnl.lib")
add_custom_command(OUTPUT "${_driver_import_lib}"
  COMMAND "${NEVERD_DRIVER_LLD_LINK}" /lib /machine:x64
    "/def:${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ntoskrnl.def"
    "/out:${_driver_import_lib}"
  DEPENDS fixtures/ntoskrnl.def
  VERBATIM)

include(UnpackDriverFixtures.cmake)

set(_driver_hal_lib "${_driver_fixture_dir}/hal.lib")
add_custom_command(OUTPUT "${_driver_hal_lib}"
  COMMAND "${NEVERD_DRIVER_LLD_LINK}" /lib /machine:x64
    "/def:${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hal.def"
    "/out:${_driver_hal_lib}"
  DEPENDS fixtures/hal.def
  VERBATIM)

set(_driver_outputs)
set(_driver_case 0)
foreach(_driver_name success failure unknown fault loop devices readonly privileged opaque gs apifault pendingentry opaqueargument)
  set(_driver_object "${_driver_fixture_dir}/${_driver_name}.obj")
  set(_driver_image "${_driver_fixture_dir}/${_driver_name}.sys")
  add_custom_command(OUTPUT "${_driver_image}" "${_driver_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding
      -fno-stack-protector -O1 "-DDRIVER_CASE=${_driver_case}" -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/driver.c" -o "${_driver_object}"
    COMMAND "${NEVERD_DRIVER_LLD_LINK}" /machine:x64 /entry:DriverEntry
      /subsystem:native /driver /nodefaultlib /fixed /base:0x180000000
      "/out:${_driver_image}" "${_driver_object}" "${_driver_import_lib}"
    DEPENDS fixtures/driver.c "${_driver_import_lib}"
    VERBATIM)
  list(APPEND _driver_outputs "${_driver_image}")
  math(EXPR _driver_case "${_driver_case} + 1")
endforeach()
# The loader fixture deliberately retains relocations and a compiler-inserted
# stack protector. The WDM fixture covers the complete synchronous lifecycle.
foreach(_driver_name loader driver_io driver_direct driver_runtime driver_mdl driver_registry driver_async driver_dispatcher driver_context_limits driver_preemptive driver_thread_priority)
  set(_driver_object "${_driver_fixture_dir}/${_driver_name}.obj")
  set(_driver_image "${_driver_fixture_dir}/${_driver_name}.sys")
  if(_driver_name STREQUAL "loader")
    set(_driver_source fixtures/driver_loader.c)
    set(_driver_stack_flag -fstack-protector-all)
    set(_driver_entry GsDriverEntry)
  else()
    set(_driver_source "fixtures/${_driver_name}.c")
    set(_driver_stack_flag -fno-stack-protector)
    set(_driver_entry DriverEntry)
  endif()
  set(_driver_additional_dependencies)
  set(_driver_additional_libraries)
  if(_driver_name STREQUAL "driver_runtime")
    list(APPEND _driver_additional_libraries "${_driver_hal_lib}")
  endif()
  if(_driver_name STREQUAL "driver_mdl")
    list(APPEND _driver_additional_dependencies fixtures/driver_mdl_actions.h)
  endif()
  if(_driver_name STREQUAL "driver_async")
    list(APPEND _driver_additional_dependencies
      fixtures/driver_io.c fixtures/DriverAsyncCases.def
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelValues.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/WindowsKernelLayout.def")
  endif()
  if(_driver_name STREQUAL "driver_dispatcher")
    list(APPEND _driver_additional_dependencies
      fixtures/driver_io.c fixtures/DriverDispatcherCases.def
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelDispatcherValues.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelValues.def")
  endif()
  if(_driver_name STREQUAL "driver_preemptive")
    list(APPEND _driver_additional_dependencies
      fixtures/driver_io.c fixtures/DriverPreemptiveCases.def
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelValues.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/WindowsKernelLayout.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelDispatcherValues.def")
  endif()
  if(_driver_name STREQUAL "driver_context_limits")
    list(APPEND _driver_additional_dependencies
      fixtures/driver_io.c fixtures/DriverContextLimitCases.def
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelValues.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelPoolFlags.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/WindowsKernelLayout.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelDispatcherValues.def")
  endif()
  if(_driver_name STREQUAL "driver_thread_priority")
    list(APPEND _driver_additional_dependencies
      fixtures/driver_io.c fixtures/DriverThreadPriorityCases.def
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelThreadPriorities.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelValues.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/WindowsKernelLayout.def"
      "${CMAKE_SOURCE_DIR}/lib/emulation/os/windows/kernel/KernelDispatcherValues.def")
  endif()
  add_custom_command(OUTPUT "${_driver_image}" "${_driver_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding
      ${_driver_stack_flag} -O1 -c
      "${CMAKE_CURRENT_SOURCE_DIR}/${_driver_source}" -o "${_driver_object}"
    COMMAND "${NEVERD_DRIVER_LLD_LINK}" /machine:x64 "/entry:${_driver_entry}"
      /subsystem:native /driver /nodefaultlib /dynamicbase /base:0x180000000
      "/out:${_driver_image}" "${_driver_object}" "${_driver_import_lib}"
      ${_driver_additional_libraries}
    DEPENDS "${_driver_source}" "${_driver_import_lib}"
      ${_driver_additional_libraries}
      ${_driver_additional_dependencies}
    VERBATIM)
  list(APPEND _driver_outputs "${_driver_image}")
endforeach()

# These original, self-contained fixtures exercise compiler-generated guard
# calls. Keep the inactive link variant so instrumentation alone does not imply
# that CFG enforcement is enabled.
foreach(_driver_name driver_guard driver_guard_inactive
    driver_guard_invalid_check driver_guard_invalid_dispatch)
  set(_driver_object "${_driver_fixture_dir}/${_driver_name}.obj")
  set(_driver_image "${_driver_fixture_dir}/${_driver_name}.sys")
  set(_driver_guard_compile_flags)
  if(_driver_name STREQUAL "driver_guard_invalid_check")
    list(APPEND _driver_guard_compile_flags -DNEVERD_GUARD_INVALID_TARGET)
  elseif(_driver_name STREQUAL "driver_guard_invalid_dispatch")
    list(APPEND _driver_guard_compile_flags -DNEVERD_GUARD_INVALID_DISPATCH)
  endif()
  set(_driver_guard_link_flags /guard:cf)
  if(_driver_name STREQUAL "driver_guard_inactive")
    set(_driver_guard_link_flags)
  endif()
  add_custom_command(OUTPUT "${_driver_image}" "${_driver_object}"
    COMMAND "${NEVERD_TEST_CLANG_EXECUTABLE}"
      --target=x86_64-pc-windows-msvc -std=c11 -ffreestanding
      -fno-stack-protector -O1 -Xclang -cfguard
      ${_driver_guard_compile_flags} -c
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/driver_guard.c"
      -o "${_driver_object}"
    COMMAND "${NEVERD_DRIVER_LLD_LINK}" /machine:x64 /entry:DriverEntry
      /subsystem:native /driver /nodefaultlib /dynamicbase /base:0x180000000
      ${_driver_guard_link_flags} "/out:${_driver_image}" "${_driver_object}"
    DEPENDS fixtures/driver_guard.c
    VERBATIM)
  list(APPEND _driver_outputs "${_driver_image}")
endforeach()
add_custom_target(NeverDDriverFixtures DEPENDS ${_driver_outputs})

# Microsoft kit files are optional external validation inputs. Never assume a
# local ignored SDK directory exists, and keep missing coverage visible.
set(_driver_kmdf_definitions)
set(_driver_has_kmdf_fixture FALSE)
set(_driver_has_kmdf_control_fixture FALSE)
set(_driver_has_kmdf_pnp_fixture FALSE)
set(_driver_has_kmdf_lock_fixture FALSE)
foreach(_driver_kmdf_variable NEVERD_KMDF_FIXTURE NEVERD_KMDF_CFG_FIXTURE
    NEVERD_KMDF_CONTROL_FIXTURE NEVERD_KMDF_CONTROL_CFG_FIXTURE
    NEVERD_KMDF_PNP_FIXTURE NEVERD_KMDF_PNP_CFG_FIXTURE
    NEVERD_KMDF_LOCK_FIXTURE NEVERD_KMDF_LOCK_CFG_FIXTURE)
  if(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_fixture)
    message(STATUS "Driver KMDF CFG tests skipped: NEVERD_KMDF_FIXTURE is unavailable")
    continue()
  endif()
  if(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_CONTROL_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_control_fixture)
    message(STATUS "Driver KMDF control CFG tests skipped: NEVERD_KMDF_CONTROL_FIXTURE is unavailable")
    continue()
  endif()
  if(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_PNP_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_pnp_fixture)
    message(STATUS "Driver KMDF PnP CFG tests skipped: NEVERD_KMDF_PNP_FIXTURE is unavailable")
    continue()
  endif()
  if(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_LOCK_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_lock_fixture)
    message(STATUS "Driver KMDF lock CFG tests skipped: NEVERD_KMDF_LOCK_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_kmdf_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_kmdf_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_kmdf_variable}}")
    get_filename_component(_driver_kmdf_path
      "${${_driver_kmdf_variable}}" ABSOLUTE)
    list(APPEND _driver_kmdf_definitions
      "${_driver_kmdf_variable}=\"${_driver_kmdf_path}\"")
    if(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_FIXTURE")
      set(_driver_has_kmdf_fixture TRUE)
    elseif(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_CONTROL_FIXTURE")
      set(_driver_has_kmdf_control_fixture TRUE)
    elseif(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_PNP_FIXTURE")
      set(_driver_has_kmdf_pnp_fixture TRUE)
    elseif(_driver_kmdf_variable STREQUAL "NEVERD_KMDF_LOCK_FIXTURE")
      set(_driver_has_kmdf_lock_fixture TRUE)
    endif()
    message(STATUS "Driver KMDF fixture ${_driver_kmdf_variable}: ${_driver_kmdf_path}")
  else()
    message(STATUS "Driver KMDF tests requiring ${_driver_kmdf_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_owned_irp_definitions)
set(_driver_has_wdm_owned_irp_fixture FALSE)
foreach(_driver_owned_irp_variable NEVERD_WDM_OWNED_IRP_FIXTURE
    NEVERD_WDM_OWNED_IRP_CFG_FIXTURE)
  if(_driver_owned_irp_variable STREQUAL "NEVERD_WDM_OWNED_IRP_CFG_FIXTURE"
      AND NOT _driver_has_wdm_owned_irp_fixture)
    message(STATUS "Driver WDM caller-owned IRP CFG tests skipped: NEVERD_WDM_OWNED_IRP_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_owned_irp_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_owned_irp_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_owned_irp_variable}}")
    get_filename_component(_driver_owned_irp_path
      "${${_driver_owned_irp_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_owned_irp_definitions
      "${_driver_owned_irp_variable}=\"${_driver_owned_irp_path}\"")
    if(_driver_owned_irp_variable STREQUAL "NEVERD_WDM_OWNED_IRP_FIXTURE")
      set(_driver_has_wdm_owned_irp_fixture TRUE)
    endif()
    message(STATUS "Driver WDM caller-owned IRP fixture ${_driver_owned_irp_variable}: ${_driver_owned_irp_path}")
  else()
    message(STATUS "Driver WDM caller-owned IRP tests requiring ${_driver_owned_irp_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_wait_wake_definitions)
set(_driver_has_wdm_wait_wake_fixture FALSE)
foreach(_driver_wait_wake_variable NEVERD_WDM_WAIT_WAKE_FIXTURE
    NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE)
  if(_driver_wait_wake_variable STREQUAL "NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE"
      AND NOT _driver_has_wdm_wait_wake_fixture)
    message(STATUS "Driver WDM native wait/wake CFG tests skipped: NEVERD_WDM_WAIT_WAKE_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_wait_wake_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_wait_wake_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_wait_wake_variable}}")
    get_filename_component(_driver_wait_wake_path
      "${${_driver_wait_wake_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_wait_wake_definitions
      "${_driver_wait_wake_variable}=\"${_driver_wait_wake_path}\"")
    if(_driver_wait_wake_variable STREQUAL "NEVERD_WDM_WAIT_WAKE_FIXTURE")
      set(_driver_has_wdm_wait_wake_fixture TRUE)
    endif()
    message(STATUS "Driver WDM native wait/wake fixture ${_driver_wait_wake_variable}: ${_driver_wait_wake_path}")
  else()
    message(STATUS "Driver WDM native wait/wake tests requiring ${_driver_wait_wake_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_multiple_wait_definitions)
set(_driver_has_wdm_multiple_wait_fixture FALSE)
foreach(_driver_multiple_wait_variable NEVERD_WDM_MULTIPLE_WAIT_FIXTURE
    NEVERD_WDM_MULTIPLE_WAIT_CFG_FIXTURE)
  if(_driver_multiple_wait_variable STREQUAL "NEVERD_WDM_MULTIPLE_WAIT_CFG_FIXTURE"
      AND NOT _driver_has_wdm_multiple_wait_fixture)
    message(STATUS "Driver WDM multiple-object wait CFG tests skipped: NEVERD_WDM_MULTIPLE_WAIT_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_multiple_wait_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_multiple_wait_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_multiple_wait_variable}}")
    get_filename_component(_driver_multiple_wait_path
      "${${_driver_multiple_wait_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_multiple_wait_definitions
      "${_driver_multiple_wait_variable}=\"${_driver_multiple_wait_path}\"")
    if(_driver_multiple_wait_variable STREQUAL "NEVERD_WDM_MULTIPLE_WAIT_FIXTURE")
      set(_driver_has_wdm_multiple_wait_fixture TRUE)
    endif()
    message(STATUS "Driver WDM multiple-object wait fixture ${_driver_multiple_wait_variable}: ${_driver_multiple_wait_path}")
  else()
    message(STATUS "Driver WDM multiple-object wait tests requiring ${_driver_multiple_wait_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_usb_idle_definitions)
set(_driver_has_wdm_usb_idle_fixture FALSE)
foreach(_driver_usb_idle_variable NEVERD_WDM_USB_IDLE_FIXTURE
    NEVERD_WDM_USB_IDLE_CFG_FIXTURE)
  if(_driver_usb_idle_variable STREQUAL "NEVERD_WDM_USB_IDLE_CFG_FIXTURE"
      AND NOT _driver_has_wdm_usb_idle_fixture)
    message(STATUS "Driver WDM USB idle CFG tests skipped: NEVERD_WDM_USB_IDLE_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_usb_idle_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_usb_idle_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_usb_idle_variable}}")
    get_filename_component(_driver_usb_idle_path
      "${${_driver_usb_idle_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_usb_idle_definitions
      "${_driver_usb_idle_variable}=\"${_driver_usb_idle_path}\"")
    if(_driver_usb_idle_variable STREQUAL "NEVERD_WDM_USB_IDLE_FIXTURE")
      set(_driver_has_wdm_usb_idle_fixture TRUE)
    endif()
    message(STATUS "Driver WDM USB idle fixture ${_driver_usb_idle_variable}: ${_driver_usb_idle_path}")
  else()
    message(STATUS "Driver WDM USB idle tests requiring ${_driver_usb_idle_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_kmdf_usb_idle_definitions)
set(_driver_has_kmdf_usb_idle_fixture FALSE)
foreach(_driver_kmdf_usb_idle_variable NEVERD_KMDF_USB_IDLE_FIXTURE
    NEVERD_KMDF_USB_IDLE_CFG_FIXTURE)
  if(_driver_kmdf_usb_idle_variable STREQUAL "NEVERD_KMDF_USB_IDLE_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_usb_idle_fixture)
    message(STATUS "Driver KMDF USB idle CFG tests skipped: NEVERD_KMDF_USB_IDLE_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_kmdf_usb_idle_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_kmdf_usb_idle_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_kmdf_usb_idle_variable}}")
    get_filename_component(_driver_kmdf_usb_idle_path
      "${${_driver_kmdf_usb_idle_variable}}" ABSOLUTE)
    list(APPEND _driver_kmdf_usb_idle_definitions
      "${_driver_kmdf_usb_idle_variable}=\"${_driver_kmdf_usb_idle_path}\"")
    if(_driver_kmdf_usb_idle_variable STREQUAL "NEVERD_KMDF_USB_IDLE_FIXTURE")
      set(_driver_has_kmdf_usb_idle_fixture TRUE)
    endif()
    message(STATUS "Driver KMDF USB idle fixture ${_driver_kmdf_usb_idle_variable}: ${_driver_kmdf_usb_idle_path}")
  else()
    message(STATUS "Driver KMDF USB idle tests requiring ${_driver_kmdf_usb_idle_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_stack_definitions)
set(_driver_has_wdm_stack_fixture FALSE)
foreach(_driver_stack_variable NEVERD_WDM_STACK_FIXTURE
    NEVERD_WDM_STACK_CFG_FIXTURE)
  if(_driver_stack_variable STREQUAL "NEVERD_WDM_STACK_CFG_FIXTURE"
      AND NOT _driver_has_wdm_stack_fixture)
    message(STATUS "Driver WDM stack CFG tests skipped: NEVERD_WDM_STACK_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_stack_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_stack_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_stack_variable}}")
    get_filename_component(_driver_stack_path
      "${${_driver_stack_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_stack_definitions
      "${_driver_stack_variable}=\"${_driver_stack_path}\"")
    if(_driver_stack_variable STREQUAL "NEVERD_WDM_STACK_FIXTURE")
      set(_driver_has_wdm_stack_fixture TRUE)
    endif()
    message(STATUS "Driver WDM stack fixture ${_driver_stack_variable}: ${_driver_stack_path}")
  else()
    message(STATUS "Driver WDM stack tests requiring ${_driver_stack_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_pnp_definitions)
set(_driver_has_wdm_pnp_fixture FALSE)
foreach(_driver_pnp_variable NEVERD_WDM_PNP_FIXTURE
    NEVERD_WDM_PNP_CFG_FIXTURE)
  if(_driver_pnp_variable STREQUAL "NEVERD_WDM_PNP_CFG_FIXTURE"
      AND NOT _driver_has_wdm_pnp_fixture)
    message(STATUS "Driver WDM PnP CFG tests skipped: NEVERD_WDM_PNP_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_pnp_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_pnp_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_pnp_variable}}")
    get_filename_component(_driver_pnp_path
      "${${_driver_pnp_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_pnp_definitions
      "${_driver_pnp_variable}=\"${_driver_pnp_path}\"")
    if(_driver_pnp_variable STREQUAL "NEVERD_WDM_PNP_FIXTURE")
      set(_driver_has_wdm_pnp_fixture TRUE)
    endif()
    message(STATUS "Driver WDM PnP fixture ${_driver_pnp_variable}: ${_driver_pnp_path}")
  else()
    message(STATUS "Driver WDM PnP tests requiring ${_driver_pnp_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_power_definitions)
set(_driver_has_wdm_power_fixture FALSE)
foreach(_driver_power_variable NEVERD_WDM_POWER_FIXTURE
    NEVERD_WDM_POWER_CFG_FIXTURE)
  if(_driver_power_variable STREQUAL "NEVERD_WDM_POWER_CFG_FIXTURE"
      AND NOT _driver_has_wdm_power_fixture)
    message(STATUS "Driver WDM power CFG tests skipped: NEVERD_WDM_POWER_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_power_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_power_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_power_variable}}")
    get_filename_component(_driver_power_path
      "${${_driver_power_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_power_definitions
      "${_driver_power_variable}=\"${_driver_power_path}\"")
    if(_driver_power_variable STREQUAL "NEVERD_WDM_POWER_FIXTURE")
      set(_driver_has_wdm_power_fixture TRUE)
    endif()
    message(STATUS "Driver WDM power fixture ${_driver_power_variable}: ${_driver_power_path}")
  else()
    message(STATUS "Driver WDM power tests requiring ${_driver_power_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_pofx_definitions)
set(_driver_has_wdm_pofx_fixture FALSE)
foreach(_driver_pofx_variable NEVERD_WDM_POFX_FIXTURE
    NEVERD_WDM_POFX_CFG_FIXTURE)
  if(_driver_pofx_variable STREQUAL "NEVERD_WDM_POFX_CFG_FIXTURE"
      AND NOT _driver_has_wdm_pofx_fixture)
    message(STATUS "Driver WDM component power CFG tests skipped: NEVERD_WDM_POFX_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_pofx_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_pofx_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_pofx_variable}}")
    get_filename_component(_driver_pofx_path
      "${${_driver_pofx_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_pofx_definitions
      "${_driver_pofx_variable}=\"${_driver_pofx_path}\"")
    if(_driver_pofx_variable STREQUAL "NEVERD_WDM_POFX_FIXTURE")
      set(_driver_has_wdm_pofx_fixture TRUE)
    endif()
    message(STATUS "Driver WDM component power fixture ${_driver_pofx_variable}: ${_driver_pofx_path}")
  else()
    message(STATUS "Driver WDM component power tests requiring ${_driver_pofx_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_kmdf_pofx_definitions)
set(_driver_has_kmdf_pofx_fixture FALSE)
foreach(_driver_custom_pofx_variable NEVERD_KMDF_POFX_FIXTURE
    NEVERD_KMDF_POFX_CFG_FIXTURE)
  if(_driver_custom_pofx_variable STREQUAL "NEVERD_KMDF_POFX_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_pofx_fixture)
    message(STATUS "Driver KMDF custom component power CFG tests skipped: NEVERD_KMDF_POFX_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_custom_pofx_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_custom_pofx_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_custom_pofx_variable}}")
    get_filename_component(_driver_custom_pofx_path
      "${${_driver_custom_pofx_variable}}" ABSOLUTE)
    list(APPEND _driver_kmdf_pofx_definitions
      "${_driver_custom_pofx_variable}=\"${_driver_custom_pofx_path}\"")
    if(_driver_custom_pofx_variable STREQUAL "NEVERD_KMDF_POFX_FIXTURE")
      set(_driver_has_kmdf_pofx_fixture TRUE)
    endif()
    message(STATUS "Driver KMDF custom component power fixture ${_driver_custom_pofx_variable}: ${_driver_custom_pofx_path}")
  else()
    message(STATUS "Driver KMDF custom component power tests requiring ${_driver_custom_pofx_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_kmdf_child_wake_definitions)
set(_driver_has_kmdf_child_wake_fixture FALSE)
foreach(_driver_child_wake_variable NEVERD_KMDF_CHILD_WAKE_FIXTURE
    NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE)
  if(_driver_child_wake_variable STREQUAL "NEVERD_KMDF_CHILD_WAKE_CFG_FIXTURE"
      AND NOT _driver_has_kmdf_child_wake_fixture)
    message(STATUS "Driver KMDF child wake CFG tests skipped: NEVERD_KMDF_CHILD_WAKE_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_child_wake_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_child_wake_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_child_wake_variable}}")
    get_filename_component(_driver_child_wake_path
      "${${_driver_child_wake_variable}}" ABSOLUTE)
    list(APPEND _driver_kmdf_child_wake_definitions
      "${_driver_child_wake_variable}=\"${_driver_child_wake_path}\"")
    if(_driver_child_wake_variable STREQUAL "NEVERD_KMDF_CHILD_WAKE_FIXTURE")
      set(_driver_has_kmdf_child_wake_fixture TRUE)
    endif()
    message(STATUS "Driver KMDF child wake fixture ${_driver_child_wake_variable}: ${_driver_child_wake_path}")
  else()
    message(STATUS "Driver KMDF child wake tests requiring ${_driver_child_wake_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_remove_lock_definitions)
set(_driver_has_wdm_remove_lock_fixture FALSE)
foreach(_driver_remove_lock_variable NEVERD_WDM_REMOVE_LOCK_FIXTURE
    NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE
    NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE)
  if(NOT _driver_remove_lock_variable STREQUAL "NEVERD_WDM_REMOVE_LOCK_FIXTURE"
      AND NOT _driver_has_wdm_remove_lock_fixture)
    message(STATUS "Driver WDM remove-lock variant skipped: NEVERD_WDM_REMOVE_LOCK_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_remove_lock_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_remove_lock_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_remove_lock_variable}}")
    get_filename_component(_driver_remove_lock_path
      "${${_driver_remove_lock_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_remove_lock_definitions
      "${_driver_remove_lock_variable}=\"${_driver_remove_lock_path}\"")
    if(_driver_remove_lock_variable STREQUAL "NEVERD_WDM_REMOVE_LOCK_FIXTURE")
      set(_driver_has_wdm_remove_lock_fixture TRUE)
    endif()
    message(STATUS "Driver WDM remove-lock fixture ${_driver_remove_lock_variable}: ${_driver_remove_lock_path}")
  else()
    message(STATUS "Driver WDM remove-lock tests requiring ${_driver_remove_lock_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_resource_definitions)
set(_driver_has_wdm_resource_fixture FALSE)
foreach(_driver_resource_variable NEVERD_WDM_RESOURCE_FIXTURE
    NEVERD_WDM_RESOURCE_CFG_FIXTURE)
  if(NOT _driver_resource_variable STREQUAL "NEVERD_WDM_RESOURCE_FIXTURE"
      AND NOT _driver_has_wdm_resource_fixture)
    message(STATUS "Driver WDM resource variant skipped: NEVERD_WDM_RESOURCE_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_resource_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_resource_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_resource_variable}}")
    get_filename_component(_driver_resource_path
      "${${_driver_resource_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_resource_definitions
      "${_driver_resource_variable}=\"${_driver_resource_path}\"")
    if(_driver_resource_variable STREQUAL "NEVERD_WDM_RESOURCE_FIXTURE")
      set(_driver_has_wdm_resource_fixture TRUE)
    endif()
    message(STATUS "Driver WDM resource fixture ${_driver_resource_variable}: ${_driver_resource_path}")
  else()
    message(STATUS "Driver WDM resource tests requiring ${_driver_resource_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_interrupt_definitions)
set(_driver_has_wdm_interrupt_fixture FALSE)
foreach(_driver_interrupt_variable NEVERD_WDM_INTERRUPT_FIXTURE
    NEVERD_WDM_INTERRUPT_CFG_FIXTURE)
  if(NOT _driver_interrupt_variable STREQUAL "NEVERD_WDM_INTERRUPT_FIXTURE"
      AND NOT _driver_has_wdm_interrupt_fixture)
    message(STATUS "Driver WDM interrupt variant skipped: NEVERD_WDM_INTERRUPT_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_interrupt_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_interrupt_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_interrupt_variable}}")
    get_filename_component(_driver_interrupt_path
      "${${_driver_interrupt_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_interrupt_definitions
      "${_driver_interrupt_variable}=\"${_driver_interrupt_path}\"")
    if(_driver_interrupt_variable STREQUAL "NEVERD_WDM_INTERRUPT_FIXTURE")
      set(_driver_has_wdm_interrupt_fixture TRUE)
    endif()
    message(STATUS "Driver WDM interrupt fixture ${_driver_interrupt_variable}: ${_driver_interrupt_path}")
  else()
    message(STATUS "Driver WDM interrupt tests requiring ${_driver_interrupt_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_dma_definitions)
set(_driver_has_wdm_dma_fixture FALSE)
foreach(_driver_dma_variable NEVERD_WDM_DMA_FIXTURE
    NEVERD_WDM_DMA_CFG_FIXTURE)
  if(NOT _driver_dma_variable STREQUAL "NEVERD_WDM_DMA_FIXTURE"
      AND NOT _driver_has_wdm_dma_fixture)
    message(STATUS "Driver WDM dma variant skipped: NEVERD_WDM_DMA_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_dma_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_dma_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_dma_variable}}")
    get_filename_component(_driver_dma_path
      "${${_driver_dma_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_dma_definitions
      "${_driver_dma_variable}=\"${_driver_dma_path}\"")
    if(_driver_dma_variable STREQUAL "NEVERD_WDM_DMA_FIXTURE")
      set(_driver_has_wdm_dma_fixture TRUE)
    endif()
    message(STATUS "Driver WDM dma fixture ${_driver_dma_variable}: ${_driver_dma_path}")
  else()
    message(STATUS "Driver WDM dma tests requiring ${_driver_dma_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_dma_channel_definitions)
set(_driver_has_wdm_dma_channel_fixture FALSE)
foreach(_driver_dma_channel_variable NEVERD_WDM_DMA_CHANNEL_FIXTURE
    NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE)
  if(NOT _driver_dma_channel_variable STREQUAL "NEVERD_WDM_DMA_CHANNEL_FIXTURE"
      AND NOT _driver_has_wdm_dma_channel_fixture)
    message(STATUS "Driver WDM DMA channel variant skipped: NEVERD_WDM_DMA_CHANNEL_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_dma_channel_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_dma_channel_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_dma_channel_variable}}")
    get_filename_component(_driver_dma_channel_path
      "${${_driver_dma_channel_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_dma_channel_definitions
      "${_driver_dma_channel_variable}=\"${_driver_dma_channel_path}\"")
    if(_driver_dma_channel_variable STREQUAL "NEVERD_WDM_DMA_CHANNEL_FIXTURE")
      set(_driver_has_wdm_dma_channel_fixture TRUE)
    endif()
    message(STATUS "Driver WDM DMA channel fixture ${_driver_dma_channel_variable}: ${_driver_dma_channel_path}")
  else()
    message(STATUS "Driver WDM DMA channel tests requiring ${_driver_dma_channel_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_seh_definitions)
set(_driver_has_wdm_seh_fixture FALSE)
foreach(_driver_seh_variable NEVERD_WDM_SEH_FIXTURE
    NEVERD_WDM_SEH_CFG_FIXTURE)
  if(NOT _driver_seh_variable STREQUAL "NEVERD_WDM_SEH_FIXTURE"
      AND NOT _driver_has_wdm_seh_fixture)
    message(STATUS "Driver WDM API exception variant skipped: NEVERD_WDM_SEH_FIXTURE is unavailable")
    continue()
  endif()
  if(NOT "${${_driver_seh_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_seh_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_seh_variable}}")
    get_filename_component(_driver_seh_path
      "${${_driver_seh_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_seh_definitions
      "${_driver_seh_variable}=\"${_driver_seh_path}\"")
    if(_driver_seh_variable STREQUAL "NEVERD_WDM_SEH_FIXTURE")
      set(_driver_has_wdm_seh_fixture TRUE)
    endif()
    message(STATUS "Driver WDM API exception fixture ${_driver_seh_variable}: ${_driver_seh_path}")
  else()
    message(STATUS "Driver WDM API exception tests requiring ${_driver_seh_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_neither_definitions)
set(_driver_has_wdm_neither_fixture FALSE)
foreach(_driver_neither_variable NEVERD_WDM_NEITHER_FIXTURE
    NEVERD_WDM_NEITHER_CFG_FIXTURE)
  if(NOT _driver_neither_variable STREQUAL "NEVERD_WDM_NEITHER_FIXTURE"
      AND NOT _driver_has_wdm_neither_fixture)
    continue()
  endif()
  if(NOT "${${_driver_neither_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_neither_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_neither_variable}}")
    get_filename_component(_driver_neither_path
      "${${_driver_neither_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_neither_definitions
      "${_driver_neither_variable}=\"${_driver_neither_path}\"")
    if(_driver_neither_variable STREQUAL "NEVERD_WDM_NEITHER_FIXTURE")
      set(_driver_has_wdm_neither_fixture TRUE)
    endif()
    message(STATUS "Driver WDM user-buffer fixture ${_driver_neither_variable}: ${_driver_neither_path}")
  else()
    message(STATUS "Driver WDM user-buffer tests requiring ${_driver_neither_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_wdm_user_mapping_definitions)
set(_driver_has_wdm_user_mapping_fixture FALSE)
foreach(_driver_user_mapping_variable NEVERD_WDM_USER_MAPPING_FIXTURE
    NEVERD_WDM_USER_MAPPING_CFG_FIXTURE)
  if(NOT _driver_user_mapping_variable STREQUAL "NEVERD_WDM_USER_MAPPING_FIXTURE"
      AND NOT _driver_has_wdm_user_mapping_fixture)
    continue()
  endif()
  if(NOT "${${_driver_user_mapping_variable}}" STREQUAL ""
      AND EXISTS "${${_driver_user_mapping_variable}}"
      AND NOT IS_DIRECTORY "${${_driver_user_mapping_variable}}")
    get_filename_component(_driver_user_mapping_path
      "${${_driver_user_mapping_variable}}" ABSOLUTE)
    list(APPEND _driver_wdm_user_mapping_definitions
      "${_driver_user_mapping_variable}=\"${_driver_user_mapping_path}\"")
    if(_driver_user_mapping_variable STREQUAL "NEVERD_WDM_USER_MAPPING_FIXTURE")
      set(_driver_has_wdm_user_mapping_fixture TRUE)
    endif()
    message(STATUS "Driver WDM user MDL mapping fixture ${_driver_user_mapping_variable}: ${_driver_user_mapping_path}")
  else()
    message(STATUS "Driver WDM user MDL mapping tests requiring ${_driver_user_mapping_variable} skipped: optional fixture is missing")
  endif()
endforeach()

set(_driver_execution_fixture_definitions
  NEVERD_DRIVER_FIXTURES="${_driver_fixture_dir}"
  NEVERD_DRIVER_SCENARIOS="${CMAKE_SOURCE_DIR}/docs/examples"
  NEVERD_DRIVER_IO_FIXTURE="${_driver_fixture_dir}/driver_io.sys"
  ${_driver_kmdf_definitions}
  ${_driver_wdm_stack_definitions}
  ${_driver_wdm_owned_irp_definitions}
  ${_driver_wdm_wait_wake_definitions}
  ${_driver_wdm_multiple_wait_definitions}
  ${_driver_wdm_usb_idle_definitions}
  ${_driver_kmdf_usb_idle_definitions}
  ${_driver_wdm_pnp_definitions}
  ${_driver_wdm_power_definitions}
  ${_driver_wdm_pofx_definitions}
  ${_driver_kmdf_pofx_definitions}
  ${_driver_kmdf_child_wake_definitions}
  ${_driver_wdm_remove_lock_definitions}
  ${_driver_wdm_resource_definitions}
  ${_driver_wdm_interrupt_definitions}
  ${_driver_wdm_dma_definitions}
  ${_driver_wdm_dma_channel_definitions}
  ${_driver_wdm_seh_definitions}
  ${_driver_wdm_neither_definitions}
  ${_driver_wdm_user_mapping_definitions})
add_neverd_unittest(NeverDNativeDriverTests
  NO_PRETTY_VALUES
  DriverNativeExecutionTests.cpp
  DriverTimestampTests.cpp
  DriverCPUIDTests.cpp
  DriverImageMemoryTests.cpp
  DriverSchedulingTests.cpp
  DriverThreadPriorityTests.cpp
  DriverMutexThreadTests.cpp
  DriverMultipleWaitTests.cpp
  KernelWaitSetTests.cpp
  DriverSIMDSEHTests.cpp
  KernelSEHTests.cpp
  TIMEOUT 30
  LINK_COMPONENTS Emulation)
target_include_directories(NeverDNativeDriverTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)
target_compile_definitions(NeverDNativeDriverTests PRIVATE
  ${_driver_execution_fixture_definitions})
add_dependencies(NeverDNativeDriverTests NeverDDriverFixtures)

add_neverd_unittest(NeverDDriverGuardMetadataTests
  DriverDormantGuardTests.cpp
  TIMEOUT 30
  LINK_COMPONENTS Emulation)
target_include_directories(NeverDDriverGuardMetadataTests PRIVATE
  ${CMAKE_SOURCE_DIR}/lib/emulation)
target_compile_definitions(NeverDDriverGuardMetadataTests PRIVATE
  ${_driver_execution_fixture_definitions})
add_dependencies(NeverDDriverGuardMetadataTests NeverDDriverFixtures)

if(NOT NEVERD_EMULATION_BACKEND_UNICORN)
  return()
endif()

add_neverd_unittest(NeverDDriverEmulationTests
  HardwareBackendTests.cpp
  DriverBackendParityTests.cpp
  DriverWDMUserMappingTests.cpp
  DriverWDMNeitherTests.cpp
  DriverWDMSEHTests.cpp
  KernelExceptionTests.cpp
  DriverWDMDMAChannelTests.cpp
  KernelDMAChannelTests.cpp
  KernelDMAChannelBridgeTests.cpp
  DriverWDMDMATests.cpp
  DriverDMAScenarioTests.cpp
  KernelDMATests.cpp
  KernelDMABridgeTests.cpp
  KernelPhysicalMemoryTests.cpp
  SchedulerDMATests.cpp
  BackendBackingTests.cpp
  DriverSessionTests.cpp
  DriverLoaderAdvancedTests.cpp
  DriverGuardTests.cpp
  DriverKMDFLifecycleTests.cpp
  DriverKMDFControlTests.cpp
  DriverKMDFPnpTests.cpp
  DriverKMDFPowerPolicyTests.cpp
  DriverKMDFLockTests.cpp
  DriverWDMStackTests.cpp
  DriverWDMOwnedIRPTests.cpp
  DriverWdmWaitWakeTests.cpp
  DriverWdmUsbIdleTests.cpp
  DriverKMDFUsbIdleTests.cpp
  DriverKMDFUsbPoFxTests.cpp
  KernelFrameworkUsbIdleTests.cpp
  KernelFrameworkUsbIdleStorageTests.cpp
  KernelFrameworkUsbIdleBridgeTests.cpp
  KernelFrameworkUsbPoFxBridgeTests.cpp
  DriverUsbIdleScenarioTests.cpp
  KernelUsbIdleTests.cpp
  KernelUsbIdleBridgeTests.cpp
  KernelUsbIdleReceiptTests.cpp
  DriverWDMPnpTests.cpp
  DriverWDMPowerTests.cpp
  DriverWDMPoFxTests.cpp
  DriverWDMInterruptTests.cpp
  DriverInterruptScenarioTests.cpp
  SchedulerInterruptTests.cpp
  KernelInterruptsTests.cpp
  KernelInterruptBridgeTests.cpp
  KernelResourceAssignmentTests.cpp
  DriverWDMResourceTests.cpp
  DriverResourceScenarioTests.cpp
  KernelMMIOTests.cpp
  KernelMMIOFailureTests.cpp
  KernelResourceBridgeTests.cpp
  UnicornMMIOTests.cpp
  DriverWDMRemoveLockTests.cpp
  DriverIOTests.cpp
  ExecutionPolicyTests.cpp
  DriverScenarioTests.cpp
  KernelModelTests.cpp
  KernelRequestOwnershipTests.cpp
  KernelDriverIRPAllocationTests.cpp
  KernelDriverIRPOwnershipTests.cpp
  KernelCancellationTests.cpp
  KernelFrameworkTests.cpp
  KernelFrameworkPowerPolicyTests.cpp
  KernelFrameworkChildWakeTests.cpp
  KernelFrameworkChildWakeBridgeTests.cpp
  KernelPoFxTests.cpp
  KernelPoFxAPITests.cpp
  KernelFrameworkPoFxPolicyTests.cpp
  KernelFrameworkPoFxTests.cpp
  KernelFrameworkPoFxBridgeTests.cpp
  KernelD3ColdResourceTests.cpp
  DriverD3ColdScenarioTests.cpp
  DriverPoFxScenarioTests.cpp
  DriverKMDFD3ColdTests.cpp
  DriverKMDFPoFxTests.cpp
  DriverKMDFCustomPoFxTests.cpp
  DriverKMDFChildWakeTests.cpp
  KernelInterruptActivityTests.cpp
  KernelFrameworkInterruptTests.cpp
  KernelFrameworkLockTests.cpp
  KernelFrameworkControlTests.cpp
  KernelFrameworkFileTests.cpp
  KernelFrameworkFileSendTests.cpp
  KernelFrameworkQueueTests.cpp
  KernelFrameworkRequestTests.cpp
  KernelFrameworkRequestAccessorTests.cpp
  KernelRequestMDLTests.cpp
  KernelMDLChainTests.cpp
  KernelMDLUserMappingTests.cpp
  KernelDeviceStackTests.cpp
  KernelPnpRequestTests.cpp
  KernelPnpDeviceTests.cpp
  KernelPnpTopologyTests.cpp
  DriverPnpScenarioTests.cpp
  DriverPnpTopologyTests.cpp
  DriverPowerScenarioTests.cpp
  DriverDeviceD2Tests.cpp
  KernelIRPStackTests.cpp
  KernelPnpCompletionTests.cpp
  KernelPowerRequestTests.cpp
  KernelPowerCompletionTests.cpp
  KernelWdmWakeEventTests.cpp
  KernelProviderWaitWakeTests.cpp
  KernelRemoveLocksTests.cpp
  KernelRemoveLockBridgeTests.cpp
  BackendFaultTests.cpp
  BackendContextTests.cpp
  KernelSchedulerTests.cpp
  DeviceLifecycleTests.cpp
  DriverAsyncTests.cpp
  DriverDispatcherTests.cpp
  KernelDispatcherTests.cpp
  KernelAPIIRQLTests.cpp
  DriverContextLimitTests.cpp
  KernelRuntimeTests.cpp
  DriverDirectIOTests.cpp
  DriverMDLTests.cpp
  DriverRegistryTests.cpp
  KernelRegistryTests.cpp
  KernelExportTests.cpp
  LINK_COMPONENTS Emulation
)
target_include_directories(NeverDDriverEmulationTests PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/lib/emulation
  ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NeverDDriverEmulationTests PRIVATE
  ${_driver_execution_fixture_definitions})
add_dependencies(NeverDDriverEmulationTests NeverDDriverFixtures)

if(TARGET neverd_shared AND TARGET neverd)
  # Keep public tests on the shared engine, without a second static copy of
  # the emulation component or LLVM's process-global registries.
  add_neverd_unittest(NeverDDriverEmulationPublicTests
    HardwareBackendPublicTests.cpp
    DriverEmulationPublicTests.cpp
    DriverScenarioPublicTests.cpp
    DriverWdmWaitWakePublicTests.cpp
    DriverWdmUsbIdlePublicTests.cpp
    DriverKMDFUsbIdlePublicTests.cpp
    DriverKMDFUsbPoFxPublicTests.cpp
  DriverD3ColdPublicTests.cpp
    LINK_LIBS neverd_shared)
  target_include_directories(NeverDDriverEmulationPublicTests PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(NeverDDriverEmulationPublicTests PRIVATE
    NEVERD_DRIVER_NESTED_SCENARIO="${CMAKE_SOURCE_DIR}/docs/examples/driver-nested-user-scenario.json"
    NEVERD_DRIVER_D2_SCENARIO="${CMAKE_SOURCE_DIR}/docs/examples/driver-d2-power-scenario.json"
    NEVERD_DRIVER_WDM_WAIT_WAKE_SCENARIO="${CMAKE_SOURCE_DIR}/docs/examples/driver-wdm-wait-wake-scenario.json"
    NEVERD_DRIVER_KMDF_USB_IDLE_SCENARIO="${CMAKE_SOURCE_DIR}/docs/examples/driver-kmdf-usb-idle-scenario.json"
    NEVERD_DRIVER_KMDF_USB_POFX_SCENARIO="${CMAKE_SOURCE_DIR}/docs/examples/driver-kmdf-usb-pofx-scenario.json"
    NEVERD_DRIVER_WDM_USB_IDLE_SCENARIO="${CMAKE_SOURCE_DIR}/docs/examples/driver-wdm-usb-idle-scenario.json"
    NEVERD_DRIVER_WDM_ELEVATED_POWER_SCENARIO="${CMAKE_SOURCE_DIR}/docs/examples/driver-wdm-elevated-power-scenario.json"
    NEVERD_DRIVER_FIXTURES="${_driver_fixture_dir}"
    NEVERD_DRIVER_IO_FIXTURE="${_driver_fixture_dir}/driver_io.sys"
    NEVERD_DRIVER_CLI="$<TARGET_FILE:neverd>"
    ${_driver_kmdf_definitions}
    ${_driver_wdm_stack_definitions}
  ${_driver_wdm_owned_irp_definitions}
  ${_driver_wdm_wait_wake_definitions}
  ${_driver_wdm_multiple_wait_definitions}
  ${_driver_wdm_usb_idle_definitions}
  ${_driver_kmdf_usb_idle_definitions}
  ${_driver_wdm_pnp_definitions}
  ${_driver_wdm_power_definitions}
  ${_driver_wdm_pofx_definitions}
  ${_driver_kmdf_pofx_definitions}
  ${_driver_kmdf_child_wake_definitions}
  ${_driver_wdm_remove_lock_definitions}
  ${_driver_wdm_resource_definitions}
  ${_driver_wdm_interrupt_definitions}
  ${_driver_wdm_dma_definitions}
  ${_driver_wdm_dma_channel_definitions}
  ${_driver_wdm_seh_definitions}
  ${_driver_wdm_neither_definitions}
  ${_driver_wdm_user_mapping_definitions})
  if(WIN32)
    target_link_libraries(NeverDDriverEmulationPublicTests PRIVATE LLVMSupport)
  endif()
  add_dependencies(NeverDDriverEmulationPublicTests NeverDDriverFixtures neverd)
endif()
