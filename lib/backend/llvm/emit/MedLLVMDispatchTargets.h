//===- MedLLVMDispatchTargets.h - Physical dispatch successors -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native and display switches choose the same address-owned successor copy.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_EMIT_MEDLLVMDISPATCHTARGETS_H
#define NEVERD_BACKEND_LLVM_EMIT_MEDLLVMDISPATCHTARGETS_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/BasicBlock.h"

#include <algorithm>
#include <map>
#include <vector>

namespace neverd {

inline std::vector<llvm::BasicBlock *>
dispatchTargetBlocks(const MedFunc &Function, const MedBlock &Block,
                     const std::vector<va_t> &Targets,
                     const std::map<int, llvm::BasicBlock *> &Blocks) {
  // x87 peeled/steady copies share an address but differ in stack TOP. The
  // dispatch's actual successor wins over a whole-function address match.
  std::map<va_t, int> Successors;
  for (const auto &Candidate : Function.Blocks)
    if (Candidate.StartAddr != InvalidVA &&
        std::find(Block.Succs.begin(), Block.Succs.end(), Candidate.Id) !=
            Block.Succs.end())
      Successors.emplace(Candidate.StartAddr, Candidate.Id);
  std::vector<llvm::BasicBlock *> Result;
  Result.reserve(Targets.size());
  for (va_t Address : Targets) {
    int Id = -1;
    if (auto It = Successors.find(Address); It != Successors.end())
      Id = It->second;
    else
      for (const auto &Candidate : Function.Blocks)
        // An optimized-away operation does not erase its block boundary.
        if (Candidate.StartAddr == Address) {
          Id = Candidate.Id;
          break;
        }
    const auto It = Blocks.find(Id);
    if (Id < 0 || It == Blocks.end())
      return {};
    Result.push_back(It->second);
  }
  return Result;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_EMIT_MEDLLVMDISPATCHTARGETS_H
