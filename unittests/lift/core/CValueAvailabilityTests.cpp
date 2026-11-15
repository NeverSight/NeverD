//===- CValueAvailabilityTests.cpp - Emitted availability paths -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent flow fixtures for the shared C availability proof, including
/// counterexamples where removing a guard would observe missing state.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/CValueAvailability.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <random>
#include <set>
#include <tuple>
#include <utility>

using namespace neverd;
using namespace neverd::c_availability;

namespace {
constexpr Value A = 1, B = 2, Payload = 3, Other = 4;

Edge from(NodeIndex N, std::optional<Predicate> P = std::nullopt) {
  Edge E;
  E.Predecessor = N;
  E.Condition = P;
  return E;
}

void setCopies(Edge &E, std::initializer_list<Assignment> Writes) {
  E.Copies.clear();
  for (const auto &A : Writes)
    E.addCopy(A);
}

Graph guarded(unsigned Bits = 64) {
  Graph G;
  G.Nodes.resize(7);
  G.Nodes[0].Entry = true;
  G.Nodes[1].Predecessors.push_back(
      from(0, Predicate{A, B, Bits, Equal | Greater}));
  G.Nodes[1].Assignments.push_back(
      {Payload, std::nullopt, 0, FlagWrite::Unknown});
  G.Nodes[2].Predecessors.push_back(from(0, Predicate{A, B, Bits, Less}));
  G.Nodes[2].Assignments.push_back(
      {Payload, std::nullopt, 0, FlagWrite::Known});
  G.Nodes[3].Predecessors = {from(1), from(2)};
  G.Nodes[4].Predecessors.push_back(
      from(3, Predicate{A, B, Bits, Less | Greater}));
  G.Nodes[5].Predecessors.push_back(
      from(4, Predicate{A, B, Bits, Less | Equal}));
  G.Nodes[6].Predecessors.push_back(from(5));
  return G;
}

bool available(const Graph &G, Point P = {6, 0}, Value V = Payload) {
  return availableBefore(G, P, V);
}
} // namespace

TEST(CValueAvailability, EveryReachingContextMustPublishAKnownPayload) {
  for (unsigned Bits : {8, 16, 32, 64}) {
    SCOPED_TRACE(Bits);
    Graph G = guarded(Bits);
    EXPECT_TRUE(available(G));
    std::swap(G.Nodes[3].Predecessors[0], G.Nodes[3].Predecessors[1]);
    EXPECT_TRUE(available(G));
    G.Nodes[4].Predecessors[0].Condition.reset();
    EXPECT_FALSE(available(G)); // Equality now reaches the unknown arm.
    G = guarded(Bits);
    G.Nodes[5].Predecessors[0].Condition.reset();
    EXPECT_FALSE(available(G)); // Greater-than now reaches that arm.
    G = guarded(Bits);
    G.Nodes[2].Assignments[0].Availability = FlagWrite::Unknown;
    EXPECT_FALSE(available(G));
  }
}

TEST(CValueAvailability, SignednessAndWidthBelongToThePredicate) {
  Graph G = guarded(8);
  G.Nodes[5].Predecessors[0].Condition->Signed = true;
  EXPECT_FALSE(available(G)); // A=255, B=0: unsigned greater, signed less.
  G = guarded(8);
  G.Nodes[5].Predecessors[0].Condition->Bits = 64;
  EXPECT_FALSE(available(G));
  G = guarded(8);
  auto &Condition = *G.Nodes[5].Predecessors[0].Condition;
  std::swap(Condition.Left, Condition.Right);
  Condition.Allowed = Greater | Equal;
  EXPECT_TRUE(available(G));
}

TEST(CValueAvailability, ExactCopiesCarryFactsAndWritesKillThem) {
  Graph G = guarded(32);
  G.Nodes[4].Assignments.push_back({Other, A, 32, FlagWrite::None});
  G.Nodes[5].Predecessors[0].Condition->Left = Other;
  EXPECT_TRUE(available(G));
  G.Nodes[4].Assignments[0].CopyBits = 8;
  EXPECT_FALSE(available(G));
  G.Nodes[4].Assignments[0].CopyBits = 32;
  G.Nodes[4].Assignments[0].Source.reset();
  EXPECT_FALSE(available(G));
  G = guarded(32);
  G.Nodes[4].Assignments.push_back({A, std::nullopt, 0, FlagWrite::None});
  EXPECT_FALSE(available(G));
}

