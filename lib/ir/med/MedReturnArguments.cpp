//===- MedReturnArguments.cpp - Incoming register returns ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedReturnArguments.h"

#include "neverd/ir/low/CallRegisterEffects.h"

#include <map>
#include <set>
#include <vector>

namespace neverd {

bool registerUntouchedFromEntry(const MedFunc &Med, const MedBlock &Block,
                                uint64_t Reg, uint16_t Width, Arch TheArch) {
  if (!Width)
    return false;
  auto Overlaps = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.Size > 0 && V.RegOff < Reg + Width &&
           Reg < V.RegOff + V.Size;
  };
  auto CallKeeps = [&](const MedOp &Op) {
    return Op.PreservesCallerSaved ||
           callPreservesRegisterBytes(TheArch, Op.CallPreservedGPRs, Reg,
                                      Width);
  };
  std::map<int, const MedBlock *> ById;
  for (const MedBlock &Candidate : Med.Blocks)
    ById.emplace(Candidate.Id, &Candidate);
  std::set<int> Seen{Block.Id};
  std::vector<const MedBlock *> Work{&Block};
  bool ReachedEntry = false;
  while (!Work.empty()) {
    const MedBlock &Current = *Work.back();
    Work.pop_back();
    ReachedEntry |= Current.isFunctionEntry(Med.Entry);
    if (!Current.ExceptionalPreds.empty())
      return false;
    for (const PhiNode &Phi : Current.Phis)
      if (Overlaps(Phi.Output))
        return false;
    for (const MedOp &Op : Current.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL
              ? !CallKeeps(Op)
              : (Overlaps(Op.Output) && !isIncomingRegisterCopy(Op) &&
                 !isRegisterLowView(Op)))
        return false;
    if (Current.Preds.empty() && !Current.isFunctionEntry(Med.Entry))
      return false;
    for (const int Pred : Current.Preds) {
      const auto It = ById.find(Pred);
      if (It == ById.end())
        return false;
      if (Seen.insert(Pred).second)
        Work.push_back(It->second);
    }
  }
  return ReachedEntry;
}

} // namespace neverd
