//===- SEHSourceRoots.cpp - Conservative source entry state -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps independent source entries from borrowing the original invocation.

#include "SEHSourceRoots.h"

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::x86_seh {

void seedSourceEntries(MedFunc &Func, const LowFunc &Low,
                       const std::vector<std::set<int>> &LiveIn,
                       const std::map<int, MedVar> &Variables,
                       Arch TargetArch) {
  // Havoc live registers even on the ordinary path to an independently
  // addressable entry. This is a conservative source approximation, not a
  // memory-preservation or native entry contract.
  for (size_t B = 1; B < Func.Blocks.size(); ++B) {
    const va_t Start = Func.Blocks[B].StartAddr;
    // A local unwind's target only its exits enter is entered by ordinary
    // flow alone.
    if (!Low.OrdinaryModuleAnalysisRoots.count(Start) ||
        llvm::any_of(Func.SEHLocalUnwindExits, [&](const auto &Exit) {
          return Exit.OwnsTarget && Exit.Target == Start;
        }))
      continue;
    std::vector<MedOp> Seeds;
    for (int Id : LiveIn[B]) {
      auto V = Variables.find(Id);
      if (V == Variables.end())
        continue;
      MedOp Seed;
      Seed.Opcode = NdOp::COPY;
      Seed.Output = V->second;
      Seed.addInput(MedVar::makeUnspecified(V->second.Size, TargetArch));
      // This is an abstract entry definition, not a decoded SP write.
      Seed.Addr = InvalidVA;
      Seeds.push_back(Seed);
    }
    auto &Ops = Func.Blocks[B].Ops;
    Ops.insert(Ops.begin(), Seeds.begin(), Seeds.end());
  }
}

void mergeSourceHandlerStack(MedBlock &Block, const MedVar &Stack,
                             const std::vector<int> &Predecessors,
                             int VirtualRoot, Arch TargetArch) {
  PhiNode Phi;
  Phi.Output = Stack;
  for (int P : Predecessors)
    if (P != VirtualRoot)
      Phi.Args.push_back({P, Stack});
  Phi.ExceptionalEntry = MedVar::makeUnspecified(Stack.Size, TargetArch);
  Block.Phis.push_back(std::move(Phi));
}

} // namespace neverd::x86_seh
