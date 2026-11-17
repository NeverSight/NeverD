//===- CValueAvailability.h - Availability on emitted paths -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete backward closure over the availability flags emitted alongside
/// scalar PHIs. Adapters describe their final control flow and local
/// identities; facts never refer to an earlier IR, a source address or a
/// rendered C string.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CVALUEAVAILABILITY_H
#define NEVERD_BACKEND_C_CVALUEAVAILABILITY_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::c_availability {

using Value = size_t;
using NodeIndex = size_t;

/// Possible outcomes of comparing two equal-width integer bit patterns.
enum Relation : uint8_t { Less = 1, Equal = 2, Greater = 4, Any = 7 };

struct Predicate {
  Value Left = 0;
  Value Right = 0;
  unsigned Bits = 0;
  uint8_t Allowed = Any;
  bool Signed = false;
};

enum class FlagWrite { None, Known, Copy, Unknown };

struct Assignment {
  Value Destination = 0;
  /// Exact, equal-width scalar identity for predicate substitution. Absence
  /// forgets facts about Destination. A narrowed payload can copy availability
  /// without copying the full-width predicate identity (CopyBits == 0).
  std::optional<Value> Source;
  unsigned CopyBits = 0;
  FlagWrite Availability = FlagWrite::None;
};

struct Edge {
  NodeIndex Predecessor = 0;
  /// PHIs read the predecessor's state simultaneously. These assignments
  /// happen after Condition was tested and before the successor starts.
  /// Index by destination: an observation needs only its payload and the
  /// current predicate operands, even when an edge has many other copies.
  llvm::DenseMap<Value, Assignment> Copies;
  void addCopy(Assignment A);
  std::optional<Predicate> Condition;
};

struct Node {
  llvm::SmallVector<Edge, 2> Predecessors;
  /// Assignments in execution order, including scalar writes which only
  /// invalidate predicates. Availability uses the renderer's flag contract:
  /// Known means that execution already published a known payload.
  llvm::SmallVector<Assignment, 1> Assignments;
  /// Every function/exception/address-taken entry must be marked. A node
  /// without predecessors which is not an entry has no emitted path into it.
  bool Entry = false;
  /// The adapter cannot describe flow or effects across this node.
  bool Barrier = false;
};

struct Graph {
  std::vector<Node> Nodes;
  /// Unresolved edges, unmodelled entry roles or unsupported structural flow
  /// invalidate the graph. Adapters validate these at construction time.
  bool Complete = true;
};

struct Point {
  NodeIndex Node = 0;
  /// The observation precedes this assignment (zero observes node entry).
  size_t Before = 0;
};

/// A proof concerns the current graph only. All predecessors must discharge;
/// an unknown entry or barrier leaves the guard intact. A state is revisited
/// unless an earlier context at the same point and payload includes all its
/// predicate outcomes. Contexts differing in one predicate share its union
/// only when that union is exact. This closes cycles and complementary
/// branches without merging distinct payloads or inventing feasible paths.
/// Structural distances prioritize complete paths to unknown roots; they
/// never discard an obligation or decide availability themselves.
/// No guessed value is produced.
bool availableBefore(const Graph &Flow, Point At, Value Payload);

} // namespace neverd::c_availability

#endif // NEVERD_BACKEND_C_CVALUEAVAILABILITY_H
