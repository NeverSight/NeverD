//===- MedStackDeltaTests.cpp - Stack pointer offsets through joins -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Entry-stack offsets through joins, bounded root reuse, and definition
/// lifetimes when ABI recovery rewrites call results.

#include "../../../lib/ir/med/abi/MedABIPassDetail.h"
#include "../../../lib/ir/med/abi/MedStackDeltaCache.h"
#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace {
using namespace neverd;

MedVar rsp(int Version) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.Id = 1;
  Value.SSAVer = Version;
  Value.RegOff = x86reg::RSP;
  Value.Size = 8;
  Value.TheArch = Arch::X64;
  return Value;
}

// Appends \p Levels joins in a row to \p F. Each join merges two arms that
// subtract \p Left and \p Right from the previous join's stack pointer; the
// first arms start from the live-in stack pointer. Returns the last join.
MedVar joinChain(MedFunc &F, int Levels, std::pair<int64_t, int64_t> Arms) {
  F.Entry = 0x1000;
  F.Blocks.emplace_back().Id = 0;
  MedVar Previous = rsp(0);
  int Version = 1;
  for (int Level = 0; Level < Levels; ++Level) {
    const int Join = 3 * Level + 3;
    PhiNode Phi;
    Phi.Output = rsp(Version++);
    for (int Arm = 0; Arm < 2; ++Arm) {
      MedBlock Block;
      Block.Id = Join - 2 + Arm;
      Block.Preds = {Join - 3};
      Block.Succs = {Join};
      MedOp Step;
      Step.Opcode = NdOp::INT_SUB;
      Step.Output = rsp(Version++);
      Step.addInput(Previous);
      Step.addInput(MedVar::makeConst(
          static_cast<uint64_t>(Arm ? Arms.second : Arms.first), 8));
      Phi.Args.emplace_back(Block.Id, Step.Output);
      Block.Ops.push_back(Step);
      F.Blocks.push_back(std::move(Block));
    }
    MedBlock Merge;
    Merge.Id = Join;
    Merge.Preds = {Join - 2, Join - 1};
    Merge.Phis.push_back(Phi);
    F.Blocks.push_back(std::move(Merge));
    Previous = Phi.Output;
  }
  return Previous;
}

std::optional<int64_t> deltaOf(const MedFunc &F, const MedVar &V,
                               bool Indexed) {
  MedDefSiteIndex Defs(F);
  return stackPtrDelta(F, getTargetRegInfo(Arch::X64), V, 0,
                       Indexed ? &Defs : nullptr);
}

// Every path through 64 joins reaches the live-in stack pointer at the
// depth limit. There are 2^64 of them; the answer must not walk each one.
TEST(MedStackDelta, ResolvesEachJoinOfAChainOnce) {
  for (const bool Indexed : {false, true}) {
    MedFunc F;
    const MedVar Last = joinChain(F, 64, {16, 16});
    EXPECT_EQ(deltaOf(F, Last, Indexed), std::optional<int64_t>(-1024))
        << "indexed=" << Indexed;
  }
}

// One more join puts the live-in stack pointer past the depth limit.
TEST(MedStackDelta, KeepsTheDepthLimitOfAJoinChain) {
  for (const bool Indexed : {false, true}) {
    MedFunc F;
    const MedVar Last = joinChain(F, 65, {16, 16});
    EXPECT_EQ(deltaOf(F, Last, Indexed), std::nullopt) << "indexed=" << Indexed;
  }
}

// Arms that adjust the stack pointer differently leave no single offset.
TEST(MedStackDelta, RefusesArmsWithDifferentOffsets) {
  for (const bool Indexed : {false, true}) {
    MedFunc F;
    const MedVar Last = joinChain(F, 40, {16, 8});
    EXPECT_EQ(deltaOf(F, Last, Indexed), std::nullopt) << "indexed=" << Indexed;
  }
}

TEST(MedStackDelta, RepeatedRootsKeepTheirOriginalDepthAllowance) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  for (bool DeepFirst : {false, true}) {
    MedFunc F;
    const MedVar Last = joinChain(F, 64, {16, 16});
    MedStackDeltaCache Cache(F, TRI);
    if (DeepFirst)
      EXPECT_FALSE(Cache.query(Last, 1));
    for (unsigned Repeat = 0; Repeat != 100; ++Repeat) {
      EXPECT_EQ(Cache.query(Last), -1024);
      // A cached depth-zero result cannot supply a deeper query's answer.
      EXPECT_FALSE(Cache.query(Last, 1));
    }
    EXPECT_EQ(Cache.statistics().Hits, 99u);
    EXPECT_EQ(Cache.retainedAnswers(), 1u);
    EXPECT_EQ(Cache.query(rsp(0), 128), 0);
    EXPECT_FALSE(Cache.query(rsp(0), 129));
  }
  MedFunc F;
  const MedVar Last = joinChain(F, 65, {16, 16});
  MedStackDeltaCache Cache(F, TRI);
  for (unsigned Repeat = 0; Repeat != 10; ++Repeat)
    EXPECT_FALSE(Cache.query(Last));
  EXPECT_EQ(Cache.statistics().Hits, 9u);
}

