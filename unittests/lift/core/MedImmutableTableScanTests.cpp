//===- MedImmutableTableScanTests.cpp - Immutable table scan semantics ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <string>
#include <tuple>

using namespace neverd;
namespace {
MedVar var(int Id, uint16_t Width) {
  MedVar V;
  V.Id = Id;
  V.Size = Width;
  V.SSAVer = 1;
  return V;
}
MedOp op(NdOp Code, MedVar Out, std::initializer_list<MedVar> Inputs) {
  MedOp O;
  O.Opcode = Code;
  O.Output = Out;
  for (const auto &V : Inputs)
    O.addInput(V);
  return O;
}
BinaryImage table(unsigned Width, unsigned Count, bool Terminated = true) {
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Width == 8 ? Arch::X64 : Arch::X86;
  Image.Bits = Width == 8 ? Bitness::Bits64 : Bitness::Bits32;
  Segment S;
  S.Name = ".rdata";
  S.VA = 0x5000;
  S.Size = S.FileSz = (Count + 2) * Width;
  S.Flags = SegmentFlags::Readable;
  S.Data.assign(S.Size, 0);
  for (unsigned I = 0; I < Width; ++I)
    S.Data[I] = 0xff;
  for (unsigned I = 1; I <= Count + !Terminated; ++I)
    S.Data[I * Width] = 42;
  Image.Segments.push_back(S);
  Section Section;
  Section.Name = S.Name;
  Section.VA = S.VA;
  Section.Size = S.Size;
  Section.FileSz = S.FileSz;
  Section.Flags = S.Flags;
  Image.Sections.push_back(Section);
  return Image;
}
MedFunc scan(unsigned Width) {
  MedFunc F;
  F.Entry = 0x1000;
  F.Blocks.resize(3);
  for (unsigned I = 0; I < 3; ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = 0x1000 + I * 16;
    F.Blocks[I].EndAddr = 0x1010 + I * 16;
  }
  F.Blocks[0].Succs = {1};
  auto &Loop = F.Blocks[1];
  Loop.Preds = {0, 1};
  Loop.Succs = {2, 1};
  Loop.Phis.push_back(
      {var(1, 4),
       {{0, MedVar::makeConst(0, 4, ConstantAddressProvenance::Scalar)},
        {1, var(2, 4)}}});
  Loop.Ops = {
      op(NdOp::INT_ADD, var(2, 4), {var(1, 4), MedVar::makeConst(1, 4)}),
      op(NdOp::INT_ZEXT, var(3, Width), {var(2, 4)}),
      op(NdOp::INT_MULT, var(4, Width),
         {var(3, Width), MedVar::makeConst(Width, Width)}),
      op(NdOp::INT_ADD, var(5, Width),
         {MedVar::makeConst(0x5000, Width,
                            ConstantAddressProvenance::DataAddress, 0x5000),
          var(4, Width)}),
      op(NdOp::LOAD, var(6, Width), {var(5, Width)}),
      op(NdOp::INT_NOTEQUAL, var(7, 1),
         {var(6, Width), MedVar::makeConst(0, Width)}),
      op(NdOp::COND_BR, {}, {MedVar::makeConst(0x1010, Width), var(7, 1)})};
  F.Blocks[2].Preds = {1};
  F.Blocks[2].Ops = {op(NdOp::RETURN, {}, {var(1, 4)})};
  return F;
}

TEST(MedImmutableTableScans, EvaluatesSentinelCountAtItsExactExit) {
  for (unsigned Width : {4u, 8u})
    for (unsigned Count : {0u, 1u, 4u, 128u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Count);
      auto Image = table(Width, Count);
      auto F = scan(Width);
      ASSERT_TRUE(foldImmutableTableScans(F, Image));
      EXPECT_TRUE(F.Blocks[1].Phis.empty());
      EXPECT_EQ(F.Blocks[1].Succs, std::vector<int>{2});
      ASSERT_FALSE(F.Blocks[1].Ops.empty());
      const auto &CountCopy = F.Blocks[1].Ops.front();
      EXPECT_EQ(CountCopy.Output, var(1, 4));
      EXPECT_EQ(CountCopy.Inputs[0].ConstVal, Count);
      EXPECT_EQ(CountCopy.Inputs[0].Provenance,
                ConstantAddressProvenance::Scalar);
      EXPECT_FALSE(foldImmutableTableScans(F, Image));
    }
}

TEST(MedImmutableTableScans, ScanCountsPropagateThroughInvariantPhiInputs) {
  for (unsigned Width : {4u, 8u}) {
    auto Image = table(Width, 4);
    auto F = scan(Width);
    F.Blocks[2].Phis.push_back({var(10, 4), {{1, var(1, 4)}}});
    F.Blocks[2].Succs = {3};
    F.Blocks[2].Ops = {
        op(NdOp::BRANCH, {}, {MedVar::makeConst(0x1030, Width)})};
    MedBlock Next;
    Next.Id = 3;
    Next.StartAddr = 0x1030;
    Next.EndAddr = 0x1040;
    Next.Preds = {2};
    Next.Phis.push_back({var(11, 4), {{2, var(10, 4)}}});
    Next.Ops = {op(NdOp::RETURN, {}, {var(11, 4)})};
    F.Blocks.push_back(std::move(Next));
    ASSERT_TRUE(foldImmutableTableScans(F, Image));
    const MedVar &Count = F.Blocks[3].Phis[0].Args[0].second;
    ASSERT_TRUE(Count.isConst());
    EXPECT_EQ(Count.ConstVal, 4u);
    EXPECT_EQ(Count.Provenance, ConstantAddressProvenance::Scalar);
    EXPECT_FALSE(foldImmutableTableScans(F, Image));
  }
}

TEST(MedImmutableTableScans,
     RefusesMutableIncompleteAndIndependentlyEnteredScans) {
  for (unsigned Case = 0; Case < 11; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = table(8, Case == 6 ? 4097 : 4, Case != 0);
    auto F = scan(8);
    if (Case == 1)
      Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    if (Case == 2)
      Image.Sections[0].FileSz = 8;
    if (Case == 3)
      F.ModuleAnalysisRoots.insert(0x1010);
    if (Case == 4)
      Image.CodeRefTargets.insert(0x1010);
    if (Case == 5)
      F.Blocks[1].Ops[4].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    if (Case == 7)
      F.Blocks[1].Phis[0].Args[0].second = var(100, 4);
    if (Case == 8)
      F.ModuleAnalysisRoots.insert(0x1014);
    if (Case == 9)
      Image.CodeRefTargets.insert(0x1014);
    if (Case == 10)
      F.Entry = 0x1010;
    foldImmutableTableScans(F, Image);
    EXPECT_EQ(F.Blocks[1].Phis.size(), 1u);
    EXPECT_EQ(F.Blocks[1].Succs, (std::vector<int>{2, 1}));
  }
}

TEST(MedImmutableTableScans, ScalarFoldingKeepsTheOperationWidth) {
  auto Image = table(8, 1);
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  auto C = [](uint64_t V, unsigned Width) {
    return MedVar::makeConst(V, Width, ConstantAddressProvenance::Scalar);
  };
  F.Blocks[0].Ops = {op(NdOp::INT_ADD, var(1, 8), {C(255, 1), C(1, 1)}),
                     op(NdOp::INT_SUB, var(2, 8), {C(0, 1), C(1, 1)}),
                     op(NdOp::INT_MULT, var(3, 8), {C(255, 1), C(255, 1)}),
                     op(NdOp::INT_SLESS, var(4, 1), {C(255, 1), C(1, 8)}),
                     op(NdOp::INT_SLESS, var(5, 1), {C(255, 1), C(1, 1)}),
                     op(NdOp::INT_LEFT, var(6, 8), {C(128, 1), C(1, 1)}),
                     op(NdOp::INT_LEFT, var(7, 8), {C(1, 1), C(8, 8)})};
  ASSERT_TRUE(foldImmutableTableScans(F, Image));
  const uint64_t Expected[] = {0, 255, 1, 0, 1, 0, 256};
  for (unsigned I = 0; I < std::size(Expected); ++I) {
    SCOPED_TRACE(I);
    EXPECT_EQ(F.Blocks[0].Ops[I].Opcode, NdOp::COPY);
    EXPECT_EQ(F.Blocks[0].Ops[I].Inputs[0].ConstVal, Expected[I]);
  }
}

TEST(MedImmutableTableScans, RetainsRelocatableArithmeticAndComparisons) {
  auto Image = table(8, 1);
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  auto Unknown = MedVar::makeConst(0x5000, 8);
  auto Scalar = MedVar::makeConst(0x5000, 8, ConstantAddressProvenance::Scalar);
  auto Address =
      MedVar::makeConst(0x5000, 8, ConstantAddressProvenance::DataAddress);
  auto OtherAddress = Address;
  OtherAddress.ConstVal += 8;
  F.Blocks[0].Ops = {
      op(NdOp::INT_EQUAL, var(1, 1), {Unknown, Scalar}),
      op(NdOp::INT_ADD, var(2, 8), {Unknown, MedVar::makeConst(8, 8)}),
      op(NdOp::INT_EQUAL, var(3, 1), {Address, OtherAddress})};
  EXPECT_FALSE(foldImmutableTableScans(F, Image));
  EXPECT_EQ(F.Blocks[0].Ops[0].Opcode, NdOp::INT_EQUAL);
  EXPECT_EQ(F.Blocks[0].Ops[1].Opcode, NdOp::INT_ADD);
  EXPECT_EQ(F.Blocks[0].Ops[2].Opcode, NdOp::INT_EQUAL);
}

TEST(MedImmutableTableScans, OnePastAddressKeepsItsOriginalOwner) {
  auto Image = table(8, 1);
  auto Next = Image.Segments[0];
  Next.VA += Next.Size;
  Image.Segments.push_back(Next);
  auto NextSection = Image.Sections[0];
  NextSection.VA = Next.VA;
  Image.Sections.push_back(NextSection);
  auto End = MedVar::makeConst(Next.VA, 8,
                               ConstantAddressProvenance::DataAddress, 0x5000);
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  F.Blocks[0].Ops = {
      op(NdOp::INT_SUB, var(1, 8), {End, MedVar::makeConst(8, 8)}),
      op(NdOp::INT_ADD, var(2, 8), {End, MedVar::makeConst(8, 8)}),
      op(NdOp::LOAD, var(3, 8), {End})};
  ASSERT_TRUE(foldImmutableTableScans(F, Image));
  const auto &Previous = F.Blocks[0].Ops[0];
  EXPECT_EQ(Previous.Opcode, NdOp::COPY);
  EXPECT_EQ(Previous.Inputs[0].ConstVal, Next.VA - 8);
  EXPECT_EQ(Previous.Inputs[0].AddressOwnerVA, 0x5000u);
  EXPECT_EQ(F.Blocks[0].Ops[1].Opcode, NdOp::INT_ADD);
  EXPECT_EQ(F.Blocks[0].Ops[2].Opcode, NdOp::LOAD);
}

class MedImmutableTableScanMatrix
    : public testing::TestWithParam<std::tuple<Arch, BinaryFormat>> {
protected:
  unsigned width() const {
    const auto A = std::get<0>(GetParam());
    return A == Arch::X86 || A == Arch::ARM ? 4 : 8;
  }
  BinaryImage image(unsigned Count = 4) const {
    auto I = table(width(), Count);
    I.Arch = std::get<0>(GetParam());
    I.Format = std::get<1>(GetParam());
    return I;
  }
};

TEST_P(MedImmutableTableScanMatrix, SentinelCountsShareOneStorageContract) {
  for (unsigned Count : {0u, 1u, 4u, 128u}) {
    SCOPED_TRACE(Count);
    auto Image = image(Count);
    auto F = scan(width());
    ASSERT_TRUE(foldImmutableTableScans(F, Image));
    EXPECT_TRUE(F.Blocks[1].Phis.empty());
    ASSERT_EQ(F.Blocks[1].Succs, std::vector<int>{2});
    EXPECT_EQ(F.Blocks[1].Ops.front().Inputs[0].ConstVal, Count);
    EXPECT_FALSE(foldImmutableTableScans(F, Image));
  }
}

TEST_P(MedImmutableTableScanMatrix, RetainsUnprovedStorageAndEntryStates) {
  for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = image();
    auto F = scan(width());
    switch (Mutation) {
    case 0:
      Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      break;
    case 1:
      Image.Sections[0].FileSz = width();
      break;
    case 2:
      Image.Sections.push_back(Image.Sections[0]);
      break;
    case 3:
      F.ModuleAnalysisRoots.insert(0x1014);
      break;
    case 4:
      Image.CodeRefTargets.insert(0x1014);
      break;
    case 5:
      F.Blocks[1].Ops[4].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 6:
      Image.IsRelocatable = true;
      break;
    case 7:
      F.SkippedSSA = true;
      break;
    case 8:
      F.Blocks[1].Phis[0].ExceptionalEntry = var(20, 4);
      break;
    case 9:
      F.Blocks[0].Succs.clear();
      break;
    case 10:
      F.Blocks[1].Phis[0].Args[1].first = 0;
      break;
    case 11:
      F.Blocks[1].Phis[0].Args[1].second.Size = 1;
      break;
    case 12:
      Image.Arch = Arch::Unknown;
      break;
    case 13:
      Image.Bits = width() == 4 ? Bitness::Bits64 : Bitness::Bits32;
      break;
    case 14: {
      // A retained source-call proof owns the original value graph, even
      // when the call is outside the otherwise evaluable loop.
      auto Call = op(NdOp::CALL, {}, {MedVar::makeConst(0x2000, width())});
      Call.SourceCallHint = std::make_shared<SourceCallTypeHint>();
      F.Blocks[2].Ops.insert(F.Blocks[2].Ops.begin(), std::move(Call));
      break;
    }
    }
    foldImmutableTableScans(F, Image);
    EXPECT_EQ(F.Blocks[1].Phis.size(), 1u);
    EXPECT_EQ(F.Blocks[1].Succs, (std::vector<int>{2, 1}));
  }
}

