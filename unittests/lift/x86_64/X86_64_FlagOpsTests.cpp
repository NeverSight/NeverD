#include "../../../lib/ir/med/flags/MedFlagsDetail.h"
#include "NeverDLiftFixture.h"

#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

class X86_64_FlagOps : public NeverDLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_flag_ops.o"; }

TEST_F(X86_64_FlagOps, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_flag_ops.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_FlagOps, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_FlagOps, StcClcLifts) {
  verifyLowIRContains(testObj(), "test_stc_clc", "COPY");
}

TEST_F(X86_64_FlagOps, TestAndJzLifts) {
  verifyLowIRContains(testObj(), "test_test_and_jz", "INT_AND");
}

TEST_F(X86_64_FlagOps, CmpFlagsLifts) {
  verifyLowIRContains(testObj(), "test_cmp_flags", "INT_SUB");
}

TEST_F(X86_64_FlagOps, SetaLifts) {
  verifyLowIRContains(testObj(), "test_seta", "INT_SUB");
}

TEST_F(X86_64_FlagOps, SetgeLifts) {
  verifyLowIRContains(testObj(), "test_setge", "INT_SUB");
}

TEST_F(X86_64_FlagOps, CmovaLifts) {
  auto r = liftToLowIR(testObj());
  ASSERT_EQ(r.exitCode, 0);
  EXPECT_TRUE(r.out.find("INT_SUB") != std::string::npos)
      << "Expected CMP-related op in LowIR for cmov";
}

TEST_F(X86_64_FlagOps, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}

namespace {

neverd::MedVar flagTestReg(int Id, int Version, uint16_t Size,
                           uint64_t Offset) {
  neverd::MedVar Var;
  Var.Kind = neverd::MedVar::Reg;
  Var.Id = Id;
  Var.SSAVer = Version;
  Var.Size = Size;
  Var.RegOff = Offset;
  return Var;
}

neverd::MedVar flagTestTemp(int Id, uint16_t Size) {
  neverd::MedVar Var;
  Var.Kind = neverd::MedVar::Temp;
  Var.Id = Id;
  Var.Size = Size;
  return Var;
}

neverd::MedOp flagTestOp(neverd::NdOp Opcode, neverd::MedVar Output,
                         std::initializer_list<neverd::MedVar> Inputs,
                         neverd::va_t Addr) {
  neverd::MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Addr;
  for (neverd::MedVar Input : Inputs)
    Op.addInput(Input);
  return Op;
}

} // namespace