TEST(CValueAvailability, PhiPredicatesReadParallelInputs) {
  Graph G = guarded(32);
  G.Nodes[5].Predecessors[0].Condition.reset();
  setCopies(G.Nodes[5].Predecessors[0],
            {{A, B, 32, FlagWrite::None}, {B, A, 32, FlagWrite::None}});
  G.Nodes[6].Predecessors[0].Condition = Predicate{A, B, 32, Less};
  EXPECT_FALSE(available(G)); // Old A>B becomes new A<B after the swap.
  setCopies(G.Nodes[5].Predecessors[0],
            {{B, A, 32, FlagWrite::None}, {A, B, 32, FlagWrite::None}});
  EXPECT_FALSE(available(G));
  G.Nodes[6].Predecessors[0].Condition->Allowed = Greater;
  EXPECT_TRUE(available(G)); // Old A<B must have taken the known arm.
}

TEST(CValueAvailability, PhiAvailabilityAlsoReadsParallelInputs) {
  Graph G;
  G.Nodes.resize(3);
  G.Nodes[0].Entry = true;
  G.Nodes[1].Predecessors.push_back(from(0));
  G.Nodes[1].Assignments = {{A, std::nullopt, 0, FlagWrite::Unknown},
                            {B, std::nullopt, 0, FlagWrite::Known}};
  G.Nodes[2].Predecessors.push_back(from(1));
  setCopies(G.Nodes[2].Predecessors[0],
            {{A, B, 32, FlagWrite::Copy}, {B, A, 32, FlagWrite::Copy}});
  EXPECT_TRUE(available(G, {2, 0}, A));
  EXPECT_FALSE(available(G, {2, 0}, B));
  setCopies(G.Nodes[2].Predecessors[0],
            {{B, A, 32, FlagWrite::Copy}, {A, B, 32, FlagWrite::Copy}});
  EXPECT_TRUE(available(G, {2, 0}, A));
  EXPECT_FALSE(available(G, {2, 0}, B));
}

TEST(CValueAvailability, ObservationPrecedesItsOwnAssignment) {
  Graph G;
  G.Nodes.resize(1);
  G.Nodes[0].Entry = true;
  G.Nodes[0].Assignments = {{Payload, std::nullopt, 0, FlagWrite::Known},
                            {Payload, std::nullopt, 0, FlagWrite::Unknown}};
  EXPECT_FALSE(available(G, {0, 0}));
  EXPECT_TRUE(available(G, {0, 1}));
  EXPECT_FALSE(available(G, {0, 2}));
}

TEST(CValueAvailability, UnknownWriteCannotBorrowAnOlderKnownWrite) {
  Graph G;
  G.Nodes.resize(2);
  G.Nodes[0].Entry = true;
  G.Nodes[0].Assignments = {{Payload, std::nullopt, 0, FlagWrite::Known},
                            {Payload, std::nullopt, 0, FlagWrite::Unknown}};
  G.Nodes[1].Predecessors.push_back(from(0));
  EXPECT_FALSE(available(G, {1, 0}));
}

TEST(CValueAvailability, RootsBarriersAndIncompleteGraphsRetainTheGuard) {
  Graph G = guarded();
  G.Nodes[4].Entry =
      true; // An exception/address-taken entry bypasses the join.
  EXPECT_FALSE(available(G));
  G = guarded();
  G.Nodes[4].Barrier = true;
  EXPECT_FALSE(available(G));
  G = guarded();
  G.Complete = false;
  EXPECT_FALSE(available(G));
  G = guarded();
  G.Nodes[6].Predecessors.push_back(from(0));
  EXPECT_FALSE(available(G));
}