TEST(MedStackDelta, InvalidationRechecksChangedArmsAndDuplicateDefinitions) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  MedFunc F;
  const MedVar Last = joinChain(F, 2, {16, 16});
  MedStackDeltaCache Cache(F, TRI);
  ASSERT_EQ(Cache.query(Last), -32);
  F.Blocks[1].Ops.front().Inputs[1] = MedVar::makeConst(8, 8);
  Cache.invalidate();
  EXPECT_EQ(Cache.retainedAnswers(), 0u);
  EXPECT_FALSE(Cache.query(Last));
  F.Blocks[1].Ops.front().Inputs[1] = MedVar::makeConst(16, 8);
  Cache.invalidate();
  ASSERT_EQ(Cache.query(Last), -32);
  F.Blocks.back().Phis.push_back(F.Blocks.back().Phis.front());
  Cache.invalidate();
  EXPECT_FALSE(Cache.query(Last));
  F.Blocks.back().Phis.pop_back();
  Cache.invalidate();
  EXPECT_EQ(Cache.query(Last), -32);
}

TEST(MedStackDelta, FunctionsTargetsAndMissingValuesStaySeparate) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  MedFunc A, B;
  const MedVar LastA = joinChain(A, 3, {16, 16});
  const MedVar LastB = joinChain(B, 3, {8, 8});
  ASSERT_EQ(LastA, LastB);
  MedStackDeltaCache First(A, TRI), Second(B, TRI);
  MedStackDeltaCache Foreign(A, getTargetRegInfo(Arch::AArch64));
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    EXPECT_EQ(First.query(LastA), -48);
    EXPECT_EQ(Second.query(LastB), -24);
    EXPECT_FALSE(Foreign.query(LastA));
  }
  const auto Retained = First.retainedAnswers();
  for (int Id = 100; Id != 200; ++Id) {
    MedVar Missing = rsp(Id);
    Missing.Kind = MedVar::Temp;
    EXPECT_FALSE(First.query(Missing));
    EXPECT_FALSE(First.query(MedVar::makeConst(Id, 8)));
  }
  EXPECT_EQ(First.retainedAnswers(), Retained);
}

TEST(MedStackDelta, InvalidationIncludesStoresThatDefineSavedStackLoads) {
  MedFunc F;
  F.Blocks.emplace_back().Id = 0;
  MedVar Address = rsp(10);
  Address.Kind = MedVar::Temp;
  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Address);
  Store.addInput(rsp(0));
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = rsp(1);
  Load.addInput(Address);
  F.Blocks.front().Ops = {Store, Load};
  MedStackDeltaCache Cache(F, getTargetRegInfo(Arch::X64));
  ASSERT_EQ(Cache.query(Load.Output), 0);
  ASSERT_EQ(Cache.query(Load.Output), 0);
  // The LOAD definition has not moved, but its reaching memory value changed.
  F.Blocks.front().Ops.front().Inputs[1] = MedVar::makeConst(7, 8);
  Cache.invalidate();
  EXPECT_FALSE(Cache.query(Load.Output));
  F.Blocks.front().Ops.front().Inputs[1] = rsp(0);
  Cache.invalidate();
  EXPECT_EQ(Cache.query(Load.Output), 0);
}

TEST(MedStackDelta, CyclicAndOverflowingRootsRetainRefusals) {
  for (bool Cycle : {false, true}) {
    MedFunc F;
    const auto Last = joinChain(F, 2, {16, 16});
    if (Cycle) {
      F.Blocks.front().Phis.push_back({});
      auto &Phi = F.Blocks.front().Phis.back();
      Phi.Output = rsp(0);
      Phi.Args.emplace_back(F.Blocks.back().Id, Last);
    } else {
      for (auto &Block : F.Blocks)
        for (auto &Op : Block.Ops)
          Op.Inputs[1] = MedVar::makeConst(uint64_t{1} << 63, 8);
    }
    MedStackDeltaCache Cache(F, getTargetRegInfo(Arch::X64));
    EXPECT_FALSE(Cache.query(Last));
    EXPECT_FALSE(Cache.query(Last));
    EXPECT_EQ(Cache.statistics().Hits, 1u);
  }
}

