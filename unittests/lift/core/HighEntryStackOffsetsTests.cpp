//===- HighEntryStackOffsetsTests.cpp - SSA frame-coordinate proofs
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/high/lower/HighEntryStackOffsets.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <set>

using namespace neverd;
using neverd::detail::HighEntryStackOffsets;

namespace {
struct Fixture {
  Arch Architecture;
  MedFunc Function;
  MedVar SP;

  explicit Fixture(Arch Architecture = Arch::X64) : Architecture(Architecture) {
    Function.Entry = 0x1000;
    Function.Name = "frame_offsets";
    Function.Blocks.resize(1);
    SP = value(0);
    SP.Kind = MedVar::Reg;
    SP.RegOff = getTargetRegInfo(Architecture).StackPointer;
  }

  MedVar value(int ID, uint16_t Width = 0) const {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = ID;
    V.SSAVer = ID ? 1 : 0;
    V.Size = Width ? Width : getTargetRegInfo(Architecture).PointerSize;
    V.TheArch = Architecture;
    return V;
  }

  MedVar copy(int ID, MedVar Input, uint16_t Width = 0,
              NdOp Opcode = NdOp::COPY) {
    MedOp Op;
    Op.Opcode = Opcode;
    Op.Output = value(ID, Width);
    Op.addInput(Input);
    Function.Blocks[0].Ops.push_back(Op);
    return Op.Output;
  }

  MedVar add(int ID, MedVar Input, int64_t Delta, bool Subtract = false,
             uint16_t ConstantWidth = 0) {
    MedOp Op;
    Op.Opcode = Subtract ? NdOp::INT_SUB : NdOp::INT_ADD;
    Op.Output = value(ID, Input.Size);
    Op.addInput(Input);
    Op.addInput(MedVar::makeConst(static_cast<uint64_t>(Delta),
                                  ConstantWidth ? ConstantWidth : Input.Size));
    Function.Blocks[0].Ops.push_back(Op);
    return Op.Output;
  }