TEST(CValueAvailability, UnreachableNodesRequireCompleteEntryInformation) {
  Graph G;
  G.Nodes.resize(1);
  EXPECT_TRUE(available(G, {0, 0}));
  G.Nodes[0].Entry = true;
  EXPECT_FALSE(available(G, {0, 0}));
}

TEST(CValueAvailability, CyclesKeepTheirEntryAndEveryIteration) {
  Graph G;
  G.Nodes.resize(3);
  G.Nodes[0].Entry = true;
  G.Nodes[1].Predecessors = {from(0), from(2)};
  G.Nodes[2].Predecessors = {from(1)};
  EXPECT_FALSE(available(G, {2, 0}));
  G.Nodes[0].Assignments.push_back(
      {Payload, std::nullopt, 0, FlagWrite::Known});
  EXPECT_TRUE(available(G, {2, 0}));
  G.Nodes[2].Assignments.push_back(
      {Payload, std::nullopt, 0, FlagWrite::Unknown});
  // The first arrival is known, but a later iteration reaches this same
  // observation after the loop has overwritten its payload.
  EXPECT_FALSE(available(G, {2, 0}));
}

TEST(CValueAvailability, LongDependenciesDoNotLimitIndependentQueries) {
  Graph G = guarded();
  NodeIndex Previous = 6;
  for (unsigned I = 0; I != 4096; ++I) {
    Node N;
    N.Predecessors.push_back(from(Previous));
    Previous = G.Nodes.size();
    G.Nodes.push_back(std::move(N));
  }
  for (unsigned I = 0; I != 16; ++I) {
    EXPECT_TRUE(available(G, {Previous, 0}));
    EXPECT_FALSE(available(G, {Previous, 0}, Other));
  }
  EXPECT_TRUE(available(G));
}

TEST(CValueAvailability, AllPredicateFactsSurviveLongPaths) {
  Graph G = guarded(32);
  NodeIndex Previous = 6;
  for (unsigned I = 0; I != 96; ++I) {
    Node N;
    N.Predecessors.push_back(
        from(Previous, Predicate{100 + 2 * I, 101 + 2 * I, 32, Less}));
    Previous = G.Nodes.size();
    G.Nodes.push_back(std::move(N));
  }
  EXPECT_TRUE(available(G, {Previous, 0}));
  G.Nodes[5].Predecessors[0].Condition.reset();
  EXPECT_FALSE(available(G, {Previous, 0}));
}

TEST(CValueAvailability, ReconvergingPathsShareEquivalentStates) {
  Graph G;
  G.Nodes.resize(1);
  G.Nodes[0].Entry = true;
  G.Nodes[0].Assignments.push_back(
      {Payload, std::nullopt, 0, FlagWrite::Known});
  NodeIndex Previous = 0;
  for (unsigned I = 0; I != 2048; ++I) {
    const auto First = G.Nodes.size();
    G.Nodes.resize(First + 3);
    G.Nodes[First].Predecessors = {from(Previous)};
    G.Nodes[First + 1].Predecessors = {from(Previous)};
    G.Nodes[First + 2].Predecessors = {from(First), from(First + 1)};
    Previous = First + 2;
  }
  EXPECT_TRUE(available(G, {Previous, 0}));
  G.Nodes[1].Assignments.push_back(
      {Payload, std::nullopt, 0, FlagWrite::Unknown});
  EXPECT_FALSE(available(G, {Previous, 0}));
}

TEST(CValueAvailability, GuardedLoopsReuseWeakerReachingContexts) {
  Graph G;
  G.Nodes.resize(2);
  G.Nodes[0].Entry = true;
  G.Nodes[0].Assignments.push_back(
      {Payload, std::nullopt, 0, FlagWrite::Known});
  G.Nodes[1].Predecessors.push_back(from(0));
  // Each trip can take any of these independent guarded backedges. The
  // payload stays known for every finite execution, even though there are
  // exponentially many accumulated combinations of path constraints.
  for (unsigned I = 0; I != 96; ++I)
    G.Nodes[1].Predecessors.push_back(
        from(1, Predicate{100 + 2 * I, 101 + 2 * I, 32, Less}));
  EXPECT_TRUE(available(G, {1, 0}));
  // One iteration may instead replace the payload with an unavailable value.
  // Its later observation must not inherit the earlier known state.
  auto &Last = G.Nodes[1].Predecessors.back();
  Last.addCopy({Payload, std::nullopt, 0, FlagWrite::Unknown});
  EXPECT_FALSE(available(G, {1, 0}));
  std::reverse(G.Nodes[1].Predecessors.begin(), G.Nodes[1].Predecessors.end());
  EXPECT_FALSE(available(G, {1, 0}));
}