TEST(MedFlagsX86, SignedCompareAcceptsSameWidthSelfSlicesOnly) {
  using namespace neverd;
  const auto &TRI = getTargetRegInfo(Arch::X64);
  constexpr va_t CmpAddr = 0x220;
  const MedVar Wide = flagTestReg(1, 0, 8, 0x100);
  const MedVar Left = flagTestReg(2, 0, 4, 0x100);
  const MedVar LeftAgain = flagTestReg(2, 1, 4, 0x100);
  const MedVar Right = flagTestTemp(3, 4);
  const MedVar Diff = flagTestTemp(4, 4);
  MedVar ZF = flagTestReg(5, 0, 1, TRI.FlagZF);
  MedVar SF = flagTestReg(6, 0, 1, TRI.FlagNF);
  MedVar OF = flagTestReg(7, 0, 1, TRI.FlagVF);
  ZF.Kind = SF.Kind = OF.Kind = MedVar::Flag;
  const MedVar Zero = MedVar::makeConst(0, 4);
  std::vector<MedOp> Ops = {
      flagTestOp(NdOp::SUBBYTES, Left, {Wide, Zero}, CmpAddr),
      flagTestOp(NdOp::INT_SUB, Diff, {Left, Right}, CmpAddr),
      flagTestOp(NdOp::INT_EQUAL, ZF, {Diff, Zero}, CmpAddr),
      flagTestOp(NdOp::INT_SLESS, SF, {Diff, Zero}, CmpAddr),
      flagTestOp(NdOp::SUBBYTES, LeftAgain, {Wide, Zero}, CmpAddr),
      flagTestOp(NdOp::INT_SBOR, OF, {LeftAgain, Right}, CmpAddr),
  };
  CmpSource Cmp;
  Cmp.A = Left;
  Cmp.B = Right;
  Cmp.Valid = Cmp.FromSub = true;
  Cmp.SourceOpIndex = 1;
  Cmp.Result = Diff;
  EXPECT_TRUE(carryFlagMatchesCmpX86(Ops, 5, CondCode::SLE, Cmp, TRI));

  auto Rewritten = Ops;
  const MedVar NewWide = flagTestReg(1, 1, 8, 0x100);
  Rewritten.insert(
      Rewritten.begin() + 4,
      flagTestOp(NdOp::COPY, NewWide, {MedVar::makeConst(7, 8)}, CmpAddr));
  Rewritten[5].Inputs[0] = NewWide;
  EXPECT_FALSE(carryFlagMatchesCmpX86(Rewritten, 6, CondCode::SLE, Cmp, TRI));

  // Even a stale reference to the old SSA parent cannot bridge a physical
  // write in the middle of the claimed single-instruction flag window.
  Rewritten[5].Inputs[0] = Wide;
  EXPECT_FALSE(carryFlagMatchesCmpX86(Rewritten, 6, CondCode::SLE, Cmp, TRI));

  // The RHS may share the wide parent's physical register and SSA identity.
  // A write to its high half after the subtraction invalidates the window.
  auto AliasedRight = Ops;
  const MedVar HighHalf = flagTestReg(1, 0, 4, 0x104);
  AliasedRight[1].Inputs[1] = HighHalf;
  AliasedRight[5].Inputs[1] = HighHalf;
  auto AliasedCmp = Cmp;
  AliasedCmp.B = HighHalf;
  EXPECT_TRUE(
      carryFlagMatchesCmpX86(AliasedRight, 5, CondCode::SLE, AliasedCmp, TRI));
  AliasedRight.insert(AliasedRight.begin() + 4,
                      flagTestOp(NdOp::COPY, flagTestReg(1, 1, 4, 0x104),
                                 {MedVar::makeConst(7, 4)}, CmpAddr));
  EXPECT_FALSE(
      carryFlagMatchesCmpX86(AliasedRight, 6, CondCode::SLE, AliasedCmp, TRI));

  auto WrongWidth = Ops;
  WrongWidth[5].Inputs[0].Size = 2;
  EXPECT_FALSE(carryFlagMatchesCmpX86(WrongWidth, 5, CondCode::SLE, Cmp, TRI));

  auto OtherByte = Ops;
  OtherByte[4].Inputs[1] = MedVar::makeConst(1, 4);
  EXPECT_FALSE(carryFlagMatchesCmpX86(OtherByte, 5, CondCode::SLE, Cmp, TRI));

  auto OtherInstruction = Ops;
  OtherInstruction[5].Addr = CmpAddr + 1;
  EXPECT_FALSE(
      carryFlagMatchesCmpX86(OtherInstruction, 5, CondCode::SLE, Cmp, TRI));

  OtherInstruction = Ops;
  OtherInstruction.insert(OtherInstruction.begin() + 4,
                          flagTestOp(NdOp::COPY, flagTestTemp(10, 4),
                                     {MedVar::makeConst(7, 4)}, CmpAddr + 1));
  EXPECT_FALSE(
      carryFlagMatchesCmpX86(OtherInstruction, 6, CondCode::SLE, Cmp, TRI));
}

