//===- MedDceImplicitSpTests.cpp - Implicit call-frame liveness ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file Checks register liveness across implicit reads and shared CFG paths.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
using namespace neverd;

MedVar reg(int Id, int Version, uint16_t Size, uint64_t Offset) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.TheArch = Arch::X86;
  Value.Id = Id;
  Value.SSAVer = Version;
  Value.Size = Size;
  Value.RegOff = Offset;
  return Value;
}

TEST(MedDceImplicitSp, KeepsPredecessorStackPointerAtCall) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  MedFunc Func;
  Func.Blocks.resize(2);
  MedBlock &Setup = Func.Blocks[0];
  Setup.Id = 0;
  Setup.Succs = {1};
  const MedVar EntrySP = reg(1, 0, 4, TRI.StackPointer);
  const MedVar CallSP = reg(1, 1, 4, TRI.StackPointer);
  MedOp Adjust;
  Adjust.Opcode = NdOp::INT_SUB;
  Adjust.Output = CallSP;
  Adjust.addInput(EntrySP);
  Adjust.addInput(MedVar::makeConst(4, 4));
  Setup.Ops.push_back(Adjust);

  MedBlock &CallBlock = Func.Blocks[1];
  CallBlock.Id = 1;
  CallBlock.Preds = {0};
  MedOp DeadCopy;
  DeadCopy.Opcode = NdOp::COPY;
  DeadCopy.Output.Kind = MedVar::Temp;
  DeadCopy.Output.Id = 2;
  DeadCopy.Output.SSAVer = 1;
  DeadCopy.Output.Size = 4;
  DeadCopy.addInput(MedVar::makeConst(7, 4));
  CallBlock.Ops.push_back(DeadCopy);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output.Kind = MedVar::Temp;
  Call.Output.Id = 3;
  Call.Output.SSAVer = 1;
  Call.Output.Size = 4;
  Call.addInput(MedVar::makeConst(0x2000, 4));
  CallBlock.Ops.push_back(Call);

  LowToMedConverter().runRegisterDce(Func, Arch::X86);
  ASSERT_EQ(Setup.Ops.size(), 1u);
  EXPECT_EQ(Setup.Ops.front().Output, CallSP);
  ASSERT_EQ(CallBlock.Ops.size(), 1u);
  EXPECT_EQ(CallBlock.Ops.front().Opcode, NdOp::CALL);
}

TEST(MedDCE, KeepsDedicatedIndirectResultSetupAcrossPredecessors) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (NdOp CallKind : {NdOp::CALL, NdOp::INDIR_CALL})
    for (bool Split : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(CallKind));
      SCOPED_TRACE(Split);
      MedFunc Func;
      Func.Blocks.resize(Split ? 2 : 1);
      auto &Setup = Func.Blocks.front();
      Setup.Id = 0;
      MedVar Buffer = reg(2, 1, 8, TRI.indirectResultReg());
      Buffer.TheArch = Arch::AArch64;
      MedVar Stack = reg(1, 0, 8, TRI.StackPointer);
      Stack.TheArch = Arch::AArch64;
      MedOp Address;
      Address.Opcode = NdOp::INT_ADD;
      Address.Output = Buffer;
      Address.addInput(Stack);
      Address.addInput(MedVar::makeConst(32, 8));
      Setup.Ops.push_back(Address);
      auto &Calls = Func.Blocks.back();
      if (Split) {
        Setup.Succs = {1};
        Calls.Id = 1;
        Calls.Preds = {0};
      }
      MedOp Call;
      Call.Opcode = CallKind;
      Call.addInput(MedVar::makeConst(0x2000, 8));
      Calls.Ops.push_back(Call);
      LowToMedConverter().runRegisterDce(Func, Arch::AArch64);
      ASSERT_FALSE(Setup.Ops.empty());
      EXPECT_EQ(Setup.Ops.front().Opcode, NdOp::INT_ADD);
      EXPECT_EQ(Setup.Ops.front().Output, Buffer);
    }
}

