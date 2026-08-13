//===- EVMOpcodes.h - Ethereum Virtual Machine opcode metadata -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exposes strongly typed opcode, hardfork, and effect metadata generated from
/// the EVM definition databases.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_BYTECODE_EVMOPCODES_H
#define NEVERD_EVM_BYTECODE_EVMOPCODES_H

#include "neverd/evm/EVMConstants.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::evm {

enum class Hardfork : uint8_t {
#define EVM_HARDFORK(NAME, SPELLING) NAME,
#define EVM_HARDFORK_LATEST(NAME, SPELLING) Latest = (NAME),
#include "neverd/evm/bytecode/EVMHardforks.def"
};

inline constexpr Hardfork kLatestStableHardfork =
#define EVM_HARDFORK_LATEST(NAME, SPELLING) Hardfork::NAME
#include "neverd/evm/bytecode/EVMHardforks.def"
    ;

inline constexpr Hardfork kNewestKnownHardfork =
#define EVM_HARDFORK_NEWEST(NAME) Hardfork::NAME
#include "neverd/evm/bytecode/EVMHardforks.def"
    ;

enum class ImmediateKind : uint8_t {
#define EVM_IMMEDIATE_KIND(NAME, SPELLING) NAME,
#include "neverd/evm/bytecode/EVMImmediateKinds.def"
};

enum class EffectKind : uint8_t {
#define EVM_EFFECT(NAME, SPELLING) NAME,
#include "neverd/evm/bytecode/EVMEffects.def"
};

/// Orthogonal access to byte-addressable EVM memory. This is deliberately
/// separate from EffectKind because an instruction can both interact with the
/// host and access memory (for example, CALL and EXTCODECOPY).
enum class MemoryAccessKind : uint8_t {
#define EVM_MEMORY_ACCESS_KIND(NAME, SPELLING) NAME,
#include "neverd/evm/bytecode/EVMMemoryAccesses.def"
};

/// Source-level state access used when recovering Solidity mutability. This
/// is separate from EffectKind: CALLDATACOPY has a context-read effect but is
/// valid in a pure function, whereas ADDRESS requires at least view.
enum class StateAccessKind : uint8_t {
#define EVM_STATE_ACCESS_KIND(NAME, SPELLING) NAME,
#include "neverd/evm/bytecode/EVMStateAccesses.def"
};

/// Source-level access to msg.value used when recovering Solidity payability.
/// It remains independent from both state mutability and the primary effect.
enum class CallValueAccessKind : uint8_t {
#define EVM_CALL_VALUE_ACCESS_KIND(NAME, SPELLING) NAME,
#include "neverd/evm/bytecode/EVMCallValueAccesses.def"
};

enum class OpcodeClass : uint8_t {
  Arithmetic,
  Comparison,
  Bitwise,
  Crypto,
  Environment,
  Block,
  Memory,
  Storage,
  Control,
  Stack,
  Log,
  System,
  Unknown,
};

enum class Opcode : uint8_t {
#define EVM_OPCODE(NAME, BYTE, POPS, PUSHES, IMMEDIATE_BYTES, IMMEDIATE_KIND,  \
                   CLASS, INTRODUCED, EFFECT, MEMORY_ACCESS, STATE_ACCESS,     \
                   CALL_VALUE_ACCESS, TERMINATOR)                              \
  NAME = (BYTE),
#include "neverd/evm/bytecode/EVMOpcodes.def"
};

struct StackDepthPair {
  uint16_t First;
  uint16_t Second;

  friend constexpr bool operator==(const StackDepthPair &,
                                   const StackDepthPair &) = default;
};

/// Canonical metadata for one active opcode. Names always have static lifetime
/// because records and historical aliases are sourced from string literals.
struct OpcodeInfo {
  /// Opcode metadata must come from the definition database or the explicit
  /// unknown-byte factory; a partially initialized record is never valid.
  OpcodeInfo() = delete;
  constexpr OpcodeInfo(Opcode OpValue, llvm::StringLiteral NameValue,
                       uint8_t StackPopsValue, uint8_t StackPushesValue,
                       uint8_t ImmediateBytesValue,
                       ImmediateKind ImmediateValue, OpcodeClass ClassValue,
                       Hardfork IntroducedValue, EffectKind EffectValue,
                       MemoryAccessKind MemoryAccessValue,
                       StateAccessKind StateAccessValue,
                       CallValueAccessKind CallValueAccessValue,
                       bool IsTerminatorValue)
      : Op(OpValue), Name(NameValue), StackPops(StackPopsValue),
        StackPushes(StackPushesValue), ImmediateBytes(ImmediateBytesValue),
        Immediate(ImmediateValue), Class(ClassValue),
        Introduced(IntroducedValue), Effect(EffectValue),
        MemoryAccess(MemoryAccessValue), StateAccess(StateAccessValue),
        CallValueAccess(CallValueAccessValue), IsTerminator(IsTerminatorValue) {
  }

