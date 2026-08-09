//===- Opcodes.h - Ethereum Virtual Machine opcode metadata -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_OPCODES_H
#define NEVERD_EVM_OPCODES_H

#include "neverd/evm/EVMConstants.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::evm {

enum class Hardfork : uint8_t {
#define EVM_HARDFORK(NAME, SPELLING) NAME,
#include "neverd/evm/EVMHardforks.def"
  Latest = Fusaka,
};

enum class EffectKind : uint8_t {
#define EVM_EFFECT(NAME, SPELLING) NAME,
#include "neverd/evm/EVMEffects.def"
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
#define EVM_OPCODE(NAME, BYTE, INPUTS, OUTPUTS, IMMEDIATE_BYTES, CLASS,         \
                   INTRODUCED, EFFECT, TERMINATOR, VIEW, PURE)                 \
  NAME = BYTE,
#include "neverd/evm/EVMOpcodes.def"
};

struct OpcodeInfo {
  Opcode Op = Opcode::STOP;
  llvm::StringRef Name = kUnknownOpcodeName;
  uint8_t StackInputs = 0;
  uint8_t StackOutputs = 0;
  uint8_t ImmediateBytes = 0;
  OpcodeClass Class = OpcodeClass::Unknown;
  Hardfork Introduced = Hardfork::Frontier;
  EffectKind Effect = EffectKind::Unknown;
  bool IsTerminator = false;
  bool IsView = true;
  bool IsPure = true;
};

inline constexpr unsigned kAssignedOpcodeCount =
#define EVM_OPCODE(NAME, BYTE, INPUTS, OUTPUTS, IMMEDIATE_BYTES, CLASS,         \
                   INTRODUCED, EFFECT, TERMINATOR, VIEW, PURE)                 \
  1U +
#include "neverd/evm/EVMOpcodes.def"
    0U;

inline constexpr uint8_t kMaxOpcodeStackInputs = [] {
  uint8_t Maximum = 0;
#define EVM_OPCODE(NAME, BYTE, INPUTS, OUTPUTS, IMMEDIATE_BYTES, CLASS,         \
                   INTRODUCED, EFFECT, TERMINATOR, VIEW, PURE)                 \
  if (INPUTS > Maximum)                                                        \
    Maximum = INPUTS;
#include "neverd/evm/EVMOpcodes.def"
  return Maximum;
}();

inline constexpr uint8_t kMaxHostOpcodeArguments = [] {
  uint8_t Maximum = 0;
#define EVM_OPCODE(NAME, BYTE, INPUTS, OUTPUTS, IMMEDIATE_BYTES, CLASS,         \
                   INTRODUCED, EFFECT, TERMINATOR, VIEW, PURE)                 \
  if (OpcodeClass::CLASS != OpcodeClass::Stack && INPUTS > Maximum)            \
    Maximum = INPUTS;
#include "neverd/evm/EVMOpcodes.def"
  return Maximum;
}();

[[nodiscard]] constexpr uint8_t opcodeByte(Opcode Op) {
  return static_cast<uint8_t>(Op);
}

[[nodiscard]] constexpr bool hardforkAtLeast(Hardfork Fork,
                                              Hardfork Introduced) {
  return static_cast<uint8_t>(Fork) >= static_cast<uint8_t>(Introduced);
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

[[nodiscard]] constexpr uint8_t maxOpcodeStackInputs() {
  return kMaxOpcodeStackInputs;
}

[[nodiscard]] constexpr uint8_t maxHostOpcodeArguments() {
  return kMaxHostOpcodeArguments;
}

[[nodiscard]] std::optional<OpcodeInfo>
opcodeInfo(Opcode Op, Hardfork Fork = Hardfork::Latest);
[[nodiscard]] std::optional<OpcodeInfo>
opcodeInfo(uint8_t Byte, Hardfork Fork = Hardfork::Latest);
[[nodiscard]] llvm::StringRef opcodeName(Opcode Op,
                                         Hardfork Fork = Hardfork::Latest);
[[nodiscard]] llvm::StringRef opcodeName(uint8_t Byte,
                                         Hardfork Fork = Hardfork::Latest);
[[nodiscard]] std::optional<Hardfork> parseHardfork(llvm::StringRef Name);
[[nodiscard]] llvm::StringRef hardforkName(Hardfork Fork);
[[nodiscard]] llvm::StringRef effectName(EffectKind Effect);

} // namespace neverd::evm

#endif // NEVERD_EVM_OPCODES_H