TEST(CValueAvailability, ShortUnknownPathsAreNotHiddenByLongBranchingPaths) {
  Graph G;
  G.Nodes.resize(2);
  G.Nodes[0].Entry = true;
  G.Nodes[1].Entry = true;
  G.Nodes[1].Assignments.push_back(
      {Payload, std::nullopt, 0, FlagWrite::Known});
  NodeIndex Previous = 1;
  // The longer alternative has distinct feasible combinations of 96 tests.
  // None can invalidate the direct path from the unknown entry to the use.
  for (unsigned I = 0; I != 96; ++I) {
    const auto First = G.Nodes.size();
    G.Nodes.resize(First + 3);
    G.Nodes[First].Predecessors = {
        from(Previous, Predicate{100 + 2 * I, 101 + 2 * I, 32, Less})};
    G.Nodes[First + 1].Predecessors = {from(
        Previous, Predicate{100 + 2 * I, 101 + 2 * I, 32, Equal | Greater})};
    G.Nodes[First + 2].Predecessors = {from(First), from(First + 1)};
    Previous = First + 2;
  }
  const auto Observation = G.Nodes.size();
  G.Nodes.resize(Observation + 1);
  auto &Incoming = G.Nodes.back().Predecessors;
  Incoming = {from(0), from(Previous)};
  EXPECT_FALSE(available(G, {Observation, 0}));
  std::reverse(Incoming.begin(), Incoming.end());
  EXPECT_FALSE(available(G, {Observation, 0}));
  // If both entry alternatives publish a value, availability does not
  // depend on enumerating any combination of those branch predicates.
  G.Nodes[0].Assignments = {{Payload, std::nullopt, 0, FlagWrite::Known}};
  EXPECT_TRUE(available(G, {Observation, 0}));
  // A distant unknown entry also has a concrete path witness. Sharing a
  // structural location must not discard it or invent a known payload.
  Incoming = {from(Previous)};
  G.Nodes[1].Assignments[0].Availability = FlagWrite::Unknown;
  EXPECT_FALSE(available(G, {Observation, 0}));
}

TEST(CValueAvailability, ARefutedFirstContextDoesNotProveTheOtherContexts) {
  Graph G;
  G.Nodes.resize(5);
  G.Nodes[0].Entry = true;
  G.Nodes[1].Predecessors = {from(0, Predicate{A, B, 32, Greater})};
  G.Nodes[2].Predecessors = {from(1, Predicate{A, B, 32, Less})};
  G.Nodes[3].Predecessors = {from(1, Predicate{A, B, 32, Greater})};
  G.Nodes[4].Predecessors = {from(2), from(3)};
  // The first structural path requires A>B and A<B. The sibling still
  // reaches the unknown payload whenever A>B, so it needs its own facts.
  EXPECT_FALSE(available(G, {4, 0}));
  std::reverse(G.Nodes[4].Predecessors.begin(), G.Nodes[4].Predecessors.end());
  EXPECT_FALSE(available(G, {4, 0}));
  G.Nodes[3].Predecessors[0].Condition->Allowed = Less;
  EXPECT_TRUE(available(G, {4, 0})); // Both contexts are now infeasible.
  G.Nodes[1].Assignments = {{A, std::nullopt, 0, FlagWrite::None}};
  EXPECT_FALSE(available(G, {4, 0})); // An intervening write kills that proof.
}

