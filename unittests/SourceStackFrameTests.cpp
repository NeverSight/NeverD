#include "gtest/gtest.h"

#include "neverd/ir/SourceStackFrame.h"
#include "neverd/ir/TargetRegInfo.h"
using namespace neverd;
namespace {
LowOp op(NdOp Code, va_t PC, NdVar Output,
         std::initializer_list<NdVar> Inputs) {
  LowOp O;
  O.Opcode = Code;
  O.Addr = PC;
  O.Output = Output;
  for (auto V : Inputs)
    O.addInput(V);
  return O;
}
LowFunc fixture() {
  const auto SP = NdVar::reg(getTargetRegInfo(Arch::X64).StackPointer, 8);
  LowFunc F;
  F.Entry = 0x1000;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.Ops = {op(NdOp::INT_SUB, 0x1000, SP, {SP, NdVar::scalar(32, 8)}),
           op(NdOp::STORE, 0x1004, {}, {SP, NdVar::scalar(7, 4)}),
           op(NdOp::LOAD, 0x1008, NdVar::reg(0, 4), {SP}),
           op(NdOp::RETURN, 0x100C, {}, {})};
  F.Blocks.push_back(B);
  return F;
}
TEST(SourceStackFrame, TracksActualWidthsAndEntryCoordinates) {
  const auto F = fixture();
  const auto A = analyzeSourceStackFrame(F, Arch::X64);
  ASSERT_TRUE(A.Error.empty()) << A.Error;
  ASSERT_EQ(A.Accesses.size(), 2u);
  EXPECT_EQ(A.Accesses[0].Offset, -32);
  EXPECT_EQ(A.Accesses[0].Bytes, 4);
  EXPECT_EQ(A.Instructions.at(0x1004).Before, -32);
  EXPECT_FALSE(A.Instructions.at(0x100C).Correctable);
}
TEST(SourceStackFrame, RepairsUnknownSPAndKeepsOriginalGraph) {
  auto F = fixture();
  F.Blocks[0].Ops[0].Opcode = NdOp::INT_AND;
  EXPECT_FALSE(
      analyzeSourceStackFrame(F, Arch::X64).Instructions.at(0x1004).Before);
  SourceStackFrame P;
  P.Points[0x1000] = -40;
  const auto A = analyzeSourceStackFrame(F, Arch::X64, P);
  ASSERT_EQ(A.Accesses.size(), 2u);
  EXPECT_EQ(A.Accesses[0].Offset, -40);
  std::string Error;
  const auto Original = F.Blocks[0].Ops;
  auto Projected = projectSourceStackFrame(F, Arch::X64, P, Error);
  ASSERT_TRUE(Projected) << Error;
  EXPECT_EQ(F.Blocks[0].Ops, Original);
  EXPECT_EQ(Projected->Blocks[0].Ops.size(), Original.size() + 2);
  EXPECT_EQ(Projected->FunctionTemporaries.size(), 1u);
}
TEST(SourceStackFrame, ConflictingJoinRemainsUnknown) {
  auto F = fixture();
  const auto SP = F.Blocks[0].Ops[0].Output;
  F.Blocks[0].Ops.resize(1);
  F.Blocks[0].Succs = {1, 2};
  LowBlock B;
  B.Id = 1;
  B.StartAddr = 0x1010;
  B.Succs = {3};
  B.Ops = {op(NdOp::INT_SUB, 0x1010, SP, {SP, NdVar::scalar(8, 8)})};
  F.Blocks.push_back(B);
  B.Id = 2;
  B.StartAddr = 0x1020;
  B.Ops.clear();
  F.Blocks.push_back(B);
  B.Id = 3;
  B.StartAddr = 0x1030;
  B.Succs.clear();
  B.Ops = {op(NdOp::LOAD, 0x1030, NdVar::reg(0, 4), {SP})};
  F.Blocks.push_back(B);
  auto A = analyzeSourceStackFrame(F, Arch::X64);
  EXPECT_TRUE(A.Error.empty());
  EXPECT_FALSE(A.Instructions.at(0x1030).Before);
  EXPECT_TRUE(A.Accesses.empty());
}
TEST(SourceStackFrame, StableLoopAndAArch64OffsetsConverge) {
  auto F = fixture();
  F.Blocks[0].Succs = {1};
  F.Blocks[0].Ops.resize(1);
  LowBlock Loop;
  Loop.Id = 1;
  Loop.StartAddr = 0x1010;
  Loop.Succs = {1};
  const auto SP = F.Blocks[0].Ops[0].Output;
  Loop.Ops = {op(NdOp::LOAD, 0x1010, NdVar::reg(0, 4), {SP})};
  F.Blocks.push_back(Loop);
  EXPECT_EQ(
      analyzeSourceStackFrame(F, Arch::X64).Instructions.at(0x1010).Before,
      -32);
  const auto ARM_SP =
      NdVar::reg(getTargetRegInfo(Arch::AArch64).StackPointer, 8);
  F.Blocks[0].Ops[0].Output = ARM_SP;
  F.Blocks[0].Ops[0].Inputs[0] = ARM_SP;
  F.Blocks[1].Ops[0].Inputs[0] = ARM_SP;
  auto Flow = analyzeSourceStackFrame(F, Arch::AArch64);
  ASSERT_TRUE(Flow.Error.empty());
  ASSERT_EQ(Flow.Accesses.size(), 1u);
  EXPECT_EQ(Flow.Accesses[0].Offset, -32);
}
TEST(SourceStackFrame, ExpiresInstructionTempsAndPartialSPWrites) {
  auto F = fixture();
  auto SP = F.Blocks[0].Ops[0].Output;
  F.Blocks[0].Ops[0] = op(NdOp::COPY, 0x1000, NdVar::tmp(100, 8), {SP});
  F.Blocks[0].Ops[1] =
      op(NdOp::LOAD, 0x1004, NdVar::reg(0, 4), {NdVar::tmp(100, 8)});
  auto A = analyzeSourceStackFrame(F, Arch::X64);
  ASSERT_EQ(A.Accesses.size(), 1u);
  EXPECT_EQ(A.Accesses[0].Instruction, 0x1008u);
  F.Blocks[0].Ops[0] =
      op(NdOp::COPY, 0x1000, NdVar::reg(SP.Offset, 1), {NdVar::scalar(0, 1)});
  EXPECT_FALSE(
      analyzeSourceStackFrame(F, Arch::X64).Instructions.at(0x1004).Before);
}
TEST(SourceStackFrame, RefusesBranchesBudgetsAndIndependentRoots) {
  auto F = fixture();
  SourceStackFrame P;
  P.Points[0x100C] = 0;
  EXPECT_FALSE(analyzeSourceStackFrame(F, Arch::X64, P).Error.empty());
  P.Points.clear();
  P.Points[0x1000] = 1LL << 40;
  EXPECT_FALSE(analyzeSourceStackFrame(F, Arch::X64, P).Error.empty());
  P.Points[0x1000] = -32;
  F.ModuleAnalysisRoots.insert(0x1008);
  LowBlock B;
  B.Id = 1;
  B.StartAddr = 0x1008;
  F.Blocks.push_back(B);
  std::string Error;
  EXPECT_FALSE(projectSourceStackFrame(F, Arch::X64, P, Error));
  EXPECT_FALSE(analyzeSourceStackFrame(F, Arch::X86).Error.empty());
}

TEST(SourceStackFrame, RefusesCorrectionsWhenEntryCanExecuteAgain) {
  for (bool SelfLoop : {false, true}) {
    auto F = fixture();
    F.Blocks[0].Ops.resize(1);
    F.Blocks[0].Succs = {SelfLoop ? 0 : 1};
    if (!SelfLoop) {
      LowBlock Backedge;
      Backedge.Id = 1;
      Backedge.StartAddr = 0x1010;
      Backedge.Succs = {0};
      F.Blocks.push_back(Backedge);
    }
    SourceStackFrame Policy;
    Policy.Points[0x1000] = -32;
    std::string Error;
    // Capturing entry SP in a loop would make each iteration's correction
    // relative to the preceding iteration instead of the function entry.
    EXPECT_FALSE(projectSourceStackFrame(F, Arch::X64, Policy, Error));
    EXPECT_NE(Error.find("entry"), std::string::npos);
    // Inspection and naming do not insert a carrier and remain available.
    EXPECT_TRUE(projectSourceStackFrame(F, Arch::X64, {}, Error));
  }
}

TEST(SourceStackFrame, RequiresTheWholeTemporaryToSurviveAnInstruction) {
  auto F = fixture();
  const auto SP = F.Blocks[0].Ops[0].Output;
  const auto Pointer = NdVar::tmp(TmpBase, 8);
  F.Blocks[0].Ops[0] = op(NdOp::COPY, 0x1000, Pointer, {SP});
  F.Blocks[0].Ops[1] = op(NdOp::LOAD, 0x1004, NdVar::reg(0, 4), {Pointer});
  F.Blocks[0].Ops.resize(2);
  F.FunctionTemporaries = {{Pointer.Offset, 1}};
  EXPECT_TRUE(analyzeSourceStackFrame(F, Arch::X64).Accesses.empty());
  F.FunctionTemporaries = {{Pointer.Offset, 8}};
  ASSERT_EQ(analyzeSourceStackFrame(F, Arch::X64).Accesses.size(), 1u);
}
TEST(SourceStackFrame, EntryPrefixRetainsOnlyExactCoordinates) {
  for (unsigned Mode = 0; Mode < 9; ++Mode) {
    SCOPED_TRACE(Mode);
    auto F = fixture();
    const auto SP = F.Blocks[0].Ops[0].Output;
    const auto FP = NdVar::reg(getTargetRegInfo(Arch::X64).FramePointer, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    auto &Ops = F.Blocks[0].Ops;
    Ops = {op(NdOp::INT_SUB, 0x1000, SP, {SP, NdVar::scalar(32, 8)}),
           op(NdOp::COPY, 0x1004, FP, {SP}),
           op(NdOp::INT_ADD, 0x1008, Address, {FP, NdVar::scalar(72, 8)}),
           op(NdOp::LOAD, 0x1008, NdVar::reg(0, 8), {Address})};
    Ops.back().Seq = 1;
    if (Mode == 1)
      Ops[1].Output.Size = 4;
    if (Mode == 2)
      Ops.back().Addr = 0x100C;
    if (Mode == 3)
      Ops[2].Opcode = NdOp::INT_OR;
    if (Mode == 4)
      Ops[2].Inputs[1] = NdVar::scalar(UINT64_MAX >> 1, 8);
    if (Mode >= 5 && Mode <= 7)
      Ops.insert(Ops.begin() + 2, op(Mode == 5   ? NdOp::CALL
                                     : Mode == 6 ? NdOp::BRANCH
                                                 : NdOp::INTRINSIC,
                                     0x1006, {}, {NdVar::scalar(0x2000, 8)}));
    size_t Remaining = Mode == 8 ? 1 : 256;
    const auto Accesses = sourceEntryStackAccesses(Ops, Arch::X64, Remaining);
    if (Mode == 0) {
      ASSERT_EQ(Accesses.size(), 1u);
      EXPECT_EQ(Accesses.front().Offset, 40);
      EXPECT_EQ(Accesses.front().Bytes, 8);
      EXPECT_EQ(Accesses.front().Sequence, 1);
    } else
      EXPECT_TRUE(Accesses.empty());
  }
}
} // namespace
