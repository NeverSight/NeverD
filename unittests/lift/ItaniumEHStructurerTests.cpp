//===- ItaniumEHStructurerTests.cpp - LSDA call sites to HighIR try -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An Itanium call-site table says which stretch of code reaches which landing
/// pad; it never says which stretches belong to one source-level `try`.  What
/// carries that is the action chain, so these tests pin the reading that turns
/// a flat table back into nested regions: which call sites a clause collects,
/// where a run has to break, and which shapes must produce no region at all.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/ExceptionInfo.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t kEntry = 0x400000;
/// Every address a test hands to a table is stated in slots of this size, so a
/// call site names a region of the body without any test having to spell an
/// address out.
constexpr uint64_t kSlotSize = 0x10;
/// Three call slots is the smallest body that can hold a nested pair, a third
/// region after them, and a return outside every region.
constexpr unsigned kCallSlots = 3;
constexpr unsigned kSlotCount = kCallSlots + 1;

/// One straight-line block holding a call in each of the first \p kCallSlots
/// slots and a return in the last.  A call survives to HighIR carrying its
/// address, which is what the structurer classifies against; separate blocks
/// would not, because nothing branches to them and an unreachable block is
/// dropped long before this transform runs.
MedFunc makeFunction() {
  MedFunc Func;
  Func.Entry = kEntry;
  Func.Name = "itanium_eh";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = kEntry;
  Block.EndAddr = kEntry + kSlotCount * kSlotSize;
  for (unsigned Slot = 0; Slot < kCallSlots; ++Slot) {
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = kEntry + Slot * kSlotSize + 4;
    Call.addInput(MedVar::makeConst(0x410000 + Slot * 0x10, 8));
    Block.Ops.push_back(std::move(Call));
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = kEntry + kCallSlots * kSlotSize + 4;
  Block.Ops.push_back(std::move(Return));
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

ExceptionFunction makeRecord() {
  ExceptionFunction EH;
  EH.CodeRange = {kEntry, kEntry + kSlotCount * kSlotSize};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  return EH;
}

ItaniumCallSite makeCallSite(unsigned FirstSlot, unsigned SlotCount,
                             va_t LandingPad,
                             std::optional<uint64_t> FirstAction) {
  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry + FirstSlot * kSlotSize,
                       kEntry + (FirstSlot + SlotCount) * kSlotSize};
  Site.LandingPadVA = LandingPad;
  Site.FirstActionOffset = FirstAction;
  return Site;
}

ItaniumAction makeAction(uint64_t TableOffset, int64_t TypeFilter,
                         std::optional<uint64_t> Next = std::nullopt) {
  ItaniumAction Action;
  Action.TableOffset = TableOffset;
  Action.TypeFilter = TypeFilter;
  Action.NextActionOffset = Next;
  return Action;
}

ItaniumTypeEntry makeType(uint64_t Index, va_t TypeInfoVA,
                          const std::string &Name) {
  ItaniumTypeEntry Entry;
  Entry.Index = Index;
  Entry.TypeInfoVA = TypeInfoVA;
  Entry.TypeName = Name;
  Entry.IsCatchAll = TypeInfoVA == 0;
  return Entry;
}

HighFunc structure(ItaniumEHInfo LSDA) {
  MedFunc Func = makeFunction();
  ExceptionFunction EH = makeRecord();
  EH.Itanium = std::move(LSDA);
  Func.ExceptionMetadata = std::move(EH);
  return MedToHighConverter().convert(Func, Arch::X64);
}

/// The landing pad sits in the last slot, which no region covers.
constexpr va_t kPad = kEntry + kCallSlots * kSlotSize;

TEST(ItaniumEHStructurer, RecoversOneTypedCatch) {
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, 1));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTISt13runtime_error"));

  HighFunc Func = structure(std::move(LSDA));
  EXPECT_EQ(Func.StructuredExceptionRegions, 1u);
  EXPECT_EQ(Func.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(Func.Body.empty());

  const HighStmt &Try = Func.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::ItaniumTry);
  EXPECT_EQ(Try.EHRange.Begin, kEntry);
  EXPECT_EQ(Try.EHRange.End, kEntry + kSlotSize);
  ASSERT_EQ(Try.EHClauses.size(), 1u);

  const HighEHClause &Catch = Try.EHClauses.front();
  EXPECT_EQ(Catch.Kind, HighEHClauseKind::ItaniumCatch);
  EXPECT_EQ(Catch.TypeFilter, 1);
  EXPECT_EQ(Catch.TypeName, "_ZTISt13runtime_error");
  EXPECT_EQ(Catch.TypeDescriptorVA, 0x500000u);
  EXPECT_EQ(Catch.HandlerVA, kPad);
  ASSERT_EQ(Catch.LandingPadVAs.size(), 1u);
  EXPECT_EQ(Catch.LandingPadVAs.front(), kPad);
  EXPECT_EQ(Catch.ParseStatus, ExceptionParseStatus::Complete);
}