  MedVar phi(int ID, std::initializer_list<MedVar> Inputs) {
    PhiNode P;
    P.Output = value(ID);
    int Pred = 0;
    for (const MedVar &Input : Inputs)
      P.Args.emplace_back(Pred++, Input);
    Function.Blocks[0].Phis.push_back(P);
    return P.Output;
  }
};

// The previous recursive implementation, restricted to native-width small
// displacements so host overflow and width coercion are outside its oracle
// domain. Definitions are found by scanning the fixture, not the new index.
std::optional<int64_t> legacyOffset(const Fixture &F, const MedVar &Query) {
  std::map<const PhiNode *, std::optional<int64_t>> Assumed;
  std::function<std::optional<int64_t>(const MedVar &, int)> Visit =
      [&](const MedVar &V, int Depth) -> std::optional<int64_t> {
    if (Depth > limits::kCallArgStoreAddressDepth)
      return std::nullopt;
    if (V.Kind == MedVar::Reg && V.RegOff == F.SP.RegOff && V.SSAVer == 0)
      return 0;
    const PhiNode *Phi = nullptr;
    const MedOp *Definition = nullptr;
    auto Same = [&](const MedVar &O) {
      return V.Kind == O.Kind && V.Id == O.Id && V.SSAVer == O.SSAVer;
    };
    for (const MedBlock &B : F.Function.Blocks) {
      for (const PhiNode &P : B.Phis)
        if (Same(P.Output))
          Phi = &P;
      for (const MedOp &Op : B.Ops)
        if (Same(Op.Output))
          Definition = &Op;
    }
    if (Phi) {
      if (auto It = Assumed.find(Phi); It != Assumed.end())
        return It->second;
      if (Phi->ExceptionalEntry || Phi->Args.empty())
        return std::nullopt;
      Assumed.emplace(Phi, std::nullopt);
      std::optional<int64_t> Offset;
      bool Agree = true;
      for (const auto &[Pred, In] : Phi->Args)
        if (auto Incoming = Visit(In, Depth + 1)) {
          Agree &= !Offset || *Offset == *Incoming;
          Offset = Incoming;
        }
      if (!Agree)
        Offset.reset();
      if (Offset) {
        Assumed[Phi] = Offset;
        for (const auto &[Pred, In] : Phi->Args)
          if (Visit(In, Depth + 1) != Offset) {
            Offset.reset();
            break;
          }
      }
      Assumed.erase(Phi);
      return Offset;
    }
    if (!Definition || !Definition->NumInputs)
      return std::nullopt;
    const auto Base = Visit(Definition->Inputs[0], Depth + 1);
    if (!Base)
      return std::nullopt;
    if (Definition->Opcode == NdOp::COPY ||
        Definition->Opcode == NdOp::INT_ZEXT)
      return Base;
    if (Definition->NumInputs == 2 && Definition->Inputs[1].isConst()) {
      const auto Delta = static_cast<int64_t>(Definition->Inputs[1].ConstVal);
      if (Definition->Opcode == NdOp::INT_ADD)
        return *Base + Delta;
      if (Definition->Opcode == NdOp::INT_SUB)
        return *Base - Delta;
    }
    return std::nullopt;
  };
  return Visit(Query, 0);
}

// A separate path-constraint oracle: find one simple path to entry for every
// demanded node, then check every equation against those path-derived values.
// A branch into an unanchored cycle has no such path and cannot borrow the
// anchor reached by another predecessor. These small fixtures cannot overflow.
struct Equation {
  std::vector<int> Inputs;
  int Delta = 0;
};

std::optional<int64_t> pathOracle(const std::vector<Equation> &Graph,
                                  int Root) {
  std::set<int> Needed{Root};
  std::vector<int> Work{Root};
  for (size_t I = 0; I < Work.size(); ++I)
    for (int Input : Graph[Work[I]].Inputs)
      if (Needed.insert(Input).second)
        Work.push_back(Input);
  std::map<int, int64_t> Values;
  for (int Node : Needed) {
    std::set<int> Path;
    std::function<std::optional<int64_t>(int)> Anchor =
        [&](int At) -> std::optional<int64_t> {
      if (At == 0)
        return 0;
      if (!Path.insert(At).second)
        return std::nullopt;
      for (int Input : Graph[At].Inputs)
        if (auto Offset = Anchor(Input)) {
          Path.erase(At);
          return *Offset + Graph[At].Delta;
        }
      Path.erase(At);
      return std::nullopt;
    };
    const auto Value = Anchor(Node);
    if (!Value)
      return std::nullopt;
    Values[Node] = *Value;
  }
  for (int Node : Needed)
    for (int Input : Graph[Node].Inputs)
      if (Values[Node] != Values[Input] + Graph[Node].Delta)
        return std::nullopt;
  return Values[Root];
}
} // namespace

TEST(HighEntryStackOffsets, BalancedLoopKeepsEveryIntermediateOffset) {
  Fixture F;
  const auto Initial = F.add(1, F.SP, -32);
  const auto Header = F.phi(2, {Initial, F.value(4)});
  const auto Pushed = F.add(3, Header, 16, true);
  const auto Restored = F.add(4, Pushed, 16);
  HighEntryStackOffsets Offsets(F.Function, F.Architecture);
  for (const auto &V : {Header, Pushed, Restored, Initial}) {
    EXPECT_EQ(Offsets.offset(V), legacyOffset(F, V));
    EXPECT_EQ(Offsets.offset(V), V.Id == 3 ? -48 : -32);
  }
}

