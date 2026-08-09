//===- EVMIR.h - Staged Ethereum Virtual Machine IR ----------*- C++ -*-===//

#ifndef NEVERD_EVM_EVMIR_H
#define NEVERD_EVM_EVMIR_H

#include "neverd/evm/Opcodes.h"

#include "llvm/ADT/APInt.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace neverd::evm {

struct Diagnostic {
  uint64_t PC = 0;
  std::string Message;
};

struct LowInstruction {
  uint64_t PC = 0;
  uint64_t NextPC = 0;
  Opcode Op = Opcode::STOP;
  OpcodeInfo Info;
  bool Known = true;
  llvm::APInt Immediate = llvm::APInt(kWordBits, 0);
  bool ImmediateTruncated = false;
  std::vector<uint8_t> Encoding;
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

struct EVMLowIR {
  Hardfork Fork = Hardfork::Latest;
  bool Strict = true;
  std::vector<uint8_t> Code;
  std::vector<LowInstruction> Instructions;
  std::vector<LowBlock> Blocks;
  std::set<uint64_t> JumpDestinations;
  std::vector<Diagnostic> Diagnostics;

  const LowBlock *findBlock(uint64_t PC) const {
    for (const auto &Block : Blocks)
      if (Block.StartPC == PC)
        return &Block;
    return nullptr;
  }
  LowBlock *findBlock(uint64_t PC) {
    for (auto &Block : Blocks)
      if (Block.StartPC == PC)
        return &Block;
    return nullptr;
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
  EffectKind Effect = EffectKind::None;
};

struct MedBlock {
  uint64_t StartPC = 0;
  std::vector<ValueID> EntryStack;
  std::vector<ValueID> PhiValues;
  std::vector<MedOperation> Operations;
  std::vector<ValueID> ExitStack;
};

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

struct EVMHighIR {
  std::vector<RecoveredFunction> Functions;
  std::vector<StorageFact> Storage;
  std::vector<EventFact> Events;
  std::vector<ErrorFact> Errors;
  std::vector<StructuredRegion> Regions;
  bool HasFallback = true;
  bool HasReceive = false;
  std::vector<Diagnostic> Diagnostics;
};

struct EVMProgram {
  EVMLowIR Low;
  EVMMedIR Med;
  EVMHighIR High;
};

} // namespace neverd::evm

#endif // NEVERD_EVM_EVMIR_H
