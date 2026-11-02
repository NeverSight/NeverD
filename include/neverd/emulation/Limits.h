//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_LIMITS_H
#define NEVERD_EMULATION_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::emulation {

/// Future regular-file objects, including generations whose names retire.
inline constexpr uint32_t kMaxLinuxFileCreations = 64;
/// Explicit parent grants and successful operations for each namespace policy.
inline constexpr uint32_t kMaxLinuxFileRenameParents = 64;
inline constexpr uint32_t kMaxLinuxFileRenames = 64;
inline constexpr uint32_t kMaxLinuxFileUnlinkParents = 64;
inline constexpr uint32_t kMaxLinuxFileUnlinks = 64;

/// Ordered mincore queries, bytes per query and aggregate observation bytes.
inline constexpr size_t kMaxLinuxResidencyObservations = 1024;
inline constexpr size_t kMaxLinuxResidencyPages = 65536;
inline constexpr size_t kMaxLinuxResidencyObservationBytes = 1024 * 1024;

/// Stored offline stream and datagram entries admitted together. Counted
/// failures retain only a cursor and do not consume this inventory.
inline constexpr int kMaxNetworkConnections = 4096;
/// Receive and expected-send event entries admitted per connection.
inline constexpr int kMaxNetworkEvents = 4096;
/// Maximum configured send or receive chunk.
inline constexpr int kMaxNetworkTransferBytes = 65536;
/// Aggregate offline network payload inventory.
inline constexpr int kMaxNetworkInputBytes = 1024 * 1024;
/// Offline Android resolver query inventory.
inline constexpr int kMaxResolverQueries = 128;
/// Resolved addresses admitted per query.
inline constexpr int kMaxResolverAddresses = 64;
/// Node, service and canonical-name byte admission.
inline constexpr int kMaxResolverTextBytes = 1024;
/// Aggregate resolver observation storage.
inline constexpr int kMaxResolverObservationBytes = 1024 * 1024;

// include/neverd/emulation/DriverPowerPolicy.h
inline constexpr size_t kDriverPowerPolicyDriverPowerPolicyEventLimit = 1024;

// include/neverd/emulation/ModuleBinding.h
inline constexpr uint64_t kModuleBindingCreateLimit = 256;

// lib/emulation/os/darwin/kernel/DarwinDirectory.h
inline constexpr uint64_t kDarwinDirectoryDirectoryPayloadLimit =
    128 * 1024 * 1024;

// lib/emulation/os/darwin/process/DarwinCallbacks.h
inline constexpr size_t kDarwinCallbacksTLSCallbackLimit = 1024;
inline constexpr size_t kDarwinCallbacksGlobalCallbackLimit = 1024;

// lib/emulation/os/linux/android/AndroidFinalizers.cpp
// A finite model limit, not a claim about Bionic's allocation capacity.
// Null callbacks are inert in API 28 but still consume registration space.
inline constexpr size_t kAndroidFinalizersMaxExitCallbacks = 4096;

// lib/emulation/os/linux/android/AndroidJNI.h
inline constexpr unsigned kAndroidJNIAttachmentLimit = 512;
inline constexpr unsigned kAndroidJNIReferenceLimit = 4096;
inline constexpr unsigned kAndroidJNIObjectLimit = 4096;
inline constexpr unsigned kAndroidJNINativeFrameLimit = 64;

// lib/emulation/os/linux/android/AndroidLog.h
inline constexpr unsigned kAndroidLogLogRecordLimit = 4096;
inline constexpr unsigned kAndroidLogLogSnapshotLimit = 1024 * 1024;

// lib/emulation/os/linux/kernel/LinuxDeviceMappings.cpp
// Binder buffers are per open, read-only to the process, and
// kernel-written.
// Match the driver's maximum receive arena. Oversized requests still need
// VMA truncation semantics and remain outside this bounded profile.
inline constexpr uint64_t kLinuxDeviceMappingsBinderArenaLimit =
    4 * 1024 * 1024;

// lib/emulation/os/linux/kernel/LinuxKGSL.h
inline constexpr uint64_t kLinuxKGSLMaximumAllocation = 32768;
inline constexpr uint64_t kLinuxKGSLMaximumAllocations = 64;
inline constexpr uint32_t kLinuxKGSLMaximumContexts = 1024;

// lib/emulation/os/windows/kernel/KernelLoaderView.h
inline constexpr size_t kKernelLoaderViewMaxEvidenceSize = 256;

// lib/emulation/os/windows/kernel/KernelModelBugCheck.cpp
inline constexpr size_t kKernelModelBugCheckRecordLimit = 64;
inline constexpr unsigned kKernelModelBugCheckComponentLimit = 256;

// lib/emulation/os/windows/kernel/KernelPhysicalInventory.h
inline constexpr size_t kKernelPhysicalInventoryMaxEvidenceSize = 256;

// lib/emulation/os/windows/kernel/KernelVaShadowQuery.h
inline constexpr size_t kKernelVaShadowQueryMaxEvidenceSize = 256;

// lib/emulation/os/windows/kernel/KernelVersionQuery.h
inline constexpr size_t kKernelVersionQueryMaxEvidenceSize = 256;

