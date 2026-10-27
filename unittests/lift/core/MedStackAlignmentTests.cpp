//===- MedStackAlignmentTests.cpp - Entry-stack alignment proofs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check entry-stack proofs, alignment rewrites and query isolation.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedProvenStackOffsets.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

MedVar reg(unsigned Version, Arch Architecture = Arch::ARM) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.TheArch = Architecture;
  V.Id = 1;
  V.SSAVer = Version;
  V.Size = getTargetRegInfo(Architecture).PointerSize;
  V.RegOff = getTargetRegInfo(Architecture).StackPointer;
  return V;
}

MedVar temp(int Id, Arch Architecture = Arch::ARM) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.TheArch = Architecture;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = getTargetRegInfo(Architecture).PointerSize;
  return V;
}

MedOp op(NdOp Opcode, MedVar Output, MedVar First,
         std::optional<MedVar> Second = std::nullopt) {
  MedOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  Result.addInput(First);
  if (Second)
    Result.addInput(*Second);
  return Result;
}

MedFunc frameWithEntrySP(Arch Architecture = Arch::ARM) {
  MedFunc Func;
  Func.Entry = 0x1000;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops.push_back(
      op(NdOp::COPY, reg(0, Architecture), reg(0, Architecture)));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

TEST(MedStackAlignment, BoundedDynamicOffsetsAreNotExactOffsets) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      auto Func = frameWithEntrySP(Architecture);
      const auto Initial = reg(1, Architecture);
      const auto Allocated = reg(2, Architecture);
      const auto Amount = temp(10, Architecture);
      const auto Rounded = temp(11, Architecture);
      const auto Aligned = temp(12, Architecture);
      const auto Small = MedVar::makeUnspecified(4, Architecture);
      auto &Ops = Func.Blocks.front().Ops;
      Ops.push_back(op(NdOp::INT_SUB, Initial, reg(0, Architecture),
                       MedVar::makeConst(64, 8)));
      Ops.push_back(op(NdOp::INT_ZEXT, Amount, Small));
      Ops.push_back(
          op(NdOp::INT_ADD, Rounded, Amount, MedVar::makeConst(15, 8)));
      Ops.push_back(op(NdOp::INT_AND, Aligned, Rounded,
                       MedVar::makeConst(~uint64_t{15}, 8)));
      Ops.push_back(op(NdOp::INT_SUB, Allocated, Initial, Aligned));
      auto Bounds =
          entryStackOffsetRanges(Func, {Allocated}, Architecture, Format);
      ASSERT_TRUE(Bounds[0]);
      EXPECT_LE(Bounds[0]->Min, -64 - int64_t{0x100000000});
      EXPECT_GE(Bounds[0]->Min, -64 - int64_t{0x10000000E});
      EXPECT_EQ(Bounds[0]->Max, -64);
      EXPECT_FALSE(entryStackOffset(Func, Allocated, Architecture, Format));
      // An unconstrained 64-bit amount can wrap to either side of the frame.
      Ops[2] = op(NdOp::COPY, Amount, MedVar::makeUnspecified(8, Architecture));
      EXPECT_FALSE(
          entryStackOffsetRanges(Func, {Allocated}, Architecture, Format)[0]);
    }
}

TEST(MedStackAlignment, LinearChainsKeepTheirOffsetPastTheDepthLimit) {
  // A long run of pushes and pops is one chain of displacements, not nested
  // proofs, so its offset is proved however long the run is.
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      const unsigned Width = getTargetRegInfo(Architecture).PointerSize;
      auto Func = frameWithEntrySP(Architecture);
      auto &Ops = Func.Blocks.front().Ops;
      const unsigned Pushes =
          3 * unsigned(limits::ir::kMedStackAlignmentResolveDepthLimit);
      MedVar Previous = reg(0, Architecture);
      MedVar Deepest;
      for (unsigned I = 0; I != 2 * Pushes; ++I) {
        const MedVar Next = temp(100 + I, Architecture);
        if (I == Pushes) {
          Ops.push_back(op(NdOp::COPY, Next, Previous));
          Deepest = Next;
        } else
          Ops.push_back(op(I < Pushes ? NdOp::INT_SUB : NdOp::INT_ADD, Next,
                           Previous, MedVar::makeConst(8, Width)));
        Previous = Next;
      }
      const int64_t Bottom = -8 * int64_t(Pushes);
      EXPECT_EQ(entryStackOffset(Func, Deepest, Architecture, Format), Bottom);
      // Pushes-1 pops follow the copy.
      EXPECT_EQ(entryStackOffset(Func, Previous, Architecture, Format), -8);
      EXPECT_EQ(
          entryStackOffsets(Func, {Previous, Deepest}, Architecture, Format),
          (std::vector<std::optional<int64_t>>{-8, Bottom}));
    }
}

TEST(MedStackAlignment, UpperBoundsHoldBelowLoopedDynamicAllocations) {
  // A loop that allocates an unsigned amount on each turn lowers the stack
  // pointer without a lower bound; it still never rises above the frame.
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Raises : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(Raises);
        auto Func = frameWithEntrySP(Architecture);
        const MedVar Frame = temp(1, Architecture);
        const MedVar Head = temp(2, Architecture);
        const MedVar Lowered = temp(3, Architecture);
        const MedVar Amount = temp(4, Architecture);
        Func.Blocks[0].Ops.push_back(op(NdOp::INT_SUB, Frame,
                                        reg(0, Architecture),
                                        MedVar::makeConst(64, 8)));
        MedBlock Loop;
        Loop.Id = 1;
        Loop.StartAddr = 0x1010;
        PhiNode Join;
        Join.Output = Head;
        Join.Args = {{0, Frame}, {1, Lowered}};
        Loop.Phis.push_back(Join);
        Loop.Ops.push_back(op(NdOp::INT_ZEXT, Amount,
                              MedVar::makeUnspecified(4, Architecture)));
        // A raise on every turn has no bound at all.
        Loop.Ops.push_back(
            Raises ? op(NdOp::INT_ADD, Lowered, Head, MedVar::makeConst(8, 8))
                   : op(NdOp::INT_SUB, Lowered, Head, Amount));
        Func.Blocks.push_back(std::move(Loop));
        EXPECT_FALSE(entryStackOffset(Func, Lowered, Architecture, Format));
        const auto Bounds = entryStackOffsetUpperBounds(
            Func, {Head, Lowered, Frame}, Architecture, Format);
        ASSERT_EQ(Bounds.size(), 3u);
        if (Raises) {
          EXPECT_FALSE(Bounds[0]);
          EXPECT_FALSE(Bounds[1]);
        } else {
          EXPECT_EQ(Bounds[0], -64);
          EXPECT_EQ(Bounds[1], -64);
        }
        EXPECT_EQ(Bounds[2], -64);
      }
}

