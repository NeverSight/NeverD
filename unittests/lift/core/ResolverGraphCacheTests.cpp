//===- ResolverGraphCacheTests.cpp - Frozen proof graph reuse -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

using namespace neverd;

namespace neverd::detail {

// Exercise the production graph/value query, including its resource account.
// The access shim changes graph facts directly so equal-size mutations cannot
// accidentally pass a test that only adds/removes instructions.
struct ResolverGraphCacheTestAccess {
  using Query = CFGBuilder::JumpTableValueQuery;
  using Relation = CFGBuilder::JumpTableValueRelation;
  using Occurrence = CFGBuilder::JumpTableValueOccurrence;

  struct Answer {
    std::vector<bool> Values;
    std::vector<bool> QueryComplete;
    std::vector<uint64_t> Masks;
    bool Complete = false;
    size_t Remaining = 0;
    bool operator==(const Answer &) const = default;
  };

  static void seed(CFGBuilder &B, const BinaryImage &Image,
                   unsigned Length = 3) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    for (unsigned I = 0; I <= Length; ++I) {
      const va_t Address = 0x100 + I;
      CFGBuilder::InsnRecord R{};
      R.Addr = Address;
      R.Size = 1;
      LowOp Op;
      Op.Addr = Address;
      if (I == Length) {
        R.IsRet = true;
        Op.Opcode = NdOp::RETURN;
        Op.addInput(NdVar::tmp((I - 1) * 8, 8));
      } else {
        Op.Opcode = NdOp::COPY;
        Op.Output = NdVar::tmp(I * 8, 8);
        Op.addInput(I ? NdVar::tmp((I - 1) * 8, 8) : NdVar::cst(7, 8));
      }
      R.Ops.push_back(Op);
      B.Insns.emplace(Address, std::move(R));
      B.BlockStarts.insert(Address);
    }
  }

  static Query seedPredecessorLane(CFGBuilder &B, const BinaryImage &Image,
                                   unsigned Length) {
    B.CurrentImg = &Image;
    B.CurrentFuncEntry = 0x100;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    const auto Lane = NdVar::reg(0, 8);
    for (unsigned I = 0; I <= Length; ++I) {
      const va_t Address = 0x100 + I;
      CFGBuilder::InsnRecord R{};
      R.Addr = Address;
      R.Size = 1;
      LowOp Op;
      Op.Addr = Address;
      if (I == 0) {
        Op.Opcode = NdOp::COPY;
        Op.Output = Lane;
        Op.addInput(NdVar::cst(7, 8));
      } else if (I == Length) {
        R.IsRet = true;
        Op.Opcode = NdOp::RETURN;
        Op.addInput(Lane);
      } else {
        R.IsBranch = true;
        R.BranchTarget = Address + 1;
        Op.Opcode = NdOp::BRANCH;
        Op.addInput(NdVar::cst(Address + 1, 8));
      }
      R.Ops.push_back(Op);
      B.Insns.emplace(Address, std::move(R));
      B.BlockStarts.insert(Address);
    }
    Query Q;
    Q.Candidate = Lane;
    Q.UseAddr = 0x100 + Length;
    Q.UseSeq = 0;
    Q.Alternatives = {{NdVar::cst(7, 8), InvalidVA, -1, false}};
    return Q;
  }

  static Query seedCountLane(CFGBuilder &B, const BinaryImage &Image,
                             NdOp Opcode, uint16_t InputSize,
                             uint16_t OutputSize, unsigned SliceOffset = 0,
                             unsigned CountInputs = 1) {
    seed(B, Image, 1);
    auto &Ops = B.Insns.at(0x100).Ops;
    Ops.clear();
    LowOp Count;
    Count.Addr = 0x100;
    Count.Seq = 0;
    Count.Opcode = Opcode;
    Count.Output = NdVar::tmp(0, OutputSize);
    for (unsigned I = 0; I < CountInputs; ++I)
      Count.addInput(NdVar::reg(I * 32, InputSize));
    Ops.push_back(Count);
    LowOp Slice;
    Slice.Addr = 0x100;
    Slice.Seq = 1;
    Slice.Opcode = NdOp::SUBBYTES;
    Slice.Output = NdVar::tmp(32, 1);
    Slice.addInput(Count.Output);
    Slice.addInput(NdVar::cst(SliceOffset, 4));
    Ops.push_back(Slice);
    B.Insns.at(0x101).Ops.front().Inputs[0] = Count.Output;

    Query Q;
    Q.Candidate = Count.Output;
    Q.UseAddr = 0x101;
    Q.UseSeq = 0;
    Q.Alternatives = {{Slice.Output, 0x100, Slice.Seq, true}};
    Q.AllowZeroExtension = true;
    return Q;
  }

  static Answer query(CFGBuilder &B, size_t Budget = 1000000,
                      unsigned Length = 3, uint32_t Depth = 0,
                      const std::vector<va_t> *Targets = nullptr) {
    CFGBuilder::JumpTableValueQuery Q;
    Q.Candidate = NdVar::tmp((Length - 1) * 8, 8);
    Q.UseAddr = 0x100 + Length;
    Q.UseSeq = 0;
    Q.Alternatives.push_back({NdVar::cst(7, 8), Q.UseAddr, 0, false});
    Answer A;
    A.Remaining = Budget;
    A.Values = B.tableValuesMatchAtUses({Q}, &A.Complete, &A.QueryComplete,
                                        0x100, Targets, &A.Remaining, 0,
                                        nullptr, nullptr, Depth);
    return A;
  }