  Opcode Op;
  llvm::StringLiteral Name;
  uint8_t StackPops;
  uint8_t StackPushes;
  uint8_t ImmediateBytes;
  ImmediateKind Immediate;
  OpcodeClass Class;
  Hardfork Introduced;
  EffectKind Effect;
  MemoryAccessKind MemoryAccess;
  StateAccessKind StateAccess;
  CallValueAccessKind CallValueAccess;
  bool IsTerminator;

  [[nodiscard]] constexpr bool isAssigned() const {
    return Class != OpcodeClass::Unknown;
  }
};

/// Returns whether \p Class belongs to the EVM's scalar ALU. Backends use
/// this shared classification to select their inline arithmetic lowering.
[[nodiscard]] constexpr bool isALU(OpcodeClass Class) {
  return Class == OpcodeClass::Arithmetic || Class == OpcodeClass::Comparison ||
         Class == OpcodeClass::Bitwise;
}

[[nodiscard]] constexpr bool isALU(const OpcodeInfo &Info) {
  return Info.isAssigned() && isALU(Info.Class);
}

[[nodiscard]] constexpr bool isAssignedOpcode(Opcode Op) {
  switch (Op) {
#define EVM_OPCODE(NAME, BYTE, POPS, PUSHES, IMMEDIATE_BYTES, IMMEDIATE_KIND,  \
                   CLASS, INTRODUCED, EFFECT, MEMORY_ACCESS, STATE_ACCESS,     \
                   CALL_VALUE_ACCESS, TERMINATOR)                              \
  case Opcode::NAME:                                                           \
    return true;
#include "neverd/evm/bytecode/EVMOpcodes.def"
  }
  return false;
}

/// Returns the memory behavior declared on the EVMOpcodes.def record.
/// Unassigned enum values are conservative rather than silently appearing
/// memory-free.
[[nodiscard]] constexpr MemoryAccessKind memoryAccess(Opcode Op) {
  switch (Op) {
#define EVM_OPCODE(NAME, BYTE, POPS, PUSHES, IMMEDIATE_BYTES, IMMEDIATE_KIND,  \
                   CLASS, INTRODUCED, EFFECT, MEMORY_ACCESS, STATE_ACCESS,     \
                   CALL_VALUE_ACCESS, TERMINATOR)                              \
  case Opcode::NAME:                                                           \
    return MemoryAccessKind::MEMORY_ACCESS;
#include "neverd/evm/bytecode/EVMOpcodes.def"
  }
  return MemoryAccessKind::Unknown;
}

[[nodiscard]] constexpr bool mayReadMemory(MemoryAccessKind Access) {
  switch (Access) {
  case MemoryAccessKind::None:
  case MemoryAccessKind::Write:
    return false;
  case MemoryAccessKind::Read:
  case MemoryAccessKind::ReadWrite:
  case MemoryAccessKind::Unknown:
    return true;
  }
  return true;
}

[[nodiscard]] constexpr bool mayWriteMemory(MemoryAccessKind Access) {
  switch (Access) {
  case MemoryAccessKind::None:
  case MemoryAccessKind::Read:
    return false;
  case MemoryAccessKind::Write:
  case MemoryAccessKind::ReadWrite:
  case MemoryAccessKind::Unknown:
    return true;
  }
  return true;
}

[[nodiscard]] constexpr bool mayReadMemory(const OpcodeInfo &Info) {
  return mayReadMemory(Info.MemoryAccess);
}

[[nodiscard]] constexpr bool mayWriteMemory(const OpcodeInfo &Info) {
  return mayWriteMemory(Info.MemoryAccess);
}

/// Least upper bound for state-access requirements across a function.
[[nodiscard]] constexpr StateAccessKind
mergeStateAccess(StateAccessKind Left, StateAccessKind Right) {
  const auto IsValid = [](StateAccessKind Access) {
    return Access == StateAccessKind::None || Access == StateAccessKind::Read ||
           Access == StateAccessKind::Write ||
           Access == StateAccessKind::Unknown;
  };
  if (!IsValid(Left) || !IsValid(Right))
    return StateAccessKind::Unknown;
  if (Left == StateAccessKind::Unknown || Right == StateAccessKind::Unknown)
    return StateAccessKind::Unknown;
  if (Left == StateAccessKind::Write || Right == StateAccessKind::Write)
    return StateAccessKind::Write;
  if (Left == StateAccessKind::Read || Right == StateAccessKind::Read)
    return StateAccessKind::Read;
  return StateAccessKind::None;
}