TEST(MedStackAlignment, AGuardedCounterBoundsRepeatedStackAllocations) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    const auto Width = getTargetRegInfo(Architecture).PointerSize;
    for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mutation);
      auto Func = frameWithEntrySP(Architecture);
      Func.Blocks.resize(6);
      for (int I = 0; I != 6; ++I) {
        Func.Blocks[I].Id = I;
        Func.Blocks[I].StartAddr = 0x1000 + I * 0x10;
      }
      auto &Entry = Func.Blocks[0];
      auto &Header = Func.Blocks[1];
      auto &Update = Func.Blocks[2];
      auto &Middle = Func.Blocks[3];
      auto &Latch = Func.Blocks[4];
      auto &Exit = Func.Blocks[5];
      Entry.Succs = {1};
      Header.Preds = {0, 4};
      Header.Succs = {2, 5};
      Update.Preds = {1};
      Update.Succs = {3};
      Middle.Preds = {2};
      Middle.Succs = {4};
      Latch.Preds = {3};
      Latch.Succs = {1};
      Exit.Preds = {1};
      Entry.Ops.push_back(op(NdOp::INT_SUB, reg(1, Architecture),
                             reg(0, Architecture),
                             MedVar::makeConst(32, Width)));
      auto Count = temp(10, Architecture);
      Count.Size = 1;
      auto Next = temp(11, Architecture);
      Next.Size = 1;
      auto Condition = temp(12, Architecture);
      Condition.Size = 1;
      Header.Phis = {{reg(2, Architecture),
                      {{0, reg(1, Architecture)}, {4, reg(3, Architecture)}}},
                     {Count, {{0, MedVar::makeConst(0, 1)}, {4, Next}}}};
      Header.Ops = {
          op(NdOp::INT_LESSEQUAL, Condition, MedVar::makeConst(16, 1), Count),
          op(NdOp::COND_BR, {}, MedVar::makeConst(Exit.StartAddr, Width),
             Condition)};
      Update.Ops = {op(NdOp::INT_SUB, reg(3, Architecture),
                       reg(2, Architecture), MedVar::makeConst(16, Width)),
                    op(NdOp::INT_ADD, Next, Count, MedVar::makeConst(4, 1))};
      switch (Mutation) {
      case 0:
        break;
      case 1: // The counter can wrap back under the limit.
        Header.Ops[0].Inputs[0] = MedVar::makeConst(254, 1);
        break;
      case 2: // The guard's wrong edge enters the loop.
        Header.Ops[1].Inputs[0].ConstVal = Update.StartAddr;
        break;
      case 3: // An ordinary bypass can miss both updates.
        Header.Succs.push_back(4);
        Latch.Preds.push_back(1);
        break;
      case 4: // Another entry does not establish this invocation's counter.
        Update.Preds.push_back(0);
        Entry.Succs.push_back(2);
        break;
      case 5: // The allocation may repeat within one counter iteration.
        Middle.Succs.push_back(2);
        Update.Preds.push_back(3);
        break;
      case 6:
        Header.Phis[1].Args[0].second =
            MedVar::makeUnspecified(1, Architecture);
        break;
      case 7:
        Update.Ops[1].Inputs[1] = MedVar::makeConst(0, 1);
        break;
      case 8:
        Header.Ops[0].Opcode = NdOp::INT_SLESSEQUAL;
        break;
      case 9:
        Header.Phis[0].ExceptionalEntry =
            MedVar::makeUnspecified(Width, Architecture);
        break;
      case 10:
        Header.Phis[1].Args[1].first = 3;
        break;
      case 11:
        Update.Ops[0].Inputs[1].Provenance =
            ConstantAddressProvenance::DataAddress;
        break;
      case 12:
        Header.Ops[0].Inputs[1].Size = 2;
        break;
      case 13:
        Func.ModuleAnalysisRoots.insert(Update.StartAddr);
        break;
      case 14:
        Update.Ops[1].MemoryOrdering = NdMemoryOrdering::Relaxed;
        break;
      case 15:
        Update.Ops[0].Inputs[1].ConstVal = UINT64_C(1) << (Width * 8 - 1);
        break;
      case 16:
        Header.Ops[0].Inputs[0].Provenance =
            ConstantAddressProvenance::DataAddress;
        break;
      case 17:
        Func.ModuleAnalysisRoots.insert(Header.StartAddr);
        break;
      }
      const auto Bounds = entryStackOffsetRanges(
          Func, {reg(2, Architecture)}, Architecture, BinaryFormat::COFF);
      EXPECT_FALSE(entryStackOffset(Func, reg(2, Architecture), Architecture,
                                    BinaryFormat::COFF));
      if (Mutation) {
        EXPECT_FALSE(Bounds[0]);
        continue;
      }
      ASSERT_TRUE(Bounds[0]);
      EXPECT_EQ(Bounds[0]->Min, -96);
      EXPECT_EQ(Bounds[0]->Max, -32);
      // Independently enumerate the original unsigned loop, including the
      // final header visit that fails the guard.
      int64_t Stack = -32;
      for (unsigned Counter = 0;; Counter += 4) {
        EXPECT_LE(Bounds[0]->Min, Stack);
        EXPECT_GE(Bounds[0]->Max, Stack);
        if (Counter >= 16)
          break;
        Stack -= 16;
      }
      // The other branch polarity and the low-byte / extension sequence
      // represent the same finite loop. Subtracting a negative immediate
      // still increments at the counter's machine width.
      Header.Ops[0] =
          op(NdOp::INT_LESS, Condition, Count, MedVar::makeConst(16, 1));
      Header.Ops[1].Inputs[0].ConstVal = Update.StartAddr;
      Update.Ops[1].Opcode = NdOp::INT_SUB;
      Update.Ops[1].Inputs[1].ConstVal = uint64_t{0} - 4;
      auto Wide = temp(20, Architecture);
      auto Narrow = temp(21, Architecture);
      Narrow.Size = 1;
      Update.Ops.push_back(op(NdOp::INT_ZEXT, Wide, Next));
      Update.Ops.push_back(
          op(NdOp::SUBBYTES, Narrow, Wide, MedVar::makeConst(0, 4)));
      Header.Phis[1].Args[1].second = Narrow;
      const auto Copied = entryStackOffsetRanges(
          Func, {reg(2, Architecture)}, Architecture, BinaryFormat::COFF);
      ASSERT_TRUE(Copied[0]);
      EXPECT_EQ(Copied[0]->Min, Bounds[0]->Min);
      EXPECT_EQ(Copied[0]->Max, Bounds[0]->Max);
    }
  }
}

TEST(MedStackAlignment, ExceptionalStackRangesAreQueryLocalAndKeepAllEntries) {
  auto Func = frameWithEntrySP(Arch::X64);
  auto &EntryOps = Func.Blocks[0].Ops;
  EntryOps.push_back(op(NdOp::INT_SUB, reg(1, Arch::X64), reg(0, Arch::X64),
                        MedVar::makeConst(72, 8)));
  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = 0x1020;
  PhiNode Join;
  Join.Output = reg(2, Arch::X64);
  Join.Args = {{0, reg(1, Arch::X64)}};
  Join.ExceptionalEntry = MedVar::makeUnspecified(8, Arch::X64);
  Handler.Phis.push_back(Join);
  Handler.Ops.push_back(op(NdOp::INT_SUB, reg(3, Arch::X64), Join.Output,
                           MedVar::makeConst(8, 8)));
  Func.Blocks.push_back(std::move(Handler));
  const auto Value = reg(3, Arch::X64);
  EXPECT_FALSE(entryStackOffset(Func, Value, Arch::X64, BinaryFormat::COFF));
  EXPECT_FALSE(
      entryStackOffsetRanges(Func, {Value}, Arch::X64, BinaryFormat::COFF)[0]);
  const auto Query = [&](const ProvenStackOffsetRange &Seed) {
    return entryStackOffsetRangesFromProvenDefinitions(
        Func, {Value}, {Seed}, Arch::X64, BinaryFormat::COFF,
        StackEntryKind::Call)[0];
  };
  const ProvenStackOffsetRange Seed{
      nullptr, &Func.Blocks[1].Phis[0], {-104, -72}};
  const auto Range = Query(Seed);
  ASSERT_TRUE(Range);
  EXPECT_EQ(Range->Min, -112);
  EXPECT_EQ(Range->Max, -80);
  EXPECT_FALSE(entryStackOffset(Func, Value, Arch::X64, BinaryFormat::COFF));
  // A copy of a definition is not the current occurrence in this function.
  const auto Detached = Func.Blocks[1].Phis[0];
  EXPECT_FALSE(Query({nullptr, &Detached, {-104, -72}}));
  EXPECT_FALSE(Query({nullptr, &Func.Blocks[1].Phis[0], {-72, -104}}));
  const auto Conflicting = entryStackOffsetRangesFromProvenDefinitions(
      Func, {Value}, {Seed, {nullptr, &Func.Blocks[1].Phis[0], {-64, -32}}},
      Arch::X64, BinaryFormat::COFF, StackEntryKind::Call);
  EXPECT_FALSE(Conflicting[0]);
}

TEST(MedStackAlignment, ScaledDynamicAllocationsRetainMachineWidthBounds) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    const auto Width = getTargetRegInfo(Architecture).PointerSize;
    for (uint64_t Factor : {uint64_t{3}, uint64_t{48}}) {
      auto Func = frameWithEntrySP(Architecture);
      const auto Amount = temp(10, Architecture);
      const auto Scaled = temp(11, Architecture);
      const auto Allocated = reg(1, Architecture);
      auto &Ops = Func.Blocks.front().Ops;
      Ops.push_back(
          op(NdOp::INT_ZEXT, Amount, MedVar::makeUnspecified(1, Architecture)));
      Ops.push_back(
          op(NdOp::INT_MULT, Scaled, Amount, MedVar::makeConst(Factor, Width)));
      Ops.push_back(op(NdOp::INT_SUB, Allocated, reg(0, Architecture), Scaled));
      const auto Bounds = entryStackOffsetRanges(
          Func, {Allocated}, Architecture, BinaryFormat::ELF);
      ASSERT_TRUE(Bounds[0]);
      // Compare against every possible input byte, independently of the
      // interval arithmetic used by the proof.
      for (int64_t Count = 0; Count <= UINT8_MAX; ++Count) {
        const int64_t Offset = -Count * static_cast<int64_t>(Factor);
        EXPECT_LE(Bounds[0]->Min, Offset);
        EXPECT_GE(Bounds[0]->Max, Offset);
      }
      EXPECT_LE(Bounds[0]->Max, 0);
      EXPECT_FALSE(
          entryStackOffset(Func, Allocated, Architecture, BinaryFormat::ELF));
      // Multiplication is modular, not an unbounded signed product: an
      // unconstrained input may wrap to either side of the entry frame.
      Ops[1] =
          op(NdOp::COPY, Amount, MedVar::makeUnspecified(Width, Architecture));
      EXPECT_FALSE(entryStackOffsetRanges(Func, {Allocated}, Architecture,
                                          BinaryFormat::ELF)[0]);
    }
  }
}