  static std::shared_ptr<const void> token(const CFGBuilder &B) {
    return B.CachedResolverGraph;
  }

  static void clear(CFGBuilder &B) { B.CachedResolverGraph.reset(); }

  static std::array<size_t, 3> stats(const CFGBuilder &B) {
    return B.resolverValueQueryCacheStatsForTesting();
  }

  static Query valueQuery(unsigned Length = 3) {
    Query Q;
    Q.Candidate = NdVar::tmp((Length - 1) * 8, 8);
    Q.UseAddr = 0x100 + Length;
    Q.UseSeq = 0;
    Q.Alternatives.push_back({NdVar::cst(7, 8), Q.UseAddr, 0, false});
    return Q;
  }

  static Answer batch(CFGBuilder &B, const std::vector<Query> &Queries,
                      size_t Budget = 1000000, uint32_t Depth = 0,
                      size_t MatchLimit = 0, bool WantComplete = true,
                      bool WantQueryComplete = true, bool WantMasks = false) {
    Answer A;
    A.Remaining = Budget;
    A.Values = B.tableValuesMatchAtUses(
        Queries, WantComplete ? &A.Complete : nullptr,
        WantQueryComplete ? &A.QueryComplete : nullptr, InvalidVA, nullptr,
        &A.Remaining, MatchLimit, nullptr, WantMasks ? &A.Masks : nullptr,
        Depth);
    return A;
  }

  static void seedOccurrences(CFGBuilder &B) {
    RelocatedInstructionAddressOccurrence Address;
    Address.InstructionAddr = 0x100;
    Address.OpSeq = 0;
    Address.ArithmeticProof.resize(1);
    B.RelocatedInstructionAddressOccurrences = {Address};
    RelocatedInstructionScalarModelOccurrence Scalar;
    Scalar.InstructionAddr = 0x100;
    Scalar.OpSeq = 0;
    B.RelocatedInstructionScalarModelOccurrences = {Scalar};
  }

  static void changeAddressOccurrence(CFGBuilder &B) {
    B.RelocatedInstructionAddressOccurrences.front().TargetOwnerVA = 0x200;
  }

  static void changeNestedAddressOccurrence(CFGBuilder &B) {
    B.RelocatedInstructionAddressOccurrences.front()
        .ArithmeticProof.front()
        .OpSeq = 1;
  }

  static void changeScalarOccurrence(CFGBuilder &B) {
    B.RelocatedInstructionScalarModelOccurrences.front().SeedOpSeq = 1;
  }

  static void changeEntry(CFGBuilder &B) { B.CurrentFuncEntry = 0x101; }

  static void changeConsumerAudit(CFGBuilder &B) {
    B.ActiveJumpTableConsumerAudit = true;
  }

  static void changeSymbolBudget(CFGBuilder &B) {
    B.FiniteSetSymbolEvidenceBudgetForTesting = 0;
  }

  static void changeImage(CFGBuilder &B, const BinaryImage &Image) {
    B.CurrentImg = &Image;
  }

  static void changeValue(CFGBuilder &B) {
    B.Insns.at(0x100).Ops.front().Inputs[0] = NdVar::cst(8, 8);
  }

  static void addBackedge(CFGBuilder &B) {
    auto &R = B.Insns.at(0x103);
    R.IsRet = false;
    R.IsBranch = true;
    R.BranchTarget = 0x100;
    R.Ops.front().Opcode = NdOp::BRANCH;
    R.Ops.front().Inputs[0] = NdVar::cst(0x100, 8);
  }

  static void changeRoot(CFGBuilder &B) {
    B.ActiveJumpTableProofRoots = std::set<va_t>{0x101};
  }

  static void changeConditionalRoot(CFGBuilder &B) {
    B.DiscoveredCodeRefSources[0x102] = {0x100};
  }

  static void addOwner(CFGBuilder &B) {
    B.Insns.at(0x100).JumpTableTargets = {0x101};
    auto &Info = B.ResolvedTableInfo[0x100];
    Info.setBaseAddr(0x102);
    Info.EntrySize = 1;
    Info.MaxEntries = 1;
    B.DiscoveredCodeRefSources[0x102] = {0x100};
  }

  static void changeOwnerRange(CFGBuilder &B) {
    B.ResolvedTableInfo.at(0x100).BaseAddr = 0x103;
  }

  static void changeOwnerTargets(CFGBuilder &B) {
    B.Insns.at(0x100).JumpTableTargets.clear();
  }

  static void explicitOwnerRange(CFGBuilder &B) {
    B.ResolvedTableInfo.at(0x100).StorageRanges = {{0x102, 1, 1, 1}};
  }

  static void rollbackValue(CFGBuilder &B) {
    B.Insns.at(0x100).Ops.front().Inputs[0] = NdVar::cst(7, 8);
  }
};

} // namespace neverd::detail

namespace {
using Access = detail::ResolverGraphCacheTestAccess;

BinaryImage image() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  return Image;
}

void expectColdAnswer(CFGBuilder &B, const Access::Answer &Warm) {
  Access::clear(B);
  EXPECT_EQ(Access::query(B), Warm);
}

