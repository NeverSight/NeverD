//===- RegistrationSEHParentRoots.cpp - Resumed parent frame definitions -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps callback-private frame definitions inside their checked invocations.

#include "RegistrationRoots.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"

#include <set>

namespace neverd::x86_registration {

void RegistrationRoots::seedSEHParentFrames(MedFunc &Func) const {
  if (!HasSEHFrame || !Direct || Func.Entry != Low.Entry ||
      Low.Entry != Low.ExceptionMetadata->CodeRange.Begin)
    return;
  const auto Exits = getRegistrationSEHParentExits(Func);
  if (!Exits || Exits->empty())
    return;

  size_t Work = 0;
  const auto Charge = [&] {
    return ++Work <= limits::kMaxRegistrationEHStateWork;
  };
  std::map<int, MedBlock *> Blocks;
  std::map<va_t, const MedBlock *> Entries;
  std::map<int, std::set<int>> Predecessors;
  for (auto &Block : Func.Blocks) {
    if (!Charge() || !Blocks.emplace(Block.Id, &Block).second ||
        !Entries.emplace(Block.StartAddr, &Block).second)
      return;
    for (int Next : Block.Succs)
      if (!Charge() || !Predecessors[Next].insert(Block.Id).second)
        return;
  }
  std::map<int, MedVar> Seeds;
  for (const auto &[Id, Exit] : *Exits) {
    if (!Charge())
      return;
    const auto Target = Blocks.find(Exit.TargetBlock);
    const auto Entry = Entries.find(Exit.EntryVA);
    if (Target == Blocks.end() || Entry == Entries.end())
      return;
    const auto &Block = *Target->second;
    if (Block.StartAddr == Func.Entry ||
        Low.OrdinaryModuleAnalysisRoots.count(Block.StartAddr) ||
        !Block.ExceptionalPreds.empty())
      continue;
    const auto &Preds = Predecessors[Block.Id];
    if (Preds.empty() || Preds.size() != Block.Preds.size() ||
        std::set<int>(Block.Preds.begin(), Block.Preds.end()) != Preds)
      continue;
    bool Exclusive = true;
    for (int Pred : Preds) {
      if (!Charge())
        return;
      const auto Bound = Exits->find(Pred);
      Exclusive &=
          Bound != Exits->end() && Bound->second.TargetBlock == Block.Id;
    }
    if (!Exclusive)
      continue;

    // The exact callback's runtime root already names the established frame.
    // Reuse its register identity, not its SSA value or private storage. Other
    // registers retain the values produced by the callback at this transfer.
    const MedOp *Frame = nullptr;
    for (const auto &Op : Entry->second->Ops) {
      if (!Charge())
        return;
      if (Op.RegistrationRoot !=
          MedOp::RegistrationRootKind::EstablishedFramePointer)
        continue;
      if (Frame || !hasValidRegistrationRootShape(Op) ||
          Op.Addr != Entry->second->StartAddr || Op.Output.Id < 0 ||
          Op.Output.SSAVer != 0)
        return;
      Frame = &Op;
    }
    if (!Frame)
      return;
    const auto [It, New] = Seeds.emplace(Block.Id, Frame->Output);
    if (!New && It->second != Frame->Output)
      return;
  }
  for (const auto &[Id, Value] : Seeds) {
    auto &Block = *Blocks.at(Id);
    MedOp Seed;
    Seed.Opcode = NdOp::COPY;
    Seed.Addr = Block.StartAddr;
    Seed.Output = Value;
    Seed.addInput(Value);
    Seed.RegistrationRoot =
        MedOp::RegistrationRootKind::EstablishedFramePointer;
    Block.Ops.insert(Block.Ops.begin(), std::move(Seed));
  }
}

} // namespace neverd::x86_registration