TEST(MedDCE, KeepsLivePhiCyclesAndDropsDeadCyclesInEitherBlockOrder) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Reverse);
      // Sparse identifiers keep liveness independent of numbering density.
      auto value = [&](int Id) {
        MedVar V;
        V.Kind = MedVar::Temp;
        V.TheArch = Architecture;
        V.Id = Id;
        V.SSAVer = std::numeric_limits<int>::max();
        V.Size = 4;
        return V;
      };
      const MedVar LivePhi = value(7);
      const MedVar LiveNext = value(std::numeric_limits<int>::max());
      const MedVar DeadPhi = value(4000);
      const MedVar DeadNext = value(8000);
      MedFunc Func;
      Func.Blocks.resize(2);
      Func.Blocks[0].Id = 0;
      Func.Blocks[0].Succs = {1};
      auto &Loop = Func.Blocks[1];
      Loop.Id = 1;
      Loop.Preds = {0, 1};
      Loop.Succs = {1};
      for (const auto &[Phi, Next] :
           {std::pair{LivePhi, LiveNext}, std::pair{DeadPhi, DeadNext}}) {
        Loop.Phis.push_back({Phi, {{0, MedVar::makeConst(0, 4)}, {1, Next}}});
        MedOp Add;
        Add.Opcode = NdOp::INT_ADD;
        Add.Output = Next;
        Add.addInput(Phi);
        Add.addInput(MedVar::makeConst(1, 4));
        Loop.Ops.push_back(Add);
      }
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.Output.Id = -1;
      const uint16_t PointerBytes =
          Architecture == Arch::X64 || Architecture == Arch::AArch64 ? 8 : 4;
      Store.addInput(MedVar::makeConst(0x2000, PointerBytes));
      Store.addInput(LiveNext);
      Loop.Ops.push_back(Store);
      if (Reverse)
        std::reverse(Func.Blocks.begin(), Func.Blocks.end());

      LowToMedConverter().runRegisterDce(Func, Architecture);
      const auto &Kept =
          *std::find_if(Func.Blocks.begin(), Func.Blocks.end(),
                        [](const MedBlock &Block) { return Block.Id == 1; });
      ASSERT_EQ(Kept.Phis.size(), 1u);
      EXPECT_EQ(Kept.Phis.front().Output, LivePhi);
      ASSERT_EQ(Kept.Ops.size(), 2u);
      EXPECT_EQ(Kept.Ops.front().Output, LiveNext);
      EXPECT_EQ(Kept.Ops.back().Opcode, NdOp::STORE);
      EXPECT_EQ(Kept.Ops.back().Inputs[1], LiveNext);
    }
}

TEST(MedDCE, NegativeConstantNumberingDoesNotNameADefinition) {
  for (const auto &[Id, Version] :
       {std::pair{-1, -1}, std::pair{-2, -2}, std::pair{-3, 0}}) {
    MedFunc Func;
    Func.Blocks.resize(1);
    auto &Block = Func.Blocks.front();
    Block.Id = 0;
    MedVar Constant = MedVar::makeConst(0x4000, 4);
    Constant.Id = Id;
    Constant.SSAVer = Version;
    MedOp SetSP;
    SetSP.Opcode = NdOp::COPY;
    SetSP.Output = reg(7, 1, 4, getTargetRegInfo(Arch::X86).StackPointer);
    SetSP.addInput(Constant);
    Block.Ops.push_back(SetSP);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Output.Id = -1;
    Block.Ops.push_back(Return);

    LowToMedConverter().runRegisterDce(Func, Arch::X86);
    ASSERT_EQ(Block.Ops.size(), 2u);
    EXPECT_EQ(Block.Ops.front().Output, SetSP.Output);
    EXPECT_EQ(Block.Ops.front().Inputs[0], Constant);
  }
}

