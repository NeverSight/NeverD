//===- MedSSAMultiRootTests.cpp - Multiple-root SSA tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

namespace {

using namespace neverd;

TEST(MedSSAMultiRoot, MergesDefinitionsFromIndependentSourcesAtAJoin) {
  constexpr va_t EntryVA = 0x1000;
  constexpr va_t ResumeVA = 0x1100;
  constexpr va_t JoinVA = 0x1200;
  constexpr uint64_t EntryValue = 0x11;
  constexpr uint64_t ResumeValue = 0x22;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "two_source_join";
  Low.Blocks.resize(3);

  auto BuildSource = [&](LowBlock &Block, int Id, va_t Address,
                         uint64_t Value) {
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 1;
    Block.Succs = {2};

    LowOp Define;
    Define.Opcode = NdOp::COPY;
    Define.Addr = Address;
    Define.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
    Define.addInput(NdVar::cst(Value, TRI.PointerSize));
    Block.Ops.push_back(Define);

    LowOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.Addr = Address;
    Branch.addInput(NdVar::cst(JoinVA, TRI.PointerSize));
    Block.Ops.push_back(Branch);
  };

  BuildSource(Low.Blocks[0], 0, EntryVA, EntryValue);
  BuildSource(Low.Blocks[1], 1, ResumeVA, ResumeValue);

  LowBlock &Join = Low.Blocks[2];
  Join.Id = 2;
  Join.StartAddr = JoinVA;
  Join.EndAddr = JoinVA + 1;
  Join.Preds = {0, 1};
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = JoinVA;
  Return.addInput(NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
  Join.Ops.push_back(Return);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 3u);
  const MedBlock &MedJoin = Med.Blocks[2];
  ASSERT_EQ(MedJoin.Phis.size(), 1u);

  const PhiNode &Phi = MedJoin.Phis.front();
  ASSERT_EQ(Phi.Output.Kind, MedVar::Reg);
  EXPECT_EQ(Phi.Output.RegOff, TRI.IntReturnReg);
  ASSERT_EQ(Phi.Args.size(), 2u);
  EXPECT_EQ(Phi.Args[0].first, 0);
  EXPECT_EQ(Phi.Args[1].first, 1);
  EXPECT_NE(Phi.Args[0].second.SSAVer, Phi.Args[1].second.SSAVer);

  ASSERT_FALSE(MedJoin.Ops.empty());
  const MedOp &MedReturn = MedJoin.Ops.back();
  ASSERT_EQ(MedReturn.Opcode, NdOp::RETURN);
  ASSERT_EQ(MedReturn.NumInputs, 1u);
  EXPECT_EQ(MedReturn.Inputs[0].Id, Phi.Output.Id);
  EXPECT_EQ(MedReturn.Inputs[0].SSAVer, Phi.Output.SSAVer);
  EXPECT_TRUE(verifyMedFunc(Med, "multi-root-join"));
}

TEST(MedSSAMultiRoot, DoesNotInventADownstreamRootFromBlockOrder) {
  constexpr va_t EntryVA = 0x2000;
  constexpr va_t DownstreamVA = 0x2100;
  constexpr va_t SourceVA = 0x2200;
  constexpr uint64_t SourceValue = 0x33;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "source_scc_order";
  Low.Blocks.resize(3);

  LowBlock &Entry = Low.Blocks[0];
  Entry.Id = 0;
  Entry.StartAddr = EntryVA;
  Entry.EndAddr = EntryVA + 1;
  LowOp EntryReturn;
  EntryReturn.Opcode = NdOp::RETURN;
  EntryReturn.Addr = EntryVA;
  Entry.Ops.push_back(EntryReturn);

  // This sink deliberately has a lower block id than its disconnected source.
  // Root discovery must follow the condensation graph, not vector order.
  LowBlock &Downstream = Low.Blocks[1];
  Downstream.Id = 1;
  Downstream.StartAddr = DownstreamVA;
  Downstream.EndAddr = DownstreamVA + 1;
  Downstream.Preds = {2};
  LowOp DownstreamReturn;
  DownstreamReturn.Opcode = NdOp::RETURN;
  DownstreamReturn.Addr = DownstreamVA;
  DownstreamReturn.addInput(
      NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
  Downstream.Ops.push_back(DownstreamReturn);

  LowBlock &Source = Low.Blocks[2];
  Source.Id = 2;
  Source.StartAddr = SourceVA;
  Source.EndAddr = SourceVA + 1;
  Source.Succs = {1};
  LowOp Define;
  Define.Opcode = NdOp::COPY;
  Define.Addr = SourceVA;
  Define.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  Define.addInput(NdVar::cst(SourceValue, TRI.PointerSize));
  Source.Ops.push_back(Define);
  LowOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = SourceVA;
  Branch.addInput(NdVar::cst(DownstreamVA, TRI.PointerSize));
  Source.Ops.push_back(Branch);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 3u);
  const MedBlock &MedDownstream = Med.Blocks[1];
  ASSERT_TRUE(MedDownstream.Phis.empty());
  ASSERT_FALSE(MedDownstream.Ops.empty());
  const MedOp &MedReturn = MedDownstream.Ops.back();
  ASSERT_EQ(MedReturn.Opcode, NdOp::RETURN);
  ASSERT_EQ(MedReturn.NumInputs, 1u);
  ASSERT_TRUE(MedReturn.Inputs[0].isConst());
  EXPECT_EQ(MedReturn.Inputs[0].ConstVal, SourceValue);
  EXPECT_TRUE(verifyMedFunc(Med, "source-scc-order"));
}

TEST(MedTempIdentity, SeparatesReusedLowTempSlotsByWidth) {
  constexpr va_t EntryVA = 0x3000;

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "temp_width_identity";
  Low.Blocks.resize(1);

  LowBlock &Block = Low.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = EntryVA;
  Block.EndAddr = EntryVA + 4;

  LowOp ByteDef;
  ByteDef.Opcode = NdOp::POPCOUNT;
  ByteDef.Addr = EntryVA;
  ByteDef.Output = NdVar::tmp(TmpBase, 1);
  ByteDef.addInput(NdVar::scalar(0x5a, 1));
  Block.Ops.push_back(ByteDef);

  LowOp WideDef;
  WideDef.Opcode = NdOp::POPCOUNT;
  WideDef.Addr = EntryVA + 1;
  WideDef.Output = NdVar::tmp(TmpBase, 2);
  WideDef.addInput(NdVar::scalar(0x1234, 2));
  Block.Ops.push_back(WideDef);

  LowOp WideUpdate;
  WideUpdate.Opcode = NdOp::POPCOUNT;
  WideUpdate.Addr = EntryVA + 2;
  WideUpdate.Output = NdVar::tmp(TmpBase, 2);
  WideUpdate.addInput(NdVar::scalar(0xabcd, 2));
  Block.Ops.push_back(WideUpdate);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = EntryVA + 3;
  Block.Ops.push_back(Return);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 1u);
  auto FindOp = [&](va_t Addr, NdOp Opcode) -> const MedOp * {
    for (const MedOp &Op : Med.Blocks.front().Ops)
      if (Op.Addr == Addr && Op.Opcode == Opcode)
        return &Op;
    return nullptr;
  };
  const MedOp *MedByteDef = FindOp(EntryVA, NdOp::POPCOUNT);
  const MedOp *MedWideDef = FindOp(EntryVA + 1, NdOp::POPCOUNT);
  const MedOp *MedWideUpdate = FindOp(EntryVA + 2, NdOp::POPCOUNT);
  ASSERT_NE(MedByteDef, nullptr);
  ASSERT_NE(MedWideDef, nullptr);
  ASSERT_NE(MedWideUpdate, nullptr);
  EXPECT_NE(MedByteDef->Output.Id, MedWideDef->Output.Id)
      << "a Med SSA identity must have exactly one value width";
  EXPECT_EQ(MedWideDef->Output.Id, MedWideUpdate->Output.Id);
  EXPECT_NE(MedWideDef->Output.SSAVer, MedWideUpdate->Output.SSAVer);
}

} // namespace
