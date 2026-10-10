//===- AffineFrameState.h - Anchored affine frame equations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_AFFINEFRAMESTATE_H
#define NEVERD_IR_AFFINEFRAMESTATE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace neverd::detail {

/// Signed frame-coordinate arithmetic never wraps, even if a later transfer
/// would cancel the overflowing intermediate offset.
inline std::optional<int64_t> checkedAffineOffset(int64_t Base, int64_t Delta,
                                                  bool Subtract = false) {
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();
  if (!Subtract) {
    if ((Delta > 0 && Base > Max - Delta) || (Delta < 0 && Base < Min - Delta))
      return std::nullopt;
    return Base + Delta;
  }
  if ((Delta > 0 && Base < Min + Delta) || (Delta < 0 && Base > Max + Delta))
    return std::nullopt;
  return Base - Delta;
}

/// Must-equality equations for offsets from the incoming stack pointer.
/// A node is equal to every incoming value, optionally after a checked offset.
/// References allow cyclic definitions without recursively re-expanding them.
/// Each state changes at most twice: Unknown -> Value -> Invalid. Unanchored
/// cycles are invalidated after propagation, including their downstream users.
class AffineFrameState {
public:
  enum class Kind : uint8_t { Invalid, Value, Reference };
  struct Result {
    Kind K = Kind::Invalid;
    int64_t Offset = 0;
    size_t Node = 0;
  };

  explicit AffineFrameState(llvm::function_ref<bool(size_t)> Consume)
      : Consume(Consume) {}

  static Result invalid() { return {}; }
  static Result constant(int64_t Offset) { return {Kind::Value, Offset}; }

  Result reference() {
    // Node storage, initialization and eventual destruction, including the
    // inlined reverse-edge vector. Its heap growth is charged per edge below.
    if (!Consume(16))
      return invalid();
    const size_t Index = Nodes.size();
    Nodes.emplace_back();
    return {Kind::Reference, 0, Index};
  }

  bool define(Result Target, Result Input) {
    return defineInputs(Target, {Input});
  }

  Result merge(llvm::ArrayRef<Result> Inputs) {
    if (!Consume(Inputs.size()) || Inputs.empty())
      return invalid();
    bool HasReference = false;
    std::optional<int64_t> Common;
    for (const Result &Input : Inputs) {
      if (Input.K == Kind::Invalid)
        return invalid();
      if (Input.K == Kind::Reference) {
        HasReference = true;
      } else if (Common && *Common != Input.Offset) {
        return invalid();
      } else {
        Common = Input.Offset;
      }
    }
    if (!HasReference)
      return constant(*Common);
    if (Inputs.size() == 1)
      return Inputs.front();
    const Result Target = reference();
    return defineInputs(Target, Inputs) ? Target : invalid();
  }

  Result adjust(Result Input, int64_t Delta, bool Subtract) {
    if (Input.K == Kind::Invalid)
      return invalid();
    if (Input.K == Kind::Value) {
      const auto Offset = checkedAffineOffset(Input.Offset, Delta, Subtract);
      return Offset ? constant(*Offset) : invalid();
    }
    // Retain each arithmetic step: combining deltas could hide an overflowing
    // intermediate frame epoch, even if the final sum fits.
    const Result Target = reference();
    return defineInputs(Target, {Input}, Delta, Subtract) ? Target : invalid();
  }

  Result value(Result Input) {
    if (Input.K != Kind::Reference)
      return Input;
    if (Input.Node >= SolvedCount && !solve())
      return invalid();
    const State &S = Nodes[Input.Node].Current;
    return S.K == StateKind::Value ? constant(S.Offset) : invalid();
  }

  void clear() {
    Nodes.clear();
    Pending.clear();
    SolvedCount = 0;
  }

private:
  enum class StateKind : uint8_t { Unknown, Value, Invalid };
  struct State {
    StateKind K = StateKind::Unknown;
    int64_t Offset = 0;
  };
  struct User {
    size_t Node;
    int64_t Delta;
    bool Subtract;
  };
  struct Node {
    llvm::SmallVector<User, 2> Users;
    State Initial, Current;
    bool Defined = false;
    bool Queued = false;
  };
  struct PendingEdge {
    size_t Source;
    User Target;
  };

