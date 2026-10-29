//===- FiniteScalarBranchTests.cpp - Finite scalar branch proofs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete value covers and invalidation by newly discovered CFG paths.

#include "../../../lib/ir/low/LowScalarExpression.h"
#include "FiniteScalarBranchTestImage.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <vector>

namespace {
using namespace neverd;

LowFunc build(const BinaryImage &Image, const std::set<va_t> &Entries = {}) {
  Decoder Dec;
  if (!Dec.init(Image)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  auto Function =
      Builder.build(Image, Dec, Image.Entry, "finite_dispatch_probe");
  auto Error = validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  return Function;
}

TEST(FiniteScalarBranch, LogicalDispatchHasNoPhysicalTableAuthority) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Architecture) << ':' << unsigned(Format));
      const auto Function =
          build(finite_scalar_branch_test::image(Architecture, Format));
      ASSERT_EQ(Function.JumpTables.size(), 1u);
      const auto &JT = Function.JumpTables.front();
      EXPECT_EQ(JT.Targets,
                (std::vector<va_t>{0x1080, 0x1088, 0x1090, 0x1098}));
      EXPECT_EQ(JT.CaseLabels,
                (std::vector<int64_t>{0x1080, 0x1088, 0x1090, 0x1098}));
      ASSERT_EQ(JT.SelectorUseRefs.size(), 1u);
      EXPECT_TRUE(JT.OriginalAddressSelector);
      EXPECT_EQ(JT.SelectorUseRefs.front().ExpectedOpcode, NdOp::INDIR_BR);
      EXPECT_FALSE(JT.HasBaseAddr);
      EXPECT_EQ(JT.EntrySize, 0u);
      EXPECT_FALSE(JT.HasDispatchSlotMap);
      EXPECT_TRUE(JT.SlotIndices.empty());
      EXPECT_TRUE(JT.StorageRanges.empty());
      EXPECT_TRUE(JT.AuthenticatedTableLoads.empty());
      EXPECT_TRUE(JT.SuppressibleRelocationSlots.empty());
      ASSERT_FALSE(Function.Blocks.empty());
      EXPECT_EQ(Function.Blocks.front().Succs.size(), 4u);
    }
}

TEST(FiniteScalarBranch, UnknownWiderAndForeignTargetsAreNotPartialCovers) {
  for (unsigned Change = 0; Change != 4; ++Change) {
    SCOPED_TRACE(Change);
    auto Image =
        finite_scalar_branch_test::image(Arch::X64, BinaryFormat::COFF);
    auto &Bytes = Image.Segments.front().Data;
    std::set<va_t> Entries;
    if (Change == 0)
      Bytes[3] = 0xc8; // or eax,3 does not bound the unknown input
    else if (Change == 1)
      Bytes[4] = 7; // half the targets have no executable bytes
    else if (Change == 2)
      Entries = {0x1000, 0x1088}; // one independent ABI entry
    else {
      // Recovered case zero re-enters after the mask with arbitrary EAX.
      Bytes[0x80] = 0x8b;
      Bytes[0x81] = 0x01; // mov eax,[rcx]
      Bytes[0x82] = 0xe9;
      writeLE<uint32_t>(Bytes.data() + 0x83, 0x1005 - 0x1087);
    }
    const auto Function = build(Image, Entries);
    EXPECT_TRUE(Function.JumpTables.empty());
  }
}

TEST(FiniteScalarBranch, RegisteredCxxResumptionsKeepTheirDynamicBranch) {
  auto Image = finite_scalar_branch_test::image(Arch::X86, BinaryFormat::COFF);
  ExceptionFunction EH;
  EH.FunctionEntry = Image.Entry;
  EH.CodeRange = {Image.Entry, Image.Entry + Image.Segments.front().Size};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Registration.emplace();
  EH.Cxx.emplace();
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();
  const auto Function = build(Image);
  ASSERT_FALSE(Function.Blocks.empty());
  ASSERT_TRUE(Function.ExceptionMetadata);
  ASSERT_TRUE(Function.ExceptionMetadata->Registration);
  ASSERT_TRUE(Function.ExceptionMetadata->Cxx);
  EXPECT_TRUE(Function.JumpTables.empty());
}