// lib/emulation/backends/hvf/HvfExecutor.cpp
inline constexpr int kHvfExecutorSubmitMilliseconds = 1;

// lib/emulation/os/darwin/process/DarwinImage.cpp
inline constexpr int kDarwinImageMapRecordsLimit = 65536;
inline constexpr int kDarwinImageInitializerRecordsLimit = 65536;

// lib/emulation/os/linux/android/AndroidAuxv.cpp
inline constexpr int
    kAndroidAuxvValidateAuxiliaryVectorOptionsAuxiliaryVectorSizeLimit = 128;

// Bionic error text placement is model policy, not a library virtual address.
inline constexpr uint64_t kAndroidErrorStringsBase = 0x7040000000;
// Per-thread storage beyond dlerror and ABI TLS slots.
inline constexpr uint64_t kAndroidErrorBufferOffset = 0x800;

// lib/emulation/os/linux/android/AndroidCalendar.cpp
inline constexpr int kAndroidCalendarValidateCalendarZNameSizeLimit = 32;

// lib/emulation/os/linux/android/AndroidDex.cpp
inline constexpr int kAndroidDexBionicInitializeDexNameSizeLimit = 1024;

// lib/emulation/os/linux/android/AndroidDlfcn.cpp
inline constexpr unsigned kAndroidDladdrObservationCount = 256;
inline constexpr unsigned kAndroidDladdrStringBytes = 4096;
inline constexpr unsigned kAndroidDladdrSnapshotBytes = 1024 * 1024;
inline constexpr int kAndroidDlfcnBionicLinkerErrorMessageSizeLimit = 1024;
inline constexpr int kAndroidDlfcnBionicOpenLibrarySizeLimit = 1024;
inline constexpr int kAndroidDlfcnBionicLookupSymbolSizeLimit = 1024;

// lib/emulation/os/linux/android/AndroidJNIMethods.cpp
inline constexpr uint64_t kAndroidJNIMethodsBionicJniMethodsRemaining = 4096;

// lib/emulation/os/linux/android/AndroidJavaLibrary.h
inline constexpr int kAndroidJavaLibraryValidJavaLibraryNameNameSizeLimit = 255;

// lib/emulation/os/linux/android/AndroidNative.cpp
inline constexpr unsigned kAndroidFailureMemoryDiagnosticBytes = 256;
inline constexpr int kAndroidNativeRunNativeLibrariesSizeLimit = 256;
inline constexpr int kAndroidNativeRunNativeLibrarySizeLimit = 1024;
inline constexpr int kAndroidNativeRunNativeSymbolCountLimit = 4096;
inline constexpr int kAndroidNativeRunNativeSymbolSizeLimit = 1024;

// lib/emulation/os/linux/android/AndroidObservation.cpp
inline constexpr int
    kAndroidObservationValidateExecutionObservationOptionsPCsSizeLimit = 64;

// lib/emulation/os/linux/android/AndroidResolver.cpp
inline constexpr int kAndroidResolverValidateResolverQueriesSizeLimit =
    kMaxResolverQueries;
inline constexpr int kAndroidResolverValidateResolverQueriesTextTextSSizeLimit =
    kMaxResolverTextBytes;
inline constexpr int
    kAndroidResolverValidateResolverQueriesQAddressesSizeLimit =
        kMaxResolverAddresses;
inline constexpr int kAndroidResolverValidateResolverQueriesBytesLimit =
    kMaxResolverObservationBytes;

// lib/emulation/os/linux/kernel/LinuxBinder.cpp
inline constexpr int
    kLinuxBinderLinuxFilesBinderIoctlDeviceBinderLoopersSizeLimit = 4096;
inline constexpr int kLinuxBinderLinuxFilesBinderIoctlWriteSizeLimit = 4096;

// lib/emulation/os/linux/kernel/LinuxBinderTransactions.cpp
inline constexpr int
    kLinuxBinderTransactionsLinuxFilesBinderCommandOwnQueueSizeLimit = 64;
inline constexpr int
    kLinuxBinderTransactionsLinuxFilesBinderCommandTargetBinderThreadWorkCallThreadSizeLimit =
        62;
inline constexpr int
    kLinuxBinderTransactionsLinuxFilesBinderCommandTargetBinderProcessWorkSizeLimit =
        64;
inline constexpr int
    kLinuxBinderTransactionsLinuxFilesBinderCommandTargetBinderBuffersSizeLimit =
        64;

// lib/emulation/os/linux/kernel/LinuxFileOptions.cpp
inline constexpr int kLinuxFileOptionsValidateFileOptionsTEventLimit = 4096;
inline constexpr int kLinuxFileOptionsValidateFileOptionsTByteLimit = 1048576;
inline constexpr int kLinuxFileOptionsValidateFileOptionsTRequestsSizeLimit =
    64;
inline constexpr int kLinuxFileOptionsValidateFileOptionsRSizeLimit = 4096;

// lib/emulation/os/linux/kernel/LinuxGPUCommands.cpp
inline constexpr unsigned kLinuxGPUCommandsRemainingWords = 4096;

