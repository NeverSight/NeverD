//===- DriverSession.h - Public WDM emulation interface -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execution options, strict scenario input, and observed driver lifecycle
/// state.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERSESSION_H
#define NEVERD_EMULATION_DRIVERSESSION_H

#include "neverd/emulation/DriverBugCheck.h"
#include "neverd/emulation/DriverCPUID.h"
#include "neverd/emulation/DriverCallbackDrain.h"
#include "neverd/emulation/DriverFileSystem.h"
#include "neverd/emulation/DriverFiniteFileSystem.h"
#include "neverd/emulation/DriverFirmware.h"
#include "neverd/emulation/DriverKernelMemory.h"
#include "neverd/emulation/DriverNdisConfiguration.h"
#include "neverd/emulation/DriverNetworkInterfaces.h"
#include "neverd/emulation/DriverObjects.h"
#include "neverd/emulation/DriverPciConfiguration.h"
#include "neverd/emulation/DriverPnp.h"
#include "neverd/emulation/DriverProcessEvents.h"
#include "neverd/emulation/DriverProcessInventory.h"
#include "neverd/emulation/DriverProfile.h"
#include "neverd/emulation/DriverRegistry.h"
#include "neverd/emulation/DriverRegistryEvents.h"
#include "neverd/emulation/DriverRegistryHives.h"
#include "neverd/emulation/DriverScheduling.h"
#include "neverd/emulation/DriverSections.h"
#include "neverd/emulation/DriverServices.h"
#include "neverd/emulation/DriverThreadNotifications.h"
#include "neverd/emulation/DriverUserMemory.h"
#include "neverd/emulation/ExecutionBackend.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {

enum class DriverRequestKind {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major) Name,
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
};

struct DriverRequest {
  static constexpr uint32_t DefaultRequestorProcessID = 0x1000;
  DriverRequestKind Kind = DriverRequestKind::DeviceControl;
  /// CREATE with an empty name selects the sole live guest device; ambiguity
  /// fails. Declared provider volumes do not participate in this selection.
  /// Later requests use their file's device unless a matching name is given.
  std::string Device;
  uint32_t ControlCode = 0;
  std::vector<uint8_t> Input;
  uint32_t OutputSize = 0;
  /// Optional page rights for nonempty WDM neither-I/O input/output buffers.
  std::optional<DriverUserPageAccess> UserInputAccess;
  std::optional<DriverUserPageAccess> UserOutputAccess;
  /// Revoke the request's original neither-I/O user virtual addresses after
  /// dispatch returns. Locked MDL system aliases retain the underlying pages.
  std::optional<bool> UserUnmapAfterDispatch;
  /// Terminate the synthetic requestor after dispatch returns, revoking all
  /// of its original user VAs while locked MDLs retain their physical pages.
  std::optional<bool> RequestorExitAfterDispatch;
  /// Synthetic requestor identity for file IRPs; system-owned IRPs use zero.
  uint32_t RequestorProcessID = DefaultRequestorProcessID;
  /// Initial contents of a direct IOCTL's second buffer, padded to OutputSize.
  std::vector<uint8_t> DirectInput;
  uint64_t ByteOffset = 0;
  /// Scenario identity of an independently opened FILE_OBJECT.
  uint32_t File = 0;
  /// CREATE-only choice of an asynchronous FILE_OBJECT. Omission models a
  /// synchronous open; later transfers inherit the opened object's mode.
  std::optional<bool> AsynchronousFile;
  /// Optional cancellation event relative to submission in virtual 100 ns
  /// units. For WDM, zero acts after dispatch returns; for KMDF, it acts after
  /// framework routing and before a guest I/O callback unless routing wins.
  std::optional<uint64_t> CancelAfter100ns;
  /// Admit the next file request before draining callbacks for this request.
  /// Same-file overlap requires an asynchronous open and a pending transfer.
  bool DeferCallbackDrain = false;
  /// Client wait before this request, after process events and before registry
  /// events at its boundary. Existing scheduler deadlines remain authoritative.
  uint64_t DelayBefore100ns = 0;
  /// Stable configured PDO identity; mutually exclusive with Device.
  std::string DeviceID;
  /// Present exactly when Kind is Pnp; those packets have no FILE_OBJECT.
  std::optional<DriverPnpOperation> Pnp;
  /// Present exactly when Kind is Power; those packets have no FILE_OBJECT.
  std::optional<DriverPowerOperation> Power;
  /// Explicit PDO response when a forwarded file lifecycle IRP reaches the
  /// configured lower provider, optionally after a virtual delay.
  std::optional<DriverBusCompletion> FileBusCompletion;
  /// Explicit external pulses for READ/WRITE/IOCTL. These events survive the
  /// source IRP's completion and do not imply device enable/ack semantics.
  std::vector<DriverInterruptEvent> InterruptEvents{};
  /// Independent external DMA transactions for READ/WRITE/IOCTL requests.
  std::vector<DriverDmaEvent> DmaEvents{};
  std::vector<DriverPowerPolicyEvent> PowerPolicyEvents{};
  /// Additional user allocations and explicit guest pointer slots for
  /// neither-I/O requests. Every allocation belongs to RequestorProcessID.
  std::vector<DriverUserBuffer> UserBuffers;
  std::vector<DriverUserPointer> UserPointers;
};