std::vector<LowOp> expression(unsigned Bytes) {
  std::vector<LowOp> Ops;
  auto Put = [&](NdOp Opcode, NdVar Output,
                 std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = 0x1000 + Ops.size();
    for (const auto Input : Inputs)
      Op.addInput(Input);
    Ops.push_back(Op);
  };
  const auto X = NdVar::reg(0, Bytes), Y = NdVar::reg(8, Bytes);
  Put(NdOp::INT_AND, Y, {X, NdVar::scalar(5, Bytes)});
  Put(NdOp::INT_SUB, Y, {NdVar::scalar(3, Bytes), Y});
  Put(NdOp::INT_MULT, Y, {Y, NdVar::scalar(8, Bytes)});
  Put(NdOp::INT_ADD, Y, {Y, NdVar::scalar(0xf8, Bytes)});
  return Ops;
}

TEST(FiniteScalarExpression, SparseMasksAndArithmeticWrapAtEveryWidth) {
  for (unsigned Bytes : {1, 2, 4, 8}) {
    SCOPED_TRACE(Bytes);
    size_t Work = limits::kMaxScalarBranchEvidenceWork;
    const auto Cover = finiteScalarExpression(
        expression(Bytes), NdVar::reg(8, Bytes), 0x2000, Work);
    ASSERT_TRUE(Cover);
    std::set<uint64_t> Expected;
    const uint64_t Mask = UINT64_MAX >> ((8 - Bytes) * 8);
    for (uint64_t Input = 0; Input != 256; ++Input)
      Expected.insert((0xf8 + 8 * (3 - (Input & 5))) & Mask);
    EXPECT_EQ(*Cover, std::vector<uint64_t>(Expected.begin(), Expected.end()));
  }
}

TEST(FiniteScalarExpression, BudgetsNeverReturnAnEnumeratedPrefix) {
  for (size_t Budget = 0; Budget < 2048; Budget += 7) {
    size_t Work = Budget;
    const auto Cover =
        finiteScalarExpression(expression(8), NdVar::reg(8, 8), 0x2000, Work);
    EXPECT_LE(Work, Budget);
    if (Cover)
      EXPECT_EQ(*Cover, (std::vector<uint64_t>{232, 240, 264, 272}));
  }
}

TEST(FiniteScalarExpression, ClobberedRegisterBytesStayUnknown) {
  for (unsigned Change = 0; Change != 4; ++Change) {
    auto Ops = expression(8);
    LowOp Clobber;
    Clobber.Addr = 0x1100;
    if (Change == 0) {
      Clobber.Opcode = NdOp::COPY;
      Clobber.Output = NdVar::reg(9, 1);
      Clobber.addInput(NdVar::reg(16, 1));
    } else if (Change == 1) {
      Clobber.Opcode = NdOp::LOAD;
      Clobber.Output = NdVar::reg(8, 8);
      Clobber.addInput(NdVar::scalar(0x5000, 8));
    } else {
      Clobber.Opcode = Change == 2 ? NdOp::CALL : NdOp::INTRINSIC;
      Clobber.addInput(NdVar::scalar(0x5000, 8));
    }
    Ops.push_back(Clobber);
    size_t Work = limits::kMaxScalarBranchEvidenceWork;
    EXPECT_FALSE(finiteScalarExpression(Ops, NdVar::reg(8, 8), 0x2000, Work));
  }
}
TEST(FiniteScalarExpression, TemporaryIdentitiesDoNotCrossInstructions) {
  LowOp Mask;
  Mask.Opcode = NdOp::INT_AND;
  Mask.Addr = 0x1000;
  Mask.Output = NdVar::tmp(0, 8);
  Mask.addInput(NdVar::reg(0, 8));
  Mask.addInput(NdVar::scalar(3, 8));
  size_t Work = limits::kMaxScalarBranchEvidenceWork;
  EXPECT_EQ(finiteScalarExpression({Mask}, Mask.Output, 0x1000, Work),
            (std::optional<std::vector<uint64_t>>{{0, 1, 2, 3}}));
  Work = limits::kMaxScalarBranchEvidenceWork;
  EXPECT_FALSE(finiteScalarExpression({Mask}, Mask.Output, 0x1001, Work));
}
} // namespace