TEST_P(MedImmutableTableScanMatrix, AddressArithmeticUsesTheTargetWidth) {
  auto Image = image();
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  auto Base = MedVar::makeConst(0x5000 + width(), width(),
                                ConstantAddressProvenance::DataAddress, 0x5000);
  auto Negative =
      MedVar::makeConst(width() == 4 ? uint32_t(-width()) : -uint64_t(width()),
                        width(), ConstantAddressProvenance::Scalar);
  F.Blocks[0].Ops = {op(NdOp::INT_ADD, var(1, width()), {Base, Negative})};
  ASSERT_TRUE(foldImmutableTableScans(F, Image));
  const auto &Result = F.Blocks[0].Ops[0];
  ASSERT_EQ(Result.Opcode, NdOp::COPY);
  EXPECT_EQ(Result.Inputs[0].ConstVal, 0x5000u);
  EXPECT_EQ(Result.Inputs[0].AddressOwnerVA, 0x5000u);
}

TEST_P(MedImmutableTableScanMatrix,
       SentinelScansKeepWideAddressCarriersAndCommutedOffsets) {
  for (bool Reverse : {false, true})
    for (bool NarrowAgain : {false, true}) {
      SCOPED_TRACE(Reverse);
      SCOPED_TRACE(NarrowAgain);
      auto Image = image();
      auto F = scan(width());
      auto &Ops = F.Blocks[1].Ops;
      if (Reverse)
        std::swap(Ops[3].Inputs[0], Ops[3].Inputs[1]);
      Ops.insert(Ops.begin() + 4,
                 op(NdOp::INT_ZEXT, var(8, 8), {var(5, width())}));
      Ops[5].Inputs[0] = var(8, 8);
      if (NarrowAgain) {
        Ops.insert(Ops.begin() + 5, op(NdOp::SUBBYTES, var(9, width()),
                                       {var(8, 8), MedVar::makeConst(0, 4)}));
        Ops[6].Inputs[0] = var(9, width());
      }
      ASSERT_TRUE(foldImmutableTableScans(F, Image));
      EXPECT_TRUE(F.Blocks[1].Phis.empty());
      EXPECT_EQ(F.Blocks[1].Succs, std::vector<int>{2});
      const auto &Count = F.Blocks[1].Ops.front();
      ASSERT_EQ(Count.Opcode, NdOp::COPY);
      EXPECT_EQ(Count.Inputs[0].ConstVal, 4u);
      EXPECT_EQ(Count.Inputs[0].Provenance, ConstantAddressProvenance::Scalar);
    }
}