TEST(MedStackAlignment, EveryPhiComponentNeedsItsOwnOutsideOrigin) {
  for (bool Dynamic : {false, true})
    for (bool Unseeded : {false, true}) {
      auto Func = frameWithEntrySP(Arch::X64);
      const auto Initial = reg(1, Arch::X64);
      const auto A = reg(2, Arch::X64);
      const auto B = reg(3, Arch::X64);
      const auto C = reg(4, Arch::X64);
      auto &Ops = Func.Blocks.front().Ops;
      const auto Amount = temp(20, Arch::X64);
      Ops.push_back(
          op(NdOp::INT_ZEXT, Amount, MedVar::makeUnspecified(1, Arch::X64)));
      Ops.push_back(op(NdOp::INT_SUB, Initial, reg(0, Arch::X64),
                       Dynamic ? Amount : MedVar::makeConst(64, 8)));
      MedBlock Loop;
      Loop.Id = 1;
      Loop.Phis = {{A, {{0, Initial}, {1, B}}},
                   {B, {{1, C}}},
                   {C, {{1, Unseeded ? B : A}}}};
      Func.Blocks.push_back(std::move(Loop));
      const auto Bounds =
          entryStackOffsetRanges(Func, {A, B, C}, Arch::X64, BinaryFormat::ELF);
      for (const auto &Range : Bounds) {
        EXPECT_EQ(Range.has_value(), !Unseeded);
        if (Range) {
          EXPECT_EQ(Range->Min, Dynamic ? -255 : -64);
          EXPECT_EQ(Range->Max, Dynamic ? 0 : -64);
        }
      }
      if (Unseeded)
        EXPECT_FALSE(entryStackOffset(Func, A, Arch::X64, BinaryFormat::ELF));
    }
}

TEST(MedStackAlignment, CopiesPreserveLoopStackOriginsAndRanges) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM})
    for (bool Dynamic : {false, true})
      for (bool Unseeded : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(Dynamic);
        SCOPED_TRACE(Unseeded);
        auto Func = frameWithEntrySP(Architecture);
        const auto Width = getTargetRegInfo(Architecture).PointerSize;
        const auto Initial = reg(1, Architecture);
        const auto LoopValue = reg(2, Architecture);
        const auto Copy = reg(3, Architecture);
        const auto Exit = reg(4, Architecture);
        const auto Amount = temp(10, Architecture);
        auto &Ops = Func.Blocks.front().Ops;
        Ops.push_back(op(NdOp::INT_ZEXT, Amount,
                         MedVar::makeUnspecified(1, Architecture)));
        Ops.push_back(op(NdOp::INT_SUB, Initial, reg(0, Architecture),
                         Dynamic ? Amount : MedVar::makeConst(64, Width)));
        MedBlock Loop;
        Loop.Id = 1;
        Loop.Phis = {{LoopValue, {{0, Unseeded ? Copy : Initial}, {1, Copy}}},
                     {Exit, {{0, Initial}, {1, Copy}}}};
        Loop.Ops.push_back(op(NdOp::COPY, Copy, LoopValue));
        Func.Blocks.push_back(std::move(Loop));
        const auto Bounds = entryStackOffsetRanges(
            Func, {Exit, Copy, LoopValue}, Architecture, BinaryFormat::ELF);
        for (const auto &Range : Bounds) {
          EXPECT_EQ(Range.has_value(), !Unseeded);
          if (Range) {
            EXPECT_EQ(Range->Min, Dynamic ? -255 : -64);
            EXPECT_EQ(Range->Max, Dynamic ? 0 : -64);
          }
        }
        EXPECT_EQ(entryStackOffset(Func, Exit, Architecture, BinaryFormat::ELF),
                  !Dynamic && !Unseeded ? std::optional<int64_t>(-64)
                                        : std::nullopt);
        // A changing backedge cannot inherit the invariant of a copy cycle.
        Func.Blocks.back().Ops.back() =
            op(NdOp::INT_SUB, Copy, LoopValue, MedVar::makeConst(1, Width));
        EXPECT_FALSE(entryStackOffsetRanges(Func, {Exit}, Architecture,
                                            BinaryFormat::ELF)[0]);
      }
}

TEST(MedStackAlignment, ProvenDefinitionsComposeOnlyWithinTheirQuery) {
  auto Func = frameWithEntrySP(Arch::X86);
  const auto Loaded = temp(2, Arch::X86);
  const auto Restored = reg(1, Arch::X86);
  const auto Adjusted = reg(2, Arch::X86);
  auto &Ops = Func.Blocks[0].Ops;
  Ops.push_back(op(NdOp::LOAD, Loaded, reg(0, Arch::X86)));
  Ops.push_back(op(NdOp::COPY, Restored, Loaded));
  Ops.push_back(
      op(NdOp::INT_ADD, Adjusted, Restored, MedVar::makeConst(16, 4)));
  const ProvenStackOffset Seed{&Ops[1], -64};
  EXPECT_EQ(entryStackOffsetsFromProvenDefinitions(
                Func, {Adjusted}, {Seed}, Arch::X86, BinaryFormat::COFF,
                StackEntryKind::Call)[0],
            -48);
  EXPECT_FALSE(entryStackOffset(Func, Adjusted, Arch::X86, BinaryFormat::COFF));
  EXPECT_EQ(Ops[1].Opcode, NdOp::LOAD);
  EXPECT_FALSE(entryStackOffsetsFromProvenDefinitions(
      Func, {Adjusted}, {Seed, {&Ops[1], -32}}, Arch::X86, BinaryFormat::COFF,
      StackEntryKind::Call)[0]);
  auto Foreign = Ops[1];
  EXPECT_FALSE(entryStackOffsetsFromProvenDefinitions(
      Func, {Adjusted}, {{&Foreign, -64}}, Arch::X86, BinaryFormat::COFF,
      StackEntryKind::Call)[0]);
  auto Narrow = Adjusted;
  Narrow.Size = 2;
  EXPECT_FALSE(entryStackOffsetsFromProvenDefinitions(
      Func, {Narrow}, {Seed}, Arch::X86, BinaryFormat::COFF,
      StackEntryKind::Call)[0]);
}

TEST(MedStackAlignment, RelativeQueriesNeverInheritTheParentStackOrigin) {
  auto Func = frameWithEntrySP(Arch::X86);
  const auto Private = reg(1, Arch::X86);
  const auto Pushed = reg(2, Arch::X86);
  auto &Ops = Func.Blocks[0].Ops;
  Ops.push_back(op(NdOp::COPY, Private, reg(0, Arch::X86)));
  Ops.push_back(op(NdOp::INT_SUB, Pushed, Private, MedVar::makeConst(8, 4)));
  auto Check = [&](llvm::ArrayRef<MedVar> Values) {
    return stackOffsetsRelativeToDefinition(Func, Values, Ops[1], Arch::X86,
                                            BinaryFormat::COFF);
  };
  EXPECT_EQ(Check({Pushed})[0], -8);
  EXPECT_FALSE(Check({reg(0, Arch::X86)})[0]);
  PhiNode Phi;
  Phi.Output = reg(3, Arch::X86);
  Phi.Args = {{0, Pushed}, {1, Pushed}};
  Func.Blocks[0].Phis.push_back(Phi);
  EXPECT_EQ(Check({Phi.Output})[0], -8);
  Func.Blocks[0].Phis[0].Args[1].second = reg(0, Arch::X86);
  EXPECT_FALSE(Check({Phi.Output})[0]);
  Func.Blocks[0].Phis[0].Args[1].second = Pushed;
  Func.Blocks[0].Phis[0].ExceptionalEntry = reg(0, Arch::X86);
  EXPECT_FALSE(Check({Phi.Output})[0]);
}

TEST(MedStackAlignment,
     Native32BitMemoryAddressContainersKeepTheirFrameOrigin) {
  for (Arch Architecture : {Arch::X86, Arch::ARM})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      auto Func = frameWithEntrySP(Architecture);
      auto &Ops = Func.Blocks[0].Ops;
      const auto Address = temp(2, Architecture);
      auto Wide = temp(3, Architecture);
      Wide.Size = 8;
      Ops.push_back(op(NdOp::INT_SUB, Address, reg(0, Architecture),
                       MedVar::makeConst(20, 4)));
      Ops.push_back(op(NdOp::INT_ZEXT, Wide, Address));
      EXPECT_EQ(entryStackOffset(Func, Wide, Architecture, Format), -20);
      Ops.back().Opcode = NdOp::INT_SEXT;
      EXPECT_FALSE(entryStackOffset(Func, Wide, Architecture, Format));
      Ops.back().Opcode = NdOp::COPY;
      EXPECT_FALSE(entryStackOffset(Func, Wide, Architecture, Format));
      Ops.back().Opcode = NdOp::INT_ZEXT;
      Ops.push_back(Ops.back());
      EXPECT_FALSE(entryStackOffset(Func, Wide, Architecture, Format));
    }
}

