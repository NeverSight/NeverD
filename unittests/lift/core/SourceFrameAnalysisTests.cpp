#include "gtest/gtest.h"

#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/lift/AArch64Regs.h"

#include <algorithm>

using namespace neverd;
namespace {
const auto SP = NdVar::reg(a64reg::SP, 8);
const auto Slot = NdVar::reg(a64reg::X19, 8);
const auto Value = NdVar::reg(a64reg::X21, 8);
const auto X0 = NdVar::reg(a64reg::X0, 8);

LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
         va_t Address) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  Result.Addr = Address;
  for (const auto &Input : Inputs)
    Result.addInput(Input);
  return Result;
}
struct FrameFixture {
  LowFunc Low;
  FrameFixture() {
    Low.Entry = 0x1000;
    LowBlock Block;
    Block.Id = 7;
    Block.StartAddr = Low.Entry;
    Block.Ops = {
        op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)}, 0x1000),
        op(NdOp::COPY, Value, {NdVar::addressFragment(0x6000, 8)}, 0x1004),
        op(NdOp::INT_ADD, Value, {Value, NdVar::scalar(128, 8)}, 0x1008),
        op(NdOp::INT_ADD, Slot, {SP, NdVar::scalar(32, 8)}, 0x100c),
        op(NdOp::STORE, {}, {Slot, Value}, 0x1010),
        op(NdOp::COPY, Value, {NdVar::scalar(0, 8)}, 0x1014),
        op(NdOp::LOAD, NdVar::tmp(16, 8), {Slot}, 0x1018),
        // Later unknown cleanup does not authorize this earlier query.
        op(NdOp::CALL, X0, {NdVar::cst(0x5000, 8)}, 0x101c),
        op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, 0x1020)};
    Low.Blocks.push_back(std::move(Block));
  }
  std::optional<SourceFrameLoadDefinition>
  query(const NativeSourceCalls &Calls = {}) const {
    for (const auto &Block : Low.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I)
        if (Block.Ops[I].Addr == 0x1018 && Block.Ops[I].Opcode == NdOp::LOAD)
          return sourceFrameLoadedDefinition(Low, Arch::AArch64, Calls,
                                             Block.Id, I);
    return std::nullopt;
  }
  void prefix(std::initializer_list<LowOp> Ops) {
    auto &B = Low.Blocks.front();
    B.Ops.insert(B.Ops.begin() + 6, Ops);
  }
  void diamond() {
    const auto Original = Low.Blocks.front();
    Low.Blocks.resize(4);
    for (size_t I = 0; I < 4; ++I) {
      Low.Blocks[I] = {};
      Low.Blocks[I].Id = 7 + I;
      Low.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    }
    Low.Blocks[0].Ops.assign(Original.Ops.begin(), Original.Ops.begin() + 6);
    Low.Blocks[0].Succs = {8, 9};
    for (size_t I : {1, 2}) {
      Low.Blocks[I].Preds = {7};
      Low.Blocks[I].Succs = {10};
      Low.Blocks[I].Ops = {op(NdOp::COPY, NdVar::reg(a64reg::X9, 8),
                              {NdVar::scalar(0, 8)}, 0x1000 + I * 0x100)};
    }
    Low.Blocks[3].Preds = {8, 9};
    Low.Blocks[3].Ops.assign(Original.Ops.begin() + 6, Original.Ops.end());
  }
};

SourceFunctionTypeHint signature(size_t Parameters = 1,
                                 bool ReturnsPointer = false) {
  SourceFunctionTypeHint Result;
  Result.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Result.ReturnType =
      ReturnsPointer ? NdType::makePtr(NdType::makeVoid()) : NdType::makeVoid();
  for (size_t I = 0; I < Parameters; ++I)
    Result.Parameters.push_back({"arg", NdType::makeInt(8, false)});
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Result, Arch::AArch64, Error))
      << Error;
  return Result;
}
NativeSourceCalls callsAt(va_t Address, const SourceFunctionTypeHint &Signature,
                          const SourceFrameEffects &Effects) {
  NativeSourceCallContract Contract;
  Contract.Signature = &Signature;
  static_cast<SourceFrameEffects &>(Contract) = Effects;
  const auto Call = op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, Address);
  return {{*nativeSourceCallKey(Call), Contract}};
}
} // namespace