TEST_P(MedImmutableTableScanMatrix, NarrowTagsCannotProvePointerIdentity) {
  auto Image = image();
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  auto Narrow = MedVar::makeConst(
      0x5000, 1, ConstantAddressProvenance::DataAddress, 0x5000);
  F.Blocks[0].Ops = {
      op(NdOp::INT_EQUAL, var(1, 1), {Narrow, MedVar::makeConst(0, 1)}),
      op(NdOp::LOAD, var(2, width()), {Narrow})};
  EXPECT_FALSE(foldImmutableTableScans(F, Image));
  EXPECT_EQ(F.Blocks[0].Ops[0].Opcode, NdOp::INT_EQUAL);
  EXPECT_EQ(F.Blocks[0].Ops[1].Opcode, NdOp::LOAD);
}

TEST_P(MedImmutableTableScanMatrix, UnmarkedPointerBitsKeepTheirLoad) {
  auto Image = image();
  for (unsigned I = 0; I < width(); ++I)
    Image.Segments[0].Data[I] = (uint64_t{0x5000} >> (I * 8)) & 0xff;
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  F.Blocks[0].Ops = {
      op(NdOp::LOAD, var(1, width()),
         {MedVar::makeConst(0x5000, width(),
                            ConstantAddressProvenance::DataAddress, 0x5000)})};
  EXPECT_FALSE(foldImmutableTableScans(F, Image));
  EXPECT_EQ(F.Blocks[0].Ops[0].Opcode, NdOp::LOAD);
}