TEST(MedStackAlignment, WiderSeedsAndTruncatedPointersAreNotNativeAddresses) {
  auto Func = frameWithEntrySP(Arch::X86);
  auto Wide = reg(0, Arch::X86);
  Wide.Size = 8;
  Func.Blocks[0].Ops = {op(NdOp::COPY, Wide, Wide)};
  EXPECT_FALSE(entryStackOffset(Func, Wide, Arch::X86, BinaryFormat::ELF));
  Func = frameWithEntrySP(Arch::X64);
  auto Narrow = temp(2, Arch::X64);
  Narrow.Size = 4;
  Wide = temp(3, Arch::X64);
  Func.Blocks[0].Ops.push_back(
      op(NdOp::SUBBYTES, Narrow, reg(0, Arch::X64), MedVar::makeConst(0, 4)));
  Func.Blocks[0].Ops.push_back(op(NdOp::INT_ZEXT, Wide, Narrow));
  EXPECT_FALSE(entryStackOffset(Func, Wide, Arch::X64, BinaryFormat::ELF));
}

TEST(MedStackAlignment, PreservesExactAndRoundedARMFrameOffsets) {
  MedFunc Func = frameWithEntrySP();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.push_back(op(NdOp::INT_SUB, temp(2), reg(0), MedVar::makeConst(96, 4)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(3), temp(2), MedVar::makeConst(0xfffffff8, 4)));
  Ops.push_back(op(NdOp::INT_ADD, temp(4), temp(2), MedVar::makeConst(1, 4)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(5), temp(4), MedVar::makeConst(0xfffffff8, 4)));

  simplifyProvenStackAlignment(Func, Arch::ARM, BinaryFormat::MachO);

  EXPECT_EQ(Ops[2].Opcode, NdOp::COPY);
  EXPECT_EQ(Ops[2].NumInputs, 1);
  EXPECT_EQ(Ops[2].Inputs[0], temp(2));
  EXPECT_EQ(Ops[4].Opcode, NdOp::INT_SUB);
  EXPECT_EQ(Ops[4].NumInputs, 2);
  EXPECT_EQ(Ops[4].Inputs[0], temp(4));
  EXPECT_TRUE(Ops[4].Inputs[1].isConst());
  EXPECT_EQ(Ops[4].Inputs[1].ConstVal, 1u);
}

TEST(MedStackAlignment, PositiveNarrowDisplacementsKeepPointerWidthFrames) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (NdOp Opcode : {NdOp::INT_ADD, NdOp::INT_SUB})
      for (uint16_t Bytes : {1, 2, 4, 8}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Opcode));
        SCOPED_TRACE(Bytes);
        const auto Width = getTargetRegInfo(Architecture).PointerSize;
        MedFunc Func = frameWithEntrySP(Architecture);
        auto &Ops = Func.Blocks.front().Ops;
        const MedVar Result = temp(2, Architecture);
        Ops.push_back(op(Opcode, Result, reg(0, Architecture),
                         MedVar::makeConst(64, Bytes)));
        const auto Expected =
            Bytes <= Width
                ? std::optional<int64_t>(Opcode == NdOp::INT_ADD ? 64 : -64)
                : std::nullopt;
        EXPECT_EQ(
            entryStackOffset(Func, Result, Architecture, BinaryFormat::ELF),
            Expected);
        // A high-bit narrow constant must not be mistaken for a signed
        // pointer-width displacement, nor may address provenance be erased.
        Ops.back().Inputs[1] = MedVar::makeConst(~uint64_t{0}, Bytes);
        if (Bytes != Width)
          EXPECT_FALSE(
              entryStackOffset(Func, Result, Architecture, BinaryFormat::ELF));
        Ops.back().Inputs[1] =
            MedVar::makeConst(64, Bytes, ConstantAddressProvenance::Address);
        EXPECT_FALSE(
            entryStackOffset(Func, Result, Architecture, BinaryFormat::ELF));
      }
}

TEST(MedStackAlignment, RejectsUnprovedOrStrongerThanABIAlignment) {
  MedFunc Func = frameWithEntrySP();
  auto &Block = Func.Blocks[0];
  auto &Ops = Block.Ops;
  Ops.push_back(op(NdOp::INT_SUB, temp(2), reg(0), MedVar::makeConst(96, 4)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(3), temp(2), MedVar::makeConst(0xfffffff0, 4)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(4), temp(9), MedVar::makeConst(0xfffffff8, 4)));
  PhiNode Phi;
  Phi.Output = temp(6);
  Phi.Args.push_back({0, temp(2)});
  Block.Phis.push_back(Phi);
  Ops.push_back(
      op(NdOp::INT_AND, temp(7), temp(6), MedVar::makeConst(0xfffffff8, 4)));

  simplifyProvenStackAlignment(Func, Arch::ARM, BinaryFormat::MachO);

  EXPECT_EQ(Ops[2].Opcode, NdOp::INT_AND);
  EXPECT_EQ(Ops[3].Opcode, NdOp::INT_AND);
  EXPECT_EQ(Ops[4].Opcode, NdOp::INT_AND);
}

TEST(MedStackAlignment, RequiresAuthenticatedEntryStackPointer) {
  MedFunc Func = frameWithEntrySP();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.erase(Ops.begin());
  Ops.push_back(
      op(NdOp::INT_AND, temp(2), reg(0), MedVar::makeConst(0xfffffff8, 4)));

  simplifyProvenStackAlignment(Func, Arch::ARM, BinaryFormat::MachO);

  EXPECT_EQ(Ops[0].Opcode, NdOp::INT_AND);
}

TEST(MedStackAlignment, RejectsAnAmbiguousEntryStackDefinition) {
  MedFunc Func = frameWithEntrySP();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.push_back(op(NdOp::COPY, reg(0), temp(9)));
  Ops.push_back(
      op(NdOp::INT_AND, temp(2), reg(0), MedVar::makeConst(0xfffffff8, 4)));

  simplifyProvenStackAlignment(Func, Arch::ARM, BinaryFormat::MachO);

  EXPECT_EQ(Ops[2].Opcode, NdOp::INT_AND);
}

TEST(MedStackAlignment, RealignmentRootsDoNotInventAnEntryDisplacement) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool MaskFirst : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(MaskFirst);
        MedFunc Func = frameWithEntrySP(Architecture);
        auto &Ops = Func.Blocks[0].Ops;
        const uint16_t Width = getTargetRegInfo(Architecture).PointerSize;
        Ops.push_back(op(NdOp::INT_SUB, reg(1, Architecture),
                         reg(0, Architecture),
                         MedVar::makeConst(Width, Width)));
        const auto Mask =
            MedVar::makeConst(Width == 4 ? uint64_t{0xffffffc0} : uint64_t(-64),
                              Width, ConstantAddressProvenance::Scalar);
        Ops.push_back(op(NdOp::INT_AND, reg(2, Architecture),
                         MaskFirst ? Mask : reg(1, Architecture),
                         MaskFirst ? reg(1, Architecture) : Mask));
        const auto Roots =
            entryStackRealignmentRoots(Func, Architecture, Format);
        ASSERT_EQ(Roots.size(), 1u);
        EXPECT_EQ(Roots.front(), reg(2, Architecture));
        EXPECT_FALSE(
            entryStackOffset(Func, Roots.front(), Architecture, Format));
        simplifyProvenStackAlignment(Func, Architecture, Format);
        EXPECT_EQ(Ops.back().Opcode, NdOp::INT_AND);
      }
}

TEST(MedStackAlignment, RejectsUnownedOrMalformedRealignmentRoots) {
  for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MedFunc Func = frameWithEntrySP(Arch::X86);
    auto &Block = Func.Blocks.front();
    Block.Ops.push_back(op(NdOp::INT_SUB, reg(1, Arch::X86), reg(0, Arch::X86),
                           MedVar::makeConst(4, 4)));
    Block.Ops.push_back(op(NdOp::INT_AND, reg(2, Arch::X86), reg(1, Arch::X86),
                           MedVar::makeConst(0xfffffff8, 4)));
    auto &Align = Block.Ops.back();
    switch (Mutation) {
    case 0:
      Block.Ops.erase(Block.Ops.begin());
      break;
    case 1:
      Align.Output.Size = 2;
      break;
    case 2:
      Align.Inputs[0].Size = 2;
      break;
    case 3:
      Align.Inputs[1].ConstVal = 0xfffffff5;
      break;
    case 4:
      Align.Inputs[1].Provenance = ConstantAddressProvenance::DataAddress;
      break;
    case 5:
      Align.Inputs[1].Size = 8;
      break;
    case 6:
      Align.Output.TheArch = Arch::ARM;
      break;
    case 7:
      Block.Ops[1].Inputs[0].TheArch = Arch::ARM;
      break;
    case 8:
      Align.Output.RegOff = getTargetRegInfo(Arch::X86).FramePointer;
      break;
    case 9:
      Block.Ops.push_back(Align);
      break;
    case 10:
      Block.Preds.push_back(0);
      break;
    case 11:
      Block.Succs.push_back(0);
      break;
    case 12:
      Align.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 13:
      Align.MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 14:
      Align.Inputs[0] = temp(99, Arch::X86);
      break;
    case 15:
      Align.Inputs[1].ConstVal = 0;
      break;
    case 16: {
      MedBlock Other = Block;
      Other.Id = 1;
      Func.Blocks.push_back(std::move(Other));
      break;
    }
    case 17:
      Block.Ops.front().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 18:
      std::swap(Block.Ops[1], Block.Ops[2]);
      break;
    case 19:
      Block.Ops[1].Inputs[0].RenameTag = 1;
      break;
    }
    EXPECT_TRUE(entryStackRealignmentRoots(Func, Arch::X86, BinaryFormat::COFF)
                    .empty());
  }
}