TEST(SourceFrameAnalysis,
     ReturnsTheOriginalDefinitionWithoutPublishingAnAddress) {
  FrameFixture F;
  const auto Result = F.query();
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->FrameOffset, -32);
  EXPECT_EQ(Result->Definition,
            (SourceFrameDefinition{7, 2, 0x1008, 0, Value}));
  EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, {}));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::X64, {}, 7, 6));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 8, 6));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 7, 99));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 7, 5));
  // Numerically equal scalar constants do not inherit an instruction's origin.
  F.Low.Blocks[0].Ops[2] =
      op(NdOp::COPY, Value, {NdVar::scalar(0x6080, 8)}, 0x1008);
  EXPECT_FALSE(F.query());
}

TEST(SourceFrameAnalysis, RejectsPartialEscapedExpiredAndUninitializedBytes) {
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    switch (Case) {
    case 0:
      Ops.erase(Ops.begin() + 4);
      break;
    case 1:
      Ops[4].Inputs[1].Size = 4;
      break;
    case 2:
      Ops[6].Output.Size = 4;
      break;
    case 3:
      Ops[4].Inputs[1] = SP;
      break;
    case 4:
      F.prefix({op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 1)}, 0x1015)});
      break;
    case 5:
      F.prefix({op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(64, 8)}, 0x1015),
                op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)}, 0x1016)});
      break;
    case 6:
      F.prefix({op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1015)});
      break;
    case 7:
      F.prefix({op(NdOp::COPY, Slot, {NdVar::reg(a64reg::X20, 8)}, 0x1015)});
      break;
    case 8:
      Ops[6].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 9:
      F.prefix({op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), SP}, 0x1015)});
      break;
    case 10:
      F.prefix(
          {op(NdOp::COPY, NdVar::reg(a64reg::X9, 4),
              {NdVar::reg(a64reg::SP, 4)}, 0x1015),
           op(NdOp::STORE, {}, {Slot, NdVar::reg(a64reg::X9, 8)}, 0x1016)});
      break;
    case 11:
      Ops[3].Inputs[1] = NdVar::scalar(64, 8);
      break;
    case 12:
      Ops[3].Inputs[1] = NdVar::scalar(uint64_t(-8), 8);
      break;
    case 13:
      F.prefix({op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, 0x1015)});
      break;
    case 14:
      Ops[4].Inputs[0].Size = 4;
      break;
    case 15:
      Ops[6].NumInputs = 0;
      break;
    case 16:
      F.prefix({op(NdOp::INT_XOR, Slot, {Slot, NdVar::scalar(1, 8)}, 0x1015)});
      break;
    case 17:
      F.prefix({op(NdOp::COPY, NdVar::reg(a64reg::X19, 4),
                   {NdVar::scalar(0, 4)}, 0x1015)});
      break;
    }
    EXPECT_FALSE(F.query());
  }
}