TEST(MedFlagsX86, SelectOnZeroFlagIgnoresTheLaterParityCompare) {
  // `cmp rdi, rsi; cmove rax, rdx, rcx`: CMP writes ZF and SF from the
  // difference, then PF as INT_EQUAL(popcount(low byte) & 1, 0).  The
  // select must fold onto the compared operands, not onto the parity
  // AND, which would look like a TEST that is nearer the select.
  using namespace neverd;
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "select_on_zero_flag_after_cmp";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  Block.EndAddr = 0x1020;
  auto op = [&](NdOp Opcode, va_t Addr, NdVar Output,
                std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Addr = Addr;
    Op.Output = Output;
    for (const NdVar &Input : Inputs)
      Op.addInput(Input);
    Block.Ops.push_back(Op);
  };
  const NdVar Left = NdVar::reg(x86reg::RDI, 8);
  const NdVar Right = NdVar::reg(x86reg::RSI, 8);
  const NdVar Taken = NdVar::reg(x86reg::RDX, 8);
  const NdVar Kept = NdVar::reg(x86reg::RCX, 8);
  for (const NdVar &LiveIn : {Left, Right, Taken, Kept})
    op(NdOp::COPY, 0x1000, LiveIn, {LiveIn});
  const NdVar Difference = NdVar::tmp(0x10000, 8);
  const NdVar LowByte = NdVar::tmp(0x10010, 1);
  const NdVar Bits = NdVar::tmp(0x10018, 1);
  const NdVar Parity = NdVar::tmp(0x10020, 1);
  op(NdOp::INT_SUB, 0x1004, Difference, {Left, Right});
  op(NdOp::INT_EQUAL, 0x1004, NdVar::reg(x86reg::ZF, 1),
     {Difference, NdVar::cst(0, 8)});
  op(NdOp::INT_SLESS, 0x1004, NdVar::reg(x86reg::SF, 1),
     {Difference, NdVar::cst(0, 8)});
  op(NdOp::SUBBYTES, 0x1004, LowByte, {Difference, NdVar::cst(0, 4)});
  op(NdOp::POPCOUNT, 0x1004, Bits, {LowByte});
  op(NdOp::INT_AND, 0x1004, Parity, {Bits, NdVar::cst(1, 1)});
  op(NdOp::INT_EQUAL, 0x1004, NdVar::reg(x86reg::PF, 1),
     {Parity, NdVar::cst(0, 1)});
  op(NdOp::INT_LESS, 0x1004, NdVar::reg(x86reg::CF, 1), {Left, Right});
  op(NdOp::INT_SBOR, 0x1004, NdVar::reg(x86reg::OF, 1), {Left, Right});
  const NdVar Condition = NdVar::tmp(0x10028, 1);
  op(NdOp::COPY, 0x1008, Condition, {NdVar::reg(x86reg::ZF, 1)});
  const NdVar Result = NdVar::reg(x86reg::RAX, 8);
  op(NdOp::SELECT, 0x1008, Result, {Condition, Taken, Kept});
  op(NdOp::RETURN, 0x100c, NdVar(), {Result});
  Low.Blocks.push_back(std::move(Block));

  const MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 1u);
  const auto &Ops = Med.Blocks.front().Ops;
  auto defOf = [&](const MedVar &V) -> const MedOp * {
    for (const MedOp &Op : Ops)
      if (!V.isConst() && Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
          Op.Output.SSAVer == V.SSAVer)
        return &Op;
    return nullptr;
  };
  const auto Select = std::find_if(Ops.begin(), Ops.end(), [](const MedOp &Op) {
    return Op.Opcode == NdOp::SELECT;
  });
  ASSERT_NE(Select, Ops.end());
  std::vector<MedVar> Work{Select->Inputs[0]};
  size_t Visited = 0;
  while (!Work.empty() && Visited++ < 64) {
    const MedVar Current = Work.back();
    Work.pop_back();
    const MedOp *Def = defOf(Current);
    if (!Def)
      continue;
    EXPECT_NE(Def->Opcode, NdOp::POPCOUNT)
        << "the select condition reads the parity of the difference";
    for (uint8_t I = 0; I < Def->NumInputs; ++I)
      Work.push_back(Def->Inputs[I]);
  }
}
