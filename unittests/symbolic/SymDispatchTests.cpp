//===- SymDispatchTests.cpp - Reading the shape of a computed branch ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pins what the analysis reads off each dispatch shape, and what it refuses
/// to read off something that is not one.
///
/// The refusals carry the weight.  Reporting a table where there is none turns
/// an indirect call into a list of addresses that are not code, and everything
/// downstream believes it.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymDispatch.h"

#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr uint64_t kIndexReg = 0;
constexpr uint64_t kScratch = 8;
constexpr uint64_t kTable = 0x402000;
constexpr uint64_t kCodeBase = 0x401000;

LowOp op(NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const NdVar &In : Inputs)
    Result.addInput(In);
  return Result;
}

TEST(SymDispatch, ReadsAnAbsoluteTableOffOneExecution) {
  // `jmp [0x402000 + idx*8]` — the entry is the address.
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(8, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 8), {NdVar::tmp(8, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(16, 8)})};

  std::optional<DispatchShape> Shape = analyzeDispatch(Ctx, Ops, kIndexReg);
  ASSERT_TRUE(Shape.has_value());
  EXPECT_EQ(Shape->Kind, DispatchKind::Absolute);
  EXPECT_EQ(Shape->TableBase, kTable);
  EXPECT_EQ(Shape->EntryStride, 8u);
  EXPECT_EQ(Shape->EntrySize, 8u);
}

TEST(SymDispatch, KeepsPhysicalStrideSeparateFromEntryWidth) {
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(8, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 4), {NdVar::tmp(8, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(16, 4)})};

  std::optional<DispatchShape> Shape = analyzeDispatch(Ctx, Ops, kIndexReg);
  ASSERT_TRUE(Shape.has_value());
  EXPECT_EQ(Shape->EntryStride, 8u);
  EXPECT_EQ(Shape->EntrySize, 4u);
}

TEST(SymDispatch, ReachesTheBranchPastAnUnrelatedUnknownOperation) {
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(8, 8)}),
      op(NdOp::FLOAT_SQRT, NdVar::reg(kScratch, 8), {NdVar::reg(kScratch, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 8), {NdVar::tmp(8, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(16, 8)})};

  std::optional<DispatchShape> Shape = analyzeDispatch(Ctx, Ops, kIndexReg);
  ASSERT_TRUE(Shape.has_value());
  EXPECT_EQ(Shape->TableBase, kTable);
  EXPECT_EQ(Shape->EntryStride, 8u);
}

TEST(SymDispatch, AShiftIsTheSameScaleAsAMultiply) {
  // Compilers write the scale as a shift; the builders have already made the
  // two the same thing, so nothing here has to know about shifts.
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_LEFT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(2, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 4), {NdVar::tmp(8, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(16, 4)})};

  std::optional<DispatchShape> Shape = analyzeDispatch(Ctx, Ops, kIndexReg);
  ASSERT_TRUE(Shape.has_value());
  EXPECT_EQ(Shape->EntryStride, 4u) << "a shift by two scales by four";
  EXPECT_EQ(Shape->EntrySize, 4u);
}

TEST(SymDispatch, ReadsARelativeTableAndNoticesTheSign) {
  // Position-independent form: the entry is a signed displacement from a base,
  // so entries can point backwards.
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(4, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 4), {NdVar::tmp(8, 8)}),
      op(NdOp::INT_SEXT, NdVar::tmp(24, 8), {NdVar::tmp(16, 4)}),
      op(NdOp::INT_ADD, NdVar::tmp(32, 8),
         {NdVar::tmp(24, 8), NdVar::cst(kCodeBase, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(32, 8)})};

  std::optional<DispatchShape> Shape = analyzeDispatch(Ctx, Ops, kIndexReg);
  ASSERT_TRUE(Shape.has_value());
  EXPECT_EQ(Shape->Kind, DispatchKind::Relative);
  EXPECT_EQ(Shape->TableBase, kTable);
  EXPECT_EQ(Shape->EntryStride, 4u);
  EXPECT_EQ(Shape->EntrySize, 4u);
  EXPECT_EQ(Shape->RelativeBase, kCodeBase);
  EXPECT_EQ(Shape->EntryScale, 1u);
  EXPECT_TRUE(Shape->EntryIsSigned);
}

