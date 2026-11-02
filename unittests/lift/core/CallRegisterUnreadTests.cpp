//===- CallRegisterUnreadTests.cpp - Callee input observation proofs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Distinguishes absent observations from argument and may-write heuristics.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/loader/BinaryImage.h"

#include <map>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr unsigned Scratch = 8;
constexpr GPRFamilyMask ScratchBit = GPRFamilyMask{1} << Scratch;

LocalRegisterEffect body(std::vector<RegisterStep> Steps) {
  LocalRegisterEffect Effect;
  Effect.Blocks.push_back({std::move(Steps), {}});
  return Effect;
}

bool unread(const std::map<va_t, GPRFamilyMask> &Proof, va_t Entry) {
  const auto It = Proof.find(Entry);
  return It != Proof.end() && (It->second & ScratchBit);
}

TEST(CallRegisterUnread, ReadsBeforeWritesAndIncompleteBodiesStayObserved) {
  RegisterStep Write, Read;
  Write.Kills = ScratchBit;
  Read.Reads[Scratch] = 8;
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Effect = body({Write, Read});
    if (Mode == 1)
      Effect = body({Read, Write});
    if (Mode == 2) {
      Effect.Blocks[0].Steps[0].Kills = 0;
      Effect.Blocks[0].Steps[0].LowWrites[Scratch] = 1;
    }
    if (Mode == 3)
      Effect.Incomplete = true;
    if (Mode == 4)
      Effect.Unknown = true;
    if (Mode == 5)
      Effect.VariadicFrom = 0;
    if (Mode == 6)
      Effect.Blocks.clear();
    if (Mode == 7)
      Effect.Blocks[0].Succs = {3};
    if (Mode == 8)
      Effect.UnknownObservers = true;
    EXPECT_EQ(unread(proveUnreadCallRegisters({{0x1000, &Effect}}), 0x1000),
              Mode == 0);
  }
}

TEST(CallRegisterUnread, ImplicitReturnAndStackChannelsSupplyNoProof) {
  auto Effect = body({});
  const auto Proof = proveUnreadCallRegisters({{0x1000, &Effect}});
  const auto &TRI = getTargetRegInfo(Arch::X64);
  for (uint64_t Register :
       {TRI.StackPointer, TRI.IntReturnReg, TRI.IntReturnReg2})
    if (const auto Family = gprFamilyOf(Arch::X64, Register))
      EXPECT_EQ(Proof.at(0x1000) & (GPRFamilyMask{1} << *Family), 0u);
}

TEST(CallRegisterUnread, BodyProjectionRetainsUnmodeledEntryObservers) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    SCOPED_TRACE(Mode);
    LowFunc Function;
    Function.Entry = 0x1000;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(Block);
    if (Mode == 1)
      Function.ModuleAnalysisRoots.insert(0x1001);
    if (Mode == 2)
      Function.Blocks[0].ExceptionalSuccs.push_back({-1, 0});
    if (Mode == 3)
      Function.Blocks[0].Succs.push_back(7);
    const auto Effect = localRegisterEffect(Image, Function);
    EXPECT_EQ(Effect.UnknownObservers, Mode != 0);
    EXPECT_EQ(unread(proveUnreadCallRegisters({{0x1000, &Effect}}), 0x1000),
              Mode == 0);
  }
}

TEST(CallRegisterUnread, OpaqueCallsDoNotAcquireAnInputContractFromHints) {
  RegisterStep Call, Write;
  Call.UnknownCall = true;
  Call.Exits = true;
  Write.Kills = ScratchBit;
  auto Leaf = body({});
  for (unsigned Mode = 0; Mode != 6; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Effect = body({Call});
    if (Mode == 0)
      Effect.Blocks[0].Steps.insert(Effect.Blocks[0].Steps.begin(), Write);
    if (Mode == 2)
      Effect.Blocks[0].Steps[0].ArgumentsFrom = 0x2000;
    if (Mode == 3) {
      Effect.Blocks[0].Steps[0].UnknownCall = false;
      Effect.Blocks[0].Steps[0].FixedArguments = GPRReadWidths{};
    }
    if (Mode == 4)
      Effect.Blocks[0].Steps[0].Kills = ScratchBit;
    if (Mode == 5) {
      Effect.Blocks[0].Steps[0].UnknownCall = false;
      Effect.Blocks[0].Steps[0].UnknownTailCall = true;
    }
    EXPECT_EQ(
        unread(proveUnreadCallRegisters({{0x1000, &Effect}, {0x2000, &Leaf}}),
               0x1000),
        Mode == 0);
  }
}

TEST(CallRegisterUnread, NestedMayWritesCannotKillLaterInputObservations) {
  RegisterStep Write, Call, Read;
  Write.Kills = ScratchBit;
  Call.Callee = 0x2000;
  Read.Reads[Scratch] = 8;
  auto Callee = body({});
  Callee.Writes = ScratchBit;
  Callee.Blocks[0].Succs = {1, 2};
  Callee.Blocks.push_back({{Write}, {}});
  Callee.Blocks.push_back({{}, {}});
  auto Caller = body({Call, Read});
  Caller.Callees.insert(Call.Callee);
  const auto Summaries = solveCallRegisterEffects(
      {{0x1000, Caller}, {0x2000, Callee}}, ScratchBit, ScratchBit);
  ASSERT_EQ(Summaries.EntryReads.at(0x1000)[Scratch], 0);
  EXPECT_FALSE(unread(Summaries.Unread, 0x1000));
  EXPECT_TRUE(unread(Summaries.Unread, 0x2000));
}

