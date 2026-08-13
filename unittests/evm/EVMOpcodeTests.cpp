//===- EVMOpcodeTests.cpp - EVM opcode metadata tests --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/evm/bytecode/EVMOpcodes.h"

#include <type_traits>

namespace neverd::evm {
namespace {

static_assert(!std::is_default_constructible_v<OpcodeInfo>);

TEST(EVMOpcodeMetadata, FrontierAddHasExactStackContract) {
  const auto Info = opcodeInfo(Opcode::ADD, Hardfork::Frontier);
  ASSERT_TRUE(Info.has_value());
  EXPECT_EQ(Info->Name, "ADD");
  EXPECT_EQ(Info->StackPops, 2);
  EXPECT_EQ(Info->StackPushes, 1);
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
    uint8_t StackPops;
    uint8_t StackPushes;
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
      {Opcode::DUP16, "DUP16", 0, 1, false},
      {Opcode::SWAP16, "SWAP16", 0, 0, false},
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
    EXPECT_EQ(Info->StackPops, Want.StackPops) << Info->Name.str();
    EXPECT_EQ(Info->StackPushes, Want.StackPushes) << Info->Name.str();
    EXPECT_EQ(Info->IsTerminator, Want.IsTerminator) << Info->Name.str();
  }

  EXPECT_FALSE(opcodeInfo(0x0c, Hardfork::Fusaka).has_value());
  EXPECT_EQ(opcodeName(0x0c, Hardfork::Fusaka), "UNKNOWN");
}

TEST(EVMOpcodeMetadata, DefinitionDatabaseRoundTripsEveryAssignedByte) {
  unsigned Assigned = 0;
  for (unsigned Byte = 0; Byte < kOpcodeSpaceSize; ++Byte) {
    const auto Info =
        opcodeInfo(static_cast<uint8_t>(Byte), kNewestKnownHardfork);
    if (!Info)
      continue;
    ++Assigned;
    EXPECT_EQ(opcodeByte(Info->Op), Byte);
    EXPECT_FALSE(Info->Name.empty());
    EXPECT_EQ(opcodeName(Info->Op, kNewestKnownHardfork), Info->Name);
  }
  EXPECT_EQ(Assigned, kAssignedOpcodeCount);
  EXPECT_EQ(kAssignedOpcodeCount, 154u);
  EXPECT_EQ(maxInstructionStackHeight(), 236u);
  EXPECT_EQ(maxHostOpcodeArguments(), 7u);
}

TEST(EVMOpcodeMetadata, AmsterdamOpcodesAreOptInAndFullyTyped) {
  EXPECT_EQ(kLatestStableHardfork, Hardfork::Fusaka);
  EXPECT_EQ(kNewestKnownHardfork, Hardfork::Bogota);

  const struct {
    Opcode Op;
    uint8_t Byte;
    llvm::StringLiteral Name;
    ImmediateKind Immediate;
  } Expected[] = {
      {Opcode::SLOTNUM, 0x4b, "SLOTNUM", ImmediateKind::None},
      {Opcode::DUPN, 0xe6, "DUPN", ImmediateKind::EIP8024Single},
      {Opcode::SWAPN, 0xe7, "SWAPN", ImmediateKind::EIP8024Single},
      {Opcode::EXCHANGE, 0xe8, "EXCHANGE", ImmediateKind::EIP8024Pair},
  };

  for (const auto &Want : Expected) {
    EXPECT_FALSE(opcodeInfo(Want.Op, Hardfork::Fusaka).has_value());
    const auto Info = opcodeInfo(Want.Op, Hardfork::Amsterdam);
    ASSERT_TRUE(Info.has_value());
    EXPECT_EQ(opcodeByte(Info->Op), Want.Byte);
    EXPECT_EQ(Info->Name, Want.Name);
    EXPECT_EQ(Info->Immediate, Want.Immediate);
  }

  unsigned StableAssigned = 0;
  unsigned AmsterdamAssigned = 0;
  for (unsigned Byte = 0; Byte < kOpcodeSpaceSize; ++Byte) {
    StableAssigned +=
        opcodeInfo(static_cast<uint8_t>(Byte), Hardfork::Latest).has_value();
    AmsterdamAssigned +=
        opcodeInfo(static_cast<uint8_t>(Byte), Hardfork::Amsterdam).has_value();
  }
  EXPECT_EQ(StableAssigned, 150u);
  EXPECT_EQ(AmsterdamAssigned, 154u);
}

TEST(EVMOpcodeMetadata, EIP8024ImmediateDecodingIsExhaustive) {
  unsigned ValidSingles = 0;
  unsigned ValidPairs = 0;
  for (unsigned Encoded = 0; Encoded <= kByteMax; ++Encoded) {
    const auto Single = decodeEIP8024Single(static_cast<uint8_t>(Encoded));
    if (Single) {
      ++ValidSingles;
      EXPECT_GE(*Single, 17u);
      EXPECT_LE(*Single, 235u);
    }

    const auto Pair = decodeEIP8024Pair(static_cast<uint8_t>(Encoded));
    if (Pair) {
      ++ValidPairs;
      EXPECT_GE(Pair->First, 1u);
      EXPECT_LT(Pair->First, Pair->Second);
      EXPECT_LE(Pair->First + Pair->Second, 30u);
    }
  }

  EXPECT_EQ(ValidSingles, 219u);
  EXPECT_EQ(ValidPairs, 210u);
  EXPECT_EQ(decodeEIP8024Single(0x80), 17u);
  EXPECT_EQ(decodeEIP8024Single(0xdb), 108u);
  EXPECT_FALSE(decodeEIP8024Single(0x5b).has_value());
  EXPECT_FALSE(decodeEIP8024Single(0x60).has_value());
  EXPECT_EQ(decodeEIP8024Pair(0x9d), (StackDepthPair{2, 3}));
  EXPECT_EQ(decodeEIP8024Pair(0x2f), (StackDepthPair{1, 19}));
  EXPECT_EQ(decodeEIP8024Pair(0x50), (StackDepthPair{14, 16}));
  EXPECT_EQ(decodeEIP8024Pair(0x51), (StackDepthPair{14, 15}));
  EXPECT_FALSE(decodeEIP8024Pair(0x52).has_value());
}

TEST(EVMOpcodeMetadata, UnknownMetadataPreservesTheRawByteAndFaultPolicy) {
  const OpcodeInfo Info = unknownOpcodeInfo(0x0c);
  EXPECT_EQ(opcodeByte(Info.Op), 0x0c);
  EXPECT_FALSE(Info.isAssigned());
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
  EXPECT_EQ(parseHardfork("amsterdam"), Hardfork::Amsterdam);
  EXPECT_EQ(parseHardfork("glamsterdam"), Hardfork::Amsterdam);
  EXPECT_EQ(parseHardfork("bogota"), Hardfork::Bogota);
  EXPECT_EQ(hardforkName(Hardfork::Latest), "fusaka");
  EXPECT_EQ(hardforkName(Hardfork::Amsterdam), "amsterdam");
  EXPECT_FALSE(parseHardfork("future-fork").has_value());

  const auto InvalidFork = static_cast<Hardfork>(kByteMax);
  EXPECT_FALSE(isValidHardfork(InvalidFork));
  EXPECT_FALSE(opcodeInfo(Opcode::ADD, InvalidFork).has_value());
  EXPECT_EQ(hardforkName(InvalidFork), kUnknownName);
}

} // namespace
} // namespace neverd::evm