/// One explicitly supplied CPUID observation. An absent Subleaf matches every
/// input ECX for this leaf; overlapping declarations are rejected.
struct DriverCPUID {
  uint32_t Leaf = 0;
  std::optional<uint32_t> Subleaf;
  std::array<uint32_t, 4> Registers{}; // EAX, EBX, ECX, EDX.
};

/// Caller-declared, read-only loader record for concrete diagnostics. Version
/// identifies a simulated layout, not a Windows version. Evidence is a bounded
/// printable ASCII label supplied by the caller, not authenticated provenance.
struct DriverLoaderView {
  uint32_t Version = 1;
  /// Required even when zero; no loader flag value is inferred.
  std::optional<uint32_t> Flags;
  std::string Evidence;
};

/// Explicit concrete base-field algorithm for RtlGetVersion. Version 1 writes
/// major 10, minor 0, declared 16-bit build, platform 2 and preserves all other
/// bytes. It does not identify or authenticate a Windows implementation.
/// Buffers must be in the profile's kernel address range with no second VA
/// mapping of the accessed RAM bytes in the current address space.
struct DriverVersionQuery {
  uint32_t Version = 1;
  std::optional<uint32_t> BuildNumber;
  /// Caller-supplied printable label, never authenticated provenance.
  std::string Evidence;
  /// Nonportable compatibility shape with a real 20-byte buffer. The default
  /// requires a 276-byte base record with its entire CSD array already zero.
  bool AllowZeroSize = false;
};

/// Explicit simulated RAM inventory for concrete checked-x64 diagnostics.
/// Selecting the model identity window does not infer installed host RAM or
/// grant access to unbacked physical pages. No native reconstruction is
/// implied.
struct DriverPhysicalMemoryInventory {
  enum class SourceKind { ModelIdentityWindow };
  uint32_t Version = 1;
  /// Required selection; omission leaves system RAM inventory unspecified.
  std::optional<SourceKind> Source;
  /// Caller-supplied printable label, never authenticated provenance.
  std::string Evidence;
};

/// Explicit class-196 system-information result for checked-x64 execution.
/// Reports the declared low fourteen flags without implementing a hardware
/// mitigation or inferring native host state. Reserved bits must be zero.
struct DriverKernelVaShadowQuery {
  uint32_t Version = 1;
  /// Required value; an explicitly declared zero is distinct from omission.
  std::optional<uint32_t> Flags;
  /// Caller-supplied printable label, never authenticated provenance.
  std::string Evidence;
};

/// Explicit debugger data, never inferred from the host. Omitting the whole
/// declaration leaves KdDebuggerEnabled/KdDebuggerNotPresent inaccessible.
struct DriverDebuggerState {
  bool Enabled = false;
  bool NotPresent = true;
};