TEST(CValueAvailability, LongEntryWitnessesPrecedeUnrelatedBranchCombinations) {
  Graph G;
  G.Nodes.resize(1);
  G.Nodes[0].Entry = true;
  auto Add = [&](llvm::SmallVector<Edge, 2> Incoming) {
    const auto Index = G.Nodes.size();
    G.Nodes.emplace_back().Predecessors = std::move(Incoming);
    return Index;
  };
  NodeIndex Previous = 0;
  for (unsigned I = 0; I != 1024; ++I)
    Previous = Add({from(Previous)});
  const auto Gate = Add({from(Previous, Predicate{A, B, 32, Greater})});
  const auto Refuted = Add({from(Gate, Predicate{A, B, 32, Less})});
  auto Witness = Add({from(Gate, Predicate{A, B, 32, Greater})});
  Witness = Add({from(Witness)});
  Previous = Gate;
  // These alternatives constrain different operands. Their combinations
  // cannot collapse into one exact predicate union. The first structural
  // path to the shared entry is refuted, so a relational traversal is needed.
  for (unsigned I = 0; I != 96; ++I) {
    const auto Left =
        Add({from(Previous, Predicate{100 + 4 * I, 101 + 4 * I, 32, Less})});
    const auto Right =
        Add({from(Previous, Predicate{102 + 4 * I, 103 + 4 * I, 32, Less})});
    Previous = Add({from(Left), from(Right)});
  }
  const auto Observation = Add({from(Refuted), from(Witness), from(Previous)});
  EXPECT_FALSE(available(G, {Observation, 0})); // A=1, B=0 reaches Witness.
  for (Node &N : G.Nodes)
    std::reverse(N.Predecessors.begin(), N.Predecessors.end());
  EXPECT_FALSE(available(G, {Observation, 0}));
  G.Nodes[0].Assignments = {{Payload, std::nullopt, 0, FlagWrite::Known}};
  EXPECT_TRUE(available(G, {Observation, 0}));
  G.Nodes[0].Assignments.clear();
  G.Nodes[Previous].Assignments = {
      {Payload, std::nullopt, 0, FlagWrite::Known}};
  G.Nodes[Observation].Predecessors = {from(Refuted), from(Previous)};
  EXPECT_TRUE(available(G, {Observation, 0}));
  G.Nodes[Observation].Predecessors.push_back(from(Witness));
  EXPECT_FALSE(available(G, {Observation, 0}));
}

TEST(CValueAvailability, ComplementaryBranchesKeepTheCompleteRelationalProof) {
  Graph G = guarded(32);
  NodeIndex Previous = 6;
  // The unknown entry is reachable structurally, but the surrounding guard
  // rules it out. A proof must cross all these independent diamonds before
  // that contradiction is visible; neither the structural pass nor dropping
  // predicates can prove it. Complementary branches have an exact union.
  for (unsigned I = 0; I != 96; ++I) {
    const auto First = G.Nodes.size();
    G.Nodes.resize(First + 3);
    G.Nodes[First].Predecessors = {from(
        Previous, Predicate{100 + 2 * I, 101 + 2 * I, 32, Less, bool(I % 2)})};
    G.Nodes[First + 1].Predecessors = {
        from(Previous, Predicate{100 + 2 * I, 101 + 2 * I, 32, Equal | Greater,
                                 bool(I % 2)})};
    G.Nodes[First + 2].Predecessors = {from(First), from(First + 1)};
    Previous = First + 2;
  }
  EXPECT_TRUE(available(G, {Previous, 0}));
  for (Node &N : G.Nodes)
    std::reverse(N.Predecessors.begin(), N.Predecessors.end());
  EXPECT_TRUE(available(G, {Previous, 0}));
  G.Nodes[5].Predecessors[0].Condition.reset();
  EXPECT_FALSE(available(G, {Previous, 0}));
  G.Nodes[5].Predecessors[0].Condition = Predicate{A, B, 32, Less | Equal};
  G.Nodes[Previous].Assignments = {
      {Payload, std::nullopt, 0, FlagWrite::Unknown}};
  EXPECT_FALSE(available(G, {Previous, 1}));
}

