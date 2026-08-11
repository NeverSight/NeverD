//===- Interpreter.h - Deterministic SBF semantic oracle ------*- C++ -*-===//
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

#ifndef NEVERD_SBF_INTERPRETER_H
#define NEVERD_SBF_INTERPRETER_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sbf {

using SyscallArguments = std::array<uint64_t, kArgumentRegisterCount>;
using SyscallCallback = std::function<std::optional<uint64_t>(
    uint32_t Hash, const SyscallArguments &Arguments)>;

/// One contiguous VM-addressed memory region.
struct MemoryRegion {
  uint64_t Address = 0;
  std::vector<uint8_t> Bytes;
  bool Writable = false;
  std::string Name;
};

/// Supplies deterministic input memory and syscall behavior.
struct ExecutionEnvironment {
  uint64_t Input = kInputStart;
  /// The address the runtime hands over in the instruction-data register, and
  /// zero on a runtime that has not activated that. Zero is what the register
  /// actually contains there, so a program that reads it anyway reads the same
  /// thing here that it would on chain.
  uint64_t InstructionData = 0;
  std::vector<MemoryRegion> Memory;
  SyscallCallback Syscall;
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

#endif // NEVERD_SBF_INTERPRETER_H