inline constexpr unsigned kAssignedOpcodeCount = [] {
  unsigned Count = 0;
#define EVM_OPCODE(NAME, BYTE, POPS, PUSHES, IMMEDIATE_BYTES, IMMEDIATE_KIND,  \
                   CLASS, INTRODUCED, EFFECT, MEMORY_ACCESS, STATE_ACCESS,     \
                   CALL_VALUE_ACCESS, TERMINATOR)                              \
  ++Count;
#include "neverd/evm/bytecode/EVMOpcodes.def"
  return Count;
}();

inline constexpr uint8_t kMaxOpcodeStackPops = [] {
  uint8_t Maximum = 0;
#define EVM_OPCODE(NAME, BYTE, POPS, PUSHES, IMMEDIATE_BYTES, IMMEDIATE_KIND,  \
                   CLASS, INTRODUCED, EFFECT, MEMORY_ACCESS, STATE_ACCESS,     \
                   CALL_VALUE_ACCESS, TERMINATOR)                              \
  if ((POPS) > Maximum)                                                        \
    Maximum = (POPS);
#include "neverd/evm/bytecode/EVMOpcodes.def"
  return Maximum;
}();

inline constexpr uint8_t kMaxHostOpcodeArguments = [] {
  uint8_t Maximum = 0;
#define EVM_OPCODE(NAME, BYTE, POPS, PUSHES, IMMEDIATE_BYTES, IMMEDIATE_KIND,  \
                   CLASS, INTRODUCED, EFFECT, MEMORY_ACCESS, STATE_ACCESS,     \
                   CALL_VALUE_ACCESS, TERMINATOR)                              \
  if (OpcodeClass::CLASS != OpcodeClass::Stack && (POPS) > Maximum)            \
    Maximum = (POPS);
#include "neverd/evm/bytecode/EVMOpcodes.def"
  return Maximum;
}();

inline constexpr uint8_t kMaxALUStackPops = [] {
  uint8_t Maximum = 0;
#define EVM_OPCODE(NAME, BYTE, POPS, PUSHES, IMMEDIATE_BYTES, IMMEDIATE_KIND,  \
                   CLASS, INTRODUCED, EFFECT, MEMORY_ACCESS, STATE_ACCESS,     \
                   CALL_VALUE_ACCESS, TERMINATOR)                              \
  if (isALU(OpcodeClass::CLASS) && (POPS) > Maximum)                           \
    Maximum = (POPS);
#include "neverd/evm/bytecode/EVMOpcodes.def"
  return Maximum;
}();

[[nodiscard]] constexpr uint8_t opcodeByte(Opcode Op) {
  return static_cast<uint8_t>(Op);
}

[[nodiscard]] constexpr bool isValidHardfork(Hardfork Fork) {
  switch (Fork) {
#define EVM_HARDFORK(NAME, SPELLING)                                           \
  case Hardfork::NAME:                                                         \
    return true;
#include "neverd/evm/bytecode/EVMHardforks.def"
  }
  return false;
}

[[nodiscard]] constexpr bool hardforkAtLeast(Hardfork Fork,
                                             Hardfork Introduced) {
  return isValidHardfork(Fork) && isValidHardfork(Introduced) &&
         static_cast<uint8_t>(Fork) >= static_cast<uint8_t>(Introduced);
}

[[nodiscard]] constexpr bool isPush(Opcode Op) {
  return opcodeByte(Op) >= opcodeByte(Opcode::PUSH0) &&
         opcodeByte(Op) <= opcodeByte(Opcode::PUSH32);
}

[[nodiscard]] constexpr uint8_t pushDataSize(Opcode Op) {
  return isPush(Op) ? opcodeByte(Op) - opcodeByte(Opcode::PUSH0) : 0;
}

[[nodiscard]] constexpr bool isDup(Opcode Op) {
  return opcodeByte(Op) >= opcodeByte(Opcode::DUP1) &&
         opcodeByte(Op) <= opcodeByte(Opcode::DUP16);
}

[[nodiscard]] constexpr uint8_t dupDepth(Opcode Op) {
  return isDup(Op) ? opcodeByte(Op) - opcodeByte(Opcode::DUP1) + 1 : 0;
}

[[nodiscard]] constexpr bool isSwap(Opcode Op) {
  return opcodeByte(Op) >= opcodeByte(Opcode::SWAP1) &&
         opcodeByte(Op) <= opcodeByte(Opcode::SWAP16);
}

[[nodiscard]] constexpr uint8_t swapDepth(Opcode Op) {
  return isSwap(Op) ? opcodeByte(Op) - opcodeByte(Opcode::SWAP1) + 1 : 0;
}

[[nodiscard]] constexpr bool isLog(Opcode Op) {
  return opcodeByte(Op) >= opcodeByte(Opcode::LOG0) &&
         opcodeByte(Op) <= opcodeByte(Opcode::LOG4);
}