/// Select strict DDI tag checks or ordinary Windows heap release behavior.
enum class DriverPoolTagPolicy { Strict, Windows19041 };
enum class DriverMDLMappingPolicy { Strict, Windows19041 };
enum class DriverIRPInitializationPolicy { Strict, Windows19041 };
/// This profile models a single-processor x64 WDM lifecycle with cooperative
/// or explicit instruction-driven preemptive scheduling, dispatcher waits,
/// and DPC execution at modeled IRQLs.
/// RDTSC/RDTSCP share the scheduler's 10 MHz clock; RDTSCP reports CPU0.
/// CET shadow stacks are disabled. Canonical RDSSPD/RDSSPQ preserve all state;
/// other shadow-stack operations and attempts to enable CET remain unsupported.
/// All pointers describe guest addresses, never native pointers. No host OS
/// services are forwarded. Unsupported APIs and CPU environment effects stop.
struct DriverOptions {
  struct WindowsVersion {
    uint32_t Major = 0, Minor = 0, Build = 0;
  };
  /// Explicit guest Windows version, independent of the host running NeverD.
  /// The bounded NT workstation profile has no service pack or suite flags.
  std::optional<WindowsVersion> Version;
  /// Optional static kernel code reference. Calls still require an OS model.
  std::filesystem::path KernelImage;
  /// Optional exact PDB companion for reference-kernel debugger layout facts.
  std::filesystem::path KernelPDB;
  /// Strict WDK-contract checks by default; the explicit Windows profile
  /// follows ordinary heap release behavior without Driver Verifier.
  DriverPoolTagPolicy PoolTagPolicy = DriverPoolTagPolicy::Strict;
  /// Explicit ordinary Windows nonpaged-MDL remapping behavior, without
  /// verifier. Strict WDK contract checking remains the default.
  DriverMDLMappingPolicy MDLMappingPolicy = DriverMDLMappingPolicy::Strict;
  /// Explicit ordinary Windows IRP IoStatus initialization, without verifier.
  /// Strict mode requires explicit producer/driver initialization knowledge.
  DriverIRPInitializationPolicy IRPInitializationPolicy =
      DriverIRPInitializationPolicy::Strict;
  /// Explicit CPUID observations keyed by input EAX/ECX, with output registers
  /// in EAX/EBX/ECX/EDX order. Missing queries require an explicit CPUIDPolicy
  /// rule or stop; guest queries never forward to the host CPU.
  /// Publish guest-owned read-only four-level Windows 19041 page tables.
  bool PageTables = false;
  ExecutionBackendKind Backend = ExecutionBackendKind::Unicorn;
  ExecutionContract Contract = ExecutionContract::Legacy;
  bool ReportBackendSelection = false;
  uint64_t InstructionLimit = profile::DefaultInstructionLimit;
  uint64_t MemoryLimit = profile::DefaultMemoryLimit;
  uint64_t EventLimit = profile::DefaultEventLimit;
  /// Retain attempted guest writes as budgeted report events. Disabling this
  /// leaves memory validation and execution observers active.
  bool TraceMemoryWrites = true;
  /// Explicit CPU environment inputs, never inferred from the host processor.
  /// A missing leaf/subleaf stops before publishing any CPUID register result.
  std::vector<DriverCPUID> CPUID;
  uint64_t TimeoutMilliseconds = profile::DefaultTimeoutMilliseconds;
  std::string ServiceName = profile::DefaultServiceName;
  /// Zero selects the preferred PE image base.
  uint64_t LoadAddress = 0;
  std::vector<DriverRequest> Requests;
  bool Unload = false;
  /// Explicit export availability overrides for this concrete environment.
  /// Unknown dynamic names stop unless availability is declared here.
  std::map<std::string, bool> KernelExports;
  /// An explicitly populated registry namespace. Omission leaves registry
  /// availability unspecified; an empty inventory models an empty namespace.
  std::optional<std::vector<DriverRegistryKey>> Registry;
  /// Explicit session-local volumes. Omission means unknown availability; an
  /// empty vector declares no filesystem providers. File I/O remains
  /// unsupported.
  std::optional<std::vector<DriverFileSystemVolume>> FileSystem;
  /// Registry system time at virtual time zero, in Windows 100 ns units.
  /// Mutations use this epoch plus the shared scheduler's elapsed time.
  std::optional<uint64_t> RegistrySystemTime100ns;
  /// Explicit finite synchronous provider; cannot coexist with FileSystem.
  std::optional<std::vector<DriverFiniteFileSystemVolume>> FiniteFileSystem;
  std::vector<DriverProcessEvent> ProcessEvents;
  /// System kernel-origin registry operations; process events run first at a
  /// shared boundary.
  std::vector<DriverRegistryEvent> RegistryEvents;
  std::vector<DriverPnpDevice> PnpDevices;
  /// Omission retains cooperative scheduling and idle-only clock advancement.
  /// An explicit policy permits instruction-boundary thread preemption below
  /// DISPATCH_LEVEL and advances virtual time while guest instructions run.
  std::optional<DriverScheduling> Scheduling;
  /// Omission leaves DriverSection opaque. An enabled view retains an
  /// unreconstructed environment dependency, including when it is not read.
  /// Driver execution requires Contract == CheckedX64 to refuse before effects.
  std::optional<DriverLoaderView> LoaderView;
  /// Optional checked-x64 diagnostic environment. No host version is inferred;
  /// even an unused declaration retains an unreconstructed dependency.
  std::optional<DriverVersionQuery> VersionQuery;
  /// Optional simulated inventory; even an unused declaration retains an
  /// unreconstructed environment dependency. Requires checked-x64 execution.
  std::optional<DriverPhysicalMemoryInventory> PhysicalMemoryInventory;
  /// Optional diagnostic query; even unused declarations retain an
  /// unreconstructed environment dependency. Requires checked-x64 execution.
  std::optional<DriverKernelVaShadowQuery> KernelVaShadowQuery;
  std::optional<DriverDebuggerState> DebuggerState;
  /// Explicit boot-fixed system-clock increment, in 100 ns units. This is
  /// independent of virtual elapsed time and instruction-driven clock ticks.
  std::optional<uint32_t> SystemClockIncrement100ns;
  /// Explicit guest secure-PCI capability observation. Omission leaves the
  /// query unavailable; this does not configure VSM, HVCI or host hardware.
  std::optional<bool> SecurePciEnabled;
  /// Explicit ACPI MCFG observation; omission leaves firmware state unknown.
  std::optional<DriverPciMcfg> PciMcfg;
  std::optional<DriverObjectNamespace> DriverNamespace;
  std::optional<DriverSmbios> Smbios;
  // Successful class-202 policy observation; this does not enable an IOMMU.
  std::optional<bool> DmaGuardPolicyEnabled;
  std::optional<DriverAcpiTables> AcpiTables;
  std::optional<DriverCPUIDPolicy> CPUIDPolicy;
  // Final successful class-103 result, not an enforcement configuration.
  std::optional<uint32_t> CodeIntegrityOptions;
  /// Omission retains complete draining. ForegroundReady yields only when
  /// remaining callback frames are parked plain system threads.
  std::optional<DriverCallbackDrainPolicy> CallbackDrainPolicy;
  /// Pool VA reservations and managed physical page capacity, in bytes.
  /// Omission retains 1 MiB; private processor/type pages are outside the
  /// maximum reservation for every capacity.
  std::optional<uint64_t> KernelMemorySize;
  /// Explicit guest image names/counters; initial membership is shared with
  /// requestors and process events. Planned CREATE records are not yet live.
  std::optional<DriverProcessInventory> ProcessInventory;
  /// Explicit cached SEC_COMMIT objects. Omission leaves named sections
  /// unknown.
  std::optional<DriverSectionNamespace> SectionNamespace;
  /// Explicit NlsMbOemCodePageTag BOOLEAN observation. Does not supply an OEM
  /// code page, conversion tables, locale or general NLS runtime.
  std::optional<bool> NlsOemMultibyte;
  std::optional<DriverPciConfiguration> PciConfiguration;
  /// Explicit effective I/O priority hint for synchronous IOCTL construction.
  std::optional<uint8_t> IRPIOPriorityHint;
  std::optional<std::vector<DriverRegistryHive>> RegistryHives;
  /// Explicit local Filter Manager name provider, initially empty name cache.
  std::optional<bool> FilterManagerFileNames;
  std::optional<DriverNetworkInterfaces> NetworkInterfaces;
  std::optional<DriverNdisConfiguration> NdisConfiguration;
  /// Immutable child PE inputs; registry ImagePath selects a guest path.
  std::optional<std::vector<DriverServiceImage>> ServiceImages;
};