TEST(MedStackAlignment, RegistrationRootsKeepDistinctRuntimeStackCoordinates) {
  MedFunc Func = frameWithEntrySP(Arch::X86);
  MedBlock Runtime;
  Runtime.Id = 1;
  Runtime.StartAddr = 0x1100;
  auto FP = reg(1, Arch::X86);
  FP.Id = 2;
  FP.RegOff = getTargetRegInfo(Arch::X86).FramePointer;
  auto IncomingFP = FP;
  IncomingFP.SSAVer = 0;
  auto Frame = op(NdOp::COPY, FP, IncomingFP);
  Frame.RegistrationRoot = MedOp::RegistrationRootKind::EstablishedFramePointer;
  Runtime.Ops.push_back(Frame);
  auto Callback = op(NdOp::COPY, reg(1, Arch::X86), reg(0, Arch::X86));
  Callback.RegistrationRoot = MedOp::RegistrationRootKind::CallbackStackPointer;
  Runtime.Ops.push_back(Callback);
  auto Resume = op(NdOp::COPY, reg(2, Arch::X86), reg(0, Arch::X86));
  Resume.RegistrationRoot = MedOp::RegistrationRootKind::RestoredStackPointer;
  Resume.RegistrationStackOffset = -28;
  Runtime.Ops.push_back(Resume);
  Runtime.Ops.push_back(op(NdOp::INT_AND, temp(3, Arch::X86), Callback.Output,
                           MedVar::makeConst(0xfffffffc, 4)));
  Runtime.Ops.push_back(op(NdOp::INT_AND, temp(4, Arch::X86), Resume.Output,
                           MedVar::makeConst(0xfffffffc, 4)));
  Func.Blocks.push_back(std::move(Runtime));

  EXPECT_EQ(entryStackOffset(Func, FP, Arch::X86, BinaryFormat::COFF), -4);
  EXPECT_EQ(
      entryStackOffset(Func, Resume.Output, Arch::X86, BinaryFormat::COFF),
      -32);
  EXPECT_FALSE(
      entryStackOffset(Func, Callback.Output, Arch::X86, BinaryFormat::COFF));
  EXPECT_FALSE(
      entryStackOffset(Func, Resume.Output, Arch::X86, BinaryFormat::ELF));
  simplifyProvenStackAlignment(Func, Arch::X86, BinaryFormat::COFF);
  EXPECT_EQ(Func.Blocks[1].Ops[3].Opcode, NdOp::INT_AND);
  EXPECT_EQ(Func.Blocks[1].Ops[4].Opcode, NdOp::COPY);
}

TEST(MedStackAlignment, RejectsMalformedRegistrationRootCarriers) {
  auto Root = op(NdOp::COPY, reg(1, Arch::X86), reg(0, Arch::X86));
  Root.RegistrationRoot = MedOp::RegistrationRootKind::RestoredStackPointer;
  Root.RegistrationStackOffset = -28;
  ASSERT_TRUE(hasValidRegistrationRootShape(Root));
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    auto Bad = Root;
    switch (Mutation) {
    case 0:
      Bad.OriginSeq = 0;
      break;
    case 1:
      Bad.Output.Size = 8;
      break;
    case 2:
      Bad.Inputs[0].RegOff = getTargetRegInfo(Arch::X86).FramePointer;
      break;
    case 3:
      Bad.Output.TheArch = Arch::X64;
      break;
    case 4:
      Bad.RegistrationStackOffset = -4;
      break;
    case 5:
      Bad.RegistrationStackOffset = INT32_MIN;
      break;
    case 6:
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 7:
      Bad.Opcode = NdOp::INT_ADD;
      break;
    case 8:
      Bad.RegistrationRoot = MedOp::RegistrationRootKind::None;
      break;
    case 9:
      ++Bad.Inputs[0].Id;
      break;
    case 10:
      Bad.Inputs[0].SSAVer = 1;
      break;
    case 11:
      Bad.Inputs[0].RenameTag = 1;
      break;
    }
    EXPECT_FALSE(hasValidRegistrationRootShape(Bad)) << Mutation;
    EXPECT_FALSE(registrationRootEntryStackOffset(Bad)) << Mutation;
  }
}

TEST(MedStackAlignment, HonorsEntryResiduesOnOtherArchitectures) {
  struct Case {
    Arch Architecture;
    BinaryFormat Format;
    uint64_t EntrySubtract;
    uint64_t Mask;
  };
  const Case Cases[] = {
      {Arch::X64, BinaryFormat::COFF, 8, UINT64_C(0xfffffffffffffff0)},
      {Arch::X86, BinaryFormat::MachO, 12, UINT32_C(0xfffffff0)},
      {Arch::AArch64, BinaryFormat::ELF, 16, UINT64_C(0xfffffffffffffff0)},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<int>(C.Architecture));
    MedFunc Func = frameWithEntrySP(C.Architecture);
    auto &Ops = Func.Blocks[0].Ops;
    const uint16_t Width = getTargetRegInfo(C.Architecture).PointerSize;
    Ops.push_back(op(NdOp::INT_SUB, temp(2, C.Architecture),
                     reg(0, C.Architecture),
                     MedVar::makeConst(C.EntrySubtract, Width)));
    Ops.push_back(op(NdOp::INT_AND, temp(3, C.Architecture),
                     temp(2, C.Architecture),
                     MedVar::makeConst(C.Mask, Width)));
    Ops.push_back(op(NdOp::INT_SUB, temp(4, C.Architecture),
                     reg(0, C.Architecture),
                     MedVar::makeConst(C.EntrySubtract - 1, Width)));
    Ops.push_back(op(NdOp::INT_AND, temp(5, C.Architecture),
                     temp(4, C.Architecture),
                     MedVar::makeConst(C.Mask, Width)));

    simplifyProvenStackAlignment(Func, C.Architecture, C.Format);

    EXPECT_EQ(Ops[2].Opcode, NdOp::COPY);
    EXPECT_EQ(Ops[4].Opcode, NdOp::INT_SUB);
    EXPECT_EQ(Ops[4].Inputs[1].ConstVal, 1u);
  }
}

TEST(MedStackAlignment, TheKernelEntersAProcessAligned) {
  // x86-64 `_start` pops argc and aligns the stack under argv.  A call would
  // have entered at 8 mod 16, where that mask clears nothing; the kernel
  // enters a System V process 16-byte aligned, where it clears eight bytes.
  // i386 code is only promised sixteen at that entry.
  struct Case {
    Arch Architecture;
    StackEntryKind Entry;
    NdOp Simplified;
  };
  const Case Cases[] = {
      {Arch::X64, StackEntryKind::Call, NdOp::COPY},
      {Arch::X64, StackEntryKind::ProcessEntry, NdOp::INT_SUB},
      {Arch::X86, StackEntryKind::Call, NdOp::INT_AND},
      {Arch::X86, StackEntryKind::ProcessEntry, NdOp::INT_SUB},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<int>(C.Architecture) * 2 +
                 static_cast<int>(C.Entry));
    MedFunc Func = frameWithEntrySP(C.Architecture);
    auto &Ops = Func.Blocks[0].Ops;
    const uint16_t Width = getTargetRegInfo(C.Architecture).PointerSize;
    const uint64_t Mask =
        Width == 8 ? UINT64_C(0xfffffffffffffff0) : UINT64_C(0xfffffff0);
    Ops.push_back(op(NdOp::INT_ADD, temp(2, C.Architecture),
                     reg(0, C.Architecture), MedVar::makeConst(Width, Width)));
    Ops.push_back(op(NdOp::INT_AND, temp(3, C.Architecture),
                     temp(2, C.Architecture), MedVar::makeConst(Mask, Width)));

    simplifyProvenStackAlignment(Func, C.Architecture, BinaryFormat::ELF,
                                 C.Entry);

    EXPECT_EQ(Ops[2].Opcode, C.Simplified);
    if (C.Simplified == NdOp::INT_SUB) {
      EXPECT_EQ(Ops[2].Inputs[0], temp(2, C.Architecture));
      EXPECT_EQ(Ops[2].Inputs[1].ConstVal, Width);
    }
  }
}

TEST(MedStackAlignment, AnEntryPointThatReturnsWasCalled) {
  // `_start` cannot return: the kernel left no return address.  A test
  // harness linking a function to the image's entry calls it, and it returns.
  MedFunc Start = frameWithEntrySP(Arch::X64);
  MedFunc Called = frameWithEntrySP(Arch::X64);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Called.Blocks[0].Ops.push_back(Return);

  EXPECT_EQ(functionEntryKind(Start, StackEntryKind::ProcessEntry),
            StackEntryKind::ProcessEntry);
  EXPECT_EQ(functionEntryKind(Called, StackEntryKind::ProcessEntry),
            StackEntryKind::Call);
  EXPECT_EQ(functionEntryKind(Start, StackEntryKind::Call),
            StackEntryKind::Call);
}

