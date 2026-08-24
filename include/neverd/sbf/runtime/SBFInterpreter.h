//===- SBFInterpreter.h - Deterministic SBF semantic oracle -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares a bounded interpreter that executes verified SBF instruction bytes
/// directly.  It deliberately does not consume MedIR, making it an independent
/// source-side oracle for differential backend tests.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_RUNTIME_SBFINTERPRETER_H
#define NEVERD_SBF_RUNTIME_SBFINTERPRETER_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sbf {

using SyscallArguments = std::array<uint64_t, kArgumentRegisterCount>;

/// Immutable facts supplied to one host syscall dispatch.
///
/// RuntimeFeatures is the already-resolved feature snapshot for this
/// execution. Hosts can use it to implement feature-sensitive syscall policy
/// such as SIMD-0459 without consulting a mutable cluster profile.
struct SyscallInvocation {
  uint32_t Hash = 0;
  SyscallArguments Arguments{};
  RuntimeFeature RuntimeFeatures = RuntimeFeature::None;

  [[nodiscard]] constexpr bool hasRuntimeFeature(RuntimeFeature Feature) const {
    return hasFeature(RuntimeFeatures, Feature);
  }
};

/// The three observable results of asking a host to dispatch one syscall.
///
/// Unregistered is distinct from a handled fault because legacy SBF continues
/// to the independent internal-function lookup after either Unregistered or
/// Returned. Only a handled non-fallback fault short-circuits that lookup.
/// FaultCode::UnknownSyscall has the same unregistered meaning at ABI
/// boundaries that can carry only a stable status code.
class SyscallOutcome {
public:
  enum class Kind : uint8_t { Unregistered, Returned, Fault };

  [[nodiscard]] static constexpr SyscallOutcome unregistered() {
    return SyscallOutcome(Kind::Unregistered, 0, FaultCode::None);
  }

  [[nodiscard]] static constexpr SyscallOutcome returned(uint64_t Value) {
    return SyscallOutcome(Kind::Returned, Value, FaultCode::None);
  }

  [[nodiscard]] static constexpr SyscallOutcome fault(FaultCode Code) {
    // Host callbacks cross an ABI boundary and can cast arbitrary integers to
    // FaultCode. Keep every execution backend on the same fail-closed path:
    // neither success nor an unknown numeric value may inhabit Fault.
    if (Code == FaultCode::None ||
        !isKnownFaultCodeValue(static_cast<uint32_t>(Code)))
      Code = FaultCode::InvalidInstruction;
    return SyscallOutcome(Kind::Fault, 0, Code);
  }

  [[nodiscard]] constexpr Kind kind() const { return TheKind; }

  [[nodiscard]] constexpr uint64_t value() const {
    assert(TheKind == Kind::Returned && "syscall outcome has no value");
    return Value;
  }

  [[nodiscard]] constexpr FaultCode faultCode() const {
    assert(TheKind == Kind::Fault && "syscall outcome has no fault");
    return TheFault;
  }

  /// Whether a fault-like outcome represents a missing runtime registration.
  /// Legacy immediate calls also continue to the function lookup after a
  /// successful return; this predicate distinguishes only fallback faults
  /// from handled faults.
  [[nodiscard]] constexpr bool representsUnregisteredSyscall() const {
    return TheKind == Kind::Unregistered ||
           (TheKind == Kind::Fault && TheFault == FaultCode::UnknownSyscall);
  }

  /// Encode this outcome for the LLVM host callback ABI. The output pointer is
  /// meaningful only when this returns FaultCode::None.
  [[nodiscard]] constexpr FaultCode abiStatus() const {
    if (TheKind == Kind::Returned)
      return FaultCode::None;
    if (TheKind == Kind::Unregistered)
      return FaultCode::UnknownSyscall;
    return TheFault;
  }

private:
  constexpr SyscallOutcome(Kind TheKind, uint64_t Value, FaultCode TheFault)
      : TheKind(TheKind), Value(Value), TheFault(TheFault) {}

  Kind TheKind;
  uint64_t Value;
  FaultCode TheFault;
};

using SyscallOutcomeCallback = std::function<SyscallOutcome(
    uint32_t Hash, const SyscallArguments &Arguments)>;

