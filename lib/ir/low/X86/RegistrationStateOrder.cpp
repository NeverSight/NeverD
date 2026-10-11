//===- RegistrationStateOrder.cpp - PE32 state worklist order ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Visit ordinary predecessors before their successors within each CFG root.
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

namespace neverd::registration_state {

bool RegistrationStateSolver::initializeOrder() {
  if (!charge(Function.Blocks.size() + 1))
    return false;
  Order.resize(Function.Blocks.size());
  std::vector<bool> Seen(Function.Blocks.size());
  size_t Next = 0;
  auto Visit = [&](size_t Root) {
    if (Seen[Root])
      return true;
    std::vector<std::pair<size_t, size_t>> Stack{{Root, 0}};
    std::vector<size_t> Postorder;
    Seen[Root] = true;
    while (!Stack.empty()) {
      if (!charge(1))
        return false;
      auto &[Block, Successor] = Stack.back();
      const auto &Edges = Function.Blocks[Block].Succs;
      if (Successor == Edges.size()) {
        Postorder.push_back(Block);
        Stack.pop_back();
        continue;
      }
      const auto Target = Index.find(Edges[Successor++]);
      // Unreachable malformed edges must not become new analysis roots.
      // transferBlock diagnoses an absent successor if its source is reached.
      if (Target == Index.end() || Seen[Target->second])
        continue;
      Seen[Target->second] = true;
      Stack.emplace_back(Target->second, 0);
    }
    for (auto It = Postorder.rbegin(); It != Postorder.rend(); ++It)
      Order[*It] = Next++;
    return true;
  };
  // Keep the parent first. Independent callbacks and newly discovered resume
  // blocks then have a stable order, even when the LowIR block vector changes.
  // This controls scheduling only; dispatch and resume still prove every edge
  // and merge every reaching frame before a fixed point can be published.
  if (!Visit(Entries.at(Function.Entry)))
    return false;
  for (const auto &[Address, Index] : Entries)
    if (!Visit(Index))
      return false;
  return true;
}

} // namespace neverd::registration_state