TEST(MedDCE, SharedPredecessorCyclesKeepEveryCallFrameInEitherOrder) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Reverse);
      const auto &TRI = getTargetRegInfo(Architecture);
      auto stackWrite = [&](int Version) {
        MedOp Op;
        Op.Opcode = NdOp::COPY;
        Op.Output = reg(1, Version, TRI.PointerSize, TRI.StackPointer);
        Op.Output.TheArch = Architecture;
        Op.addInput(MedVar::makeConst(0x1000 * Version, TRI.PointerSize));
        return Op;
      };
      MedFunc Func;
      Func.Blocks.resize(6);
      for (unsigned I = 0; I < Func.Blocks.size(); ++I)
        Func.Blocks[I].Id = I;
      Func.Blocks[0].Ops = {stackWrite(1)};
      Func.Blocks[0].Succs = {1};
      Func.Blocks[1].Preds = {0, 2};
      Func.Blocks[1].Succs = {2};
      Func.Blocks[2].Preds = {1};
      Func.Blocks[2].Succs = {1, 3, 5};
      Func.Blocks[3].Preds = {2};
      Func.Blocks[4].Ops = {stackWrite(2)};
      Func.Blocks[4].Succs = {5};
      Func.Blocks[5].Preds = {2, 4};
      MedOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(MedVar::makeConst(0x4000, TRI.PointerSize));
      Func.Blocks[3].Ops.push_back(Call);
      Call.Opcode = NdOp::INDIR_BR;
      Func.Blocks[5].Ops.push_back(Call);
      if (Reverse)
        std::reverse(Func.Blocks.begin(), Func.Blocks.end());

      LowToMedConverter().runRegisterDce(Func, Architecture);
      for (const auto &Block : Func.Blocks) {
        if (Block.Id == 0 || Block.Id == 4) {
          ASSERT_EQ(Block.Ops.size(), 1u);
          EXPECT_EQ(Block.Ops.front().Output,
                    stackWrite(Block.Id == 0 ? 1 : 2).Output);
        } else if (Block.Id == 3 || Block.Id == 5) {
          ASSERT_EQ(Block.Ops.size(), 1u);
          EXPECT_EQ(Block.Ops.front().Opcode,
                    Block.Id == 3 ? NdOp::CALL : NdOp::INDIR_BR);
        }
      }
    }
}

TEST(MedDCE, SeparateCallPositionsKeepTheirOwnStackPointerDefinitions) {
  MedFunc Func;
  Func.Blocks.resize(1);
  auto &Block = Func.Blocks.front();
  Block.Id = 0;
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (int Version = 1; Version <= 3; ++Version) {
    MedOp Write;
    Write.Opcode = NdOp::COPY;
    Write.Output = reg(1, Version, TRI.PointerSize, TRI.StackPointer);
    Write.addInput(MedVar::makeConst(0x1000 * Version, TRI.PointerSize));
    Block.Ops.push_back(Write);
    if (Version < 3) {
      MedOp Call;
      Call.Opcode = Version == 1 ? NdOp::CALL : NdOp::INDIR_CALL;
      Call.addInput(MedVar::makeConst(0x4000, TRI.PointerSize));
      Block.Ops.push_back(Call);
    }
  }
  LowToMedConverter().runRegisterDce(Func, Arch::X86);
  ASSERT_EQ(Block.Ops.size(), 4u);
  EXPECT_EQ(Block.Ops[0].Output.SSAVer, 1);
  EXPECT_EQ(Block.Ops[1].Opcode, NdOp::CALL);
  EXPECT_EQ(Block.Ops[2].Output.SSAVer, 2);
  EXPECT_EQ(Block.Ops[3].Opcode, NdOp::INDIR_CALL);
}