enum class DriverStopReason {
#define NEVERD_DRIVER_STOP(Name, Spelling) Name,
#include "neverd/emulation/DriverStopReasons.def"
#undef NEVERD_DRIVER_STOP
};

struct DriverAPIEvent {
  uint64_t PC = 0;
  std::string Phase;
  std::string Name;
  std::vector<uint64_t> Arguments;
  std::optional<uint64_t> Result;
  std::string Detail;
  /// Guest caller's saved return PC, captured from the validated ABI frame.
  std::optional<uint64_t> ReturnAddress;
};

struct DriverMemoryWrite {
  std::string Phase;
  /// An attempted CPU write observed before the memory access. It may fault;
  /// API model writes and stack writes are not part of this trace.
  uint64_t PC = 0;
  uint64_t Address = 0;
  uint32_t Size = 0;
  /// The value is available only for scalar writes of at most eight bytes.
  std::optional<uint64_t> Value;
};

struct DriverDevice {
  uint64_t Address = 0;
  uint64_t Extension = 0;
  uint32_t Type = 0;
  std::string Name;
  /// Independent notification last given to PoSetPowerState, when known.
  std::optional<DevicePowerState> ReportedDevicePower = std::nullopt;
};

struct DriverRequestDelayResult {
  size_t SourceIndex = 0;
  uint64_t Duration100ns = 0, StartedAt100ns = 0;
  std::optional<uint64_t> CompletedAt100ns;
};
struct DriverRequestResult {
  DriverRequestKind Kind = DriverRequestKind::DeviceControl;
  std::string Device;
  uint32_t ControlCode = 0;
  uint64_t IRP = 0;
  bool Completed = false;
  std::optional<uint32_t> DispatchStatus;
  std::optional<uint32_t> IOStatus;
  uint64_t Information = 0;
  std::vector<uint8_t> Output;
  uint32_t File = 0;
  uint32_t RequestorProcessID = 0;
  uint64_t ByteOffset = 0;
  /// Actual virtual time when cancellation was requested; absent when the
  /// request completed before its configured cancellation event could fire.
  std::optional<uint64_t> CancelRequestedAt100ns;
  std::string DeviceID;
  std::optional<DriverPnpRequestResult> Pnp;
  std::optional<DriverPowerRequestResult> Power;
  DriverRequestOrigin Origin = DriverRequestOrigin::Scenario;
  /// Consumed per-PDO RequestedDevicePower entry; absent for scenario requests.
  std::optional<uint32_t> ResponseIndex;
  std::vector<DriverUserBufferResult> UserBuffers;
  std::optional<DriverUsbIdleRequestResult> UsbIdle;
  /// Original FILE_INFORMATION_CLASS supplied to an internal query producer.
  std::optional<uint32_t> FileInformationClass;
  /// Actual file body, independent of an optional scenario identifier.
  std::optional<uint64_t> FileObject;
  /// Driver-originated open and terminal provider identity, when established.
  std::optional<uint64_t> FileOpenOperation;
  std::optional<uint64_t> FileSystemDevice;
  std::optional<uint64_t> FileSystemFileID;
};

