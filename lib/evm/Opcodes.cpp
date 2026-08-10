//===- Opcodes.cpp - Ethereum Virtual Machine opcode metadata -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Opcodes.h"

#include "llvm/ADT/StringSwitch.h"

#include <array>
#include <optional>
#include <string>

namespace neverd::evm {
namespace {

using OpcodeTable = std::array<std::optional<OpcodeInfo>, kOpcodeSpaceSize>;
inline constexpr uint8_t kLogDataStackInputs = 2;

constexpr OpcodeTable buildOpcodeTable() {
  OpcodeTable Result{};
#define EVM_OPCODE(NAME, BYTE, INPUTS, OUTPUTS, IMMEDIATE_BYTES, CLASS,        \
                   INTRODUCED, EFFECT, MEMORY_ACCESS, STATE_ACCESS,            \
                   CALL_VALUE_ACCESS, TERMINATOR)                              \
  Result[BYTE] = OpcodeInfo{Opcode::NAME,                                      \
                            llvm::StringLiteral(#NAME),                        \
                            static_cast<uint8_t>(INPUTS),                      \
                            static_cast<uint8_t>(OUTPUTS),                     \
                            static_cast<uint8_t>(IMMEDIATE_BYTES),             \
                            OpcodeClass::CLASS,                                \
                            Hardfork::INTRODUCED,                              \
                            EffectKind::EFFECT,                                \
                            MemoryAccessKind::MEMORY_ACCESS,                   \
                            StateAccessKind::STATE_ACCESS,                     \
                            CallValueAccessKind::CALL_VALUE_ACCESS,            \
                            TERMINATOR};
#include "neverd/evm/EVMOpcodes.def"
  return Result;
}

inline constexpr OpcodeTable OpcodeTableInstance = buildOpcodeTable();

constexpr bool stateAccessMatchesEffect(const OpcodeInfo &Info) {
  switch (Info.Effect) {
  case EffectKind::None:
  case EffectKind::Return:
  case EffectKind::Revert:
  case EffectKind::Control:
  case EffectKind::Halt:
    return Info.StateAccess == StateAccessKind::None;
  case EffectKind::StorageRead:
  case EffectKind::TransientRead:
    return Info.StateAccess == StateAccessKind::Read;
  case EffectKind::StorageWrite:
  case EffectKind::TransientWrite:
  case EffectKind::Create:
  case EffectKind::Log:
  case EffectKind::SelfDestruct:
    return Info.StateAccess == StateAccessKind::Write;
  case EffectKind::ExternalCall:
    return Info.StateAccess == (Info.Op == Opcode::STATICCALL
                                    ? StateAccessKind::Read
                                    : StateAccessKind::Write);
  case EffectKind::ContextRead:
    return Info.StateAccess == StateAccessKind::None ||
           Info.StateAccess == StateAccessKind::Read;
  case EffectKind::Unknown:
    return false;
  }
  return false;
}

constexpr bool validateOpcodeTable() {
  unsigned Assigned = 0;
  for (std::size_t Byte = 0; Byte < OpcodeTableInstance.size(); ++Byte) {
    const auto &Info = OpcodeTableInstance[Byte];
    if (!Info)
      continue;
    ++Assigned;
    if (opcodeByte(Info->Op) != Byte || Info->Name.empty() ||
        Info->ImmediateBytes > kWordBytes ||
        !hardforkAtLeast(Hardfork::Latest, Info->Introduced) ||
        Info->Class == OpcodeClass::Unknown ||
        Info->Effect == EffectKind::Unknown ||
        Info->MemoryAccess == MemoryAccessKind::Unknown ||
        Info->MemoryAccess != memoryAccess(Info->Op) ||
        Info->StateAccess == StateAccessKind::Unknown ||
        Info->CallValueAccess == CallValueAccessKind::Unknown ||
        !stateAccessMatchesEffect(*Info))
      return false;
    if ((Info->CallValueAccess == CallValueAccessKind::Read) !=
        (Info->Op == Opcode::CALLVALUE))
      return false;
    if (isALU(*Info) &&
        (Info->StackInputs == 0 || Info->StackOutputs != 1 ||
         Info->ImmediateBytes != 0 || Info->Effect != EffectKind::None ||
         Info->MemoryAccess != MemoryAccessKind::None || Info->IsTerminator ||
         Info->StateAccess != StateAccessKind::None))
      return false;
    // The shared host ABI returns one word. Stack-family instructions are
    // lowered directly and may have wider logical stack contracts (DUP16 is
    // the current maximum); every other opcode must fit the host result ABI.
    if (Info->Class != OpcodeClass::Stack && Info->StackOutputs > 1)
      return false;
    // Adding a new stack-family encoding requires an explicit family helper
    // and backend lowering; do not let it silently fall through to a host ABI
    // whose argument bound deliberately excludes stack-only operations.
    if (Info->Class == OpcodeClass::Stack && !isPush(Info->Op) &&
        !isDup(Info->Op) && !isSwap(Info->Op) && Info->Op != Opcode::POP)
      return false;
    if (isPush(Info->Op) &&
        (Info->ImmediateBytes != pushDataSize(Info->Op) ||
         Info->StackInputs != 0 || Info->StackOutputs != 1 ||
         Info->Class != OpcodeClass::Stack))
      return false;
    if (isDup(Info->Op) && (Info->StackInputs != dupDepth(Info->Op) ||
                            Info->StackOutputs != dupDepth(Info->Op) + 1 ||
                            Info->Class != OpcodeClass::Stack))
      return false;
    if (isSwap(Info->Op) && (Info->StackInputs != swapDepth(Info->Op) + 1 ||
                             Info->StackOutputs != Info->StackInputs ||
                             Info->Class != OpcodeClass::Stack))
      return false;
    if (isLog(Info->Op) &&
        (Info->StackInputs != logTopicCount(Info->Op) + kLogDataStackInputs ||
         Info->StackOutputs != 0 || Info->Class != OpcodeClass::Log))
      return false;
    const bool EffectTerminates = Info->Effect == EffectKind::Halt ||
                                  Info->Effect == EffectKind::Return ||
                                  Info->Effect == EffectKind::Revert ||
                                  Info->Effect == EffectKind::SelfDestruct;
    if (Info->IsTerminator != (isJump(Info->Op) || EffectTerminates))
      return false;
  }
  return Assigned == kAssignedOpcodeCount;
}

static_assert(validateOpcodeTable(),
              "EVMOpcodes.def contains invalid or duplicate opcode metadata");
static_assert(kMaxOpcodeStackInputs <= kStackLimit,
              "an opcode cannot consume more than the EVM stack limit");
static_assert(kMaxALUStackInputs > 0 &&
                  kMaxALUStackInputs <= kMaxHostOpcodeArguments,
              "scalar ALU inputs must fit the shared host argument bound");

} // namespace

std::optional<OpcodeInfo> assignedOpcodeInfo(Opcode Op) {
  const auto &Info = OpcodeTableInstance[opcodeByte(Op)];
  return Info;
}

std::optional<OpcodeInfo> assignedOpcodeInfo(uint8_t Byte) {
  return assignedOpcodeInfo(static_cast<Opcode>(Byte));
}

std::optional<OpcodeInfo> opcodeInfo(Opcode Op, Hardfork Fork) {
  const auto Info = assignedOpcodeInfo(Op);
  if (!Info || !hardforkAtLeast(Fork, Info->Introduced))
    return std::nullopt;

  OpcodeInfo Result = *Info;
#define EVM_OPCODE_NAME_ALIAS(OPCODE, ALIAS, FIRST_FORK, LAST_FORK_EXCLUSIVE)  \
  if (Op == Opcode::OPCODE && hardforkAtLeast(Fork, Hardfork::FIRST_FORK) &&   \
      !hardforkAtLeast(Fork, Hardfork::LAST_FORK_EXCLUSIVE))                   \
    Result.Name = llvm::StringLiteral(#ALIAS);
#include "neverd/evm/EVMOpcodes.def"
  return Result;
}

std::optional<OpcodeInfo> opcodeInfo(uint8_t Byte, Hardfork Fork) {
  return opcodeInfo(static_cast<Opcode>(Byte), Fork);
}

OpcodeInfo unknownOpcodeInfo(uint8_t Byte) {
  return OpcodeInfo{
      static_cast<Opcode>(Byte),
      kUnknownOpcodeName,
      0,
      0,
      0,
      OpcodeClass::Unknown,
      Hardfork::Frontier,
      EffectKind::Unknown,
      MemoryAccessKind::Unknown,
      StateAccessKind::Unknown,
      CallValueAccessKind::Unknown,
      true,
  };
}

llvm::StringRef opcodeName(Opcode Op, Hardfork Fork) {
  const auto Info = opcodeInfo(Op, Fork);
  return Info ? Info->Name : kUnknownOpcodeName;
}

llvm::StringRef opcodeName(uint8_t Byte, Hardfork Fork) {
  return opcodeName(static_cast<Opcode>(Byte), Fork);
}

std::optional<Hardfork> parseHardfork(llvm::StringRef Name) {
  const std::string Lower = Name.lower();
  return llvm::StringSwitch<std::optional<Hardfork>>(Lower)
#define EVM_HARDFORK(NAME, SPELLING) .Case(SPELLING, Hardfork::NAME)
#define EVM_HARDFORK_ALIAS(SPELLING, NAME) .Case(SPELLING, Hardfork::NAME)
#define EVM_HARDFORK_LATEST(NAME, SPELLING) .Case(SPELLING, Hardfork::NAME)
#include "neverd/evm/EVMHardforks.def"
      .Default(std::nullopt);
}

llvm::StringRef hardforkName(Hardfork Fork) {
  switch (Fork) {
#define EVM_HARDFORK(NAME, SPELLING)                                           \
  case Hardfork::NAME:                                                         \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMHardforks.def"
  }
  return kUnknownName;
}

llvm::StringRef effectName(EffectKind Effect) {
  switch (Effect) {
#define EVM_EFFECT(NAME, SPELLING)                                             \
  case EffectKind::NAME:                                                       \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMEffects.def"
  }
  return kUnknownName;
}

llvm::StringRef memoryAccessName(MemoryAccessKind Access) {
  switch (Access) {
#define EVM_MEMORY_ACCESS_KIND(NAME, SPELLING)                                 \
  case MemoryAccessKind::NAME:                                                 \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMMemoryAccesses.def"
  }
  return kUnknownName;
}

llvm::StringRef stateAccessName(StateAccessKind Access) {
  switch (Access) {
#define EVM_STATE_ACCESS_KIND(NAME, SPELLING)                                  \
  case StateAccessKind::NAME:                                                  \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMStateAccesses.def"
  }
  return kUnknownName;
}

llvm::StringRef callValueAccessName(CallValueAccessKind Access) {
  switch (Access) {
#define EVM_CALL_VALUE_ACCESS_KIND(NAME, SPELLING)                             \
  case CallValueAccessKind::NAME:                                              \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMCallValueAccesses.def"
  }
  return kUnknownName;
}

} // namespace neverd::evm
