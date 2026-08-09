//===- Opcodes.cpp - Ethereum Virtual Machine opcode metadata -----------===//

#include "neverd/evm/Opcodes.h"

#include <array>
#include <optional>

namespace neverd::evm {
namespace {

using OpcodeTable =
    std::array<std::optional<OpcodeInfo>, kOpcodeSpaceSize>;

constexpr OpcodeTable buildOpcodeTable() {
  OpcodeTable Result{};
#define EVM_OPCODE(NAME, BYTE, INPUTS, OUTPUTS, IMMEDIATE_BYTES, CLASS,         \
                   INTRODUCED, EFFECT, TERMINATOR, VIEW, PURE)                 \
  Result[BYTE] = OpcodeInfo{                                                   \
      Opcode::NAME, llvm::StringLiteral(#NAME), static_cast<uint8_t>(INPUTS),  \
      static_cast<uint8_t>(OUTPUTS), static_cast<uint8_t>(IMMEDIATE_BYTES),    \
      OpcodeClass::CLASS, Hardfork::INTRODUCED, EffectKind::EFFECT,            \
      TERMINATOR, VIEW, PURE};
#include "neverd/evm/EVMOpcodes.def"
  return Result;
}

inline constexpr OpcodeTable OpcodeTableInstance = buildOpcodeTable();

constexpr bool validateOpcodeTable() {
  unsigned Assigned = 0;
  for (std::size_t Byte = 0; Byte < OpcodeTableInstance.size(); ++Byte) {
    const auto &Info = OpcodeTableInstance[Byte];
    if (!Info)
      continue;
    ++Assigned;
    if (opcodeByte(Info->Op) != Byte || Info->Name.empty() ||
        Info->ImmediateBytes > kWordBytes)
      return false;
  }
  return Assigned == kAssignedOpcodeCount;
}

static_assert(validateOpcodeTable(),
              "EVMOpcodes.def contains invalid or duplicate opcode metadata");
static_assert(kMaxOpcodeStackInputs <= kStackLimit,
              "an opcode cannot consume more than the EVM stack limit");

} // namespace

std::optional<OpcodeInfo> opcodeInfo(Opcode Op, Hardfork Fork) {
  const auto &Info = OpcodeTableInstance[opcodeByte(Op)];
  if (!Info || !hardforkAtLeast(Fork, Info->Introduced))
    return std::nullopt;

  OpcodeInfo Result = *Info;
#define EVM_OPCODE_NAME_ALIAS(OPCODE, ALIAS, FIRST_FORK, LAST_FORK_EXCLUSIVE)   \
  if (Op == Opcode::OPCODE && hardforkAtLeast(Fork, Hardfork::FIRST_FORK) &&   \
      !hardforkAtLeast(Fork, Hardfork::LAST_FORK_EXCLUSIVE))                   \
    Result.Name = llvm::StringLiteral(#ALIAS);
#include "neverd/evm/EVMOpcodes.def"
  return Result;
}

std::optional<OpcodeInfo> opcodeInfo(uint8_t Byte, Hardfork Fork) {
  return opcodeInfo(static_cast<Opcode>(Byte), Fork);
}

llvm::StringRef opcodeName(Opcode Op, Hardfork Fork) {
  const auto Info = opcodeInfo(Op, Fork);
  return Info ? Info->Name : kUnknownOpcodeName;
}

llvm::StringRef opcodeName(uint8_t Byte, Hardfork Fork) {
  return opcodeName(static_cast<Opcode>(Byte), Fork);
}

std::optional<Hardfork> parseHardfork(llvm::StringRef Name) {
#define EVM_HARDFORK(NAME, SPELLING)                                           \
  if (Name.equals_insensitive(SPELLING))                                      \
    return Hardfork::NAME;
#define EVM_HARDFORK_ALIAS(SPELLING, NAME)                                    \
  if (Name.equals_insensitive(SPELLING))                                      \
    return Hardfork::NAME;
#include "neverd/evm/EVMHardforks.def"
  return std::nullopt;
}

llvm::StringRef hardforkName(Hardfork Fork) {
  switch (Fork) {
#define EVM_HARDFORK(NAME, SPELLING)                                           \
  case Hardfork::NAME:                                                        \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMHardforks.def"
  }
  return kUnknownName;
}

llvm::StringRef effectName(EffectKind Effect) {
  switch (Effect) {
#define EVM_EFFECT(NAME, SPELLING)                                             \
  case EffectKind::NAME:                                                      \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMEffects.def"
  }
  return kUnknownName;
}

} // namespace neverd::evm