struct DriverFault {
  std::string Kind;
  uint64_t PC = 0;
  std::optional<uint64_t> Address;
  std::optional<uint64_t> Size;
  std::optional<std::string> Access;
  std::optional<uint32_t> Interrupt;
  std::optional<uint64_t> ErrorCode;
  std::optional<std::string> Cause;
};

/// Live registrations only. ComponentStorageReleased records an admitted
/// revocation of the original borrowed name storage; it never makes that
/// pointer readable or implies that a bug check has dereferenced it.
struct DriverBugCheckCallback {
  uint64_t Record = 0;
  uint64_t Routine = 0;
  uint64_t Component = 0;
  bool ComponentStorageReleased = false;
};

struct DriverResult {
  ExecutionBackendKind SelectedBackend = ExecutionBackendKind::Unicorn;
  std::string BackendSelectionReason;
  DriverOptions Configuration;
  DriverStopReason Stop = DriverStopReason::EngineError;
  uint64_t PC = 0;
  /// Number of instruction attempts admitted by the environment policy.
  /// CPU-faulting attempts count; policy-rejected instructions do not.
  /// Synthetic API dispatch and the return sentinel are not instructions.
  uint64_t Instructions = 0;
  std::optional<uint32_t> NTStatus;
  uint64_t ImageBase = 0;
  uint64_t Entry = 0;
  uint64_t PreferredImageBase = 0;
  uint64_t SecurityCookieAddress = 0;
  uint64_t DriverObject = 0;
  /// Digest of the exact reference PE bytes loaded for this run, if supplied.
  std::string KernelImageSHA256;
  std::string KernelPDBSHA256;
  uint64_t DriverUnload = 0;
  uint64_t AddDevice = 0;
  std::array<uint64_t, profile::MajorFunctionCount> MajorFunctions{};
  std::vector<DriverDevice> Devices;
  std::vector<DriverAPIEvent> Calls;
  std::vector<DriverProcessEventResult> ProcessEvents;
  std::vector<DriverRegistryEventResult> RegistryEvents;
  std::vector<DriverRequestDelayResult> RequestDelays;
  std::vector<DriverThreadNotificationResult> ThreadNotifications;
  std::vector<DriverMemoryWrite> Writes;
  std::vector<std::string> Messages;
  std::vector<DriverRequestResult> Requests;
  std::string Phase = profile::EntryPhase;
  bool UnloadCompleted = false;
  std::string Diagnostic;
  std::optional<DriverFault> Fault;
  /// Final registry state, including changes observed before execution stops.
  std::optional<std::vector<DriverRegistryKey>> Registry;
  /// Observed filesystem owner state, including bound volume devices.
  std::optional<std::vector<DriverFileSystemVolumeState>> FileSystem;
  /// Finite synchronous provider state, separate from metadata-backed volumes.
  std::optional<std::vector<DriverFiniteFileSystemVolume>> FiniteFileSystem;
  std::vector<DriverPnpDeviceResult> PnpDevices;
  std::vector<DriverInterruptResult> Interrupts;
  std::vector<DriverDmaResult> DmaTransfers;
  std::vector<DriverPowerPolicyResult> PowerPolicyEvents;
  std::vector<DriverBugCheckCallback> BugCheckCallbacks;
  /// A requested terminal guest crash. Dump/callback execution is unmodeled.
  std::optional<DriverBugCheck> BugCheck;
  std::optional<DriverPciConfiguration> PciConfiguration;
  std::optional<std::vector<DriverRegistryHiveState>> RegistryHives;
  /// Final provider-owned section backing, including API writes and page
  /// padding after client views have gone. Omission means no provider supplied.
  std::optional<DriverSectionNamespace> SectionNamespace;
};