TEST(MedDCE, ReturnQueriesKeepX87SlotsAfterCallStackPointerQueries) {
  MedFunc Func;
  Func.Blocks.resize(3);
  for (unsigned I = 0; I < Func.Blocks.size(); ++I) {
    Func.Blocks[I].Id = I;
    if (I)
      Func.Blocks[I].Preds = {static_cast<int>(I - 1)};
    if (I + 1 < Func.Blocks.size())
      Func.Blocks[I].Succs = {static_cast<int>(I + 1)};
  }
  const auto &TRI = getTargetRegInfo(Arch::X86);
  int Id = 1;
  for (const auto &[Offset, Size] :
       {std::pair<uint64_t, uint16_t>{TRI.StackPointer, TRI.PointerSize},
        {x86reg::ST7, x86reg::FPURegSize},
        {x86reg::ST6, x86reg::FPURegSize}}) {
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = reg(Id, 1, Size, Offset);
    Copy.addInput(reg(Id, 0, Size, Offset));
    Func.Blocks[0].Ops.push_back(Copy);
    ++Id;
  }
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x4000, TRI.PointerSize));
  Func.Blocks[2].Ops.push_back(Call);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Func.Blocks[2].Ops.push_back(Return);

  LowToMedConverter().runRegisterDce(Func, Arch::X86);
  ASSERT_EQ(Func.Blocks[0].Ops.size(), 3u);
  EXPECT_EQ(Func.Blocks[0].Ops[0].Output.RegOff, TRI.StackPointer);
  EXPECT_EQ(Func.Blocks[0].Ops[1].Output.RegOff, x86reg::ST7);
  EXPECT_EQ(Func.Blocks[0].Ops[2].Output.RegOff, x86reg::ST6);
}

TEST(MedDCE, DuplicateBlockIdsRetainQueryLocalVisitation) {
  MedFunc Func;
  Func.Blocks.resize(2);
  auto &First = Func.Blocks[0];
  auto &Root = Func.Blocks[1];
  First.Id = Root.Id = 7;
  Root.Preds = {7};
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = reg(1, 1, 4, getTargetRegInfo(Arch::X86).StackPointer);
  Copy.addInput(MedVar::makeConst(0x1000, 4));
  First.Ops.push_back(Copy);
  Copy.Output.Kind = MedVar::Temp;
  Copy.Output.Id = 2;
  Root.Ops.push_back(Copy);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(MedVar::makeConst(0x4000, 4));
  Root.Ops.push_back(Call);

  // The root's prefix and the first block's end share the old visitation key.
  // Keeping queries separate must not turn this ambiguous graph into a new
  // implicit read of the first block's stack-pointer write.
  LowToMedConverter().runRegisterDce(Func, Arch::X86);
  EXPECT_TRUE(First.Ops.empty());
  ASSERT_EQ(Root.Ops.size(), 1u);
  EXPECT_EQ(Root.Ops.front().Opcode, NdOp::CALL);
}

TEST(MedDCE, ReachingDefinitionsAcceptTheFullBlockIdRange) {
  for (int Id :
       {std::numeric_limits<int>::min(), -2, -1,
        std::numeric_limits<int>::max() - 1, std::numeric_limits<int>::max()}) {
    SCOPED_TRACE(Id);
    MedFunc Func;
    Func.Blocks.resize(2);
    auto &Setup = Func.Blocks[0];
    Setup.Id = Id;
    auto &CallBlock = Func.Blocks[1];
    CallBlock.Id = 0;
    CallBlock.Preds = {Id};
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = reg(1, 1, 4, getTargetRegInfo(Arch::X86).StackPointer);
    Copy.addInput(MedVar::makeConst(0x1000, 4));
    Setup.Ops.push_back(Copy);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(MedVar::makeConst(0x4000, 4));
    CallBlock.Ops.push_back(Call);

    LowToMedConverter().runRegisterDce(Func, Arch::X86);
    ASSERT_EQ(Setup.Ops.size(), 1u);
    EXPECT_EQ(Setup.Ops.front().Output, Copy.Output);
    ASSERT_EQ(CallBlock.Ops.size(), 1u);
    EXPECT_EQ(CallBlock.Ops.front().Opcode, NdOp::CALL);
  }
}
TEST(MedDCE, MipsUnpredictableDefinitionsAreIndependentAndSSAOwned) {
  for (Arch Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Observed : {0u, 1u, 2u})
      for (bool Overwrite : {false, true}) {
        SCOPED_TRACE(testing::Message() << unsigned(Target) << ':' << Observed
                                        << ':' << Overwrite);
        const unsigned Width = isMIPS64(Target) ? 8 : 4;
        MedFunc Func;
        Func.Blocks.resize(2);
        Func.Blocks[0].Id = 0;
        Func.Blocks[0].Succs = {1};
        Func.Blocks[1].Id = 1;
        Func.Blocks[1].Preds = {0};
        const auto Value = [&](int Id, int Version, uint64_t Offset) {
          auto V = reg(Id, Version, Width, Offset);
          V.TheArch = Target;
          return V;
        };
        const auto HI = Value(1, 1, mipsreg::HI);
        const auto LO = Value(2, 1, mipsreg::LO);
        for (auto Output : {HI, LO}) {
          MedOp Op;
          Op.Opcode = NdOp::INTRINSIC;
          Op.Output = Output;
          Op.addInput(MedVar::makeConst(uint64_t(Intrinsic::MIPSUnpredictable),
                                        sizeof(Intrinsic)));
          Func.Blocks[0].Ops.push_back(Op);
        }
        auto Input = Observed == 1 ? HI : LO;
        if (Overwrite) {
          MedOp Copy;
          Copy.Opcode = NdOp::COPY;
          Copy.Output = Input;
          Copy.Output.SSAVer = 2;
          Copy.addInput(MedVar::makeConst(123, Width));
          Func.Blocks[1].Ops.push_back(Copy);
          Input = Copy.Output;
        }
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.addInput(Observed ? Input : MedVar::makeConst(7, Width));
        Func.Blocks[1].Ops.push_back(Return);
        LowToMedConverter().runRegisterDce(Func, Target);
        EXPECT_EQ(Func.Blocks[0].Ops.size(), Observed && !Overwrite ? 1u : 0u);
        if (Observed && !Overwrite)
          EXPECT_EQ(Func.Blocks[0].Ops.front().Output, Input);
      }
}