TEST(CValueAvailability, ContextUnionsPreserveSignedAndUnsignedCorrelation) {
  // These concrete bytes exercise every joint signed/unsigned ordering.
  // Compare their actual values, then check every pair of masked branches.
  const std::array<std::pair<unsigned, unsigned>, 5> Inputs{
      {{0, 1}, {0, 255}, {5, 5}, {255, 0}, {1, 0}}};
  for (uint8_t LU = 1; LU <= Any; ++LU)
    for (uint8_t LS = 1; LS <= Any; ++LS)
      for (uint8_t RU = 1; RU <= Any; ++RU)
        for (uint8_t RS = 1; RS <= Any; ++RS)
          for (auto [X, Y] : Inputs) {
            const int SX = X < 128 ? int(X) : int(X) - 256;
            const int SY = Y < 128 ? int(Y) : int(Y) - 256;
            const uint8_t U = X < Y ? Less : X == Y ? Equal : Greater;
            const uint8_t S = SX < SY ? Less : SX == SY ? Equal : Greater;
            const bool Reaches =
                ((LU & U) && (LS & S)) || ((RU & U) && (RS & S));
            Graph G;
            G.Nodes.resize(8);
            G.Nodes[0].Entry = true;
            G.Nodes[1].Predecessors = {from(0, Predicate{A, B, 8, U})};
            G.Nodes[2].Predecessors = {from(1, Predicate{A, B, 8, S, true})};
            G.Nodes[3].Predecessors = {from(2, Predicate{A, B, 8, LU})};
            G.Nodes[4].Predecessors = {from(3, Predicate{A, B, 8, LS, true})};
            G.Nodes[5].Predecessors = {from(2, Predicate{A, B, 8, RU})};
            G.Nodes[6].Predecessors = {from(5, Predicate{A, B, 8, RS, true})};
            G.Nodes[7].Predecessors = {from(4), from(6)};
            for (unsigned Order = 0; Order != 2; ++Order) {
              EXPECT_EQ(available(G, {7, 0}), !Reaches)
                  << "masks=" << unsigned(LU) << ',' << unsigned(LS) << ','
                  << unsigned(RU) << ',' << unsigned(RS) << " inputs=" << X
                  << ',' << Y << " order=" << Order;
              std::reverse(G.Nodes[7].Predecessors.begin(),
                           G.Nodes[7].Predecessors.end());
            }
          }
}

TEST(CValueAvailability, ContextUnionsKeepDifferentPredicatesCorrelated) {
  Graph G;
  G.Nodes.resize(8);
  G.Nodes[0].Entry = true;
  G.Nodes[1].Predecessors = {from(0, Predicate{A, B, 32, Less})};
  G.Nodes[2].Predecessors = {from(1, Predicate{Other, 5, 32, Equal | Greater})};
  G.Nodes[3].Predecessors = {from(2, Predicate{A, B, 32, Less})};
  G.Nodes[4].Predecessors = {from(3, Predicate{Other, 5, 32, Less})};
  G.Nodes[5].Predecessors = {from(2, Predicate{A, B, 32, Equal | Greater})};
  G.Nodes[6].Predecessors = {from(5, Predicate{Other, 5, 32, Equal | Greater})};
  G.Nodes[7].Predecessors = {from(4), from(6)};
  // The two branches admit (<,<) or (>=,>=). Independently joining their
  // predicates would invent the (<,>=) combination admitted by the entry.
  EXPECT_TRUE(available(G, {7, 0}));
  std::reverse(G.Nodes[7].Predecessors.begin(), G.Nodes[7].Predecessors.end());
  EXPECT_TRUE(available(G, {7, 0}));
  G.Nodes[2].Predecessors[0].Condition->Allowed = Less;
  EXPECT_FALSE(available(G, {7, 0}));
  G.Nodes[2].Predecessors[0].Condition->Allowed = Equal | Greater;
  G.Nodes[1].Predecessors[0].Condition.reset();
  EXPECT_FALSE(available(G, {7, 0}));
}