TEST(SourceFrameAnalysis, RequiresAllReachingPathsIndependentOfBlockOrder) {
  FrameFixture F;
  F.diamond();
  const auto Expected = F.query();
  ASSERT_TRUE(Expected);
  std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
  EXPECT_EQ(F.query(), Expected);
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture Bad;
    Bad.diamond();
    auto &B = Bad.Low.Blocks;
    switch (Case) {
    case 0:
      B[2].Ops.push_back(
          op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 8)}, 0x1204));
      break;
    case 1:
      B[2].Ops.push_back(
          op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), SP}, 0x1204));
      break;
    case 2:
      B[0].Ops.erase(B[0].Ops.begin() + 4);
      B[1].Ops.push_back(op(NdOp::STORE, {}, {Slot, Value}, 0x1104));
      break;
    case 3:
      B[3].Preds.pop_back();
      break;
    case 4:
      B[3].Preds.push_back(8);
      break;
    case 5:
      B[2].Succs.push_back(9);
      B[2].Preds.push_back(9);
      break;
    case 6:
      B[3].Succs = {7};
      B[0].Preds = {10};
      break;
    case 7:
      B[1].Id = 9;
      break;
    case 8:
      B[2].Ops.clear();
      break;
    case 9:
      B[0].Succs = {8};
      B[2].Preds.clear();
      break;
    }
    EXPECT_FALSE(Bad.query());
  }
  // A loop reached only after the query is irrelevant to this prefix.
  F = FrameFixture{};
  auto &Root = F.Low.Blocks[0];
  Root.Ops.resize(7);
  Root.Succs = {8};
  LowBlock Later;
  Later.Id = 8;
  Later.StartAddr = 0x2000;
  Later.Preds = {7, 8};
  Later.Succs = {8};
  Later.Ops = {op(NdOp::BRANCH, {}, {NdVar::cst(0x2000, 8)}, 0x2000)};
  F.Low.Blocks.push_back(Later);
  EXPECT_TRUE(F.query());
}

TEST(SourceFrameAnalysis, UsesBoundedCallWritesAndPossibleReturnAliases) {
  auto Signature = signature(1, true);
  for (unsigned Case = 0; Case < 11; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    SourceFrameEffects Effects;
    Effects.WritableFrameParameters[0] = 24;
    F.prefix({op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(0, 8)}, 0x1015),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1016)});
    switch (Case) {
    case 1:
      Effects.WritableFrameParameters[0] = 33;
      break;
    case 2:
      Effects = {};
      break;
    case 3:
      Effects.WritableFrameParameters[0] = 65;
      break;
    case 4:
      Effects.ReadOnlyFrameParameters[0] = 24;
      break;
    case 5:
      Effects.ReturnFrameOrExternal = {0, 24};
      break;
    case 6:
      Effects.ReturnFrameOrExternal = {0, 24};
      F.Low.Blocks[0].Ops[8].Inputs[0] = X0;
      break;
    case 7:
      Effects.ReturnFrameOrExternal = {0, 24};
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + 8,
                                 op(NdOp::STORE, {}, {Slot, X0}, 0x1017));
      break;
    case 8:
      Effects.WritableFrameParameters[0] = 0;
      break;
    case 9:
      Effects = {};
      Effects.ReadOnlyFrameParameters[0] = 40;
      break;
    case 10:
      F.Low.Blocks[0].Ops[6].Inputs[1] = NdVar::scalar(16, 8);
      break;
    }
    EXPECT_EQ(bool(F.query(callsAt(0x1016, Signature, Effects))),
              Case == 0 || Case == 5 || Case == 9);
  }
}

