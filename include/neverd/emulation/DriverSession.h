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

#include "neverd/emulation/DriverPnp.h"
#include "neverd/emulation/DriverProfile.h"
#include "neverd/emulation/DriverRegistry.h"

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
  DriverRequestKind Kind = DriverRequestKind::DeviceControl;
  /// CREATE with an empty name selects the sole live device; ambiguity fails.
  /// Later requests use their file's device unless a matching name is given.
  std::string Device;
  uint32_t ControlCode = 0;
  std::vector<uint8_t> Input;
  uint32_t OutputSize = 0;
  /// Initial contents of a direct IOCTL's second buffer, padded to OutputSize.
  std::vector<uint8_t> DirectInput;
  uint64_t ByteOffset = 0;
  /// Scenario identity of an independently opened FILE_OBJECT.
  uint32_t File = 0;
  /// Optional cancellation event relative to submission in virtual 100 ns
  /// units. Zero requests cancellation before dispatch; only READ/WRITE/IOCTL
  /// accept this field. Runtime cancellation requires a supported KMDF route.
  std::optional<uint64_t> CancelAfter100ns;
  /// Stable configured PDO identity; mutually exclusive with Device.
  std::string DeviceID;
  /// Present exactly when Kind is Pnp; those packets have no FILE_OBJECT.
  std::optional<DriverPnpOperation> Pnp;
  /// Present exactly when Kind is Power; those packets have no FILE_OBJECT.
  std::optional<DriverPowerOperation> Power;
  /// Explicit external pulses for READ/WRITE/IOCTL. These events survive the
  /// source IRP's completion and do not imply device enable/ack semantics.
  std::vector<DriverInterruptEvent> InterruptEvents{};
};

/// This profile models a single-processor x64 WDM lifecycle with cooperative
/// worker scheduling, dispatcher waits and DPC execution at modeled IRQLs.
/// All pointers describe guest addresses, never native pointers. No host OS
/// services are forwarded. Unsupported APIs and CPU environment effects stop.
struct DriverOptions {
  uint64_t InstructionLimit = profile::DefaultInstructionLimit;
  uint64_t MemoryLimit = profile::DefaultMemoryLimit;
  uint64_t EventLimit = profile::DefaultEventLimit;
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
  std::vector<DriverPnpDevice> PnpDevices;
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
};

struct DriverFault {
  std::string Kind;
  uint64_t PC = 0;
  std::optional<uint64_t> Address;
  std::optional<uint64_t> Size;
  std::optional<std::string> Access;
  std::optional<uint32_t> Interrupt;
};

struct DriverResult {
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
  uint64_t DriverUnload = 0;
  uint64_t AddDevice = 0;
  std::array<uint64_t, profile::MajorFunctionCount> MajorFunctions{};
  std::vector<DriverDevice> Devices;
  std::vector<DriverAPIEvent> Calls;
  std::vector<DriverMemoryWrite> Writes;
  std::vector<std::string> Messages;
  std::vector<DriverRequestResult> Requests;
  std::string Phase = profile::EntryPhase;
  bool UnloadCompleted = false;
  std::string Diagnostic;
  std::optional<DriverFault> Fault;
  /// Final registry state, including changes observed before execution stops.
  std::optional<std::vector<DriverRegistryKey>> Registry;
  std::vector<DriverPnpDeviceResult> PnpDevices;
  std::vector<DriverInterruptResult> Interrupts;
};

/// Parse and validate a fresh complete PE image. Request/format/setup failures
/// are llvm::Error; execution stops (including unsupported behavior) are a
/// DriverResult and retain the observations made before the stop.
llvm::Expected<DriverResult> emulateDriver(const std::filesystem::path &Path,
                                           const DriverOptions &Options = {});

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