TEST(HighEntryStackOffsets, UnknownConflictingAndUnanchoredPathsStayUnknown) {
  for (int Mode = 0; Mode < 4; ++Mode) {
    Fixture F;
    const auto Base = F.add(1, F.SP, -16);
    MedVar Other;
    if (Mode == 0)
      Other = F.value(99);
    else if (Mode == 1) {
      Other = F.phi(2, {F.value(3)});
      F.copy(3, Other);
    } else if (Mode == 2)
      Other = F.add(2, F.SP, -8);
    else {
      Other = F.phi(2, {Base, F.value(3)});
      F.add(3, Other, 1);
    }
    const auto Joined = F.phi(4, {Base, Other});
    const auto User = F.add(5, Joined, 16);
    HighEntryStackOffsets Offsets(F.Function, F.Architecture);
    EXPECT_EQ(Offsets.offset(Base), -16);
    EXPECT_EQ(Offsets.offset(Joined), std::nullopt) << Mode;
    EXPECT_EQ(Offsets.offset(User), std::nullopt) << Mode;
    EXPECT_EQ(Offsets.offset(Joined), legacyOffset(F, Joined)) << Mode;
  }
}

TEST(HighEntryStackOffsets, CheckedIntermediateOverflowCannotBeCancelled) {
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  for (bool Negative : {false, true}) {
    Fixture F;
    const auto Boundary = F.add(1, F.SP, Negative ? Min : Max);
    const auto Overflow = F.add(2, Boundary, Negative ? -1 : 1);
    const auto Cancelled = F.add(3, Overflow, Negative ? 1 : -1);
    const auto SubOverflow = F.add(4, F.SP, Min, true);
    HighEntryStackOffsets Offsets(F.Function, F.Architecture);
    EXPECT_EQ(Offsets.offset(Boundary), Negative ? Min : Max);
    EXPECT_EQ(Offsets.offset(Overflow), std::nullopt);
    EXPECT_EQ(Offsets.offset(Cancelled), std::nullopt);
    EXPECT_EQ(Offsets.offset(SubOverflow), std::nullopt);
  }
}

TEST(HighEntryStackOffsets, DepthBoundCannotBeBypassedByWarmQueryOrder) {
  Fixture F;
  std::vector<MedVar> Values{F.SP};
  for (int I = 1; I <= limits::kCallArgStoreAddressDepth + 2; ++I)
    Values.push_back(F.copy(I, Values.back()));
  for (bool Reverse : {false, true}) {
    HighEntryStackOffsets Offsets(F.Function, F.Architecture);
    auto Order = Values;
    if (Reverse)
      std::reverse(Order.begin(), Order.end());
    for (const auto &V : Order) {
      const auto Expected = V.Id <= limits::kCallArgStoreAddressDepth
                                ? std::optional<int64_t>(0)
                                : std::nullopt;
      EXPECT_EQ(Offsets.offset(V), Expected);
      EXPECT_EQ(Offsets.offset(V), legacyOffset(F, V));
    }
  }
}

TEST(HighEntryStackOffsets, CyclicDepthIncludesNonPhiQueryPrefix) {
  const int Limit = limits::kCallArgStoreAddressDepth;
  for (int Size : {Limit - 1, Limit, Limit + 1}) {
    for (bool ReverseInputs : {false, true}) {
      Fixture F;
      const auto Header = ReverseInputs ? F.phi(1, {F.value(Size), F.SP})
                                        : F.phi(1, {F.SP, F.value(Size)});
      std::vector<MedVar> Values{Header};
      for (int I = 2; I <= Size; ++I)
        Values.push_back(F.copy(I, Values.back()));
      for (bool ReverseQueries : {false, true}) {
        HighEntryStackOffsets Offsets(F.Function, F.Architecture);
        auto Queries = Values;
        if (ReverseQueries)
          std::reverse(Queries.begin(), Queries.end());
        for (const MedVar &V : Queries) {
          SCOPED_TRACE(::testing::Message() << "size=" << Size << " id=" << V.Id
                                            << " inputs=" << ReverseInputs
                                            << " query=" << ReverseQueries);
          const auto Expected = Size + V.Id - 1 <= Limit
                                    ? std::optional<int64_t>(0)
                                    : std::nullopt;
          EXPECT_EQ(legacyOffset(F, V), Expected);
          EXPECT_EQ(Offsets.offset(V), Expected);
        }
      }
    }
  }
}