TEST(ItaniumEHStructurer, KeepsSiblingCatchesInChainOrder) {
  // `try { … } catch (A) { … } catch (B) { … }`: one chain, two filters, and
  // one call site that names both.  The clauses share a region because they
  // collect the same call sites, and they keep the order the personality
  // tests them in rather than the order the table stores them in.
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 4));
  LSDA.Actions.push_back(makeAction(0, 2));
  LSDA.Actions.push_back(makeAction(4, 1, 0));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI1A"));
  LSDA.TypeTable.push_back(makeType(2, 0x500008, "_ZTI1B"));

  HighFunc Func = structure(std::move(LSDA));
  EXPECT_EQ(Func.StructuredExceptionRegions, 1u);
  ASSERT_FALSE(Func.Body.empty());

  const HighStmt &Try = Func.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::ItaniumTry);
  ASSERT_EQ(Try.EHClauses.size(), 2u);
  EXPECT_EQ(Try.EHClauses[0].TypeName, "_ZTI1A");
  EXPECT_EQ(Try.EHClauses[0].ChainDepth, 0u);
  EXPECT_EQ(Try.EHClauses[1].TypeName, "_ZTI1B");
  EXPECT_EQ(Try.EHClauses[1].ChainDepth, 1u);
}

TEST(ItaniumEHStructurer, OrdersClausesByDepthInTheirOwnChain) {
  // The enclosing clause is reached at depth 1 from the inner call site and at
  // depth 0 from the one after the inner handler, so no single position in the
  // table stands for it.  What has to decide the order in a region is the
  // depth in *that* region's chain: reading the table's own order, or the
  // earliest depth anywhere, puts the enclosing catch before the inner one.
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 4));
  LSDA.CallSites.push_back(makeCallSite(1, 1, 0, std::nullopt));
  LSDA.CallSites.push_back(makeCallSite(2, 1, kPad + 4, 0));
  LSDA.Actions.push_back(makeAction(0, 2));
  LSDA.Actions.push_back(makeAction(4, 1, 0));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI5Inner"));
  LSDA.TypeTable.push_back(makeType(2, 0x500008, "_ZTI5Outer"));

  HighFunc Func = structure(std::move(LSDA));
  ASSERT_FALSE(Func.Body.empty());
  const HighStmt &Try = Func.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::ItaniumTry);
  EXPECT_EQ(Try.EHRange.End, kEntry + kSlotSize);
  ASSERT_EQ(Try.EHClauses.size(), 2u);
  EXPECT_EQ(Try.EHClauses[0].TypeName, "_ZTI5Inner");
  EXPECT_EQ(Try.EHClauses[0].ChainDepth, 0u);
  EXPECT_EQ(Try.EHClauses[1].TypeName, "_ZTI5Outer");
  EXPECT_EQ(Try.EHClauses[1].ChainDepth, 1u);
}

TEST(ItaniumEHStructurer, NestsRegionsFromTheSharedActionChain) {
  // An inner `try` whose chain continues into the enclosing one.  The outer
  // clause is named by a superset of the call sites, so its region contains
  // the inner region rather than merely overlapping it.
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 4));
  LSDA.CallSites.push_back(makeCallSite(1, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, 2));
  LSDA.Actions.push_back(makeAction(4, 1, 0));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI5Inner"));
  LSDA.TypeTable.push_back(makeType(2, 0x500008, "_ZTI5Outer"));

  HighFunc Func = structure(std::move(LSDA));
  EXPECT_EQ(Func.UnstructuredExceptionRegions, 0u);
  ASSERT_FALSE(Func.Body.empty());

  const HighStmt &Outer = Func.Body.front();
  ASSERT_EQ(Outer.Kind, StmtKind::ItaniumTry);
  EXPECT_EQ(Outer.EHRange.Begin, kEntry);
  EXPECT_EQ(Outer.EHRange.End, kEntry + 2 * kSlotSize);
  ASSERT_EQ(Outer.EHClauses.size(), 1u);
  EXPECT_EQ(Outer.EHClauses.front().TypeName, "_ZTI5Outer");

  ASSERT_FALSE(Outer.Body.empty());
  const HighStmt &Inner = Outer.Body.front();
  ASSERT_EQ(Inner.Kind, StmtKind::ItaniumTry);
  EXPECT_EQ(Inner.EHRange.Begin, kEntry);
  EXPECT_EQ(Inner.EHRange.End, kEntry + kSlotSize);
  ASSERT_EQ(Inner.EHClauses.size(), 1u);
  EXPECT_EQ(Inner.EHClauses.front().TypeName, "_ZTI5Inner");
}

