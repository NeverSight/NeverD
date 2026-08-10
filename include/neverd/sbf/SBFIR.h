//===- SBFIR.h - Staged Solana SBF intermediate representations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SBFIR_H
#define NEVERD_SBF_SBFIR_H

#include "neverd/Common.h"
#include "neverd/sbf/Opcodes.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/SBFMetadata.h"
#include "neverd/sbf/Syscalls.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sbf {

enum class DiagnosticSeverity : uint8_t { Warning, Error };

struct Diagnostic {
  DiagnosticSeverity Severity = DiagnosticSeverity::Error;
  size_t Slot = 0;
  va_t Address = 0;
  std::string Message;
};

enum class CallKind : uint8_t {
  None,
  Syscall,
  Internal,
  Indirect,
  Unresolved,
};

struct LowInstruction {
  size_t Slot = 0;
  va_t Address = 0;
  std::array<uint8_t, kInstructionSize> Encoding{};
  uint8_t RawOpcode = 0;
  uint8_t Dst = 0;
  uint8_t Src = 0;
  int16_t Offset = 0;
  int32_t RawImmediate = 0;
  uint64_t Immediate = 0;
  const OpcodeInfo *Info = nullptr;
  bool IsContinuation = false;
  uint8_t SlotWidth = 1;
  std::optional<size_t> BranchTarget;
  CallKind Call = CallKind::None;
  std::optional<size_t> CallTarget;
  uint8_t CallRegister = 0;
  uint32_t SyscallHash = 0;
  const SyscallInfo *Syscall = nullptr;
  std::string ResolvedName;
};

enum class EdgeKind : uint8_t {
  Fallthrough,
  BranchTaken,
  Branch,
  Call,
  IndirectCall,
  Return,
  Invalid,
};

struct CFGEdge {
  size_t From = 0;
  std::optional<size_t> To;
  EdgeKind Kind = EdgeKind::Fallthrough;
};

struct BasicBlock {
  size_t ID = 0;
  size_t StartSlot = 0;
  size_t EndSlot = 0;
  std::vector<size_t> Predecessors;
  std::vector<size_t> Successors;
  bool Reachable = false;
};

struct LowIR {
  Version TheVersion = Version::Reserved;
  va_t TextAddress = 0;
  size_t EntrySlot = 0;
  std::vector<LowInstruction> Instructions;
  std::vector<BasicBlock> Blocks;
  std::vector<CFGEdge> Edges;
  std::vector<Diagnostic> Diagnostics;
};

enum class ResultExtension : uint8_t { None, Zero32, Sign32 };
enum class ImmediateExtension : uint8_t { Sign32, Zero32, Full64 };

struct MedInstruction {
  size_t Slot = 0;
  va_t Address = 0;
  Opcode SourceOpcode = Opcode::Unknown;
  Operation Op = Operation::Invalid;
  OperandForm Form = OperandForm::None;
  uint8_t Width = 0;
  uint8_t Dst = 0;
  uint8_t Src = 0;
  int16_t Offset = 0;
  uint64_t Immediate = 0;
  ImmediateExtension ImmediateMode = ImmediateExtension::Sign32;
  ResultExtension Extension = ResultExtension::None;
  bool SwapOperands = false;
  std::optional<size_t> BranchTarget;
  CallKind Call = CallKind::None;
  std::optional<size_t> CallTarget;
  uint8_t CallRegister = 0;
  uint32_t SyscallHash = 0;
  const SyscallInfo *Syscall = nullptr;
};

struct RegisterValue {
  enum class Kind : uint8_t { Unknown, Constant, StackAddress, RodataAddress };
  Kind ValueKind = Kind::Unknown;
  uint64_t Value = 0;
  int64_t Offset = 0;

  bool operator==(const RegisterValue &) const = default;
};

struct MedBlock {
  size_t ID = 0;
  size_t StartSlot = 0;
  size_t EndSlot = 0;
  std::array<RegisterValue, kRegisterCount> Inputs{};
  std::array<RegisterValue, kRegisterCount> Outputs{};
};

struct MedIR {
  Version TheVersion = Version::Reserved;
  std::vector<MedInstruction> Instructions;
  std::vector<MedBlock> Blocks;
};

struct Function {
  size_t EntrySlot = 0;
  va_t Address = 0;
  std::string Name;
  std::vector<size_t> Blocks;
};

struct CallEdge {
  size_t SourceSlot = 0;
  std::optional<size_t> TargetSlot;
  CallKind Kind = CallKind::Unresolved;
  std::string Name;
};

struct SyscallUse {
  size_t Slot = 0;
  uint32_t Hash = 0;
  const SyscallInfo *Info = nullptr;
};

struct RecoveredString {
  va_t Address = 0;
  std::string Value;
};

enum class RegionKind : uint8_t { If, Loop, Irreducible };

struct Region {
  RegionKind Kind = RegionKind::Irreducible;
  size_t HeaderBlock = 0;
  std::optional<size_t> ExitBlock;
  std::vector<size_t> Blocks;
};

struct HighIR {
  std::vector<Function> Functions;
  std::vector<CallEdge> Calls;
  std::vector<SyscallUse> Syscalls;
  std::vector<RecoveredString> Strings;
  std::vector<Region> Regions;
  bool UsesCPI = false;
  bool UsesAccounts = false;
};

struct SBFProgram {
  Metadata Image;
  std::vector<uint8_t> Text;
  std::vector<uint8_t> Rodata;
  LowIR Low;
  MedIR Med;
  HighIR High;
};

} // namespace neverd::sbf

#endif // NEVERD_SBF_SBFIR_H