TEST(MedStackAlignment, AJoinHoldsTheOffsetEveryWayInAgreesOn) {
  // An x86 filter reads its frame through the EBP a join merges.  The join
  // holds an offset when every way in brings it, a way back through the join
  // included; a loop that moves the pointer holds none.
  for (bool Moves : {false, true}) {
    SCOPED_TRACE(Moves);
    MedFunc Func = frameWithEntrySP(Arch::X86);
    Func.Blocks[0].Ops.push_back(op(NdOp::INT_SUB, temp(2, Arch::X86),
                                    reg(0, Arch::X86),
                                    MedVar::makeConst(8, 4)));
    MedBlock Loop;
    Loop.Id = 1;
    Loop.StartAddr = 0x1010;
    PhiNode Join;
    Join.Output = temp(5, Arch::X86);
    Join.Args.push_back({0, temp(2, Arch::X86)});
    Join.Args.push_back({1, temp(6, Arch::X86)});
    Loop.Phis.push_back(Join);
    Loop.Ops.push_back(
        Moves ? op(NdOp::INT_ADD, temp(6, Arch::X86), temp(5, Arch::X86),
                   MedVar::makeConst(8, 4))
              : op(NdOp::COPY, temp(6, Arch::X86), temp(5, Arch::X86)));
    Func.Blocks.push_back(std::move(Loop));

    const auto Offset = entryStackOffset(Func, temp(5, Arch::X86), Arch::X86,
                                         BinaryFormat::COFF);
    if (Moves) {
      EXPECT_FALSE(Offset.has_value());
    } else {
      ASSERT_TRUE(Offset.has_value());
      EXPECT_EQ(*Offset, -8);
    }
  }
}

TEST(MedStackAlignment, EntryLiveInQueriesKeepExactRegisterOccurrences) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      MedFunc Func = frameWithEntrySP(Architecture);
      MedVar Entry = reg(0, Architecture);
      Entry.RenameTag = 7;
      Func.Blocks[0].Ops[0] = op(NdOp::COPY, Entry, Entry);
      MedVar Narrow = Entry;
      Narrow.Size /= 2;
      MedVar OtherArch = Entry;
      OtherArch.TheArch = Architecture == Arch::X64 ? Arch::AArch64 : Arch::X64;
      MedVar OtherName = Entry;
      OtherName.RenameTag = 8;
      MedVar OtherRegister = Entry;
      ++OtherRegister.RegOff;
      MedVar OtherVersion = Entry;
      ++OtherVersion.SSAVer;
      MedVar OtherId = Entry;
      ++OtherId.Id;
      Func.Blocks[0].Ops.push_back(
          op(NdOp::COPY, OtherId, temp(10, Architecture)));
      std::vector<MedVar> Values = {Narrow,        OtherArch,    OtherName,
                                    OtherRegister, OtherVersion, OtherId,
                                    Entry,         Entry};
      std::vector<std::optional<int64_t>> Expected = {std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      0,
                                                      0};
      for (bool Reversed : {false, true}) {
        SCOPED_TRACE(Reversed);
        if (Reversed) {
          std::reverse(Values.begin(), Values.end());
          std::reverse(Expected.begin(), Expected.end());
        }
        EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                  Expected);
        for (size_t I = 0; I < Values.size(); ++I) {
          SCOPED_TRACE(I);
          EXPECT_EQ(entryStackOffset(Func, Values[I], Architecture, Format),
                    Expected[I]);
        }
      }
    }
  }
}

TEST(MedStackAlignment, EntryStackQueriesRejectUnauthenticatedMarkers) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
        SCOPED_TRACE(Mutation);
        MedFunc Func = frameWithEntrySP(Architecture);
        MedVar Entry = reg(0, Architecture);
        MedBlock Other;
        Other.Id = 1;
        Other.StartAddr = 0x1010;
        auto &Ops = Func.Blocks[0].Ops;
        switch (Mutation) {
        case 0:
          Func.Entry = 0x2000;
          break;
        case 1:
          Other.StartAddr = Func.Entry;
          break;
        case 2:
          Other.StartAddr = Func.Entry;
          Func.Blocks[0].StartAddr = 0x2000;
          break;
        case 3:
          Ops.insert(Ops.begin(), op(NdOp::INT_ADD, temp(11, Architecture),
                                     temp(12, Architecture),
                                     MedVar::makeConst(1, Entry.Size)));
          break;
        case 4:
          Ops[0].Inputs[0].RenameTag = 1;
          break;
        case 5:
          Ops[0].addInput(MedVar::makeConst(0, Entry.Size));
          break;
        case 6:
          Ops[0].NumInputs = 0;
          break;
        case 7:
          Other.Ops.push_back(op(NdOp::COPY, Entry, Entry));
          break;
        case 8:
          ++Entry.SSAVer;
          Ops[0] = op(NdOp::COPY, Entry, Entry);
          break;
        }
        Func.Blocks.push_back(std::move(Other));
        EXPECT_FALSE(entryStackOffset(Func, Entry, Architecture, Format));
        EXPECT_EQ(
            entryStackOffsets(Func, {Entry, Entry}, Architecture, Format),
            (std::vector<std::optional<int64_t>>{std::nullopt, std::nullopt}));
      }
    }
  }
}

TEST(MedStackAlignment, EntryStackQueriesPreserveUndefinedIncomingSP) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      for (bool WithJoin : {false, true}) {
        SCOPED_TRACE(WithJoin);
        MedFunc Func = frameWithEntrySP(Architecture);
        Func.Blocks[0].Ops.clear();
        const MedVar Entry = reg(0, Architecture);
        if (WithJoin) {
          PhiNode Join;
          Join.Output = Entry;
          Join.Args = {{0, temp(20, Architecture)}};
          Func.Blocks[0].Phis.push_back(std::move(Join));
        }
        EXPECT_EQ(entryStackOffset(Func, Entry, Architecture, Format), 0);
        EXPECT_EQ(entryStackOffsets(Func, {Entry, Entry}, Architecture, Format),
                  (std::vector<std::optional<int64_t>>{0, 0}));
      }
    }
  }
}

TEST(MedStackAlignment, EntryStackQueriesReauthenticateAfterMutation) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      MedFunc Func = frameWithEntrySP(Architecture);
      const MedVar Entry = reg(0, Architecture);
      auto Expect = [&](std::optional<int64_t> Expected) {
        EXPECT_EQ(entryStackOffset(Func, Entry, Architecture, Format),
                  Expected);
        EXPECT_EQ(entryStackOffsets(Func, {Entry, Entry}, Architecture, Format),
                  (std::vector<std::optional<int64_t>>{Expected, Expected}));
      };
      Expect(0);
      Func.Blocks[0].Ops[0].Inputs[0].RenameTag = 1;
      Expect(std::nullopt);
      Func.Blocks[0].Ops[0].Inputs[0] = Entry;
      Expect(0);
      Func.Entry = 0x2000;
      Expect(std::nullopt);
      Func.Entry = Func.Blocks[0].StartAddr;
      Expect(0);
    }
  }
}