TEST(ItaniumEHStructurer, BreaksARunAtACallSiteWithNoHandler) {
  // Two separate `try` blocks that caught the same type, so the compiler gave
  // them one action record.  What keeps them apart is the entry between them:
  // a stretch that can throw and reaches no handler here.  Merging across it
  // would put code that is outside both try blocks inside one.
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.CallSites.push_back(makeCallSite(1, 1, 0, std::nullopt));
  LSDA.CallSites.push_back(makeCallSite(2, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, 1));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI1A"));

  HighFunc Func = structure(std::move(LSDA));
  EXPECT_EQ(Func.StructuredExceptionRegions, 2u);
  EXPECT_EQ(Func.UnstructuredExceptionRegions, 0u);

  std::vector<std::pair<va_t, va_t>> Regions;
  std::optional<size_t> FirstTry;
  std::optional<size_t> LastTry;
  for (size_t I = 0; I < Func.Body.size(); ++I) {
    if (Func.Body[I].Kind != StmtKind::ItaniumTry)
      continue;
    Regions.emplace_back(Func.Body[I].EHRange.Begin, Func.Body[I].EHRange.End);
    if (!FirstTry)
      FirstTry = I;
    LastTry = I;
  }
  const std::vector<std::pair<va_t, va_t>> Expected = {
      {kEntry, kEntry + kSlotSize},
      {kEntry + 2 * kSlotSize, kEntry + 3 * kSlotSize}};
  EXPECT_EQ(Regions, Expected);
  // The unguarded call has to stay between the two regions rather than being
  // swallowed by either of them.
  ASSERT_TRUE(FirstTry && LastTry);
  EXPECT_EQ(*LastTry - *FirstTry, 2u);
}

TEST(ItaniumEHStructurer, MergesAcrossAGapNoEntryCovers) {
  // The same two call sites with nothing between them.  A compiler emits no
  // entry for a stretch that cannot throw, so the gap is code inside the try
  // rather than code outside it, and the region has to span it.
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.CallSites.push_back(makeCallSite(2, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, 1));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI1A"));

  HighFunc Func = structure(std::move(LSDA));
  EXPECT_EQ(Func.StructuredExceptionRegions, 2u);
  ASSERT_FALSE(Func.Body.empty());
  EXPECT_EQ(Func.Body.front().Kind, StmtKind::ItaniumTry);
  EXPECT_EQ(Func.Body.front().EHRange.Begin, kEntry);
  EXPECT_EQ(Func.Body.front().EHRange.End, kEntry + 3 * kSlotSize);
}

TEST(ItaniumEHStructurer, CollectsEveryLandingPadOneClauseIsReachedThrough) {
  // Two calls in one try block, the second made with one more local object
  // alive.  Each unwinds through its own pad, and both reach the same catch.
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.CallSites.push_back(makeCallSite(1, 1, kPad + 4, 0));
  LSDA.Actions.push_back(makeAction(0, 1));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI1A"));

  HighFunc Func = structure(std::move(LSDA));
  ASSERT_FALSE(Func.Body.empty());
  const HighStmt &Try = Func.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::ItaniumTry);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  EXPECT_EQ(Try.EHClauses.front().LandingPadVAs,
            std::vector<va_t>({kPad, kPad + 4}));
  EXPECT_EQ(Try.EHClauses.front().HandlerVA, kPad);
}

TEST(ItaniumEHStructurer, NamesACatchAllWithoutATypeDescriptor) {
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, 1));
  LSDA.TypeTable.push_back(makeType(1, 0, ""));

  HighFunc Func = structure(std::move(LSDA));
  ASSERT_FALSE(Func.Body.empty());
  const HighStmt &Try = Func.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::ItaniumTry);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  EXPECT_EQ(Try.EHClauses.front().Kind, HighEHClauseKind::ItaniumCatch);
  EXPECT_TRUE(Try.EHClauses.front().TypeName.empty());
  EXPECT_EQ(Try.EHClauses.front().TypeDescriptorVA, 0u);
  EXPECT_EQ(Try.EHClauses.front().ParseStatus, ExceptionParseStatus::Complete);
}

TEST(ItaniumEHStructurer, ReadsAnEmptyExceptionSpecificationAsNoexcept) {
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, -1));
  ItaniumExceptionSpec Spec;
  Spec.Index = 1;
  LSDA.ExceptionSpecs.push_back(std::move(Spec));

  HighFunc Func = structure(std::move(LSDA));
  ASSERT_FALSE(Func.Body.empty());
  const HighStmt &Try = Func.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::ItaniumTry);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  EXPECT_EQ(Try.EHClauses.front().Kind, HighEHClauseKind::ItaniumSpec);
  EXPECT_EQ(Try.EHClauses.front().TypeFilter, -1);
  EXPECT_TRUE(Try.EHClauses.front().SpecTypeNames.empty());
}

