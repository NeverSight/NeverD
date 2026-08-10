//===- EVMIR.h - Staged Ethereum Virtual Machine IR ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the lossless EVM LowIR, 256-bit stack-SSA MedIR, recovered HighIR,
/// and the aggregate program passed to backends.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_EVMIR_H
#define NEVERD_EVM_EVMIR_H

#include "neverd/evm/Opcodes.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd::evm {

struct Diagnostic {
  uint64_t PC = 0;
  std::string Message;
};

enum class OpcodeDecodeStatus : uint8_t {
#define EVM_OPCODE_DECODE_STATUS(NAME, SPELLING) NAME,
#include "neverd/evm/EVMDecodeStatuses.def"
};

enum class ImmediateDecodeStatus : uint8_t {
#define EVM_IMMEDIATE_DECODE_STATUS(NAME, SPELLING) NAME,
#include "neverd/evm/EVMDecodeStatuses.def"
};

/// A decoded instruction whose metadata records both byte identity and
/// activation in the selected hardfork.
struct LowInstruction {
  uint64_t PC = 0;
  uint64_t NextPC = 0;
  OpcodeInfo Info;
  OpcodeDecodeStatus DecodeStatus = OpcodeDecodeStatus::Unknown;
  llvm::APInt Immediate = llvm::APInt(kWordBits, 0);
  ImmediateDecodeStatus ImmediateStatus = ImmediateDecodeStatus::None;
  std::array<uint16_t, kMaxImmediateStackOperands> StackOperands{};
  uint8_t StackOperandCount = 0;
  std::vector<uint8_t> Encoding;

  [[nodiscard]] Opcode opcode() const { return Info.Op; }
  [[nodiscard]] bool isAssigned() const { return Info.isAssigned(); }
  [[nodiscard]] bool isActive() const {
    return DecodeStatus == OpcodeDecodeStatus::Active;
  }
  [[nodiscard]] bool hasWellFormedImmediate() const {
    switch (Info.Immediate) {
    case ImmediateKind::None:
      return ImmediateStatus == ImmediateDecodeStatus::None &&
             StackOperandCount == 0;
    case ImmediateKind::PushData:
      return (ImmediateStatus == ImmediateDecodeStatus::Complete ||
              ImmediateStatus == ImmediateDecodeStatus::Truncated) &&
             StackOperandCount == 0;
    case ImmediateKind::EIP8024Single:
      return (ImmediateStatus == ImmediateDecodeStatus::Complete ||
              ImmediateStatus == ImmediateDecodeStatus::Truncated) &&
             StackOperandCount == 1;
    case ImmediateKind::EIP8024Pair:
      return (ImmediateStatus == ImmediateDecodeStatus::Complete ||
              ImmediateStatus == ImmediateDecodeStatus::Truncated) &&
             StackOperandCount == 2;
    }
    return false;
  }
  [[nodiscard]] bool isExecutable() const {
    return isActive() && isAssigned() && hasWellFormedImmediate();
  }
  /// Any non-executable instruction faults at runtime and therefore terminates
  /// the semantic block even when its assigned opcode metadata does not.
  [[nodiscard]] bool isTerminator() const {
    return !isExecutable() || Info.IsTerminator;
  }
  [[nodiscard]] uint8_t stackPops() const {
    return isExecutable() ? Info.StackPops : 0;
  }
  [[nodiscard]] uint8_t stackPushes() const {
    return isExecutable() ? Info.StackPushes : 0;
  }
  /// Returns true only when this is an active opcode equal to \p Candidate.
  [[nodiscard]] bool is(Opcode Candidate) const {
    return isExecutable() && opcode() == Candidate;
  }
  /// Opcode-family queries include the hardfork activation check. This keeps
  /// relaxed decoding from treating a future opcode as executable semantics.
  [[nodiscard]] bool isPush() const {
    return isExecutable() && evm::isPush(opcode());
  }
  [[nodiscard]] bool isDup() const {
    return isExecutable() && (evm::isDup(opcode()) || evm::isDeepDup(opcode()));
  }
  [[nodiscard]] bool isSwap() const {
    return isExecutable() &&
           (evm::isSwap(opcode()) || evm::isDeepSwap(opcode()));
  }
  [[nodiscard]] bool isExchange() const {
    return isExecutable() && evm::isExchange(opcode());
  }
  [[nodiscard]] uint16_t dupDepth() const {
    if (!isDup())
      return 0;
    return evm::isDeepDup(opcode()) && StackOperandCount == 1
               ? StackOperands[0]
               : evm::dupDepth(opcode());
  }
  [[nodiscard]] uint16_t swapDepth() const {
    if (!isSwap())
      return 0;
    return evm::isDeepSwap(opcode()) && StackOperandCount == 1
               ? StackOperands[0]
               : evm::swapDepth(opcode());
  }
  [[nodiscard]] std::optional<StackDepthPair> exchangeDepths() const {
    if (!isExchange() || StackOperandCount != 2)
      return std::nullopt;
    return StackDepthPair{StackOperands[0], StackOperands[1]};
  }
  [[nodiscard]] size_t requiredStackHeight() const {
    if (!isExecutable())
      return 0;
    if (isDup())
      return dupDepth();
    if (isSwap())
      return static_cast<size_t>(swapDepth()) + 1;
    if (const auto Depths = exchangeDepths())
      return static_cast<size_t>(std::max(Depths->First, Depths->Second)) + 1;
    return Info.StackPops;
  }
  [[nodiscard]] std::ptrdiff_t stackDelta() const {
    if (!isExecutable())
      return 0;
    return static_cast<std::ptrdiff_t>(Info.StackPushes) -
           static_cast<std::ptrdiff_t>(Info.StackPops);
  }
  [[nodiscard]] bool isLog() const {
    return isExecutable() && evm::isLog(opcode());
  }
  [[nodiscard]] bool isJump() const {
    return isExecutable() && evm::isJump(opcode());
  }
};