TEST(ResolverGraphCache, ReusesFrozenGraphAndPaysEveryColdBudgetBoundary) {
  const auto Image = image();
  CFGBuilder Warm;
  Access::seed(Warm, Image);
  const auto First = Access::query(Warm);
  ASSERT_EQ(First.Values, (std::vector<bool>{true}));
  ASSERT_TRUE(First.Complete);
  const auto Token = Access::token(Warm);
  ASSERT_TRUE(Token);
  EXPECT_EQ(Access::query(Warm), First);
  EXPECT_EQ(Access::token(Warm), Token);

  const size_t Work = 1000000 - First.Remaining;
  ASSERT_LT(Work, 50000u);
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image);
    EXPECT_EQ(Access::query(Warm, Budget), Access::query(Cold, Budget));
  }
}

TEST(ResolverGraphCache, EqualCountsDoNotHideBackedgesRootsOrChangedOps) {
  const auto Image = image();
  for (auto Mutate : {Access::changeValue, Access::addBackedge,
                      Access::changeRoot, Access::changeConditionalRoot}) {
    CFGBuilder B;
    Access::seed(B, Image);
    ASSERT_TRUE(Access::query(B).Complete);
    const auto Before = Access::token(B);
    Mutate(B);
    const auto After = Access::query(B);
    EXPECT_NE(Access::token(B), Before);
    expectColdAnswer(B, After);
  }
}

TEST(ResolverGraphCache, StorageOwnershipAndCallbackChargesRemainCurrent) {
  const auto Image = image();
  for (auto Mutate : {Access::changeOwnerRange, Access::changeOwnerTargets,
                      Access::explicitOwnerRange}) {
    CFGBuilder B;
    Access::seed(B, Image);
    Access::addOwner(B);
    const auto First = Access::query(B);
    ASSERT_TRUE(First.Complete);
    const auto Before = Access::token(B);
    ASSERT_TRUE(Before);
    Mutate(B);
    const auto After = Access::query(B);
    EXPECT_NE(Access::token(B), Before);
    expectColdAnswer(B, After);
  }
}

TEST(ResolverGraphCache, OverridePayloadAndRolledBackPayloadAreOwned) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  std::vector<va_t> Targets{0x101};
  const auto First = Access::query(B, 1000000, 3, 0, &Targets);
  const auto FirstToken = Access::token(B);
  Targets.front() = 0x102;
  const auto Changed = Access::query(B, 1000000, 3, 0, &Targets);
  EXPECT_NE(Access::token(B), FirstToken);
  Access::clear(B);
  EXPECT_EQ(Access::query(B, 1000000, 3, 0, &Targets), Changed);
  Targets.front() = 0x101;
  EXPECT_EQ(Access::query(B, 1000000, 3, 0, &Targets), First);

  Access::clear(B);
  const auto BeforeMutation = Access::query(B);
  const auto Frozen = Access::token(B);
  Access::changeValue(B);
  EXPECT_NE(Access::query(B).Values, BeforeMutation.Values);
  EXPECT_NE(Access::token(B), Frozen);
  Access::rollbackValue(B);
  EXPECT_EQ(Access::query(B), BeforeMutation);
}

TEST(ResolverGraphCache, ResourceFailureAndAnalysisIncompleteAreDistinct) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  const auto Exhausted = Access::query(B, 1, 16);
  EXPECT_FALSE(Exhausted.Complete);
  EXPECT_EQ(Exhausted.Remaining, 0u);
  EXPECT_FALSE(Access::token(B));

  const auto Incomplete = Access::query(B, 1000000, 16, 1);
  EXPECT_FALSE(Incomplete.Complete);
  EXPECT_GT(Incomplete.Remaining, 0u);
  const auto Graph = Access::token(B);
  ASSERT_TRUE(Graph);
  EXPECT_EQ(Access::query(B, 1000000, 16, 1), Incomplete);
  const auto Complete = Access::query(B, 1000000, 16);
  EXPECT_TRUE(Complete.Complete);
  EXPECT_EQ(Complete.Values, (std::vector<bool>{true}));
  EXPECT_EQ(Access::token(B), Graph);
}

TEST(ResolverValueQueryCache, ExpandedValueDepthKeepsTheDefaultGuardCap) {
  const auto Image = image();
  for (unsigned Length : {80u, 160u}) {
    SCOPED_TRACE(Length);
    CFGBuilder Builder;
    const auto Query = Access::seedPredecessorLane(Builder, Image, Length);
    const auto Default = Access::batch(Builder, {Query});
    EXPECT_FALSE(Default.Complete);
    EXPECT_EQ(Default.QueryComplete, (std::vector<bool>{false}));
    EXPECT_EQ(Default.Values, (std::vector<bool>{false}));
    EXPECT_GT(Default.Remaining, 0u); // Depth, not aggregate work exhaustion.
    const auto Expanded = Access::batch(
        Builder, {Query}, 1000000, limits::kMaxJumpTableExpandedResolverDepth);
    EXPECT_EQ(Expanded.Complete, Length == 80);
    EXPECT_EQ(Expanded.QueryComplete, (std::vector<bool>{Length == 80}));
    EXPECT_EQ(Expanded.Values, (std::vector<bool>{Length == 80}));
    EXPECT_GT(Expanded.Remaining, 0u);
    if (Length == 80) {
      const auto Hits = Access::stats(Builder)[0];
      EXPECT_EQ(Access::batch(Builder, {Query}, 1000000,
                              limits::kMaxJumpTableExpandedResolverDepth),
                Expanded);
      EXPECT_EQ(Access::stats(Builder)[0], Hits + 1);
    }
    // A successful expanded query cannot turn a later default-depth request
    // into evidence, including when both requests use the same graph cache.
    EXPECT_EQ(Access::batch(Builder, {Query}), Default);
  }
}

