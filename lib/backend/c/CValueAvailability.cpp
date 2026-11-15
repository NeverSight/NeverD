//===- CValueAvailability.cpp - Availability on emitted paths -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared path policy for HighC locals and LLVM-C's edge-assigned PHIs.
//===----------------------------------------------------------------------===//

#include "CValueAvailability.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <utility>

namespace neverd::c_availability {
namespace {

uint8_t reversed(uint8_t Mask) {
  return ((Mask & Less) ? Greater : 0) | (Mask & Equal) |
         ((Mask & Greater) ? Less : 0);
}

struct Fact {
  Value Left;
  Value Right;
  unsigned Bits;
  uint8_t Unsigned = Any;
  uint8_t Signed = Any;

  bool operator==(const Fact &) const = default;

  /// Equality agrees under both orderings; all four unequal combinations
  /// are possible. Keep their correlation when joining two path contexts.
  unsigned relationPairs() const {
    unsigned Result = 0;
    for (unsigned U : {Less, Equal, Greater})
      for (unsigned S : {Less, Equal, Greater})
        if ((U == Equal) == (S == Equal) && (Unsigned & U) && (Signed & S))
          Result |= U << (S == Less ? 0 : S == Equal ? 3 : 6);
    return Result;
  }
};

struct Facts {
  llvm::SmallVector<Fact, 8> Items;
  bool Contradiction = false;

  bool operator==(const Facts &) const = default;

  /// Every state satisfying Other also satisfies these weaker constraints.
  /// Missing predicates are unconstrained; signed and unsigned masks must
  /// both include the corresponding outcomes, at the same integer width.
  bool covers(const Facts &Other) const {
    if (Items.size() > Other.Items.size())
      return false;
    auto It = Other.Items.begin();
    for (const Fact &F : Items) {
      const auto Key = std::tie(F.Left, F.Right, F.Bits);
      while (It != Other.Items.end() &&
             std::tie(It->Left, It->Right, It->Bits) < Key)
        ++It;
      if (It == Other.Items.end() ||
          std::tie(It->Left, It->Right, It->Bits) != Key ||
          (It->Unsigned & ~F.Unsigned) || (It->Signed & ~F.Signed))
        return false;
    }
    return true;
  }

  /// Factor (Common && A) || (Common && B) only when A || B is exactly
  /// representable by one predicate. Joining unrelated facts independently
  /// would invent combinations of paths which neither input admitted.
  bool unite(const Facts &Other) {
    if (Items.size() != Other.Items.size())
      return false;
    size_t Different = Items.size();
    Fact Joined{};
    for (size_t I = 0; I != Items.size(); ++I) {
      const Fact &L = Items[I], &R = Other.Items[I];
      if (L == R)
        continue;
      if (Different != Items.size() || std::tie(L.Left, L.Right, L.Bits) !=
                                           std::tie(R.Left, R.Right, R.Bits))
        return false;
      Different = I;
      Joined = L;
      Joined.Unsigned |= R.Unsigned;
      Joined.Signed |= R.Signed;
      if (Joined.relationPairs() != (L.relationPairs() | R.relationPairs()))
        return false;
    }
    if (Different == Items.size())
      return true;
    if (Joined.Unsigned == Any && Joined.Signed == Any)
      Items.erase(Items.begin() + Different);
    else
      Items[Different] = Joined;
    return true;
  }

  void intersect(Fact Incoming) {
    if (Incoming.Unsigned == Any && Incoming.Signed == Any)
      return;
    if (Incoming.Left == Incoming.Right) {
      Contradiction |=
          !(Incoming.Unsigned & Equal) || !(Incoming.Signed & Equal);
      return;
    }
    if (Incoming.Right < Incoming.Left) {
      std::swap(Incoming.Left, Incoming.Right);
      Incoming.Unsigned = reversed(Incoming.Unsigned);
      Incoming.Signed = reversed(Incoming.Signed);
    }
    auto It =
        llvm::lower_bound(Items, Incoming, [](const Fact &L, const Fact &R) {
          return std::tie(L.Left, L.Right, L.Bits) <
                 std::tie(R.Left, R.Right, R.Bits);
        });
    if (It == Items.end() || It->Left != Incoming.Left ||
        It->Right != Incoming.Right || It->Bits != Incoming.Bits) {
      It = Items.insert(It, Incoming);
    } else {
      It->Unsigned &= Incoming.Unsigned;
      It->Signed &= Incoming.Signed;
    }
    // Equality has the same meaning under either integer ordering. No other
    // signed/unsigned ordering fact is shared.
    if (!(It->Unsigned & Equal) || !(It->Signed & Equal)) {
      It->Unsigned &= ~Equal;
      It->Signed &= ~Equal;
    }
    if (It->Unsigned == Equal || It->Signed == Equal) {
      It->Unsigned &= Equal;
      It->Signed &= Equal;
    }
    Contradiction |= !It->Unsigned || !It->Signed;
  }