// lib/emulation/os/linux/kernel/LinuxNetwork.cpp
inline constexpr int kLinuxNetworkReceiveEventsSizeLimit = kMaxNetworkEvents;
inline constexpr int
    kLinuxNetworkValidateNetworkOptionsConnectionsSizeOptionsDatagramConnectionsSizeLimit =
        kMaxNetworkConnections;
inline constexpr int kLinuxNetworkValidateNetworkOptionsCMaxSendLimit =
    kMaxNetworkTransferBytes;
inline constexpr int kLinuxNetworkValidateNetworkOptionsCMaxReceiveLimit =
    kMaxNetworkTransferBytes;
inline constexpr int kLinuxNetworkValidateNetworkOptionsBytesLimit =
    kMaxNetworkInputBytes;
inline constexpr int kLinuxNetworkValidateNetworkOptionsCExpectedSendSizeLimit =
    kMaxNetworkEvents;
inline constexpr int kLinuxNetworkLinuxFilesNetworkSizeLimit = 128;

// lib/emulation/os/linux/kernel/LinuxProcMaps.cpp
inline constexpr int kLinuxProcMapsLinuxMemoryProcMappingsOutSizeLimit = 65536;

// lib/emulation/os/linux/kernel/LinuxSignalFrame.cpp
inline constexpr int kLinuxSignalFrameLinuxSignalsEnterThreadFramesSizeLimit =
    32;

// lib/emulation/os/windows/kernel/KernelModelNamespace.cpp
inline constexpr int
    kKernelModelNamespaceKernelModelLookupNamedObjectLinkDepthLimit = 16;

// lib/emulation/os/windows/process/WindowsProcessFiles.cpp
inline constexpr int kWindowsProcessFilesServicesToolhelpSizeLimit = 0x1000;

// lib/emulation/runtime/BufferExecution.cpp
inline constexpr int kBufferExecutionDecodeASizeLimit = 256;

// lib/emulation/runtime/ExecutionRunJSON.cpp
inline constexpr int kExecutionRunJSONUnambiguousStackSizeLimit = 64;

// lib/emulation/runtime/ModuleMappingTransaction.cpp
inline constexpr int kModuleMappingTransactionValidateExportsSizeLimit =
    1000000;
inline constexpr int kModuleMappingTransactionValidateExportsENameSizeLimit =
    4096;

// lib/emulation/runtime/ProcessAndroidJSON.cpp
inline constexpr int kMaxJNIClassLookupFailures = 4096;
inline constexpr uint64_t kProcessAndroidJSONAndroidOptionsFromJSONRemaining =
    1024 * 1024;

// lib/emulation/runtime/ProcessLinuxFilesJSON.cpp
inline constexpr int kProcessLinuxFilesJSONIoctlTraceRowsSizeLimit = 64;
inline constexpr int kProcessLinuxFilesJSONIoctlTraceBytesLimit = 1048576;

// lib/emulation/runtime/ProcessNetworkJSON.cpp
inline constexpr int kProcessNetworkJSONTextSSizeLimit = kMaxResolverTextBytes;
inline constexpr int kProcessNetworkJSONReceiveEventsSizeLimit =
    kMaxNetworkEvents;
inline constexpr int
    kProcessNetworkJSONLinuxNetworkOptionsFromJSONVGetAsArraySizeLimit =
        kMaxNetworkConnections;
inline constexpr uint64_t
    kProcessNetworkJSONLinuxNetworkOptionsFromJSONRemaining =
        kMaxNetworkInputBytes + kMaxNetworkConnections * 28;
inline constexpr int
    kProcessNetworkJSONLinuxNetworkOptionsFromJSONPacketsSizeLimit =
        kMaxNetworkEvents;
inline constexpr int
    kProcessNetworkJSONAndroidResolverQueriesFromJSONQueriesSizeLimit =
        kMaxResolverQueries;
inline constexpr uint64_t
    kProcessNetworkJSONAndroidResolverQueriesFromJSONRemaining =
        kMaxResolverObservationBytes;
inline constexpr int
    kProcessNetworkJSONAndroidResolverQueriesFromJSONAddressesSizeLimit =
        kMaxResolverAddresses;

// lib/emulation/os/linux/kernel/LinuxBinderTransactions.cpp
inline constexpr int kMaxBinderTransactionBytes = 65536;

// lib/emulation/os/linux/kernel/LinuxNetwork.cpp
inline constexpr int kMaxNetworkTimeoutSeconds = 86400;

// lib/emulation/os/darwin/process/DarwinTLSPreparation.cpp
inline constexpr int kMaxDarwinStartupRecords = 65536;

// lib/emulation/os/linux/android/AndroidDex.cpp
inline constexpr int kMaxAndroidDexInputBytes = 1024 * 1024;

// lib/emulation/runtime/ProcessLinuxFilesJSON.cpp
inline constexpr int kMaxIoctlTraceEvents = 4096;

// lib/emulation/os/linux/kernel/LinuxGPUCommands.cpp
inline constexpr int kMaxGPUCommandWords = 4096;

} // namespace neverd::limits::emulation

#endif // NEVERD_EMULATION_LIMITS_H