TEST(ResolverCountLane, ScalarCountsEqualTheirZeroExtendedLowByte) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64}) {
    auto Image = image();
    Image.Arch = Architecture;
    for (auto Opcode : {NdOp::POPCOUNT, NdOp::LZCOUNT}) {
      for (uint16_t InputSize : {1, 2, 4, 8}) {
        for (uint16_t OutputSize : {2, 4, 8}) {
          SCOPED_TRACE(static_cast<unsigned>(Architecture));
          SCOPED_TRACE(static_cast<unsigned>(Opcode));
          SCOPED_TRACE(InputSize);
          SCOPED_TRACE(OutputSize);
          CFGBuilder Builder;
          const auto Query = Access::seedCountLane(Builder, Image, Opcode,
                                                   InputSize, OutputSize);
          const auto Result = Access::batch(Builder, {Query});
          EXPECT_TRUE(Result.Complete);
          EXPECT_EQ(Result.QueryComplete, (std::vector<bool>{true}));
          EXPECT_EQ(Result.Values, (std::vector<bool>{true}));
        }
      }
    }
  }
}

TEST(ResolverCountLane, OtherWidthsLanesAndOperationsGrantNoProof) {
  const auto Image = image();
  for (auto Opcode : {NdOp::POPCOUNT, NdOp::LZCOUNT, NdOp::INT_NOT}) {
    for (const auto Shape : {std::array<unsigned, 4>{0, 4, 0, 1},
                             {16, 4, 0, 1},
                             {4, 16, 0, 1},
                             {4, 4, 1, 1},
                             {4, 4, 0, 0},
                             {4, 4, 0, 2}}) {
      SCOPED_TRACE(static_cast<unsigned>(Opcode));
      SCOPED_TRACE(::testing::PrintToString(Shape));
      CFGBuilder Builder;
      const auto Query = Access::seedCountLane(Builder, Image, Opcode, Shape[0],
                                               Shape[1], Shape[2], Shape[3]);
      EXPECT_EQ(Access::batch(Builder, {Query}).Values,
                (std::vector<bool>{false}));
    }
  }
  CFGBuilder Other;
  const auto Query = Access::seedCountLane(Other, Image, NdOp::INT_NOT, 4, 4);
  EXPECT_EQ(Access::batch(Other, {Query}).Values, (std::vector<bool>{false}));
}

TEST(ResolverCountLane, ExtensionsAndCachedProofsKeepTheirWorkContract) {
  const auto Image = image();
  CFGBuilder Warm;
  auto Query = Access::seedCountLane(Warm, Image, NdOp::POPCOUNT, 8, 8);
  const auto First = Access::batch(Warm, {Query});
  ASSERT_TRUE(First.Complete);
  ASSERT_EQ(First.Values, (std::vector<bool>{true}));
  const auto Hits = Access::stats(Warm)[0];
  EXPECT_EQ(Access::batch(Warm, {Query}), First);
  EXPECT_GT(Access::stats(Warm)[0], Hits);
  const size_t Work = 1000000 - First.Remaining;
  ASSERT_GT(Work, 1u);
  for (size_t Budget :
       {size_t{0}, size_t{1}, Work / 2, Work - 1, Work, Work + 1}) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seedCountLane(Cold, Image, NdOp::POPCOUNT, 8, 8);
    EXPECT_EQ(Access::batch(Warm, {Query}, Budget),
              Access::batch(Cold, {Query}, Budget));
  }
  EXPECT_FALSE(Access::batch(Warm, {Query}, 0).Complete);
  EXPECT_TRUE(Access::batch(Warm, {Query}, 0).Values.empty());
  Query.AllowZeroExtension = false;
  const auto NoExtension = Access::batch(Warm, {Query});
  EXPECT_TRUE(NoExtension.Complete);
  EXPECT_EQ(NoExtension.Values, (std::vector<bool>{false}));
}

TEST(ResolverValueQueryCache, ReplaysOrderedResultsAndEveryBudgetBoundary) {
  const auto Image = image();
  CFGBuilder Warm;
  Access::seed(Warm, Image);
  auto True = Access::valueQuery();
  auto False = True;
  False.Alternatives.front().Value = NdVar::cst(8, 8);
  auto Missing = True;
  Missing.UseAddr = 0x9999;
  auto Feasible = True;
  Feasible.Relation = Access::Relation::UnsignedFeasibleSet;
  Feasible.UnsignedUpperBound = 8;
  Feasible.Alternatives.clear();
  const std::vector<Access::Query> Queries{True, False, Missing, Feasible};
  auto Run = [&](CFGBuilder &B, size_t Budget) {
    return Access::batch(B, Queries, Budget, 0, 0, true, true, true);
  };
  const auto First = Run(Warm, 1000000);
  ASSERT_TRUE(First.Complete);
  ASSERT_EQ(First.Values, (std::vector<bool>{true, false, false, true}));
  ASSERT_EQ(First.QueryComplete, (std::vector<bool>{true, true, true, true}));
  ASSERT_EQ(First.Masks, (std::vector<uint64_t>{0, 0, 0, 128}));
  ASSERT_GT(Access::stats(Warm)[1], 0u);
  const size_t Hits = Access::stats(Warm)[0];
  EXPECT_EQ(Run(Warm, 1000000), First);
  ASSERT_EQ(Access::stats(Warm)[0], Hits + 1);

  const size_t Work = 1000000 - First.Remaining;
  ASSERT_LT(Work, 50000u);
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image);
    const size_t BeforeHits = Access::stats(Warm)[0];
    EXPECT_EQ(Run(Warm, Budget), Run(Cold, Budget));
    if (Budget < Work)
      EXPECT_EQ(Access::stats(Warm)[0], BeforeHits);
  }

  auto Reordered = Queries;
  std::swap(Reordered[0], Reordered[1]);
  const auto Changed =
      Access::batch(Warm, Reordered, 1000000, 0, 0, true, true, true);
  EXPECT_EQ(Changed.Values, (std::vector<bool>{false, true, false, true}));
  Access::clear(Warm);
  EXPECT_EQ(Access::batch(Warm, Reordered, 1000000, 0, 0, true, true, true),
            Changed);
}