/// Preferred feature-aware callback. The invocation owns both its arguments
/// and the resolved runtime-feature snapshot for the duration of the call.
using FeatureAwareSyscallCallback =
    std::function<SyscallOutcome(const SyscallInvocation &Invocation)>;

/// Legacy callback retained for source compatibility. A value means Returned
/// and std::nullopt means Unregistered; use SyscallOutcomeCallback to report a
/// handled host fault.
using SyscallCallback = std::function<std::optional<uint64_t>(
    uint32_t Hash, const SyscallArguments &Arguments)>;

/// One contiguous VM-addressed memory region supplied by the execution host.
///
/// AccountDataDirectMapping does not cause executeRaw to invent account
/// serialization or backing storage. A caller reproducing that Agave topology
/// must provide the exact account-data regions (including writability and VM
/// gaps) here; ordinary load/store semantics then operate on those regions.
struct MemoryRegion {
  uint64_t Address = 0;
  std::vector<uint8_t> Bytes;
  bool Writable = false;
  std::string Name;
  /// Size of each backed interval when an equally sized unmapped interval
  /// follows it. Zero describes an ordinary contiguous region.
  size_t VMGapSize = 0;
};

/// Supplies deterministic input memory, a resolved runtime snapshot, and host
/// syscall behavior. Full Agave transaction/account ownership and CPI state
/// transitions are intentionally outside this decompiler-side execution
/// boundary; the host represents their VM-visible result with MemoryRegion.
struct ExecutionEnvironment {
  uint64_t Input = kInputStart;
  /// The address the runtime hands over in the instruction-data register, and
  /// zero on a runtime that has not activated that. Zero is what the register
  /// actually contains there, so a program that reads it anyway reads the same
  /// thing here that it would on chain.
  uint64_t InstructionData = 0;
  std::vector<MemoryRegion> Memory;
  SyscallCallback Syscall;
  /// Preferred fault-aware callback. When present, this takes precedence over
  /// the legacy optional-valued callback above.
  SyscallOutcomeCallback HostSyscall;
  /// Optional explicit runtime-feature snapshot. When absent, executeRaw uses
  /// SBFProgram::ActiveRuntimeFeatures. An explicitly supplied None is still
  /// an authoritative empty snapshot and never falls back to the program.
  std::optional<RuntimeFeature> RuntimeFeatures;
  /// Preferred typed callback. When present, this takes precedence over both
  /// legacy callback forms and receives the exact resolved feature snapshot.
  FeatureAwareSyscallCallback FeatureAwareSyscall;
};

struct InterpreterTraceEntry {
  size_t Slot = 0;
  va_t Address = 0;
  uint8_t RawOpcode = 0;
  Opcode Op = Opcode::Unknown;
  size_t CallDepth = 0;
};

struct SyscallTraceEntry {
  size_t Slot = 0;
  uint32_t Hash = 0;
  SyscallArguments Arguments{};
  uint64_t Result = 0;
};

enum class ExecutionStatus : uint8_t {
  Running,
  Returned,
  Faulted,
  StepLimit,
};

/// Captures every observable value needed by differential backend tests.
struct ExecutionResult {
  ExecutionStatus Status = ExecutionStatus::Running;
  FaultCode Fault = FaultCode::None;
  std::string Error;
  size_t FinalSlot = 0;
  size_t Steps = 0;
  size_t MaxCallDepth = 0;
  uint64_t ReturnValue = 0;
  std::array<uint64_t, kRegisterCount> Registers{};
  std::vector<MemoryRegion> Memory;
  std::vector<InterpreterTraceEntry> Trace;
  std::vector<SyscallTraceEntry> Syscalls;
};

/// Bounds execution resources and controls trace collection.
struct InterpreterOptions {
  size_t MaxSteps = kDefaultMaxExecutionSteps;
  std::optional<size_t> MaxCallDepth;
  bool RecordTrace = true;
};

/// Executes Program.text() directly. Runtime faults are represented in the
/// returned ExecutionResult; llvm::Error is reserved for malformed API input.
llvm::Expected<ExecutionResult>
executeRaw(const SBFProgram &Program, ExecutionEnvironment Environment = {},
           InterpreterOptions Options = {});

} // namespace neverd::sbf

#endif // NEVERD_SBF_RUNTIME_SBFINTERPRETER_H