TEST(CValueAvailability, CyclicPhiSwapsRetainDistinctPayloadStates) {
  Graph G;
  G.Nodes.resize(3);
  G.Nodes[0].Entry = true;
  G.Nodes[0].Assignments = {{A, std::nullopt, 0, FlagWrite::Known},
                            {B, std::nullopt, 0, FlagWrite::Unknown}};
  G.Nodes[1].Predecessors = {from(0), from(2)};
  G.Nodes[2].Predecessors = {from(1)};
  setCopies(G.Nodes[1].Predecessors[1],
            {{A, B, 32, FlagWrite::Copy}, {B, A, 32, FlagWrite::Copy}});
  EXPECT_FALSE(available(G, {1, 0}, A));
  EXPECT_FALSE(available(G, {1, 0}, B));
  G.Nodes[0].Assignments[1].Availability = FlagWrite::Known;
  EXPECT_TRUE(available(G, {1, 0}, A));
  EXPECT_TRUE(available(G, {1, 0}, B));
}

TEST(CValueAvailability, IntegerRelationsMatchAnExhaustiveByteOracle) {
  // Enumerate machine bit patterns independently of the proof. These five
  // pairs exhaust the possible unsigned/signed orderings of two bytes.
  std::set<std::pair<uint8_t, uint8_t>> Possible;
  for (unsigned X = 0; X != 256; ++X)
    for (unsigned Y = 0; Y != 256; ++Y) {
      const int SX = X < 128 ? int(X) : int(X) - 256;
      const int SY = Y < 128 ? int(Y) : int(Y) - 256;
      const uint8_t U = X < Y ? Less : X == Y ? Equal : Greater;
      const uint8_t S = SX < SY ? Less : SX == SY ? Equal : Greater;
      Possible.emplace(U, S);
    }
  ASSERT_EQ(Possible.size(), 5u);
  for (unsigned Modes = 0; Modes != 8; ++Modes)
    for (uint8_t First = 1; First <= Any; ++First)
      for (uint8_t Second = 1; Second <= Any; ++Second)
        for (uint8_t Third = 1; Third <= Any; ++Third) {
          const std::array<uint8_t, 3> Masks{First, Second, Third};
          bool Feasible = false;
          for (const auto &[Unsigned, Signed] : Possible) {
            bool Match = true;
            for (unsigned I = 0; I != 3; ++I)
              Match &=
                  bool(Masks[I] & ((Modes & (1 << I)) ? Signed : Unsigned));
            Feasible |= Match;
          }
          Graph G;
          G.Nodes.resize(4);
          G.Nodes[0].Entry = true;
          for (unsigned I = 0; I != 3; ++I)
            G.Nodes[I + 1].Predecessors.push_back(
                from(I, Predicate{A, B, 8, Masks[I], bool(Modes & (1 << I))}));
          EXPECT_EQ(available(G, {3, 0}), !Feasible)
              << "modes=" << Modes << " masks=" << unsigned(First) << ','
              << unsigned(Second) << ',' << unsigned(Third);
        }
}