TEST(ResolverValueQueryCache, EqualSizedQueryMutationsCannotReuseOldProofs) {
  using Query = Access::Query;
  using Mutation = std::pair<const char *, std::function<void(Query &)>>;
  const std::vector<Mutation> Mutations{
      {"candidate space",
       [](Query &Q) { Q.Candidate.Space = VnodeSpace::REG; }},
      {"candidate offset", [](Query &Q) { Q.Candidate.Offset = 8; }},
      {"candidate width", [](Query &Q) { Q.Candidate.Size = 4; }},
      {"candidate provenance",
       [](Query &Q) {
         Q.Candidate.Provenance = ConstantAddressProvenance::Scalar;
       }},
      {"candidate owner", [](Query &Q) { Q.Candidate.AddressOwnerVA = 0x200; }},
      {"use address", [](Query &Q) { Q.UseAddr = 0x102; }},
      {"use sequence", [](Query &Q) { Q.UseSeq = 1; }},
      {"alternative space",
       [](Query &Q) { Q.Alternatives.front().Value.Space = VnodeSpace::TEMP; }},
      {"alternative value",
       [](Query &Q) { Q.Alternatives.front().Value.Offset = 9; }},
      {"alternative width",
       [](Query &Q) { Q.Alternatives.front().Value.Size = 4; }},
      {"alternative provenance",
       [](Query &Q) {
         Q.Alternatives.front().Value.Provenance =
             ConstantAddressProvenance::DataAddress;
       }},
      {"alternative owner",
       [](Query &Q) { Q.Alternatives.front().Value.AddressOwnerVA = 0x200; }},
      {"alternative address",
       [](Query &Q) { Q.Alternatives.front().Addr = 0x101; }},
      {"alternative sequence",
       [](Query &Q) { Q.Alternatives.front().Seq = 1; }},
      {"alternative definition",
       [](Query &Q) { Q.Alternatives.front().DefinedAtPoint = true; }},
      {"alternative order",
       [](Query &Q) { std::swap(Q.Alternatives[0], Q.Alternatives[1]); }},
      {"zero extension", [](Query &Q) { Q.AllowZeroExtension = true; }},
      {"sign extension", [](Query &Q) { Q.AllowSignExtension = true; }},
      {"occurrence roots",
       [](Query &Q) { Q.UseDefinedAlternativesAsOccurrenceRoots = true; }},
      {"private frame calls",
       [](Query &Q) { Q.AllowPrivateFrameMemoryAcrossCalls = true; }},
      {"relation", [](Query &Q) { Q.Relation = Access::Relation::MayDepend; }},
      {"unsigned bound", [](Query &Q) { Q.UnsignedUpperBound = 8; }},
      {"exact owner", [](Query &Q) { Q.RequireExactAddressOwner = true; }},
      {"frame addend", [](Query &Q) { Q.FrameByteAddend = 1; }},
      {"alternative frame addend",
       [](Query &Q) { Q.AlternativeFrameByteAddend = 1; }},
      {"frame address", [](Query &Q) { Q.FrameAddressUseAddr = 0x101; }},
      {"frame sequence", [](Query &Q) { Q.FrameAddressUseSeq = 1; }},
      {"frame width", [](Query &Q) { Q.FrameMemorySize = 4; }},
      {"frame value offsets",
       [](Query &Q) { Q.AlternativeFrameValueOffsets.front() = 1; }},
      {"authenticated store",
       [](Query &Q) { Q.AuthenticatedFrameStoreWriters.front().Seq = 1; }},
      {"authenticated memcpy",
       [](Query &Q) { Q.AuthenticatedFrameMemcpyWriters.front().Seq = 1; }},
      {"scalar folding", [](Query &Q) { Q.FoldScalarConstantOps = true; }},
  };
  const auto Image = image();
  for (const auto &[Name, Mutate] : Mutations) {
    SCOPED_TRACE(Name);
    CFGBuilder B;
    Access::seed(B, Image);
    auto Q = Access::valueQuery();
    auto Other = Q.Alternatives.front();
    Other.Value = NdVar::cst(8, 8);
    Q.Alternatives.push_back(Other);
    Q.AlternativeFrameValueOffsets = {0, 0};
    Q.AuthenticatedFrameStoreWriters = {Q.Alternatives.front()};
    Q.AuthenticatedFrameMemcpyWriters = {Q.Alternatives.front()};
    ASSERT_TRUE(Access::batch(B, {Q}).Complete);
    ASSERT_GT(Access::stats(B)[1], 0u);
    const auto Graph = Access::token(B);
    const size_t Hits = Access::stats(B)[0];
    Mutate(Q);
    const auto Changed = Access::batch(B, {Q});
    EXPECT_EQ(Access::token(B), Graph);
    EXPECT_EQ(Access::stats(B)[0], Hits);
    Access::clear(B);
    EXPECT_EQ(Access::batch(B, {Q}), Changed);
  }
}