TEST(HighEntryStackOffsets, WidePhiFanoutRetainsShallowAndExternalDepth) {
  const int Limit = limits::kCallArgStoreAddressDepth;
  for (int AnchorDepth : {0, Limit - 2, Limit - 1, Limit}) {
    for (bool ReverseInputs : {false, true}) {
      Fixture F;
      MedVar Anchor = F.SP;
      for (int I = 1; I <= AnchorDepth; ++I)
        Anchor = F.copy(I, Anchor);
      const auto Header = F.phi(100, {Anchor});
      std::vector<MedVar> Branches;
      // More SCC members than the limit, but a cycle is only two edges long.
      for (int I = 0; I < Limit + 8; ++I) {
        Branches.push_back(F.copy(200 + I, Header));
        F.Function.Blocks[0].Phis[0].Args.emplace_back(I + 1, Branches.back());
      }
      if (ReverseInputs)
        std::reverse(F.Function.Blocks[0].Phis[0].Args.begin(),
                     F.Function.Blocks[0].Phis[0].Args.end());
      const auto User = F.copy(300, Branches.front());
      for (bool ReverseQueries : {false, true}) {
        HighEntryStackOffsets Offsets(F.Function, F.Architecture);
        std::vector<MedVar> Queries{Header, Branches.front(), Branches.back(),
                                    User, Anchor};
        if (ReverseQueries)
          std::reverse(Queries.begin(), Queries.end());
        for (const MedVar &V : Queries) {
          SCOPED_TRACE(::testing::Message()
                       << "anchor depth=" << AnchorDepth << " id=" << V.Id
                       << " inputs=" << ReverseInputs
                       << " query=" << ReverseQueries);
          EXPECT_EQ(Offsets.offset(V), legacyOffset(F, V));
        }
        const int PhiDepth = std::max(2, AnchorDepth + 1);
        EXPECT_EQ(Offsets.offset(Header),
                  PhiDepth <= Limit ? std::optional<int64_t>(0) : std::nullopt);
        EXPECT_EQ(Offsets.offset(Branches.front()),
                  PhiDepth + 1 <= Limit ? std::optional<int64_t>(0)
                                        : std::nullopt);
      }
    }
  }
}

TEST(HighEntryStackOffsets, MultiplePhisShareUnaryChainAtDepthBoundary) {
  const int Limit = limits::kCallArgStoreAddressDepth;
  for (int ChainLength : {Limit - 4, Limit - 3, Limit - 2}) {
    for (bool ReverseInputs : {false, true}) {
      Fixture F;
      const auto First = F.phi(100, {F.SP});
      const auto Second = F.phi(101, {F.SP, First});
      MedVar Shared = Second;
      MedVar ChainStart;
      for (int I = 0; I < ChainLength; ++I) {
        Shared = F.copy(1000 + I, Shared);
        if (!I)
          ChainStart = Shared;
      }
      MedVar Branch;
      for (int I = 0; I < Limit + 8; ++I) {
        Branch = F.copy(2000 + I, Shared);
        F.Function.Blocks[0].Phis[0].Args.emplace_back(I + 1, Branch);
      }
      if (ReverseInputs)
        for (PhiNode &Phi : F.Function.Blocks[0].Phis)
          std::reverse(Phi.Args.begin(), Phi.Args.end());
      // First -> branch -> shared chain -> Second -> First is the longest
      // PHI-rooted path. The shared chain counts once on this path even though
      // many incoming arms use it, and a non-PHI query adds its own prefix.
      const int PhiDepth = ChainLength + 3;
      for (bool ReverseQueries : {false, true}) {
        HighEntryStackOffsets Offsets(F.Function, F.Architecture);
        std::vector<MedVar> Queries{First, Second, ChainStart, Shared, Branch};
        if (ReverseQueries)
          std::reverse(Queries.begin(), Queries.end());
        for (const MedVar &V : Queries) {
          SCOPED_TRACE(::testing::Message()
                       << "chain=" << ChainLength << " id=" << V.Id
                       << " inputs=" << ReverseInputs
                       << " query=" << ReverseQueries);
          EXPECT_EQ(Offsets.offset(V), legacyOffset(F, V));
        }
        const auto PhiExpected =
            PhiDepth <= Limit ? std::optional<int64_t>(0) : std::nullopt;
        EXPECT_EQ(Offsets.offset(First), PhiExpected);
        EXPECT_EQ(Offsets.offset(Second), PhiExpected);
        EXPECT_EQ(Offsets.offset(ChainStart), PhiDepth + 1 <= Limit
                                                  ? std::optional<int64_t>(0)
                                                  : std::nullopt);
      }
    }
  }
}