enum class EdgeKind : uint8_t {
  Fallthrough,
  Jump,
  ConditionalTrue,
  ConditionalFalse,
  Indirect,
};

struct LowEdge {
  EdgeKind Kind = EdgeKind::Fallthrough;
  std::optional<uint64_t> Target;
};

struct LowBlock {
  uint64_t StartPC = 0;
  uint64_t EndPC = 0;
  size_t FirstInstruction = 0;
  size_t InstructionCount = 0;
  std::vector<LowEdge> Successors;
  std::vector<uint64_t> Predecessors;
  bool Reachable = false;
  bool HasIndirectSuccessor = false;
  std::optional<size_t> EntryStackHeight;
  std::optional<size_t> ExitStackHeight;
};

/// Lossless bytecode and CFG representation at the decoder boundary.
struct EVMLowIR {
  Hardfork Fork = Hardfork::Latest;
  bool Strict = true;
  std::vector<uint8_t> Code;
  std::vector<LowInstruction> Instructions;
  std::vector<LowBlock> Blocks;
  std::set<uint64_t> JumpDestinations;
  std::vector<Diagnostic> Diagnostics;

  const LowBlock *findBlock(uint64_t PC) const {
    const auto It = llvm::lower_bound(
        Blocks, PC, [](const LowBlock &Block, uint64_t Address) {
          return Block.StartPC < Address;
        });
    return It != Blocks.end() && It->StartPC == PC ? &*It : nullptr;
  }
  LowBlock *findBlock(uint64_t PC) {
    const auto It = llvm::lower_bound(
        Blocks, PC, [](const LowBlock &Block, uint64_t Address) {
          return Block.StartPC < Address;
        });
    return It != Blocks.end() && It->StartPC == PC ? &*It : nullptr;
  }
  bool hasEdge(uint64_t From, uint64_t To, EdgeKind Kind) const {
    const LowBlock *Block = findBlock(From);
    if (!Block)
      return false;
    for (const auto &Edge : Block->Successors)
      if (Edge.Kind == Kind && Edge.Target && *Edge.Target == To)
        return true;
    return false;
  }
};