TEST(CallRegisterUnread, ExactEntryWritesSatisfyLaterCallerReads) {
  RegisterStep Write, Call, Read;
  Write.Kills = ScratchBit;
  Read.Reads[Scratch] = 8;
  Call.Callee = 0x2000;
  for (unsigned Mode = 0; Mode < 7; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Callee = body({Write});
    if (Mode == 1)
      Callee.Blocks[0].Steps = {Read, Write};
    if (Mode == 2) {
      Callee.Blocks[0].Steps[0].Kills = 0;
      Callee.Blocks[0].Steps[0].LowWrites[Scratch] = 1;
    }
    if (Mode == 3)
      Callee.Incomplete = true;
    if (Mode == 4)
      Callee.UnknownObservers = true;
    if (Mode == 5) {
      RegisterStep Unknown;
      Unknown.UnknownCall = true;
      Callee.Blocks[0].Steps = {Unknown, Write};
    }
    if (Mode == 6) {
      Callee.Blocks[0].Steps.clear();
      Callee.Writes = ScratchBit;
    }
    auto Caller = body({Call, Read});
    EXPECT_EQ(
        unread(proveUnreadCallRegisters({{0x1000, &Caller}, {0x2000, &Callee}}),
               0x1000),
        Mode == 0);
  }
}

TEST(CallRegisterUnread, ReturnFamiliesNeedAnExplicitEntryOverwrite) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  for (auto Register : {TRI.IntReturnReg, TRI.IntReturnReg2}) {
    const auto Family = gprFamilyOf(Arch::X64, Register);
    ASSERT_TRUE(Family);
    const auto Bit = GPRFamilyMask{1} << *Family;
    RegisterStep Write, Read;
    Write.Kills = Bit;
    Read.Reads[*Family] = 8;
    for (unsigned Mode = 0; Mode < 3; ++Mode) {
      auto F = body(Mode == 0   ? std::vector<RegisterStep>{}
                    : Mode == 1 ? std::vector<RegisterStep>{Write}
                                : std::vector<RegisterStep>{Read, Write});
      const auto Proof = proveUnreadCallRegisters({{0x1000, &F}});
      const auto At = Proof.find(0x1000);
      const bool Proved = At != Proof.end() && (At->second & Bit);
      EXPECT_EQ(Proved, Mode == 1);
    }
  }
}

TEST(CallRegisterUnread, NestedEntryWritesDoNotDependOnPrototypeArity) {
  RegisterStep Setup, Call, Opaque;
  Setup.Kills = (GPRFamilyMask{1} << 1) | (GPRFamilyMask{1} << 2);
  Call.Callee = 0x2000;
  Opaque.FixedArguments = GPRReadWidths{};
  auto Caller = body({Setup, Call, Opaque});
  RegisterStep NestedSetup;
  NestedSetup.Kills = (GPRFamilyMask{1} << 8) | (GPRFamilyMask{1} << 9);
  auto Callee = body({NestedSetup, Opaque});
  const auto Proof =
      proveUnreadCallRegisters({{0x1000, &Caller}, {0x2000, &Callee}});
  ASSERT_TRUE(Proof.count(0x1000));
  for (unsigned Family : {1u, 2u, 8u, 9u})
    EXPECT_NE(Proof.at(0x1000) & (GPRFamilyMask{1} << Family), 0u);
}

TEST(CallRegisterUnread, EveryBypassAndLoopRetainsItsIncomingObservation) {
  RegisterStep Write, Read;
  Write.Kills = ScratchBit;
  Read.Reads[Scratch] = 8;
  for (bool Bypass : {false, true}) {
    auto Effect = body({});
    Effect.Blocks[0].Succs = {1, 2};
    Effect.Blocks.push_back({{Write}, {3}});
    Effect.Blocks.push_back({Bypass ? std::vector<RegisterStep>{}
                                    : std::vector<RegisterStep>{Write},
                             {3}});
    Effect.Blocks.push_back({{Read}, {3}});
    EXPECT_EQ(unread(proveUnreadCallRegisters({{0x1000, &Effect}}), 0x1000),
              !Bypass);
  }
}

TEST(CallRegisterUnread, RecursiveSummariesConvergeInBothAddressOrders) {
  for (bool Reverse : {false, true}) {
    const va_t First = Reverse ? 0x2000 : 0x1000;
    const va_t Second = Reverse ? 0x1000 : 0x2000;
    RegisterStep ToFirst, ToSecond, Read;
    ToFirst.Callee = First;
    ToSecond.Callee = Second;
    Read.Reads[Scratch] = 8;
    auto A = body({ToSecond});
    auto B = body({Read, ToFirst});
    auto Proof = proveUnreadCallRegisters({{First, &A}, {Second, &B}});
    EXPECT_FALSE(unread(Proof, First));
    EXPECT_FALSE(unread(Proof, Second));
    B.Blocks[0].Steps.erase(B.Blocks[0].Steps.begin());
    Proof = proveUnreadCallRegisters({{First, &A}, {Second, &B}});
    EXPECT_TRUE(unread(Proof, First));
    EXPECT_TRUE(unread(Proof, Second));
  }
}

TEST(CallRegisterUnread, ExhaustionPublishesNoPartialProofAndDoesNotPersist) {
  auto Effect = body({});
  Effect.Blocks.resize(2048);
  Effect.Blocks[0].Succs = {Effect.Blocks.size() - 1};
  RegisterStep Read;
  Read.Reads[Scratch] = 8;
  Effect.Blocks[1].Steps = {Read};
  for (size_t B = 2; B < Effect.Blocks.size(); ++B)
    Effect.Blocks[B].Succs = {B - 1};
  EXPECT_TRUE(proveUnreadCallRegisters({{0x1000, &Effect}}).empty());
  Effect = body({});
  EXPECT_TRUE(unread(proveUnreadCallRegisters({{0x1000, &Effect}}), 0x1000));
}
} // namespace