TEST(HighEntryStackOffsets, CoupledPhiCyclesRequireCompleteGlobalProof) {
  for (bool ReverseInputs : {false, true}) {
    Fixture F;
    const auto Base = F.add(1, F.SP, -48);
    const auto First = ReverseInputs ? F.phi(2, {F.value(5), Base})
                                     : F.phi(2, {Base, F.value(5)});
    const auto Pushed = F.add(3, First, -16);
    const auto Second = ReverseInputs ? F.phi(4, {F.value(6), Pushed})
                                      : F.phi(4, {Pushed, F.value(6)});
    const auto Restored = F.add(5, Second, 16);
    const auto Back = F.copy(6, Second);
    const std::vector<Equation> Graph{{{}, 0},    {{0}, -48},  {{1, 5}, 0},
                                      {{2}, -16}, {{3, 6}, 0}, {{4}, 16},
                                      {{4}, 0}};
    // A cold recursive query of Second cannot validate First until Second's
    // assumption exists. It therefore misses this fully anchored solution.
    EXPECT_EQ(legacyOffset(F, First), -48);
    EXPECT_EQ(legacyOffset(F, Second), std::nullopt);
    for (bool ReverseQueries : {false, true}) {
      HighEntryStackOffsets Offsets(F.Function, F.Architecture);
      std::vector<MedVar> Queries{Base, First, Pushed, Second, Restored, Back};
      if (ReverseQueries)
        std::reverse(Queries.begin(), Queries.end());
      for (const MedVar &V : Queries) {
        EXPECT_EQ(Offsets.offset(V), pathOracle(Graph, V.Id));
        EXPECT_EQ(Offsets.offset(V),
                  V.Id == 3 || V.Id == 4 || V.Id == 6 ? -64 : -48);
      }
    }
  }
}

TEST(HighEntryStackOffsets, ScalarWidthAndNativePointerViewsStayDistinct) {
  for (Arch A : {Arch::X64, Arch::X86, Arch::AArch64, Arch::ARM}) {
    Fixture F(A);
    const auto SmallLiteral = F.add(1, F.SP, 0xf0, false, 1);
    const auto Negative = F.add(2, F.SP, -16);
    const auto Narrow = F.copy(3, F.SP, 2);
    auto Foreign = F.SP;
    Foreign.TheArch = A == Arch::X64 ? Arch::AArch64 : Arch::X64;
    const auto ForeignCopy = F.copy(4, Foreign);
    const auto Extended = F.copy(5, F.SP, 8, NdOp::INT_ZEXT);
    auto Truncated = F.value(5, 2);
    const auto OverWide = F.copy(6, F.SP, 16, NdOp::INT_ZEXT);
    const auto ConstantRoot = F.copy(7, MedVar::makeConst(0, F.SP.Size));
    HighEntryStackOffsets Offsets(F.Function, A);
    EXPECT_EQ(Offsets.offset(SmallLiteral), 240);
    EXPECT_EQ(Offsets.offset(Negative), -16);
    EXPECT_EQ(Offsets.offset(Narrow), std::nullopt);
    EXPECT_EQ(Offsets.offset(ForeignCopy), std::nullopt);
    EXPECT_EQ(Offsets.offset(Extended), 0);
    EXPECT_EQ(Offsets.offset(Truncated), std::nullopt);
    EXPECT_EQ(Offsets.offset(OverWide), std::nullopt);
    EXPECT_EQ(Offsets.offset(ConstantRoot), std::nullopt);
  }
}