TEST(ResolverValueQueryCache,
     ModesLimitsAndOptionalOutputsFollowColdExecution) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  auto Q = Access::valueQuery(16);
  Q.Relation = Access::Relation::UnsignedFeasibleSet;
  Q.UnsignedUpperBound = 8;
  Q.Alternatives.clear();
  for (bool WantComplete : {false, true})
    for (bool WantQueryComplete : {false, true})
      for (bool WantMasks : {false, true}) {
        SCOPED_TRACE(WantComplete);
        SCOPED_TRACE(WantQueryComplete);
        SCOPED_TRACE(WantMasks);
        const auto Warm = Access::batch(B, {Q}, 1000000, 0, 0, WantComplete,
                                        WantQueryComplete, WantMasks);
        CFGBuilder Cold;
        Access::seed(Cold, Image, 16);
        EXPECT_EQ(Warm, Access::batch(Cold, {Q}, 1000000, 0, 0, WantComplete,
                                      WantQueryComplete, WantMasks));
        EXPECT_EQ(Warm, Access::batch(B, {Q}, 1000000, 0, 0, WantComplete,
                                      WantQueryComplete, WantMasks));
        if (WantMasks)
          EXPECT_EQ(Warm.Masks, (std::vector<uint64_t>{128}));
      }
  for (uint32_t Depth : {1u, 2u, 16u, 64u}) {
    SCOPED_TRACE(Depth);
    const auto Warm = Access::batch(B, {Q}, 1000000, Depth);
    CFGBuilder Cold;
    Access::seed(Cold, Image, 16);
    EXPECT_EQ(Warm, Access::batch(Cold, {Q}, 1000000, Depth));
  }
  Q = Access::valueQuery(16);
  for (size_t Limit : {1u, 2u, 4u, 16u, 64u, 128u}) {
    SCOPED_TRACE(Limit);
    const auto Warm = Access::batch(B, {Q}, 1000000, 0, Limit);
    CFGBuilder Cold;
    Access::seed(Cold, Image, 16);
    EXPECT_EQ(Warm, Access::batch(Cold, {Q}, 1000000, 0, Limit));
  }
}

TEST(ResolverValueQueryCache, GraphIdentityDoesNotReplaceProofContextIdentity) {
  const auto Image = image();
  for (auto Mutate :
       {Access::changeAddressOccurrence, Access::changeNestedAddressOccurrence,
        Access::changeScalarOccurrence, Access::changeEntry,
        Access::changeConsumerAudit, Access::changeSymbolBudget}) {
    CFGBuilder B;
    Access::seed(B, Image);
    Access::seedOccurrences(B);
    const auto Q = Access::valueQuery();
    const auto First = Access::batch(B, {Q});
    ASSERT_TRUE(First.Complete);
    EXPECT_EQ(Access::batch(B, {Q}), First);
    ASSERT_GT(Access::stats(B)[0], 0u);
    const auto Graph = Access::token(B);
    const size_t Hits = Access::stats(B)[0];
    Mutate(B);
    const auto Changed = Access::batch(B, {Q});
    EXPECT_EQ(Access::token(B), Graph);
    EXPECT_EQ(Access::stats(B)[0], Hits);
    Access::clear(B);
    EXPECT_EQ(Access::batch(B, {Q}), Changed);
  }

  CFGBuilder B;
  Access::seed(B, Image);
  const auto Q = Access::valueQuery();
  ASSERT_TRUE(Access::batch(B, {Q}).Complete);
  const auto Graph = Access::token(B);
  const auto OtherImage = image();
  Access::changeImage(B, OtherImage);
  const size_t Hits = Access::stats(B)[0];
  const auto Changed = Access::batch(B, {Q});
  EXPECT_EQ(Access::token(B), Graph);
  EXPECT_EQ(Access::stats(B)[0], Hits);
  Access::clear(B);
  EXPECT_EQ(Access::batch(B, {Q}), Changed);
}

TEST(ResolverValueQueryCache, DepthRefusalRemainsIncompleteAndRetriesNewLimit) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  const auto Q = Access::valueQuery(16);
  const auto Incomplete = Access::batch(B, {Q}, 1000000, 1);
  ASSERT_FALSE(Incomplete.Complete);
  ASSERT_GT(Incomplete.Remaining, 0u);
  ASSERT_TRUE(Access::token(B));
  EXPECT_EQ(Access::stats(B)[0], 0u);
  EXPECT_EQ(Access::stats(B)[1], 1u);
  EXPECT_EQ(Access::batch(B, {Q}, 1000000, 1), Incomplete);
  EXPECT_EQ(Access::stats(B)[0], 1u);
  EXPECT_EQ(Access::stats(B)[1], 1u);
  const auto Complete = Access::batch(B, {Q});
  ASSERT_TRUE(Complete.Complete);
  ASSERT_GT(Access::stats(B)[1], 0u);
  EXPECT_EQ(Access::batch(B, {Q}, 1000000, 1), Incomplete);
  EXPECT_EQ(Access::stats(B)[0], 2u);
  EXPECT_EQ(Access::batch(B, {Q}), Complete);
  EXPECT_EQ(Access::stats(B)[0], 3u);
}