TEST(MedStackAlignment, BatchedQueriesRestoreStateAfterCyclesAndDepthRefusals) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      const unsigned Width = getTargetRegInfo(Architecture).PointerSize;
      MedFunc Func = frameWithEntrySP(Architecture);
      Func.Blocks[0].Ops.push_back(op(NdOp::INT_SUB, temp(2, Architecture),
                                      reg(0, Architecture),
                                      MedVar::makeConst(8, Width)));
      MedBlock Loop;
      Loop.Id = 1;
      Loop.StartAddr = 0x1010;
      for (int Id : {5, 7}) {
        PhiNode Join;
        Join.Output = temp(Id, Architecture);
        Join.Args = {{0, temp(2, Architecture)},
                     {1, temp(Id + 1, Architecture)}};
        Loop.Phis.push_back(std::move(Join));
        Loop.Ops.push_back(
            Id == 5 ? op(NdOp::COPY, temp(Id + 1, Architecture),
                         temp(Id, Architecture))
                    : op(NdOp::INT_ADD, temp(Id + 1, Architecture),
                         temp(Id, Architecture), MedVar::makeConst(1, Width)));
      }
      PhiNode Mixed;
      Mixed.Output = temp(9, Architecture);
      Mixed.Args = {{0, temp(5, Architecture)}, {1, temp(7, Architecture)}};
      Loop.Phis.push_back(std::move(Mixed));
      // Multiple definitions stay ambiguous, independently of a previous
      // query's accepted loop assumption or depth refusal.
      Loop.Ops.push_back(
          op(NdOp::COPY, temp(13, Architecture), reg(0, Architecture)));
      Loop.Ops.push_back(
          op(NdOp::COPY, temp(13, Architecture), temp(2, Architecture)));
      // Joins nest their proofs; copies and displacements do not.
      MedVar Deep = reg(0, Architecture);
      for (unsigned I = 0; I <= limits::ir::kMedStackAlignmentResolveDepthLimit;
           ++I) {
        PhiNode Next;
        Next.Output = temp(100 + I, Architecture);
        // The second way in is cheap: only depth, not work, refuses.
        Next.Args = {{0, Deep}, {1, reg(0, Architecture)}};
        Loop.Phis.push_back(Next);
        Deep = Next.Output;
      }
      Func.Blocks.push_back(std::move(Loop));
      MedVar Narrow = temp(2, Architecture);
      Narrow.Size /= 2;
      std::vector<MedVar> Values = {temp(5, Architecture),
                                    temp(9, Architecture),
                                    Narrow,
                                    temp(12, Architecture),
                                    temp(13, Architecture),
                                    temp(7, Architecture),
                                    temp(5, Architecture),
                                    Deep,
                                    temp(100, Architecture),
                                    Deep,
                                    reg(0, Architecture),
                                    temp(2, Architecture)};
      std::vector<std::optional<int64_t>> Expected = {-8,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      std::nullopt,
                                                      -8,
                                                      std::nullopt,
                                                      0,
                                                      std::nullopt,
                                                      0,
                                                      -8};
      for (bool Reversed : {false, true}) {
        SCOPED_TRACE(Reversed);
        if (Reversed) {
          std::reverse(Values.begin(), Values.end());
          std::reverse(Expected.begin(), Expected.end());
        }
        const auto Offsets =
            entryStackOffsets(Func, Values, Architecture, Format);
        ASSERT_EQ(Offsets.size(), Expected.size());
        for (size_t I = 0; I < Values.size(); ++I) {
          SCOPED_TRACE(I);
          EXPECT_EQ(Offsets[I], Expected[I]);
          EXPECT_EQ(entryStackOffset(Func, Values[I], Architecture, Format),
                    Expected[I]);
        }
      }
    }
  }
}

TEST(MedStackAlignment, InterlockingPhiLoopsShareAllIncomingStackOrigins) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Reversed : {false, true})
        for (unsigned Case = 0; Case != 5; ++Case) {
          SCOPED_TRACE(testing::Message() << static_cast<int>(Architecture)
                                          << ":" << static_cast<int>(Format)
                                          << ":" << Reversed << ":" << Case);
          auto Func = frameWithEntrySP(Architecture);
          const unsigned Width = getTargetRegInfo(Architecture).PointerSize;
          const auto Entry = reg(0, Architecture);
          const auto Initial = reg(1, Architecture);
          const auto Restored = reg(2, Architecture);
          const auto A = reg(3, Architecture);
          const auto B = reg(4, Architecture);
          const auto C = reg(5, Architecture);
          const auto D = reg(6, Architecture);
          auto &Ops = Func.Blocks[0].Ops;
          Ops.push_back(
              op(NdOp::INT_SUB, Initial, Entry, MedVar::makeConst(64, Width)));
          Ops.push_back(op(NdOp::INT_SUB, Restored, Entry,
                           MedVar::makeConst(Case == 1 ? 72 : 64, Width)));
          if (Case == 2)
            Ops.back().Inputs[0] = MedVar::makeUnspecified(Width, Architecture);
          // Model the ordinary loop and the independently restored SEH arm:
          // A = phi(initial, C), B = phi(A, restored, D), C = phi(B, D),
          // D = phi(C, restored). Every loop header needs the same invariant.
          const std::vector<std::pair<MedVar, std::vector<MedVar>>> Joins = {
              {A, {Case == 3 ? D : Initial, C}},
              {B, {A, Case == 3 ? C : Restored, D}},
              {C, {B, D}},
              {D, {C, Case == 3 ? A : Restored}}};
          for (const auto &[Output, Inputs] : Joins) {
            MedBlock Block;
            Block.Id = Func.Blocks.size();
            Block.StartAddr = 0x1000 + 16 * Block.Id;
            PhiNode Phi;
            Phi.Output = Output;
            for (size_t I = 0; I < Inputs.size(); ++I)
              Phi.Args.emplace_back(I, Inputs[I]);
            if (Case == 4 && Output == D)
              Phi.Args.back().second.Size /= 2;
            Block.Phis.push_back(std::move(Phi));
            Func.Blocks.push_back(std::move(Block));
          }
          Func.Blocks.back().Ops.push_back(op(NdOp::INT_ADD,
                                              temp(20, Architecture), C,
                                              MedVar::makeConst(16, Width)));
          if (Reversed)
            std::reverse(Func.Blocks.begin() + 1, Func.Blocks.end());
          const std::vector<MedVar> Values{
              A, B, C, D, temp(20, Architecture), Entry, Initial};
          const std::optional<int64_t> Stack =
              Case ? std::nullopt : std::optional<int64_t>(-64);
          const std::vector<std::optional<int64_t>> Expected{
              Stack,
              Stack,
              Stack,
              Stack,
              Case ? std::nullopt : std::optional<int64_t>(-48),
              0,
              -64};
          EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                    Expected);
          for (size_t I = 0; I < Values.size(); ++I)
            EXPECT_EQ(entryStackOffset(Func, Values[I], Architecture, Format),
                      Expected[I]);
        }
}

TEST(MedStackAlignment, ReusedJoinKeepsTheDepthOfItsNestedInputs) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      MedFunc Func = frameWithEntrySP(Architecture);
      MedBlock Block;
      Block.Id = 1;
      Block.StartAddr = 0x1010;
      PhiNode Join;
      Join.Output = temp(100, Architecture);
      Join.Args = {{0, reg(0, Architecture)}, {1, reg(0, Architecture)}};
      Block.Phis.push_back(Join);
      MedVar Previous = Join.Output;
      MedVar WithinLimit;
      for (unsigned I = 0; I < limits::ir::kMedStackAlignmentResolveDepthLimit;
           ++I) {
        PhiNode Next;
        Next.Output = temp(200 + I, Architecture);
        // The second way in is cheap: only depth, not work, refuses.
        Next.Args = {{0, Previous}, {1, reg(0, Architecture)}};
        Block.Phis.push_back(Next);
        Previous = Next.Output;
        if (I + 2 == limits::ir::kMedStackAlignmentResolveDepthLimit)
          WithinLimit = Next.Output;
      }
      Func.Blocks.push_back(std::move(Block));
      // The shallow query can be reused at the last valid depth, but its
      // incoming SP still needs one more level than a deeper use permits.
      std::vector<MedVar> Values{Join.Output, WithinLimit, Previous,
                                 Join.Output, Previous};
      std::vector<std::optional<int64_t>> Expected{0, 0, std::nullopt, 0,
                                                   std::nullopt};
      for (bool Reversed : {false, true}) {
        SCOPED_TRACE(Reversed);
        if (Reversed) {
          std::reverse(Values.begin(), Values.end());
          std::reverse(Expected.begin(), Expected.end());
        }
        EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                  Expected);
        for (size_t I = 0; I < Values.size(); ++I)
          EXPECT_EQ(entryStackOffset(Func, Values[I], Architecture, Format),
                    Expected[I]);
      }
    }
  }
}

TEST(MedStackAlignment, SharedJoinsKeepThePerQueryWorkLimit) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      MedFunc Func = frameWithEntrySP(Architecture);
      MedVar Previous = reg(0, Architecture);
      MedVar WithinBudget;
      // Fifteen shared joins take 65,535 visits. One more join exceeds the
      // per-query limit even though every incoming value has the same offset.
      for (unsigned I = 0; I != 16; ++I) {
        MedBlock Block;
        Block.Id = I + 1;
        Block.StartAddr = 0x1010 + 0x10 * I;
        PhiNode Join;
        Join.Output = temp(100 + I, Architecture);
        Join.Args = {{0, Previous}, {int(I + 1), Previous}};
        Previous = Join.Output;
        Block.Phis.push_back(std::move(Join));
        Func.Blocks.push_back(std::move(Block));
        if (I == 14)
          WithinBudget = Previous;
      }
      // This join reaches a previously proved input with some budget left,
      // but one step too little for the input's entire original traversal.
      PhiNode Asymmetric;
      Asymmetric.Output = temp(200, Architecture);
      Asymmetric.Args = {{0, reg(0, Architecture)}, {1, WithinBudget}};
      Func.Blocks.back().Phis.push_back(Asymmetric);
      std::vector<MedVar> Values = {WithinBudget,      Previous,
                                    Asymmetric.Output, reg(0, Architecture),
                                    WithinBudget,      Asymmetric.Output,
                                    Previous};
      std::vector<std::optional<int64_t>> Expected = {
          0, std::nullopt, std::nullopt, 0, 0, std::nullopt, std::nullopt};
      for (bool Reversed : {false, true}) {
        SCOPED_TRACE(Reversed);
        if (Reversed) {
          std::reverse(Values.begin(), Values.end());
          std::reverse(Expected.begin(), Expected.end());
        }
        EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                  Expected);
        for (size_t I = 0; I < Values.size(); ++I)
          EXPECT_EQ(entryStackOffset(Func, Values[I], Architecture, Format),
                    Expected[I]);
      }
      // An upper bound visits each shared join once, so the values the exact
      // proof leaves to its work limit still have one.
      EXPECT_EQ(entryStackOffsetUpperBounds(Func, Values, Architecture, Format),
                std::vector<std::optional<int64_t>>(Values.size(), 0));
    }
  }
}