TEST(CValueAvailability, BackwardProofsAgreeWithForwardFourBitExecution) {
  // An independent, concrete forward executor. It enumerates every entry
  // input and reachable concrete state in generated DAGs and cyclic graphs;
  // it does not implement
  // backward substitutions or the proof's relational fact representation.
  struct State {
    NodeIndex Node = 0;
    std::array<unsigned, 5> Values{};
    std::array<bool, 5> Known{};
    bool operator<(const State &Other) const {
      return std::tie(Node, Values, Known) <
             std::tie(Other.Node, Other.Values, Other.Known);
    }
  };
  auto Apply = [](State &S, const Assignment &A, const State &Before) {
    S.Values[A.Destination] = A.Source ? Before.Values[*A.Source] : 13;
    switch (A.Availability) {
    case FlagWrite::None:
      break;
    case FlagWrite::Known:
      S.Known[A.Destination] = true;
      break;
    case FlagWrite::Unknown:
      S.Known[A.Destination] = false;
      break;
    case FlagWrite::Copy:
      S.Known[A.Destination] = Before.Known[*A.Source];
      break;
    }
  };
  auto Allows = [](const State &S, const Predicate &P) {
    int L = S.Values[P.Left], R = S.Values[P.Right];
    if (P.Signed) {
      if (L >= 8)
        L -= 16;
      if (R >= 8)
        R -= 16;
    }
    const unsigned Relation = L < R ? Less : L == R ? Equal : Greater;
    return bool(P.Allowed & Relation);
  };
  std::minstd_rand Random(271828);
  unsigned Proven = 0, Unproven = 0;
  for (unsigned Trial = 0; Trial != 512; ++Trial) {
    Graph G;
    G.Nodes.resize(7);
    G.Nodes[0].Entry = true;
    G.Nodes[0].Assignments = {{A, A, 4, FlagWrite::Known},
                              {B, B, 4, FlagWrite::Known}};
    auto Write = [&](Value Destination) {
      Assignment A;
      A.Destination = Destination;
      if (Random() % 4) {
        A.Source = 1 + Random() % 4;
        A.CopyBits = 4;
      }
      A.Availability =
          A.Source ? FlagWrite(Random() % 4)
                   : (Random() % 2 ? FlagWrite::Known : FlagWrite::Unknown);
      return A;
    };
    for (NodeIndex N = 1; N != G.Nodes.size(); ++N) {
      G.Nodes[N].Assignments.push_back(Write(1 + Random() % 4));
      for (NodeIndex Pred : {N - 1, NodeIndex(Random() % N)}) {
        Edge E = from(Pred);
        if (Random() % 2)
          E.Condition =
              Predicate{1 + Random() % 4, 1 + Random() % 4, 4,
                        uint8_t(1 + Random() % 7), bool(Random() % 2)};
        if (Random() % 2) {
          E.addCopy(Write(A));
          E.addCopy(Write(B));
        }
        G.Nodes[N].Predecessors.push_back(std::move(E));
      }
    }
    if (Trial % 2) {
      const NodeIndex Destination = 1 + Random() % (G.Nodes.size() - 1);
      Edge Back = from(1 + Random() % (G.Nodes.size() - 1));
      if (Random() % 2)
        Back.Condition =
            Predicate{A, B, 4, uint8_t(1 + Random() % 7), bool(Random() % 2)};
      if (Random() % 2) {
        Back.addCopy(Write(A));
        Back.addCopy(Write(B));
      }
      G.Nodes[Destination].Predecessors.push_back(std::move(Back));
    }
    const Point Observation{G.Nodes.size() - 1, Random() % 2};
    const bool Proved = available(G, Observation);
    Proved ? ++Proven : ++Unproven;
    for (unsigned X = 0; X != 16; ++X)
      for (unsigned Y = 0; Y != 16; ++Y) {
        std::vector<State> Pending{{0, {0, X, Y, 0, 0}, {}}};
        std::set<State> Seen;
        while (!Pending.empty()) {
          State S = Pending.back();
          Pending.pop_back();
          if (!Seen.insert(S).second)
            continue;
          const auto &Writes = G.Nodes[S.Node].Assignments;
          const size_t Stop =
              S.Node == Observation.Node ? Observation.Before : Writes.size();
          for (size_t I = 0; I != Stop; ++I) {
            const State Before = S;
            Apply(S, Writes[I], Before);
          }
          if (S.Node == Observation.Node) {
            ASSERT_FALSE(Proved && !S.Known[Payload])
                << "trial=" << Trial << " x=" << X << " y=" << Y;
            // Later iterations can return with a different payload. Finish
            // this node and keep exploring after checking the observation.
            for (size_t I = Stop; I != Writes.size(); ++I) {
              const State Before = S;
              Apply(S, Writes[I], Before);
            }
          }
          for (NodeIndex Next = 0; Next != G.Nodes.size(); ++Next)
            for (const auto &E : G.Nodes[Next].Predecessors) {
              if (E.Predecessor != S.Node ||
                  (E.Condition && !Allows(S, *E.Condition)))
                continue;
              State Incoming = S;
              Incoming.Node = Next;
              for (const auto &[Destination, Copy] : E.Copies)
                Apply(Incoming, Copy, S);
              Pending.push_back(Incoming);
            }
        }
      }
  }
  EXPECT_GT(Proven, 0u);
  EXPECT_GT(Unproven, 0u);
}