  static bool meet(State &Target, State Input) {
    if (Input.K == StateKind::Unknown || Target.K == StateKind::Invalid)
      return false;
    if (Target.K == StateKind::Unknown) {
      Target = Input;
      return true;
    }
    if (Input.K == StateKind::Invalid || Target.Offset != Input.Offset) {
      Target = {StateKind::Invalid};
      return true;
    }
    return false;
  }

  bool defineInputs(Result Target, llvm::ArrayRef<Result> Inputs,
                    int64_t Delta = 0, bool Subtract = false) {
    if (Target.K != Kind::Reference)
      return false;
    assert(Target.Node < Nodes.size() && !Nodes[Target.Node].Defined);
    Nodes[Target.Node].Defined = true;
    if (Target.Node < SolvedCount) {
      // A previously queried placeholder acquired its definition. Rebuild
      // the old users too; a normal completed graph grows only downstream.
      SolvedCount = 0;
      Pending.clear();
    }
    for (Result Input : Inputs) {
      if (!Consume(12))
        return false;
      if (Input.K == Kind::Reference) {
        const User U{Target.Node, Delta, Subtract};
        Nodes[Input.Node].Users.push_back(U);
        if (Input.Node < SolvedCount)
          Pending.push_back({Input.Node, U});
      } else {
        const auto Offset =
            Input.K == Kind::Value
                ? checkedAffineOffset(Input.Offset, Delta, Subtract)
                : std::nullopt;
        meet(Nodes[Target.Node].Initial, Offset
                                             ? State{StateKind::Value, *Offset}
                                             : State{StateKind::Invalid});
      }
    }
    return true;
  }

  bool solve() {
    const size_t Count = Nodes.size() - SolvedCount;
    if (Count > std::numeric_limits<size_t>::max() / 8 || !Consume(Count * 8))
      return false;
    std::vector<size_t> Queue;
    Queue.reserve(Count * 2);
    const auto enqueue = [&](size_t Index) {
      if (!Nodes[Index].Queued) {
        Nodes[Index].Queued = true;
        Queue.push_back(Index);
      }
    };
    for (size_t I = SolvedCount; I < Nodes.size(); ++I) {
      Node &N = Nodes[I];
      N.Queued = false;
      N.Current = N.Initial;
      if (N.Current.K != StateKind::Unknown)
        enqueue(I);
    }
    const auto propagateEdge = [&](State Source, const User &U) {
      if (!Consume(4))
        return false;
      const auto Offset =
          Source.K == StateKind::Value
              ? checkedAffineOffset(Source.Offset, U.Delta, U.Subtract)
              : std::nullopt;
      const State Incoming =
          Offset ? State{StateKind::Value, *Offset} : State{StateKind::Invalid};
      if (meet(Nodes[U.Node].Current, Incoming))
        enqueue(U.Node);
      return true;
    };
    // Existing equations are immutable. New queries may depend on their
    // results but cannot change them; replay only newly added outgoing edges.
    for (const PendingEdge &P : Pending)
      if (!propagateEdge(Nodes[P.Source].Current, P.Target))
        return false;
    size_t Next = 0;
    const auto propagate = [&] {
      while (Next < Queue.size()) {
        const size_t Index = Queue[Next++];
        Node &N = Nodes[Index];
        N.Queued = false;
        const State Source = N.Current;
        for (const User &U : N.Users)
          if (!propagateEdge(Source, U))
            return false;
      }
      return true;
    };
    if (!propagate())
      return false;
    // A value from another predecessor cannot authenticate an unanchored
    // incoming cycle. Make these unknown inputs explicit and propagate them.
    for (size_t I = SolvedCount; I < Nodes.size(); ++I)
      if (Nodes[I].Current.K == StateKind::Unknown) {
        Nodes[I].Current = {StateKind::Invalid};
        enqueue(I);
      }
    if (!propagate())
      return false;
    Pending.clear();
    SolvedCount = Nodes.size();
    return true;
  }

  llvm::function_ref<bool(size_t)> Consume;
  std::vector<Node> Nodes;
  std::vector<PendingEdge> Pending;
  size_t SolvedCount = 0;
};

} // namespace neverd::detail

#endif