[[nodiscard]] constexpr uint8_t logTopicCount(Opcode Op) {
  return isLog(Op) ? opcodeByte(Op) - opcodeByte(Opcode::LOG0) : 0;
}

[[nodiscard]] constexpr bool isJump(Opcode Op) {
  return Op == Opcode::JUMP || Op == Opcode::JUMPI;
}

[[nodiscard]] constexpr bool isDeepDup(Opcode Op) { return Op == Opcode::DUPN; }

[[nodiscard]] constexpr bool isDeepSwap(Opcode Op) {
  return Op == Opcode::SWAPN;
}

[[nodiscard]] constexpr bool isExchange(Opcode Op) {
  return Op == Opcode::EXCHANGE;
}

[[nodiscard]] constexpr std::optional<uint16_t>
decodeEIP8024Single(uint8_t Encoded) {
  if (Encoded >= kEIP8024SingleForbiddenFirst &&
      Encoded <= kEIP8024SingleForbiddenLast)
    return std::nullopt;
  const auto Depth = static_cast<uint16_t>(
      (static_cast<unsigned>(Encoded) + kEIP8024SingleDecodeBias) & kByteMax);
  if (Depth < kEIP8024MinimumSingleDepth || Depth > kEIP8024MaximumSingleDepth)
    return std::nullopt;
  return Depth;
}

[[nodiscard]] constexpr std::optional<StackDepthPair>
decodeEIP8024Pair(uint8_t Encoded) {
  if (Encoded >= kEIP8024PairForbiddenFirst &&
      Encoded <= kEIP8024PairForbiddenLast)
    return std::nullopt;
  const unsigned Grid = Encoded ^ kEIP8024PairXorMask;
  const unsigned Row = Grid >> kEIP8024PairGridBits;
  const unsigned Column = Grid & kEIP8024PairGridMask;
  StackDepthPair Result =
      Row < Column ? StackDepthPair{static_cast<uint16_t>(Row + 1),
                                    static_cast<uint16_t>(Column + 1)}
                   : StackDepthPair{static_cast<uint16_t>(Column + 1),
                                    static_cast<uint16_t>(
                                        kEIP8024PairLowerTriangleSum - Row)};
  if (Result.First == 0 || Result.First >= Result.Second ||
      Result.Second > kEIP8024MaximumPairDepth ||
      Result.First + Result.Second > kEIP8024PairLowerTriangleSum + 1)
    return std::nullopt;
  return Result;
}

[[nodiscard]] constexpr uint8_t maxOpcodeStackPops() {
  return kMaxOpcodeStackPops;
}

[[nodiscard]] constexpr uint16_t maxInstructionStackHeight() {
  return kMaximumInstructionStackHeight;
}

[[nodiscard]] constexpr uint8_t maxHostOpcodeArguments() {
  return kMaxHostOpcodeArguments;
}

/// Returns canonical metadata for an assigned byte without applying hardfork
/// activation. Decoders use this to preserve instruction width and identity
/// for relaxed analysis of future/inactive opcodes.
[[nodiscard]] std::optional<OpcodeInfo> assignedOpcodeInfo(Opcode Op);
[[nodiscard]] std::optional<OpcodeInfo> assignedOpcodeInfo(uint8_t Byte);
[[nodiscard]] std::optional<OpcodeInfo>
opcodeInfo(Opcode Op, Hardfork Fork = Hardfork::Latest);
/// Returns no value for an unassigned byte or an opcode inactive at \p Fork.
[[nodiscard]] std::optional<OpcodeInfo>
opcodeInfo(uint8_t Byte, Hardfork Fork = Hardfork::Latest);
/// Builds conservative faulting metadata for an unassigned byte. Assigned but
/// fork-inactive bytes retain their canonical database record in the decoder.
[[nodiscard]] OpcodeInfo unknownOpcodeInfo(uint8_t Byte);
[[nodiscard]] llvm::StringRef opcodeName(Opcode Op,
                                         Hardfork Fork = Hardfork::Latest);
[[nodiscard]] llvm::StringRef opcodeName(uint8_t Byte,
                                         Hardfork Fork = Hardfork::Latest);
[[nodiscard]] std::optional<Hardfork> parseHardfork(llvm::StringRef Name);
[[nodiscard]] llvm::StringRef hardforkName(Hardfork Fork);
[[nodiscard]] llvm::StringRef immediateKindName(ImmediateKind Kind);
[[nodiscard]] llvm::StringRef effectName(EffectKind Effect);
[[nodiscard]] llvm::StringRef memoryAccessName(MemoryAccessKind Access);
[[nodiscard]] llvm::StringRef stateAccessName(StateAccessKind Access);
[[nodiscard]] llvm::StringRef callValueAccessName(CallValueAccessKind Access);

} // namespace neverd::evm

#endif // NEVERD_EVM_BYTECODE_EVMOPCODES_H
