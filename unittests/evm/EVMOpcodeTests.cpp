//===- EVMOpcodeTests.cpp - EVM opcode metadata tests --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/Opcodes.h"

#include <type_traits>

namespace neverd::evm {
namespace {

static_assert(!std::is_default_constructible_v<OpcodeInfo>);

TEST(EVMOpcodeMetadata, FrontierAddHasExactStackContract) {
  const auto Info = opcodeInfo(Opcode::ADD, Hardfork::Frontier);
  ASSERT_TRUE(Info.has_value());
  EXPECT_EQ(Info->Name, "ADD");
  EXPECT_EQ(Info->StackInputs, 2);
  EXPECT_EQ(Info->StackOutputs, 1);
  EXPECT_FALSE(Info->IsTerminator);
  EXPECT_EQ(Info->StateAccess, StateAccessKind::None);
  EXPECT_EQ(Info->CallValueAccess, CallValueAccessKind::None);
  EXPECT_EQ(Info->Effect, EffectKind::None);
  EXPECT_EQ(opcodeByte(Info->Op), 0x01u);
}

TEST(EVMOpcodeMetadata, LatestContainsTheCompleteAssignedLegacySet) {
  unsigned Assigned = 0;
  for (unsigned Byte = 0; Byte < kOpcodeSpaceSize; ++Byte)
    Assigned +=
        opcodeInfo(static_cast<uint8_t>(Byte), Hardfork::Fusaka).has_value();

  EXPECT_EQ(Assigned, 150u);

  struct ExpectedOpcode {
    Opcode Op;
    const char *Name;
    uint8_t StackInputs;
    uint8_t StackOutputs;
    bool IsTerminator;
  };
  constexpr ExpectedOpcode Expected[] = {
      {Opcode::STOP, "STOP", 0, 0, true},
      {Opcode::SIGNEXTEND, "SIGNEXTEND", 2, 1, false},
      {Opcode::CLZ, "CLZ", 1, 1, false},
      {Opcode::SHA3, "SHA3", 2, 1, false},
      {Opcode::CALLDATALOAD, "CALLDATALOAD", 1, 1, false},
      {Opcode::BLOBHASH, "BLOBHASH", 1, 1, false},
      {Opcode::BLOBBASEFEE, "BLOBBASEFEE", 0, 1, false},
      {Opcode::SLOAD, "SLOAD", 1, 1, false},
      {Opcode::SSTORE, "SSTORE", 2, 0, false},
      {Opcode::JUMPI, "JUMPI", 2, 0, true},
      {Opcode::TLOAD, "TLOAD", 1, 1, false},
      {Opcode::TSTORE, "TSTORE", 2, 0, false},
      {Opcode::MCOPY, "MCOPY", 3, 0, false},
      {Opcode::PUSH0, "PUSH0", 0, 1, false},
      {Opcode::PUSH32, "PUSH32", 0, 1, false},
      {Opcode::DUP16, "DUP16", 16, 17, false},
      {Opcode::SWAP16, "SWAP16", 17, 17, false},
      {Opcode::LOG4, "LOG4", 6, 0, false},
      {Opcode::CREATE2, "CREATE2", 4, 1, false},
      {Opcode::STATICCALL, "STATICCALL", 6, 1, false},
      {Opcode::REVERT, "REVERT", 2, 0, true},
      {Opcode::INVALID, "INVALID", 0, 0, true},
      {Opcode::SELFDESTRUCT, "SELFDESTRUCT", 1, 0, true},
  };

  for (const auto &Want : Expected) {
    const auto Info = opcodeInfo(Want.Op, Hardfork::Fusaka);
    ASSERT_TRUE(Info.has_value()) << "opcode 0x" << std::hex
                                  << static_cast<unsigned>(opcodeByte(Want.Op));
    EXPECT_EQ(Info->Name, llvm::StringRef(Want.Name));
    EXPECT_EQ(Info->StackInputs, Want.StackInputs) << Info->Name.str();
    EXPECT_EQ(Info->StackOutputs, Want.StackOutputs) << Info->Name.str();
    EXPECT_EQ(Info->IsTerminator, Want.IsTerminator) << Info->Name.str();
  }

  EXPECT_FALSE(opcodeInfo(0x0c, Hardfork::Fusaka).has_value());
  EXPECT_EQ(opcodeName(0x0c, Hardfork::Fusaka), "UNKNOWN");
}

TEST(EVMOpcodeMetadata, DefinitionDatabaseRoundTripsEveryAssignedByte) {
  unsigned Assigned = 0;
  for (unsigned Byte = 0; Byte < kOpcodeSpaceSize; ++Byte) {
    const auto Info = opcodeInfo(static_cast<uint8_t>(Byte), Hardfork::Latest);
    if (!Info)
      continue;
    ++Assigned;
    EXPECT_EQ(opcodeByte(Info->Op), Byte);
    EXPECT_FALSE(Info->Name.empty());
    EXPECT_EQ(opcodeName(Info->Op, Hardfork::Latest), Info->Name);
  }
  EXPECT_EQ(Assigned, kAssignedOpcodeCount);
  EXPECT_EQ(kAssignedOpcodeCount, 150u);
  EXPECT_EQ(maxOpcodeStackInputs(), 17u);
  EXPECT_EQ(maxHostOpcodeArguments(), 7u);
}

TEST(EVMOpcodeMetadata, UnknownMetadataPreservesTheRawByteAndFaultPolicy) {
  const OpcodeInfo Info = unknownOpcodeInfo(0x0c);
  EXPECT_EQ(opcodeByte(Info.Op), 0x0c);
  EXPECT_FALSE(Info.isKnown());
  EXPECT_EQ(Info.Name, kUnknownOpcodeName);
  EXPECT_EQ(Info.Effect, EffectKind::Unknown);
  EXPECT_EQ(Info.MemoryAccess, MemoryAccessKind::Unknown);
  EXPECT_EQ(Info.StateAccess, StateAccessKind::Unknown);
  EXPECT_EQ(Info.CallValueAccess, CallValueAccessKind::Unknown);
  EXPECT_TRUE(mayReadMemory(Info));
  EXPECT_TRUE(mayWriteMemory(Info));
  EXPECT_TRUE(Info.IsTerminator);
}

TEST(EVMOpcodeMetadata, OpcodeFamiliesExposeWidthsAndDepthsWithoutMagicRanges) {
  EXPECT_TRUE(isPush(Opcode::PUSH0));
  EXPECT_TRUE(isPush(Opcode::PUSH32));
  EXPECT_FALSE(isPush(Opcode::DUP1));
  EXPECT_EQ(pushDataSize(Opcode::PUSH0), 0u);
  EXPECT_EQ(pushDataSize(Opcode::PUSH1), 1u);
  EXPECT_EQ(pushDataSize(Opcode::PUSH32), kWordBytes);

  EXPECT_EQ(dupDepth(Opcode::DUP1), 1u);
  EXPECT_EQ(dupDepth(Opcode::DUP16), 16u);
  EXPECT_EQ(dupDepth(Opcode::ADD), 0u);
  EXPECT_EQ(swapDepth(Opcode::SWAP1), 1u);
  EXPECT_EQ(swapDepth(Opcode::SWAP16), 16u);
  EXPECT_EQ(logTopicCount(Opcode::LOG0), 0u);
  EXPECT_EQ(logTopicCount(Opcode::LOG4), 4u);
  EXPECT_TRUE(isJump(Opcode::JUMP));
  EXPECT_TRUE(isJump(Opcode::JUMPI));
  EXPECT_FALSE(isJump(Opcode::JUMPDEST));
}

TEST(EVMOpcodeMetadata, EffectsAndSourceConstraintsComeFromTheDatabase) {
  EXPECT_EQ(opcodeInfo(Opcode::MLOAD)->Effect, EffectKind::None);
  EXPECT_EQ(opcodeInfo(Opcode::CALLDATACOPY)->Effect, EffectKind::ContextRead);
  EXPECT_EQ(opcodeInfo(Opcode::CALLVALUE)->Effect, EffectKind::ContextRead);
  EXPECT_EQ(opcodeInfo(Opcode::SSTORE)->Effect, EffectKind::StorageWrite);
  EXPECT_EQ(opcodeInfo(Opcode::STATICCALL)->Effect, EffectKind::ExternalCall);
  EXPECT_EQ(opcodeInfo(Opcode::LOG4)->Effect, EffectKind::Log);
  EXPECT_EQ(opcodeInfo(Opcode::SELFDESTRUCT)->Effect, EffectKind::SelfDestruct);
  EXPECT_EQ(effectName(EffectKind::None), "none");
  EXPECT_EQ(effectName(EffectKind::SelfDestruct), "selfdestruct");
  EXPECT_EQ(effectName(EffectKind::TransientWrite), "transient.write");
  EXPECT_EQ(effectName(EffectKind::ContextRead), "context.read");

  EXPECT_EQ(opcodeInfo(Opcode::SHA3)->StateAccess, StateAccessKind::None);
  EXPECT_EQ(opcodeInfo(Opcode::ADDRESS)->StateAccess, StateAccessKind::Read);
  EXPECT_EQ(opcodeInfo(Opcode::CALLVALUE)->StateAccess, StateAccessKind::Read);
  EXPECT_EQ(opcodeInfo(Opcode::STATICCALL)->StateAccess, StateAccessKind::Read);
  EXPECT_EQ(opcodeInfo(Opcode::CALL)->StateAccess, StateAccessKind::Write);
  EXPECT_EQ(stateAccessName(StateAccessKind::Write), "state.write");

  EXPECT_EQ(opcodeInfo(Opcode::CALLVALUE)->CallValueAccess,
            CallValueAccessKind::Read);
  EXPECT_EQ(opcodeInfo(Opcode::ADDRESS)->CallValueAccess,
            CallValueAccessKind::None);
  EXPECT_EQ(callValueAccessName(CallValueAccessKind::Read), "callvalue.read");

  EXPECT_TRUE(isALU(*opcodeInfo(Opcode::ADD)));
  EXPECT_TRUE(isALU(*opcodeInfo(Opcode::CLZ)));
  EXPECT_FALSE(isALU(*opcodeInfo(Opcode::SHA3)));
  EXPECT_FALSE(isALU(unknownOpcodeInfo(0x0c)));
}

TEST(EVMOpcodeMetadata, MemoryAccessesDescribeCompoundInstructionBehavior) {
  EXPECT_EQ(memoryAccess(Opcode::ADD), MemoryAccessKind::None);
  EXPECT_EQ(memoryAccess(static_cast<Opcode>(0x0c)), MemoryAccessKind::Unknown);
  EXPECT_EQ(memoryAccess(Opcode::SHA3), MemoryAccessKind::Read);
  EXPECT_EQ(memoryAccess(Opcode::MSTORE8), MemoryAccessKind::Write);
  EXPECT_EQ(memoryAccess(Opcode::MCOPY), MemoryAccessKind::ReadWrite);

  EXPECT_TRUE(mayReadMemory(*opcodeInfo(Opcode::SHA3)));
  EXPECT_FALSE(mayWriteMemory(*opcodeInfo(Opcode::SHA3)));
  EXPECT_TRUE(mayWriteMemory(*opcodeInfo(Opcode::EXTCODECOPY)));
  EXPECT_TRUE(mayReadMemory(*opcodeInfo(Opcode::CALL)));
  EXPECT_TRUE(mayWriteMemory(*opcodeInfo(Opcode::CALL)));
  EXPECT_FALSE(mayReadMemory(*opcodeInfo(Opcode::ADD)));
  EXPECT_FALSE(mayWriteMemory(*opcodeInfo(Opcode::ADD)));
  EXPECT_EQ(opcodeInfo(Opcode::CALL)->MemoryAccess,
            MemoryAccessKind::ReadWrite);
  EXPECT_EQ(memoryAccessName(MemoryAccessKind::ReadWrite), "memory.readwrite");
}

TEST(EVMOpcodeMetadata, HardforkActivationIsExplicitAndNameAware) {
  EXPECT_FALSE(opcodeInfo(0x5f, Hardfork::London).has_value());
  EXPECT_TRUE(opcodeInfo(0x5f, Hardfork::Shanghai).has_value());
  EXPECT_FALSE(opcodeInfo(0x5c, Hardfork::Shanghai).has_value());
  EXPECT_TRUE(opcodeInfo(0x5c, Hardfork::Cancun).has_value());
  EXPECT_FALSE(opcodeInfo(0x1e, Hardfork::Pectra).has_value());
  EXPECT_TRUE(opcodeInfo(0x1e, Hardfork::Fusaka).has_value());

  EXPECT_EQ(opcodeName(0x44, Hardfork::London), "DIFFICULTY");
  EXPECT_EQ(opcodeName(0x44, Hardfork::Paris), "PREVRANDAO");
  EXPECT_EQ(parseHardfork("CaNcUn"), Hardfork::Cancun);
  EXPECT_EQ(parseHardfork("berlin"), Hardfork::Berlin);
  EXPECT_EQ(parseHardfork("merge"), Hardfork::Paris);
  EXPECT_EQ(parseHardfork("prague"), Hardfork::Pectra);
  EXPECT_EQ(parseHardfork("osaka"), Hardfork::Fusaka);
  EXPECT_EQ(hardforkName(Hardfork::Latest), "fusaka");
  EXPECT_FALSE(parseHardfork("future-fork").has_value());

  const auto InvalidFork = static_cast<Hardfork>(kByteMax);
  EXPECT_FALSE(isValidHardfork(InvalidFork));
  EXPECT_FALSE(opcodeInfo(Opcode::ADD, InvalidFork).has_value());
  EXPECT_EQ(hardforkName(InvalidFork), kUnknownName);
}

} // namespace
} // namespace neverd::evm