TEST(ResolverValueQueryCache, DepthRefusalPreservesIndependentQueryOutputs) {
  const auto Image = image();
  const auto Deep = Access::valueQuery(16);
  auto Direct = Deep;
  Direct.Candidate = NdVar::cst(7, 8);
  auto Different = Direct;
  Different.Alternatives.front().Value = NdVar::cst(8, 8);
  auto Range = Direct;
  Range.Relation = Access::Relation::UnsignedFeasibleSet;
  Range.UnsignedUpperBound = 16;
  Range.Alternatives.clear();
  for (bool WantComplete : {false, true}) {
    for (bool WantQueryComplete : {false, true}) {
      for (bool WantMasks : {false, true}) {
        SCOPED_TRACE(std::to_string(WantComplete) + ":" +
                     std::to_string(WantQueryComplete) + ":" +
                     std::to_string(WantMasks));
        CFGBuilder B;
        Access::seed(B, Image, 16);
        auto Run = [&](const std::vector<Access::Query> &Queries) {
          return Access::batch(B, Queries, 1000000, 1, 0, WantComplete,
                               WantQueryComplete, WantMasks);
        };
        const std::vector<Access::Query> Queries{Range, Deep, Direct,
                                                 Different};
        const auto First = Run(Queries);
        EXPECT_FALSE(First.Complete);
        EXPECT_EQ(First.Values, (std::vector<bool>{true, false, true, false}));
        if (WantQueryComplete)
          EXPECT_EQ(First.QueryComplete,
                    (std::vector<bool>{true, false, true, true}));
        if (WantMasks)
          EXPECT_EQ(First.Masks, (std::vector<uint64_t>{128, 0, 0, 0}));
        EXPECT_EQ(Run(Queries), First);
        EXPECT_EQ(Access::stats(B)[0], 1u);
        const std::vector<Access::Query> Reordered{Deep, Range, Different,
                                                   Direct};
        const auto Changed = Run(Reordered);
        EXPECT_EQ(Access::stats(B)[0], 1u);
        Access::clear(B);
        EXPECT_EQ(Run(Reordered), Changed);
      }
    }
  }
}

TEST(ResolverValueQueryCache, DepthRefusalPaysColdWorkAndTracksContext) {
  const auto Image = image();
  const auto Deep = Access::valueQuery(16);
  auto Direct = Deep;
  Direct.Candidate = NdVar::cst(7, 8);
  CFGBuilder Warm;
  Access::seed(Warm, Image, 16);
  auto Run = [&](CFGBuilder &B, size_t Budget) {
    return Access::batch(B, {Deep, Direct}, Budget, 1, 0, true, true, true);
  };
  const auto First = Run(Warm, 1000000);
  ASSERT_FALSE(First.Complete);
  ASSERT_EQ(First.QueryComplete, (std::vector<bool>{false, true}));
  ASSERT_EQ(Access::stats(Warm)[1], 1u);
  const size_t Work = 1000000 - First.Remaining;
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image, 16);
    EXPECT_EQ(Run(Warm, Budget), Run(Cold, Budget));
  }
  for (auto Mutate :
       {Access::changeValue, Access::changeEntry,
        Access::changeAddressOccurrence, Access::changeNestedAddressOccurrence,
        Access::changeScalarOccurrence}) {
    CFGBuilder B;
    Access::seed(B, Image, 16);
    Access::seedOccurrences(B);
    const auto Original = Run(B, 1000000);
    ASSERT_EQ(Run(B, 1000000), Original);
    const auto Graph = Access::token(B);
    const auto Hits = Access::stats(B)[0];
    Mutate(B);
    const auto Changed = Run(B, 1000000);
    EXPECT_EQ(Access::stats(B)[0], Access::token(B) == Graph ? Hits : 0u);
    Access::clear(B);
    EXPECT_EQ(Run(B, 1000000), Changed);
  }
}

TEST(ResolverValueQueryCache, SharedMatchFailureClearsEveryOutput) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  auto Range = Access::valueQuery();
  Range.Relation = Access::Relation::UnsignedFeasibleSet;
  Range.UnsignedUpperBound = 16;
  Range.Alternatives.clear();
  const auto Match = Access::valueQuery();
  const auto Result =
      Access::batch(B, {Range, Match, Range}, 1000000, 0, 1, true, true, true);
  EXPECT_FALSE(Result.Complete);
  EXPECT_EQ(Result.Values, (std::vector<bool>{false, false, false}));
  EXPECT_EQ(Result.QueryComplete, (std::vector<bool>{false, false, false}));
  EXPECT_EQ(Result.Masks, (std::vector<uint64_t>{0, 0, 0}))
      << "a failed transaction must not expose a prefix or suffix mask";
}