/// Parse and validate a fresh complete PE image. Request/format/setup failures
/// are llvm::Error; execution stops (including unsupported behavior) are a
/// DriverResult and retain the observations made before the stop.
llvm::Expected<DriverResult> emulateDriver(const std::filesystem::path &Path,
                                           const DriverOptions &Options = {});

class ProcessObserver;
/// Observe the stopped driver environment without modifying guest state.
/// DriverEntry, nested callbacks and later lifecycle invocations retain their
/// own provenance. Observer failures are API errors, not guest outcomes.
llvm::Expected<DriverResult> observeDriver(const std::filesystem::path &Path,
                                           const DriverOptions &Options,
                                           ProcessObserver &Observer);

#define NEVERD_DRIVER_SCENARIO_LIMIT(Name, CName, Value)                       \
  inline constexpr size_t Name = Value;
#include "neverd/emulation/DriverScenarioLimits.def"
#undef NEVERD_DRIVER_SCENARIO_LIMIT

/// Parse the strict scenario object over Base. Omitted fields preserve Base.
/// Unknown fields, wrong types, unsupported request kinds and excessive input
/// fail before any image is loaded or guest instruction is executed.
llvm::Expected<DriverOptions>
driverOptionsFromScenarioJSON(llvm::StringRef JSON, DriverOptions Base = {});

const char *driverStopReasonName(DriverStopReason Reason);
std::string driverResultJSON(const DriverResult &Result);

} // namespace neverd::emulation
#endif