TEST(MedDCE, DeadMipsMergeResultsDropOldValuesButKeepBothReads) {
  for (Arch Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Observe : {0u, 1u, 2u}) {
      SCOPED_TRACE(testing::Message() << unsigned(Target) << ':' << Observe);
      const unsigned Width = isMIPS64(Target) ? 8 : 4;
      MedFunc Func;
      Func.Blocks.resize(2);
      Func.Blocks[0].Id = 0;
      Func.Blocks[0].Succs = {1};
      Func.Blocks[1].Id = 1;
      Func.Blocks[1].Preds = {0};
      auto Old = reg(1, 1, 4, mipsreg::r(8));
      Old.TheArch = Target;
      MedOp Seed;
      Seed.Opcode = NdOp::COPY;
      Seed.Output = Old;
      Seed.addInput(MedVar::makeConst(0x12345678, 4));
      Func.Blocks[0].Ops.push_back(Seed);
      MedVar Values[2];
      for (unsigned I = 0; I != 2; ++I) {
        auto &Value = Values[I];
        Value = Old;
        Value.Kind = MedVar::Temp;
        Value.Id = I + 2;
        MedOp Load;
        Load.Opcode = NdOp::INTRINSIC;
        Load.Output = Value;
        Load.addInput(MedVar::makeConst(
            uint64_t(I ? Intrinsic::MIPSLwr : Intrinsic::MIPSLwl),
            sizeof(Intrinsic)));
        Load.addInput(MedVar::makeConst(0x1001, Width));
        Load.addInput(I ? Values[0] : Old);
        Func.Blocks[0].Ops.push_back(Load);
      }
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Observe ? Values[Observe - 1] : MedVar::makeConst(7, 4));
      Func.Blocks[1].Ops.push_back(Return);
      LowToMedConverter().runRegisterDce(Func, Target);
      const auto &Ops = Func.Blocks[0].Ops;
      ASSERT_EQ(Ops.size(), Observe ? 3u : 2u);
      for (unsigned I = 0; I != 2; ++I) {
        const auto &Load = Ops[I + (Observe ? 1 : 0)];
        EXPECT_EQ(Load.Opcode, NdOp::INTRINSIC);
        EXPECT_EQ(Load.Inputs[1].ConstVal, 0x1001u);
        const bool Live = Observe == 2 || (Observe == 1 && I == 0);
        EXPECT_EQ(Load.Inputs[2].isConst(), !Live);
        if (!Live)
          EXPECT_EQ(Load.Inputs[2].ConstVal, 0u);
      }
    }
}

} // namespace