TEST(MedStackAlignment, StackQueriesKeepDistinctValueKindsAndVersions) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      MedFunc Func = frameWithEntrySP(Architecture);
      const unsigned Width = getTargetRegInfo(Architecture).PointerSize;
      std::vector<MedVar> Values;
      std::vector<std::optional<int64_t>> Expected;
      for (unsigned I = 0; I != 128; ++I) {
        MedVar Register = reg(I + 1, Architecture);
        MedVar Temporary = temp(Register.Id, Architecture);
        Temporary.SSAVer = Register.SSAVer;
        for (MedVar Value : {Register, Temporary}) {
          const int64_t Offset =
              Value.Kind == MedVar::Reg ? -int64_t(I + 1) : int64_t(I + 1);
          Func.Blocks[0].Ops.push_back(
              op(NdOp::INT_ADD, Value, reg(0, Architecture),
                 MedVar::makeConst(static_cast<uint64_t>(Offset) &
                                       (Width == 4 ? UINT32_MAX : UINT64_MAX),
                                   Width)));
          Values.push_back(Value);
          Expected.push_back(Offset);
        }
      }
      EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                Expected);
      std::reverse(Values.begin(), Values.end());
      std::reverse(Expected.begin(), Expected.end());
      EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                Expected);
    }
  }
}

TEST(MedStackAlignment, ZeroMasksDoNotProveStackAlignment) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(Reversed);
      MedFunc Func = frameWithEntrySP(Architecture);
      auto &Ops = Func.Blocks[0].Ops;
      const uint16_t Width = getTargetRegInfo(Architecture).PointerSize;
      const MedVar Stack = reg(0, Architecture);
      const MedVar Zero =
          MedVar::makeConst(0, Width, ConstantAddressProvenance::Scalar);
      Ops.push_back(op(NdOp::INT_AND, temp(2, Architecture),
                       Reversed ? Zero : Stack, Reversed ? Stack : Zero));
      const MedOp Original = Ops.back();

      simplifyProvenStackAlignment(Func, Architecture, BinaryFormat::MachO);

      // Clearing every bit produces zero, never a stack-relative value.
      // In particular, complement(mask) + 1 must not wrap into an accepted
      // zero alignment for a 64-bit operand.
      EXPECT_EQ(Ops.back().Opcode, NdOp::INT_AND);
      ASSERT_EQ(Ops.back().NumInputs, 2);
      EXPECT_EQ(Ops.back().Inputs[0], Original.Inputs[0]);
      EXPECT_EQ(Ops.back().Inputs[1], Original.Inputs[1]);
    }
  }
}

TEST(MedStackAlignment, ZeroMaskedValuesDoNotBecomeFrameAliases) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(Reversed);
      MedFunc Func = frameWithEntrySP(Architecture);
      auto &Ops = Func.Blocks[0].Ops;
      const uint16_t Width = getTargetRegInfo(Architecture).PointerSize;
      const MedVar Stack = reg(0, Architecture);
      const MedVar Zero =
          MedVar::makeConst(0, Width, ConstantAddressProvenance::Scalar);
      const MedVar Zeroed = temp(2, Architecture);
      const uint64_t Mask =
          Width == 8 ? UINT64_C(0xfffffffffffffff8) : UINT32_C(0xfffffff8);
      const MedVar Alignment =
          MedVar::makeConst(Mask, Width, ConstantAddressProvenance::Scalar);
      Ops.push_back(op(NdOp::INT_AND, Zeroed, Reversed ? Zero : Stack,
                       Reversed ? Stack : Zero));
      Ops.push_back(op(NdOp::INT_AND, temp(3, Architecture),
                       Reversed ? Alignment : Zeroed,
                       Reversed ? Zeroed : Alignment));
      const MedOp Original = Ops.back();

      simplifyProvenStackAlignment(Func, Architecture, BinaryFormat::MachO);

      // A later valid alignment mask does not authenticate its zero-valued
      // input as an entry-stack alias. Neither recursive proof nor rewrites
      // of an earlier definition may introduce that relationship.
      EXPECT_EQ(Ops.back().Opcode, NdOp::INT_AND);
      ASSERT_EQ(Ops.back().NumInputs, 2);
      EXPECT_EQ(Ops.back().Inputs[0], Original.Inputs[0]);
      EXPECT_EQ(Ops.back().Inputs[1], Original.Inputs[1]);
    }
  }
}

TEST(MedStackAlignment, LoopInductionRechecksInteriorQueries) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      MedFunc Func = frameWithEntrySP(Architecture);
      const MedVar A = temp(2, Architecture);
      const MedVar B = temp(3, Architecture);
      MedBlock Loop;
      Loop.Id = 1;
      Loop.StartAddr = 0x1010;
      PhiNode Anchored;
      Anchored.Output = A;
      Anchored.Args = {{0, reg(0, Architecture)}, {1, B}};
      Loop.Phis.push_back(std::move(Anchored));
      Loop.Ops.push_back(op(NdOp::COPY, B, A));
      Func.Blocks.push_back(std::move(Loop));
      // A's entry arm anchors an induction proof. Starting at its backedge
      // must recheck B under that assumption, independently of query order.
      EXPECT_EQ(entryStackOffset(Func, A, Architecture, Format), 0);
      EXPECT_EQ(entryStackOffset(Func, B, Architecture, Format), 0);
      std::vector<MedVar> Values{A, B, A, B};
      std::vector<std::optional<int64_t>> Expected{0, 0, 0, 0};
      for (bool Reversed : {false, true}) {
        SCOPED_TRACE(Reversed);
        if (Reversed) {
          std::reverse(Values.begin(), Values.end());
          std::reverse(Expected.begin(), Expected.end());
        }
        EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                  Expected);
      }
      // A conditional backedge value cannot become an independent proof
      // when the complete induction step fails.
      Func.Blocks[1].Ops[0] =
          op(NdOp::INT_ADD, B, A,
             MedVar::makeConst(1, getTargetRegInfo(Architecture).PointerSize));
      for (bool Reversed : {false, true}) {
        if (Reversed)
          std::reverse(Values.begin(), Values.end());
        EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                  std::vector<std::optional<int64_t>>(Values.size()));
      }
    }
  }
}

TEST(MedStackAlignment, BalancedPushGroupsKeepEveryLoopStackCoordinate) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64, Arch::X86, Arch::ARM})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Drifts : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << static_cast<int>(Architecture) << ":"
                     << static_cast<int>(Format) << ":" << Drifts);
        const unsigned Width = getTargetRegInfo(Architecture).PointerSize;
        auto Func = frameWithEntrySP(Architecture);
        Func.Blocks[0].Ops.push_back(op(NdOp::INT_SUB, reg(1, Architecture),
                                        reg(0, Architecture),
                                        MedVar::makeConst(256, Width)));
        Func.Blocks[0].Succs = {1};
        MedBlock Loop;
        Loop.Id = 1;
        Loop.StartAddr = 0x1010;
        Loop.Preds = {0, 1};
        Loop.Succs = {1};
        MedVar Previous = reg(2, Architecture);
        std::vector<MedVar> Values{Previous};
        std::vector<std::optional<int64_t>> Expected{-256};
        int Version = 3;
        int64_t Offset = -256;
        // The linked x64 fixture calls with four integer stack arguments,
        // one floating argument, then five packed arguments and padding.
        for (int Delta :
             {-8, -8, -8, -8, 32, -8, -8, 16, -8, -8, -8, -8, -8, -8, 48}) {
          const MedVar Next = reg(Version++, Architecture);
          Loop.Ops.push_back(op(NdOp::INT_ADD, Next, Previous,
                                MedVar::makeConst(Delta, Width)));
          Previous = Next;
          Offset += Delta;
          Values.push_back(Next);
          Expected.push_back(Offset);
        }
        PhiNode Join;
        Join.Output = reg(2, Architecture);
        Join.Args = {{0, reg(1, Architecture)}, {1, Previous}};
        Loop.Phis.push_back(std::move(Join));
        if (Drifts) {
          Loop.Ops.back().Inputs[1] = MedVar::makeConst(49, Width);
          Expected.assign(Values.size(), std::nullopt);
        }
        Func.Blocks.push_back(std::move(Loop));
        for (bool Reversed : {false, true}) {
          if (Reversed) {
            std::reverse(Values.begin(), Values.end());
            std::reverse(Expected.begin(), Expected.end());
          }
          EXPECT_EQ(entryStackOffsets(Func, Values, Architecture, Format),
                    Expected);
          for (size_t I = 0; I < Values.size(); ++I)
            EXPECT_EQ(entryStackOffset(Func, Values[I], Architecture, Format),
                      Expected[I]);
        }
      }
}

} // namespace