TEST(MedStackDelta, FloatingResultNormalizationRebuildsMovedDefinitions) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (unsigned Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    const bool Warm = Mode != 0;
    const bool Indirect = Mode == 2;
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    for (va_t Slot : {0x5000, 0x5004}) {
      Import Binding;
      Binding.Module = "msvcrt.dll";
      Binding.Name = "abs";
      Binding.IATAddr = Slot;
      Image.Imports.push_back(Binding);
    }
    MedFunc F;
    F.Entry = 0x1000;
    F.Blocks.emplace_back().Id = 0;
    auto &Ops = F.Blocks.front().Ops;
    auto Reg = [&](int Id, int Version, uint64_t Offset, uint16_t Size = 4) {
      MedVar V = rsp(Version);
      V.Id = Id;
      V.RegOff = Offset;
      V.Size = Size;
      V.TheArch = Arch::X86;
      return V;
    };
    const auto EntrySP = Reg(1, 0, TRI.StackPointer);
    auto Adjust = [&](MedVar Output, int64_t Offset) {
      MedOp Op;
      Op.Opcode = NdOp::INT_SUB;
      Op.Output = Output;
      Op.addInput(EntrySP);
      Op.addInput(MedVar::makeConst(Offset, 4));
      Ops.push_back(Op);
    };
    auto Store = [&](MedVar Address, uint64_t Value) {
      MedOp Op;
      Op.Opcode = NdOp::STORE;
      Op.addInput(Address);
      Op.addInput(MedVar::makeConst(Value, 4));
      Ops.push_back(Op);
    };
    auto Call = [&](va_t Target) {
      MedOp Op;
      Op.Opcode = NdOp::CALL;
      MedVar Destination = MedVar::makeConst(Target, 4);
      if (Indirect && Target != 0x3000) {
        MedOp Load;
        Load.Opcode = NdOp::LOAD;
        Destination = Reg(100 + Target / 0x1000, 1, 0);
        Destination.Kind = MedVar::Temp;
        Load.Output = Destination;
        Load.addInput(MedVar::makeConst(Target == 0x2000 ? 0x5000 : 0x5004, 4));
        Ops.push_back(Load);
        Op.Opcode = NdOp::INDIR_CALL;
      }
      Op.Addr = F.Entry + Ops.size();
      Op.addInput(Destination);
      Ops.push_back(Op);
    };
    MedVar Address = Reg(10, 1, 0);
    Address.Kind = MedVar::Temp;
    Adjust(Address, 16);
    if (Warm) {
      const auto SP = Reg(1, 1, TRI.StackPointer);
      Adjust(SP, 4);
      Store(SP, 11);
      Call(0x2000);
    }
    Call(0x3000);
    auto &FPCall = Ops.back();
    FPCall.CallSiteId = 1;
    MedVar Wide = Reg(20, 1, 0, 8);
    Wide.Kind = MedVar::Temp;
    FPCall.Output = Wide;
    const auto FPCallAddress = FPCall.Addr;
    const auto FPResult = Reg(21, 1, TRI.FPReturnReg, 16);
    F.CallClobbers.push_back({FPResult, FPCall.CallSiteId});
    for (unsigned Half = 0; Half != 2; ++Half) {
      MedOp Op;
      Op.Opcode = NdOp::SUBBYTES;
      Op.Addr = FPCallAddress;
      Op.Output =
          Reg(30 + Half, 1, Half ? TRI.IntReturnReg2 : TRI.IntReturnReg);
      Op.addInput(Wide);
      Op.addInput(MedVar::makeConst(Half * 4, 4));
      Ops.push_back(Op);
    }
    Adjust(Reg(1, 2, TRI.StackPointer), 16);
    Store(Address, 42);
    Call(0x4000);
    // Occupy the old definition positions after erasure: a stale pointer
    // must not accidentally read unchanged bytes in the vector's old tail.
    for (int Id : {200, 201}) {
      MedOp Op;
      Op.Opcode = NdOp::COPY;
      Op.Output = Reg(Id, 1, 0);
      Op.Output.Kind = MedVar::Temp;
      Op.addInput(MedVar::makeConst(0, 4));
      Ops.push_back(Op);
    }
    const auto Before = Ops.size();
    std::map<va_t, int> Registers{{0x2000, 0}, {0x3000, 0}, {0x4000, 0}};
    std::map<va_t, int> Counts{{0x2000, 1}, {0x3000, 0}, {0x4000, 1}};
    const std::map<va_t, int> FPCounts{
        {0x2000, 0}, {0x3000, 0}, {0x4000, 0}, {0x5000, 0}, {0x5004, 0}};
    const std::map<va_t, uint16_t> Returns{{0x3000, 8}};
    recoverCallAbi(F, Arch::X86, {}, Indirect ? &Image : nullptr, &Registers,
                   &Counts, &FPCounts, &Returns);
    ASSERT_EQ(Ops.size(), Before - 2);
    ASSERT_EQ(F.CallInfos.size(), Warm ? 3u : 2u);
    const auto &Last = F.CallInfos.back();
    ASSERT_EQ(Last.TargetAddr, Indirect ? 0x5004u : 0x4000u);
    ASSERT_EQ(Last.Args.size(), 1u);
    EXPECT_EQ(Last.Args.front(), MedVar::makeConst(42, 4));
  }
}

} // namespace