TEST(SourceFrameAnalysis, RetainsConditionalScratchAndLiveRecordRequirements) {
  auto Begin = signature(4);
  auto End = signature(1);
  SourceFrameEffects Initialize, Finish;
  using Scratch = SourceFrameScratchEffect;
  Initialize.WritableFrameParameters[1] = 24;
  Initialize.Scratch = {Scratch::Domain::SwiftUntrackedAccess,
                        Scratch::Action::Initialize, 1, 24,
                        SourceFrameScalarCondition{2, {0, 1}}};
  Finish.ReadOnlyFrameParameters[0] = 24;
  Finish.Scratch = {Scratch::Domain::SwiftUntrackedAccess,
                    Scratch::Action::Finish, 0, 24, std::nullopt};
  auto Calls = callsAt(0x1034, Begin, Initialize);
  Calls.merge(callsAt(0x103c, End, Finish));
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    const auto X1 = NdVar::reg(a64reg::X1, 8);
    const auto X2 = NdVar::reg(a64reg::X2, 8);
    F.prefix({op(NdOp::COPY, X0, {NdVar::scalar(0, 8)}, 0x1024),
              op(NdOp::COPY, X1, {SP}, 0x1028),
              op(NdOp::COPY, X2,
                 {NdVar::scalar(Case == 1   ? 1
                                : Case == 2 ? 32
                                            : 0,
                                8)},
                 0x102c),
              op(NdOp::COPY, NdVar::reg(a64reg::X3, 8), {NdVar::scalar(0, 8)},
                 0x1030),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1034),
              op(NdOp::COPY, X0, {SP}, 0x1038),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x103c)});
    auto &Ops = F.Low.Blocks[0].Ops;
    auto Contract = Calls;
    switch (Case) {
    case 3:
      Ops[8].Inputs[0] = NdVar::reg(a64reg::X9, 8);
      break;
    case 4:
      Ops[8].Inputs[0].Size = 4;
      break;
    case 5:
      Ops.erase(Ops.begin() + 10);
      break;
    case 6:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::STORE, {}, {SP, NdVar::scalar(0, 1)}, 0x1039));
      break;
    case 7:
      Ops[11] = op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(8, 8)}, 0x1038);
      break;
    case 8:
      Ops[7] = op(NdOp::INT_ADD, X1, {SP, NdVar::scalar(16, 8)}, 0x1028);
      break;
    case 9:
      Contract.begin()->second.Scratch->Condition->Parameter = 4;
      break;
    case 10:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 1)}, 0x1039));
      break;
    case 11:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(16, 8)}, 0x1039));
      break;
    }
    EXPECT_EQ(bool(F.query(Contract)), Case == 0 || Case == 1);
  }
}

TEST(SourceFrameAnalysis, BoundsDefinitionAndTransferWork) {
  FrameFixture F;
  std::vector<LowOp> Prefix;
  for (size_t I = 0; I < 4097; ++I)
    Prefix.push_back(op(NdOp::COPY, Value,
                        {NdVar::addressFragment(0x6000 + I, 8)},
                        0x4000 + I * 4));
  F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + 6, Prefix.begin(),
                             Prefix.end());
  EXPECT_FALSE(F.query());
  F = FrameFixture{};
  F.Low.Blocks.resize(16385);
  EXPECT_FALSE(F.query());
}

TEST(SourceFrameAnalysis, JoinsOnlyOrderedBytesOfOneDefinition) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    const auto LowHalf = NdVar::reg(a64reg::X10, 4);
    const auto HighHalf = NdVar::reg(a64reg::X11, 4);
    const auto NextSlot = NdVar::reg(a64reg::X12, 8);
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.erase(Ops.begin() + 4);
    Ops.insert(Ops.begin() + 4,
               {op(NdOp::SUBBYTES, LowHalf,
                   {Value, NdVar::scalar(Case == 1 ? 4 : 0, 4)}, 0x1010),
                op(NdOp::SUBBYTES, HighHalf,
                   {Value, NdVar::scalar(Case == 1 ? 0 : 4, 4)}, 0x1011),
                op(NdOp::INT_ADD, NextSlot,
                   {Slot, NdVar::scalar(Case == 2 ? 3 : 4, 8)}, 0x1012),
                op(NdOp::STORE, {}, {Slot, LowHalf}, 0x1013),
                op(NdOp::STORE, {}, {NextSlot, HighHalf}, 0x1014)});
    if (Case == 3)
      Ops.insert(
          Ops.begin() + 5,
          op(NdOp::COPY, Value, {NdVar::addressFragment(0x6080, 8)}, 0x1010));
    const auto Result = F.query();
    EXPECT_EQ(bool(Result), Case == 0);
    if (Result)
      EXPECT_EQ(Result->Definition.Instruction, 0x1008U);
  }
}