  void add(const Predicate &P) {
    assert(P.Bits && !(P.Allowed & ~Any));
    Fact F{P.Left, P.Right, P.Bits};
    (P.Signed ? F.Signed : F.Unsigned) = P.Allowed;
    intersect(F);
  }

  /// Rewrite all operands against the old state before publishing any of
  /// the replacements. Sequential substitution would mishandle PHI swaps.
  template <typename Lookup> void backward(const Lookup &FindWrite) {
    auto Before = std::move(Items);
    Items.clear();
    for (Fact F : Before) {
      bool Keep = true;
      auto Substitute = [&](Value &Operand) {
        const Assignment *Write = FindWrite(Operand);
        if (!Write)
          return;
        if (!Write->Source || Write->CopyBits != F.Bits) {
          Keep = false;
          return;
        }
        Operand = *Write->Source;
      };
      Substitute(F.Left);
      Substitute(F.Right);
      if (Keep)
        intersect(F);
    }
  }
};

struct Location {
  Point At;
  /// No value means the last write was Unknown. Earlier writes cannot
  /// restore it, but earlier branch conditions may still refute the path.
  std::optional<Value> Payload;
};

struct State : Location {
  Facts Conditions;
};

struct LocationInfo {
  static Location getEmptyKey() {
    return {{std::numeric_limits<NodeIndex>::max(), 0}, std::nullopt};
  }
  static Location getTombstoneKey() {
    return {{std::numeric_limits<NodeIndex>::max() - 1, 0}, std::nullopt};
  }
  static unsigned getHashValue(const Location &S) {
    return static_cast<unsigned>(llvm::hash_combine(
        S.At.Node, S.At.Before, S.Payload.has_value(), S.Payload.value_or(0)));
  }
  static bool isEqual(const Location &L, const Location &R) {
    return L.At.Node == R.At.Node && L.At.Before == R.At.Before &&
           L.Payload == R.Payload;
  }
};

/// The structural pass already follows every payload dependency. Retain its
/// reverse edges so the relational pass can prefer paths nearing an unknown
/// root, instead of enumerating unrelated combinations near the observation.
struct StructuralPaths {
  struct Reached {
    llvm::SmallVector<Location, 2> Successors;
    size_t Distance = std::numeric_limits<size_t>::max();
  };
  llvm::DenseMap<Location, Reached, LocationInfo> Locations;
  llvm::SmallVector<Location> Roots;