using ValueID = uint32_t;

enum class ValueKind : uint8_t { Constant, Instruction, Phi, Unknown };

struct MedValue {
  ValueID ID = 0;
  ValueKind Kind = ValueKind::Unknown;
  uint64_t PC = 0;
  std::string Name;
  std::vector<ValueID> Inputs;
  std::vector<uint64_t> IncomingBlocks;
  std::optional<llvm::APInt> Constant;
};

struct MedOperation {
  uint64_t PC = 0;
  Opcode Op = Opcode::STOP;
  std::string Name;
  std::vector<ValueID> Inputs;
  std::vector<ValueID> Outputs;
  EffectKind Effect = EffectKind::Unknown;
  MemoryAccessKind MemoryAccess = MemoryAccessKind::Unknown;
  StateAccessKind StateAccess = StateAccessKind::Unknown;
  CallValueAccessKind CallValueAccess = CallValueAccessKind::Unknown;
};

struct MedBlock {
  uint64_t StartPC = 0;
  std::vector<ValueID> EntryStack;
  std::vector<ValueID> PhiValues;
  std::vector<MedOperation> Operations;
  std::vector<ValueID> ExitStack;
};

/// Stack SSA representation with explicit values, phis, and semantic effects.
struct EVMMedIR {
  std::vector<MedValue> Values;
  std::vector<MedBlock> Blocks;
  std::vector<Diagnostic> Diagnostics;

  const MedValue *findValue(ValueID ID) const {
    if (ID >= Values.size())
      return nullptr;
    return &Values[ID];
  }
};

enum class Mutability : uint8_t { Pure, View, NonPayable, Payable };

struct RecoveredArgument {
  unsigned Index = 0;
  uint64_t CalldataOffset = 0;
  std::string Type = kDefaultRecoveredWordType.str();
  std::string Name;
};

struct RecoveredFunction {
  uint32_t Selector = 0;
  uint64_t EntryPC = 0;
  std::string Name;
  std::vector<RecoveredArgument> Arguments;
  std::vector<std::string> Returns;
  Mutability StateMutability = Mutability::Pure;
};

struct StorageFact {
  uint64_t PC = 0;
  bool IsWrite = false;
  bool IsTransient = false;
  std::optional<llvm::APInt> Slot;
  std::string SuggestedName;
};

struct EventFact {
  uint64_t PC = 0;
  unsigned Topics = 0;
  std::optional<llvm::APInt> Topic0;
  std::string SuggestedName;
};

struct ErrorFact {
  uint64_t PC = 0;
  std::optional<uint32_t> Selector;
  std::string SuggestedName;
};

enum class RegionKind : uint8_t { Function, CFG };

struct StructuredRegion {
  uint64_t EntryPC = 0;
  RegionKind Kind = RegionKind::CFG;
  std::vector<uint64_t> Blocks;
};

/// Best-effort source-level facts recovered without claiming source identity.
struct EVMHighIR {
  std::vector<RecoveredFunction> Functions;
  std::vector<StorageFact> Storage;
  std::vector<EventFact> Events;
  std::vector<ErrorFact> Errors;
  std::vector<StructuredRegion> Regions;
  bool HasFallback = false;
  bool HasReceive = false;
  std::vector<Diagnostic> Diagnostics;
};

/// Owns all EVM pipeline stages for one normalized runtime program.
struct EVMProgram {
  EVMLowIR Low;
  EVMMedIR Med;
  EVMHighIR High;
};

} // namespace neverd::evm

#endif // NEVERD_EVM_EVMIR_H