TEST(SymDispatch, ReadsTheCompactFormThatScalesWhatItLoaded) {
  // The AArch64 byte-table shape: the entry counts instructions rather than
  // bytes, so it is scaled before being added to the base.
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_ADD, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(8, 1), {NdVar::tmp(0, 8)}),
      op(NdOp::INT_ZEXT, NdVar::tmp(16, 8), {NdVar::tmp(8, 1)}),
      op(NdOp::INT_LEFT, NdVar::tmp(24, 8),
         {NdVar::tmp(16, 8), NdVar::cst(2, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(32, 8),
         {NdVar::tmp(24, 8), NdVar::cst(kCodeBase, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(32, 8)})};

  std::optional<DispatchShape> Shape = analyzeDispatch(Ctx, Ops, kIndexReg);
  ASSERT_TRUE(Shape.has_value());
  EXPECT_EQ(Shape->Kind, DispatchKind::Relative);
  EXPECT_EQ(Shape->TableBase, kTable);
  EXPECT_EQ(Shape->EntryStride, 1u) << "one byte per entry";
  EXPECT_EQ(Shape->EntrySize, 1u);
  EXPECT_EQ(Shape->EntryScale, 4u) << "entries count instructions";
  EXPECT_EQ(Shape->RelativeBase, kCodeBase);
  EXPECT_FALSE(Shape->EntryIsSigned);
}

//===----------------------------------------------------------------------===//
// What it must refuse
//===----------------------------------------------------------------------===//

TEST(SymDispatch, RefusesABranchThatDoesNotDependOnTheIndex) {
  // A call through a function pointer loaded from a fixed address.  It reaches
  // an unresolved target, but nothing about it varies with the index, so it is
  // not a table and must not be described as one.
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::LOAD, NdVar::tmp(0, 8), {NdVar::cst(0x403000, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(0, 8)})};
  EXPECT_FALSE(analyzeDispatch(Ctx, Ops, kIndexReg).has_value());
}

TEST(SymDispatch, RefusesATargetThatWasNeverLoaded) {
  // `jmp base + idx*8` with no load: an address computed from the index, not a
  // table of them.  Reporting a table here would invent entries out of code.
  SymContext Ctx;
  std::vector<LowOp> Ops = {op(NdOp::INT_MULT, NdVar::tmp(0, 8),
                               {NdVar::reg(kIndexReg, 8), NdVar::cst(8, 8)}),
                            op(NdOp::INT_ADD, NdVar::tmp(8, 8),
                               {NdVar::tmp(0, 8), NdVar::cst(kCodeBase, 8)}),
                            op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(8, 8)})};
  EXPECT_FALSE(analyzeDispatch(Ctx, Ops, kIndexReg).has_value());
}

TEST(SymDispatch, RefusesAnAddressThatTwoUnknownsReach) {
  // Indexed by the sum of two registers.  There is a table somewhere in here,
  // but not one this analysis can name a stride for.
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_ADD, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::reg(kScratch, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 8), {NdVar::tmp(8, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(16, 8)})};
  EXPECT_FALSE(analyzeDispatch(Ctx, Ops, kIndexReg).has_value());
}