  void computeDistances() {
    for (const Location &Root : Roots)
      Locations.find(Root)->second.Distance = 0;
    for (size_t I = 0; I != Roots.size(); ++I) {
      const auto &Current = Locations.find(Roots[I])->second;
      for (const Location &Next : Current.Successors) {
        auto &Successor = Locations.find(Next)->second;
        if (Successor.Distance != std::numeric_limits<size_t>::max())
          continue;
        Successor.Distance = Current.Distance + 1;
        Roots.push_back(Next);
      }
    }
  }
};

bool transport(std::optional<Value> &Payload, const Assignment *Write) {
  if (!Payload || !Write)
    return false;
  switch (Write->Availability) {
  case FlagWrite::Known:
    return true;
  case FlagWrite::Copy:
    assert(Write->Source);
    Payload = Write->Source;
    break;
  case FlagWrite::Unknown:
    Payload.reset();
    break;
  case FlagWrite::None:
    break;
  }
  return false;
}

} // namespace

void Edge::addCopy(Assignment A) {
  [[maybe_unused]] const bool Inserted =
      Copies.try_emplace(A.Destination, std::move(A)).second;
  assert(Inserted && "parallel assignments have distinct destinations");
}

static std::optional<bool> analyzePaths(const Graph &Flow, Point At,
                                        Value Payload, StructuralPaths &Paths,
                                        bool FirstContextOnly) {
  struct Work {
    size_t Rank, Order, Depth;
    State Current;
  };
  auto Later = [](const Work &L, const Work &R) {
    return std::tie(L.Rank, L.Order) > std::tie(R.Rank, R.Order);
  };
  std::vector<Work> Pending;
  size_t Depth = 0, Order = 0;
  llvm::DenseMap<Location, llvm::SmallVector<Facts, 1>, LocationInfo> Seen;
  // Publish the canonical union when enqueuing, before either alternative
  // expands. Otherwise superseded narrow contexts can spawn exponentially
  // many pending descendants even though Seen eventually coalesces them.
  auto Enqueue = [&](State S, std::optional<Location> Successor) {
    if (FirstContextOnly) {
      auto [It, Inserted] = Paths.Locations.try_emplace(S);
      if (Successor)
        It->second.Successors.push_back(*Successor);
      if (!Inserted)
        return;
    } else {
      auto &Contexts = Seen[S];
      for (size_t I = 0; I != Contexts.size();) {
        if (Contexts[I].covers(S.Conditions))
          return;
        if (S.Conditions.covers(Contexts[I])) {
          Contexts.erase(Contexts.begin() + I);
          continue;
        }
        if (S.Conditions.unite(Contexts[I])) {
          Contexts.erase(Contexts.begin() + I);
          // This exact union can now cover or join an earlier context.
          I = 0;
          continue;
        }
        ++I;
      }
      Contexts.push_back(S.Conditions);
    }
    size_t Rank = Depth;
    if (!FirstContextOnly) {
      const size_t Distance = Paths.Locations.find(S)->second.Distance;
      Rank = Distance == std::numeric_limits<size_t>::max() ? Distance
                                                            : Depth + Distance;
    }
    Pending.push_back({Rank, Order++, Depth, std::move(S)});
    std::push_heap(Pending.begin(), Pending.end(), Later);
  };
  Enqueue({{At, Payload}, {}}, std::nullopt);
  bool RefutedEntry = false;
  // At the same observation point and transported payload, a weaker context
  // already explores every predecessor path of a stronger one. Keep only
  // incomparable contexts: copies, killed facts and edge conjunctions all
  // preserve this inclusion. In particular, a loop adding independent tests
  // cannot multiply the initial unconstrained state into every fact subset.
  // Identities, widths and masks still come from the finite graph; neither a
  // depth nor a work cutoff decides whether the closure is complete.
  // The first pass is breadth-first. The complete pass uses traversed depth
  // plus the shortest structural distance to an unknown root. FIFO ties let
  // equal-length complementary branches coalesce before expanding a join.
  // Distance only orders work: even rootless dependencies remain queued,
  // and positive proofs must discharge the entire closure.
  while (!Pending.empty()) {
    std::pop_heap(Pending.begin(), Pending.end(), Later);
    Depth = Pending.back().Depth + 1;
    State S = std::move(Pending.back().Current);
    Pending.pop_back();
    if (!FirstContextOnly &&
        !llvm::is_contained(Seen.find(S)->second, S.Conditions))
      continue; // A queued exact union now owns this narrower obligation.
    const Node &N = Flow.Nodes[S.At.Node];
    const Location Current = S;
    assert(S.At.Before <= N.Assignments.size());
    bool Known = false;
    for (size_t I = S.At.Before; I-- > 0;) {
      const auto &Write = N.Assignments[I];
      const auto FindWrite = [&](Value V) -> const Assignment * {
        return V == Write.Destination ? &Write : nullptr;
      };
      if (transport(S.Payload, S.Payload ? FindWrite(*S.Payload) : nullptr)) {
        Known = true;
        break;
      }
      S.Conditions.backward(FindWrite);
      if (S.Conditions.Contradiction && !FirstContextOnly)
        break;
    }
    if (Known || (S.Conditions.Contradiction && !FirstContextOnly))
      continue;
    if (N.Entry || N.Barrier) {
      if (!S.Conditions.Contradiction)
        return false;
      // This first structural path is refuted, but another context merged
      // at an earlier location may reach the same unknown entry. Only the
      // complete relational traversal can discharge that obligation.
      RefutedEntry = true;
      Paths.Roots.push_back(Current);
      continue;
    }
    for (const Edge &E : N.Predecessors) {
      // One lookup for the payload and two per predicate. The index was
      // built once by the adapter; unrelated PHIs are never scanned here.
      assert(E.Predecessor < Flow.Nodes.size());
      State Incoming = S;
      const auto FindWrite = [&](Value V) -> const Assignment * {
        const auto At = E.Copies.find(V);
        return At == E.Copies.end() ? nullptr : &At->second;
      };
      if (transport(Incoming.Payload,
                    Incoming.Payload ? FindWrite(*Incoming.Payload) : nullptr))
        continue;
      Incoming.Conditions.backward(FindWrite);
      if (E.Condition)
        Incoming.Conditions.add(*E.Condition);
      if (Incoming.Conditions.Contradiction && !FirstContextOnly)
        continue;
      Incoming.At = {E.Predecessor,
                     Flow.Nodes[E.Predecessor].Assignments.size()};
      Enqueue(std::move(Incoming), Current);
    }
  }
  return RefutedEntry ? std::nullopt : std::optional<bool>(true);
}

bool availableBefore(const Graph &Flow, Point At, Value Payload) {
  if (!Flow.Complete)
    return false;
  assert(At.Node < Flow.Nodes.size());
  // First traverse each structural location/payload once, carrying its first
  // path's facts without pruning contradictions. No unknown root proves
  // availability independent of predicates; an unrefuted unknown root is a
  // witness for retaining the guard. Refuted roots need every context, so
  // retry those queries with the complete relational closure. Neither pass
  // has a depth, state or work cutoff.
  StructuralPaths Paths;
  if (auto Result = analyzePaths(Flow, At, Payload, Paths, true))
    return *Result;
  Paths.computeDistances();
  return *analyzePaths(Flow, At, Payload, Paths, false);
}

} // namespace neverd::c_availability