TEST(HighEntryStackOffsets, AmbiguousDefinitionsAndExceptionEntriesRefuse) {
  Fixture F;
  const auto Duplicate = F.copy(1, F.SP);
  F.copy(1, F.SP);
  const auto DuplicatePhi = F.phi(2, {F.SP});
  F.phi(2, {F.SP});
  const auto Both = F.phi(3, {F.SP});
  F.copy(3, F.SP);
  const auto Exceptional = F.phi(4, {F.SP});
  F.Function.Blocks[0].Phis.back().ExceptionalEntry = F.SP;
  const auto Ordered = F.copy(5, F.SP);
  F.Function.Blocks[0].Ops.back().MemoryOrdering =
      NdMemoryOrdering::SequentiallyConsistent;
  HighEntryStackOffsets Offsets(F.Function, F.Architecture);
  for (const auto &V : {Duplicate, DuplicatePhi, Both, Exceptional, Ordered})
    EXPECT_EQ(Offsets.offset(V), std::nullopt) << V.Id;
}

TEST(HighEntryStackOffsets, SharedDiamondsDoNotExpandAnExponentialTree) {
  Fixture F;
  MedVar Current = F.SP;
  // The recursive implementation visits the shared input once for discovery
  // and once for validation on each arm: 4^30 visits without a graph proof.
  for (int I = 1; I <= 30; ++I)
    Current = F.phi(I, {Current, Current});
  HighEntryStackOffsets Offsets(F.Function, F.Architecture);
  EXPECT_EQ(Offsets.offset(Current), 0);
  for (int I = 1; I <= 30; ++I)
    EXPECT_EQ(Offsets.offset(F.value(I)), 0);
}

TEST(HighEntryStackOffsets, SmallGraphsMatchIndependentPathConstraints) {
  std::mt19937 Random(0x735041);
  for (unsigned Trial = 0; Trial < 160; ++Trial) {
    Fixture F;
    std::vector<Equation> Graph(7);
    for (int I = 1; I < 7; ++I) {
      const int First = Random() % Graph.size();
      auto Value = [&](int ID) { return ID ? F.value(ID) : F.SP; };
      if (Random() % 2) {
        const int Second = Random() % Graph.size();
        Graph[I].Inputs = {First, Second};
        F.phi(I, {Value(First), Value(Second)});
      } else {
        Graph[I].Inputs = {First};
        Graph[I].Delta = int(Random() % 3) - 1;
        F.add(I, Value(First), Graph[I].Delta);
      }
    }
    HighEntryStackOffsets Forward(F.Function, F.Architecture);
    HighEntryStackOffsets Reverse(F.Function, F.Architecture);
    for (int I = 1; I < 7; ++I) {
      EXPECT_EQ(Forward.offset(F.value(I)), pathOracle(Graph, I))
          << "trial=" << Trial << " node=" << I;
      EXPECT_EQ(Reverse.offset(F.value(7 - I)), pathOracle(Graph, 7 - I))
          << "trial=" << Trial << " node=" << 7 - I;
    }
  }
}

TEST(HighEntryStackOffsets, ReliableRecursiveDomainMatchesEveryQueryOrder) {
  for (unsigned Seed = 0; Seed < 32; ++Seed) {
    Fixture F;
    std::mt19937 Random(Seed);
    std::vector<MedVar> Values{F.SP};
    for (int I = 1; I <= 9; ++I) {
      const auto First = Values[Random() % Values.size()];
      if (Random() % 2)
        Values.push_back(F.phi(I, {First, Values[Random() % Values.size()]}));
      else
        Values.push_back(F.add(I, First, int(Random() % 9) - 4));
    }
    HighEntryStackOffsets Offsets(F.Function, F.Architecture);
    std::shuffle(Values.begin(), Values.end(), Random);
    for (const auto &V : Values)
      EXPECT_EQ(Offsets.offset(V), legacyOffset(F, V)) << Seed << ':' << V.Id;
  }
}