TEST(ItaniumEHStructurer, ListsTheTypesAnExceptionSpecificationPermits) {
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, -1));
  ItaniumExceptionSpec Spec;
  Spec.Index = 1;
  Spec.TypeIndices = {1, 2};
  LSDA.ExceptionSpecs.push_back(std::move(Spec));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI1A"));
  LSDA.TypeTable.push_back(makeType(2, 0x500008, "_ZTI1B"));

  HighFunc Func = structure(std::move(LSDA));
  ASSERT_FALSE(Func.Body.empty());
  const HighStmt &Try = Func.Body.front();
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  EXPECT_EQ(Try.EHClauses.front().SpecTypeNames,
            std::vector<std::string>({"_ZTI1A", "_ZTI1B"}));
}

TEST(ItaniumEHStructurer, LeavesACleanupOnlyFrameUnstructured) {
  // Destructors running on the way out are not a `try`.  A frame whose only
  // actions are cleanups has no region to recover, and reporting one would
  // invent a handler the source never wrote.
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.CallSites.push_back(makeCallSite(1, 1, kPad, std::nullopt));
  LSDA.Actions.push_back(makeAction(0, 0));
  ASSERT_TRUE(LSDA.isCleanupOnly());

  HighFunc Func = structure(std::move(LSDA));
  EXPECT_EQ(Func.StructuredExceptionRegions, 0u);
  EXPECT_EQ(Func.UnstructuredExceptionRegions, 0u);
  for (const HighStmt &Stmt : Func.Body)
    EXPECT_NE(Stmt.Kind, StmtKind::ItaniumTry);
}

TEST(ItaniumEHStructurer, RefusesTheSJLJCallSiteForm) {
  // The SJLJ form's "ranges" are call-site indices the compiler handed out,
  // not addresses, so laying a HighIR interval over them would structure the
  // body against numbers that name no code.
  ItaniumEHInfo LSDA;
  LSDA.IsCallSiteAddressForm = false;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.CallSites.push_back(makeCallSite(1, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, 1));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI1A"));

  HighFunc Func = structure(std::move(LSDA));
  EXPECT_EQ(Func.StructuredExceptionRegions, 0u);
  EXPECT_EQ(Func.UnstructuredExceptionRegions, 2u);
  for (const HighStmt &Stmt : Func.Body)
    EXPECT_NE(Stmt.Kind, StmtKind::ItaniumTry);
}

TEST(ItaniumEHStructurer, RefusesARegionThatEscapesTheFunction) {
  ItaniumEHInfo LSDA;
  ItaniumCallSite Site = makeCallSite(0, 1, kPad, 0);
  Site.GuardedRange.End = kEntry + (kSlotCount + 1) * kSlotSize;
  LSDA.CallSites.push_back(Site);
  LSDA.Actions.push_back(makeAction(0, 1));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI1A"));

  HighFunc Func = structure(std::move(LSDA));
  EXPECT_EQ(Func.StructuredExceptionRegions, 0u);
  EXPECT_EQ(Func.UnstructuredExceptionRegions, 1u);
}

TEST(ItaniumEHStructurer, MarksACatchWhoseTypeSlotIsMissingAsPartial) {
  // A filter naming a slot the type table does not have is a table this
  // decoder could not fully read.  The clause still describes a catch, so it
  // is kept, but nothing may present it as a resolved type.
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, 7));

  HighFunc Func = structure(std::move(LSDA));
  ASSERT_FALSE(Func.Body.empty());
  const HighStmt &Try = Func.Body.front();
  ASSERT_EQ(Try.Kind, StmtKind::ItaniumTry);
  ASSERT_EQ(Try.EHClauses.size(), 1u);
  EXPECT_EQ(Try.EHClauses.front().ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_EQ(Try.EHClauses.front().TypeDescriptorVA, 0u);
}

TEST(ItaniumEHStructurer, RejectsEveryCallSiteOfAnIncompleteRecord) {
  MedFunc Func = makeFunction();
  ExceptionFunction EH = makeRecord();
  EH.ParseStatus = ExceptionParseStatus::Partial;
  ItaniumEHInfo LSDA;
  LSDA.CallSites.push_back(makeCallSite(0, 1, kPad, 0));
  LSDA.Actions.push_back(makeAction(0, 1));
  LSDA.TypeTable.push_back(makeType(1, 0x500000, "_ZTI1A"));
  EH.Itanium = std::move(LSDA);
  Func.ExceptionMetadata = std::move(EH);

  HighFunc High = MedToHighConverter().convert(Func, Arch::X64);
  EXPECT_EQ(High.StructuredExceptionRegions, 0u);
  EXPECT_EQ(High.UnstructuredExceptionRegions, 1u);
}

} // namespace