TEST_P(MedImmutableTableScanMatrix,
       EveryPhiNeedsTheCompleteCurrentPredecessors) {
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Image = image();
      MedFunc F;
      F.Entry = 0x1000;
      F.Blocks.resize(4);
      for (unsigned I = 0; I < 4; ++I) {
        F.Blocks[I].Id = I;
        F.Blocks[I].StartAddr = 0x1000 + I * 16;
        F.Blocks[I].EndAddr = 0x1010 + I * 16;
      }
      F.Blocks[0].Succs = {1, 2};
      F.Blocks[1].Preds = F.Blocks[2].Preds = {0};
      F.Blocks[1].Succs = F.Blocks[2].Succs = {3};
      auto &Join = F.Blocks[3];
      Join.Preds = {1, 2};
      const auto Three =
          MedVar::makeConst(3, width(), ConstantAddressProvenance::Scalar);
      Join.Phis = {{var(1, width()), {{1, Three}, {2, Three}}}};
      Join.Ops = {op(NdOp::INT_ADD, var(2, width()),
                     {var(1, width()), MedVar::makeConst(1, width())})};
      switch (Mutation) {
      case 1:
        Join.Phis[0].Args[1].first = 1;
        break;
      case 2:
        Join.Preds = {1, 1};
        break;
      case 3:
        F.Blocks[2].Succs.clear();
        break;
      case 4:
        Join.Phis[0].Args[1].second.Size = 1;
        break;
      case 5:
        F.Blocks[1].Id = F.Blocks[2].Id;
        break;
      }
      if (Reverse) {
        std::reverse(Join.Preds.begin(), Join.Preds.end());
        std::reverse(Join.Phis[0].Args.begin(), Join.Phis[0].Args.end());
        std::swap(F.Blocks[1], F.Blocks[2]);
      }
      EXPECT_EQ(foldImmutableTableScans(F, Image), Mutation == 0);
      EXPECT_EQ(Join.Ops[0].Opcode, Mutation == 0 ? NdOp::COPY : NdOp::INT_ADD);
    }
  }
}

