//===- MedCFGSimplify.cpp - CFG simplification for MedIR ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Removes trivial jump-only basic blocks from the MedIR CFG and
/// renumbers the remaining blocks.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace neverd {

std::vector<va_t> codePointerTargetsOf(const BinaryImage &Img) {
  std::vector<va_t> Targets;
  const unsigned PointerSize = Img.getPointerSize();
  if (PointerSize != 0)
    for (va_t Slot : Img.CodePtrRelocSlots)
      if (const uint8_t *Bytes = Img.readVA(Slot, PointerSize))
        Targets.push_back(normalizeCodeAddress(
            static_cast<va_t>(
                readPtr(Bytes, Img.is64Bit(), archLittleEndian(Img.Arch))),
            Img.Arch, Img.Mode));
  llvm::sort(Targets);
  Targets.erase(std::unique(Targets.begin(), Targets.end()), Targets.end());
  return Targets;
}

void LowToMedConverter::simplifyCfg(MedFunc &Func) {
  // Whether code the image takes the address of starts at \p Address: a
  // published code reference, or the value of a relocated code pointer.  An
  // image holds tens of thousands of each, and most functions have no
  // jump-only block to ask about, so the pointers are read only once one
  // asks, once for the converter or once for the run that shares them.
  auto AddressTaken = [&](va_t Address) {
    if (!Image)
      return false;
    // normalizeCodeAddress clears at most the Thumb bit, so only these two
    // references can normalize to Address.
    for (auto Target = Image->CodeRefTargets.lower_bound(Address);
         Target != Image->CodeRefTargets.end() && *Target - Address <= 1;
         ++Target)
      if (normalizeCodeAddress(*Target, Image->Arch, Image->Mode) == Address)
        return true;
    if (!SharedCodePointerTargets && !CodePointerTargets)
      CodePointerTargets = codePointerTargetsOf(*Image);
    const std::vector<va_t> &PointerTargets = SharedCodePointerTargets
                                                  ? *SharedCodePointerTargets
                                                  : *CodePointerTargets;
    return std::binary_search(PointerTargets.begin(), PointerTargets.end(),
                              Address);
  };

  // Native registration reconstruction replays exact source intervals and
  // their ordinary edges. Retain every reached interval until that replay;
  // jump threading cannot silently discard its source occurrence receipt.
  // SEH replay includes the original normal edge after an ordinary finally
  // call, even when that invocation always leaves through longjmp. Threading
  // its dead continuation would also renumber the remaining source identities.
  const bool HasSEHSource = Func.ExceptionMetadata &&
                            Func.ExceptionMetadata->Registration &&
                            (Func.ExceptionMetadata->Personality ==
                                 ExceptionPersonality::ExceptHandler3 ||
                             Func.ExceptionMetadata->Personality ==
                                 ExceptionPersonality::ExceptHandler4);
  std::set<std::pair<va_t, va_t>> RegistrationIntervals;
  if (Func.RegistrationStates)
    for (const auto &State : Func.RegistrationStates->Blocks)
      if (State.Reached || HasSEHSource)
        RegistrationIntervals.emplace(State.Range.Begin, State.Range.End);

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (int I = 0; I < static_cast<int>(Func.Blocks.size()); ++I) {
      auto &Blk = Func.Blocks[I];
      if (RegistrationIntervals.count({Blk.StartAddr, Blk.EndAddr}))
        continue;
      if (Blk.Succs.size() != 1)
        continue;
      // Exception edges name the exact block where native unwinding transfers
      // control.  Removing either endpoint without rewriting that independent
      // graph can make an invoke skip the landing-pad prologue, so retain such
      // blocks.  The final renumbering below still remaps all retained edges.
      if (!Blk.ExceptionalPreds.empty() || !Blk.ExceptionalSuccs.empty())
        continue;

      bool IsTrivial = true;
      for (auto &Op : Blk.Ops) {
        if (Op.Opcode != NdOp::BRANCH && Op.Opcode != NdOp::NOP) {
          IsTrivial = false;
          break;
        }
      }
      if (!IsTrivial)
        continue;
      if (Blk.Id == 0)
        continue;

      // A relocation-backed code value names this exact native label, not
      // merely its eventual CFG successor.  Removing the jump-only block would
      // leave the pointer table targeting an address with no LLVM BlockAddress
      // owner.  Direct branches and JumpTable metadata can be rewritten below;
      // an externally stored code identity cannot, so retain its block.
      const va_t BlockAddress = Blk.StartAddr != 0 || Blk.Ops.empty()
                                    ? Blk.StartAddr
                                    : Blk.Ops.front().Addr;
      if (AddressTaken(normalizeCodeAddress(BlockAddress, TargetArch,
                                            Image ? Image->Mode
                                                  : InstructionMode::Default)))
        continue;

      int Target = Blk.Succs[0];
      if (Target == Blk.Id)
        continue;
      if (Target < 0 || Target >= static_cast<int>(Func.Blocks.size()))
        continue;

      std::vector<int> PredsCopy = Blk.Preds;

      for (int PredId : PredsCopy) {
        if (PredId < 0 || PredId >= static_cast<int>(Func.Blocks.size()))
          continue;
        auto &Pred = Func.Blocks[PredId];
        for (auto &S : Pred.Succs) {
          if (S == Blk.Id)
            S = Target;
        }
        Func.Blocks[Target].Preds.push_back(PredId);

        for (auto &Op : Pred.Ops) {
          if ((Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) &&
              Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
            if (!Func.Blocks[Target].Ops.empty()) {
              va_t TrivialAddr = Blk.Ops.empty() ? 0 : Blk.Ops.front().Addr;
              if (Op.Inputs[0].ConstVal == TrivialAddr) {
                Op.Inputs[0] = MedVar::makeConst(
                    Func.Blocks[Target].Ops.front().Addr, Op.Inputs[0].Size);
              }
            }
          }
        }
      }

      auto &TPreds = Func.Blocks[Target].Preds;
      TPreds.erase(std::remove(TPreds.begin(), TPreds.end(), Blk.Id),
                   TPreds.end());

      // A jump-table dispatch reaches this trivial block through INDIR_BR,
      // whose case targets are carried as JumpTable metadata rather than a
      // constant branch operand, so the predecessor-operand rewrite above
      // misses them. Redirect those targets too — otherwise the emitter cannot
      // map the removed block's address back to a switch case (it would drop
      // the whole switch).
      if (!Blk.Ops.empty() && !Func.Blocks[Target].Ops.empty()) {
        va_t TrivialAddr = Blk.Ops.front().Addr;
        va_t NewAddr = Func.Blocks[Target].Ops.front().Addr;
        if (TrivialAddr != NewAddr)
          for (auto &JT : Func.JumpTables)
            for (auto &Tgt : JT.Targets)
              if (Tgt == TrivialAddr)
                Tgt = NewAddr;
      }

      Blk.Ops.clear();
      Blk.Succs.clear();
      Blk.Preds.clear();
      Changed = true;
    }
  }

  // Remove empty blocks and renumber
  std::vector<MedBlock> NewBlocks;
  std::map<int, int> OldToNew;
  for (auto &Blk : Func.Blocks) {
    if (Blk.Ops.empty() && Blk.Phis.empty() && Blk.Id != 0 &&
        !RegistrationIntervals.count({Blk.StartAddr, Blk.EndAddr}))
      continue;
    int NewId = static_cast<int>(NewBlocks.size());
    OldToNew[Blk.Id] = NewId;
    Blk.Id = NewId;
    NewBlocks.push_back(std::move(Blk));
  }
  for (auto &Blk : NewBlocks) {
    std::vector<int> RemappedSuccs;
    RemappedSuccs.reserve(Blk.Succs.size());
    for (int S : Blk.Succs) {
      auto It = OldToNew.find(S);
      if (It != OldToNew.end())
        RemappedSuccs.push_back(It->second);
    }
    Blk.Succs = std::move(RemappedSuccs);
    Blk.Preds.clear();
    Blk.ExceptionalPreds.clear();
    for (ExceptionalEdge &Edge : Blk.ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        continue;
      auto It = OldToNew.find(Edge.BlockId);
      Edge.BlockId = It != OldToNew.end() ? It->second : -1;
    }
  }
  // Jump threading can merge the taken and fallthrough edges of a conditional.
  // Canonicalize it before SSA: downstream structuring must not infer a false
  // edge from physical block order when both outcomes have the same target.
  // Keep condition-producing operations, including their observable effects.
  for (auto &Blk : NewBlocks) {
    if (Blk.Ops.empty() || Blk.Succs.empty() || Blk.Succs.size() > 2 ||
        (Blk.Succs.size() == 2 && Blk.Succs[0] != Blk.Succs[1]))
      continue;
    auto &Terminator = Blk.Ops.back();
    if (Terminator.Opcode != NdOp::COND_BR || Terminator.NumInputs != 2 ||
        !Terminator.Inputs[0].isConst())
      continue;
    const auto &Target = NewBlocks[Blk.Succs.front()];
    const va_t Address = Terminator.Inputs[0].ConstVal;
    if (Target.StartAddr != Address &&
        (Target.Ops.empty() || Target.Ops.front().Addr != Address))
      continue;
    Terminator.Opcode = NdOp::BRANCH;
    Terminator.NumInputs = 1;
    Terminator.Inputs[1] = {};
    Blk.Succs.resize(1);
  }
  // Preds are derived data. Rebuild them from the retained successor edges so
  // removing an empty block cannot leave a stale predecessor ID or make the
  // two edge directions disagree.
  for (const auto &Blk : NewBlocks)
    for (int S : Blk.Succs)
      NewBlocks[S].Preds.push_back(Blk.Id);
  // Exceptional predecessors are likewise derived from the successor graph.
  // Rebuild them after renumbering so SSA recognizes every landing pad as an
  // independent root and the LLVM emitter targets the prologue block itself.
  for (const auto &Blk : NewBlocks) {
    // Renumbering gives each source a unique ID. Only edges from this source
    // can duplicate one of its predecessors, so do not scan every earlier
    // source already entering a shared cleanup or landing pad.
    std::set<ExceptionalEdge> Seen;
    for (const ExceptionalEdge &Edge : Blk.ExceptionalSuccs)
      if (Edge.BlockId >= 0 &&
          Edge.BlockId < static_cast<int>(NewBlocks.size()) &&
          Seen.insert(Edge).second) {
        ExceptionalEdge Pred = Edge;
        Pred.BlockId = Blk.Id;
        NewBlocks[Edge.BlockId].ExceptionalPreds.push_back(Pred);
      }
  }
  Func.Blocks = std::move(NewBlocks);
}

} // namespace neverd