TEST(SymDispatch, RefusesAValueLoadedFromTwoCandidateTables) {
  // Expression interning makes the two loads equal because both tables were
  // given the same symbolic entry.  Picking whichever address was recorded
  // first would make the reported table depend on instruction order.
  SymContext Ctx;
  constexpr uint64_t OtherTable = kTable + 0x1000;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(8, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(16, 8),
         {NdVar::tmp(0, 8), NdVar::cst(OtherTable, 8)}),
      op(NdOp::STORE, NdVar{}, {NdVar::tmp(8, 8), NdVar::reg(kScratch, 8)}),
      op(NdOp::STORE, NdVar{}, {NdVar::tmp(16, 8), NdVar::reg(kScratch, 8)}),
      op(NdOp::LOAD, NdVar::tmp(24, 8), {NdVar::tmp(8, 8)}),
      op(NdOp::LOAD, NdVar::tmp(32, 8), {NdVar::tmp(16, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(32, 8)})};

  EXPECT_FALSE(analyzeDispatch(Ctx, Ops, kIndexReg).has_value());
  EXPECT_TRUE(dispatchVariesWithIndex(Ctx, Ops, kIndexReg));
}

TEST(SymDispatch, RefusesABranchThatIsNotComputedAtAll) {
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::BRANCH, NdVar{}, {NdVar::cst(kCodeBase, 8)})};
  EXPECT_FALSE(analyzeDispatch(Ctx, Ops, kIndexReg).has_value());
}

TEST(SymDispatch, SurvivesAStoreThroughAnAddressItCannotPinDown) {
  // Once memory has been clobbered every load is unknown, which is correct and
  // costs the analysis the table.  It must give up rather than describe one.
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::STORE, NdVar{}, {NdVar::reg(kScratch, 8), NdVar::cst(0, 8)}),
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(8, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 8), {NdVar::tmp(8, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(16, 8)})};

  // The address is still linear in the index, so the shape is still readable —
  // what was lost is any hope of folding the entry itself.
  std::optional<DispatchShape> Shape = analyzeDispatch(Ctx, Ops, kIndexReg);
  ASSERT_TRUE(Shape.has_value());
  EXPECT_EQ(Shape->TableBase, kTable);
  EXPECT_EQ(Shape->EntryStride, 8u);
}

//===----------------------------------------------------------------------===//
// The narrower question
//===----------------------------------------------------------------------===//

TEST(SymDispatch, KnowsWhetherTheTargetVariesWithTheIndexAtAll) {
  SymContext Ctx;

  // A table: the target is the contents of somewhere, and the somewhere is
  // where the index appears.  Following the load's address is what finds it.
  std::vector<LowOp> Table = {
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(8, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 8), {NdVar::tmp(8, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(16, 8)})};
  EXPECT_TRUE(dispatchVariesWithIndex(Ctx, Table, kIndexReg));

  // A function pointer from a fixed address: unresolved, but it goes to one
  // place whatever the index is.
  std::vector<LowOp> Pointer = {
      op(NdOp::LOAD, NdVar::tmp(0, 8), {NdVar::cst(0x403000, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(0, 8)})};
  EXPECT_FALSE(dispatchVariesWithIndex(Ctx, Pointer, kIndexReg));

  // Through a different register: unresolved and index-independent again.
  std::vector<LowOp> Other = {
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::reg(kScratch, 8)})};
  EXPECT_FALSE(dispatchVariesWithIndex(Ctx, Other, kIndexReg));
}

TEST(SymDispatch, FollowsTheIndexThroughMoreThanOneLoad) {
  // Two levels of indirection — a table of pointers to tables.  The shape is
  // not one this can name, but the dependence on the index is still plain, and
  // saying otherwise would throw away a real dispatch.
  SymContext Ctx;
  std::vector<LowOp> Ops = {
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kIndexReg, 8), NdVar::cst(8, 8)}),
      op(NdOp::INT_ADD, NdVar::tmp(8, 8),
         {NdVar::tmp(0, 8), NdVar::cst(kTable, 8)}),
      op(NdOp::LOAD, NdVar::tmp(16, 8), {NdVar::tmp(8, 8)}),
      op(NdOp::LOAD, NdVar::tmp(24, 8), {NdVar::tmp(16, 8)}),
      op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(24, 8)})};

  EXPECT_TRUE(dispatchVariesWithIndex(Ctx, Ops, kIndexReg));
  EXPECT_FALSE(analyzeDispatch(Ctx, Ops, kIndexReg).has_value())
      << "the shape is unreadable, which is not the same as index-independent";
}

} // namespace