TEST(MedImmutableTableScans, DefinitionInventorySharesTheEvaluationBudget) {
  auto Image = table(8, 1);
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  for (int I = 0; I < 70000; ++I)
    F.Blocks[0].Ops.push_back(
        op(NdOp::COPY, var(I, 8), {MedVar::makeConst(3, 8)}));
  F.Blocks[0].Ops.push_back(op(NdOp::INT_ADD, var(70000, 8),
                               {var(69999, 8), MedVar::makeConst(1, 8)}));
  EXPECT_FALSE(foldImmutableTableScans(F, Image));
  EXPECT_EQ(F.Blocks[0].Ops.back().Opcode, NdOp::INT_ADD);
}

INSTANTIATE_TEST_SUITE_P(
    NativeImages, MedImmutableTableScanMatrix,
    testing::Combine(testing::Values(Arch::X86, Arch::X64, Arch::ARM,
                                     Arch::AArch64),
                     testing::Values(BinaryFormat::COFF, BinaryFormat::ELF,
                                     BinaryFormat::MachO)),
    [](const testing::TestParamInfo<MedImmutableTableScanMatrix::ParamType>
           &Info) {
      const auto Architecture = std::get<0>(Info.param);
      const auto Format = std::get<1>(Info.param);
      const char *A = Architecture == Arch::X86   ? "X86"
                      : Architecture == Arch::X64 ? "X64"
                      : Architecture == Arch::ARM ? "ARM"
                                                  : "AArch64";
      const char *F = Format == BinaryFormat::COFF  ? "PE"
                      : Format == BinaryFormat::ELF ? "ELF"
                                                    : "MachO";
      return std::string(A) + F;
    });
} // namespace