TEST(ResolverValueQueryCache, MatchRefusalReplaysWithoutBecomingAProof) {
  const auto Image = image();
  const auto Q = Access::valueQuery();
  for (bool WantComplete : {false, true}) {
    for (bool WantQueryComplete : {false, true}) {
      for (bool WantMasks : {false, true}) {
        SCOPED_TRACE(std::to_string(WantComplete) + ":" +
                     std::to_string(WantQueryComplete) + ":" +
                     std::to_string(WantMasks));
        CFGBuilder B;
        Access::seed(B, Image);
        auto Run = [&](size_t MatchLimit) {
          return Access::batch(B, {Q}, 1000000, 0, MatchLimit, WantComplete,
                               WantQueryComplete, WantMasks);
        };
        const auto Refusal = Run(1);
        ASSERT_FALSE(Refusal.Complete);
        ASSERT_EQ(Refusal.Values, (std::vector<bool>{false}));
        ASSERT_GT(Refusal.Remaining, 0u);
        ASSERT_EQ(Access::stats(B)[1], 1u);
        EXPECT_EQ(Run(1), Refusal);
        EXPECT_EQ(Access::stats(B)[0], 1u);
        const auto Retried = Run(0);
        EXPECT_EQ(Retried.Complete, WantComplete);
        EXPECT_EQ(Retried.Values, (std::vector<bool>{true}));
        EXPECT_EQ(Access::stats(B)[0], 1u)
            << "a larger match allowance must reanalyze the query";
        EXPECT_EQ(Run(1), Refusal);
        EXPECT_EQ(Access::stats(B)[0], 2u);
      }
    }
  }
}

TEST(ResolverValueQueryCache, MatchRefusalPaysColdWorkAndTracksGraphChanges) {
  const auto Image = image();
  const auto Q = Access::valueQuery();
  CFGBuilder Warm;
  Access::seed(Warm, Image);
  auto Run = [&](CFGBuilder &B, size_t Budget) {
    return Access::batch(B, {Q}, Budget, 0, 1, true, true, true);
  };
  const auto Refusal = Run(Warm, 1000000);
  ASSERT_FALSE(Refusal.Complete);
  ASSERT_EQ(Access::stats(Warm)[1], 1u);
  const size_t Work = 1000000 - Refusal.Remaining;
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image);
    EXPECT_EQ(Run(Warm, Budget), Run(Cold, Budget));
  }
  const auto OriginalGraph = Access::token(Warm);
  Access::changeValue(Warm);
  const auto Changed = Run(Warm, 1000000);
  EXPECT_NE(Access::token(Warm), OriginalGraph);
  EXPECT_EQ(Access::stats(Warm)[0], 0u);
  Access::clear(Warm);
  EXPECT_EQ(Run(Warm, 1000000), Changed);
  const auto Complete = Access::batch(Warm, {Q});
  EXPECT_TRUE(Complete.Complete);
  EXPECT_EQ(Complete.Values, (std::vector<bool>{false}));
}

TEST(ResolverValueQueryCache, EarlierUnknownPreventsMatchRefusalRetention) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  auto Unknown = Access::valueQuery(16);
  Unknown.Candidate = NdVar::cst(7, 8);
  Unknown.Relation = Access::Relation::UnsignedFeasibleSet;
  Unknown.UnsignedUpperBound = 0;
  auto Direct = Access::valueQuery(16);
  Direct.Candidate = NdVar::cst(7, 8);
  const auto First = Access::batch(B, {Unknown, Direct}, 1000000, 1, 1);
  ASSERT_FALSE(First.Complete);
  ASSERT_EQ(First.Values, (std::vector<bool>{false, false}));
  EXPECT_EQ(Access::stats(B)[1], 0u);
  EXPECT_EQ(Access::batch(B, {Unknown, Direct}, 1000000, 1, 1), First);
  EXPECT_EQ(Access::stats(B)[0], 0u);
  EXPECT_EQ(Access::stats(B)[1], 0u);
  Unknown.UnsignedUpperBound = 16;
  const auto Complete = Access::batch(B, {Unknown, Direct});
  EXPECT_TRUE(Complete.Complete);
  EXPECT_EQ(Complete.Values, (std::vector<bool>{true, true}));
}

TEST(ResolverValueQueryCache, RetainedCompletedBatchesAreBounded) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  auto Q = Access::valueQuery();
  Q.Candidate = NdVar::cst(7, 8);
  for (unsigned I = 0; I != 66; ++I) {
    // Exact identities differ, while every query proves the same constant.
    Q.UseAddr = 0x1000 + I;
    const auto Answer = Access::batch(B, {Q});
    ASSERT_TRUE(Answer.Complete);
    ASSERT_EQ(Answer.Values, (std::vector<bool>{true}));
    EXPECT_LE(Access::stats(B)[1], 64u);
    EXPECT_LE(Access::stats(B)[2], 8u * 1024 * 1024);
  }
  EXPECT_GT(Access::stats(B)[1], 0u);
}

TEST(ResolverValueQueryCache, OversizedCompletedInputRunsWithoutRetention) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  auto Q = Access::valueQuery();
  Q.Candidate = NdVar::cst(7, 8);
  constexpr size_t Capacity = 8 * 1024 * 1024;
  const auto Alternative = Q.Alternatives.front();
  Q.Alternatives.assign(Capacity / sizeof(Access::Occurrence) + 1, Alternative);
  const auto Answer = Access::batch(B, {Q}, 100000000);
  ASSERT_TRUE(Answer.Complete);
  EXPECT_EQ(Answer.Values, (std::vector<bool>{true}));
  EXPECT_EQ(Access::stats(B)[0], 0u);
  EXPECT_EQ(Access::stats(B)[1], 0u);
  EXPECT_LE(Access::stats(B)[2], Capacity);

  Q.Alternatives = {Alternative};
  const auto Small = Access::batch(B, {Q});
  ASSERT_TRUE(Small.Complete);
  ASSERT_GT(Access::stats(B)[1], 0u);
  EXPECT_EQ(Access::batch(B, {Q}), Small);
  EXPECT_EQ(Access::stats(B)[0], 1u);
}

} // namespace
