//===- LowToMedX86Flags.cpp - Machine flags in x86 PUSHF images -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The x86 lifter builds a PUSHF image from the machine flags (Pushf): it
/// clears each modelled flag's bit and merges in that flag's value, which
/// carries the computation that set the flag and which a C translation cannot
/// reproduce in its own flags.  Where no modelled instruction has defined a
/// flag since its root was entered, or since an unmodelled effect left it,
/// the machine still holds that bit, so the image takes it from the machine
/// value exactly as __readeflags returns it.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
namespace {

using ValueKey = std::tuple<int, int, int>;

ValueKey keyOf(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

struct ValueGraph {
  std::map<ValueKey, MedOp *> Defs;
  std::map<ValueKey, const PhiNode *> PhiDefs;
  std::map<ValueKey, std::vector<MedOp *>> Users;
  std::set<ValueKey> PhiUsed;

  explicit ValueGraph(MedFunc &Func) {
    for (MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        PhiDefs[keyOf(Phi.Output)] = &Phi;
      for (MedOp &Op : Block.Ops) {
        if (Op.Dead)
          continue;
        if (Op.Output.Size != 0)
          Defs[keyOf(Op.Output)] = &Op;
        for (const MedVar &Aux : Op.IntrinsicOutputs)
          Defs[keyOf(Aux)] = &Op;
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (!Op.Inputs[I].isConst())
            Users[keyOf(Op.Inputs[I])].push_back(&Op);
      }
    }
    // Only a PHI something reads passes a value on: SSA construction also
    // places PHIs that nothing reads, and a temporary's first definition
    // shares version 0 with its undefined incoming value.
    std::vector<const PhiNode *> Live;
    std::set<ValueKey> Seen;
    for (const auto &[Key, Phi] : PhiDefs)
      if (Users.count(Key) && Seen.insert(Key).second)
        Live.push_back(Phi);
    while (!Live.empty()) {
      const PhiNode *Phi = Live.back();
      Live.pop_back();
      for (const auto &[Pred, Arg] : Phi->Args) {
        (void)Pred;
        const ValueKey Key = keyOf(Arg);
        PhiUsed.insert(Key);
        if (auto It = PhiDefs.find(Key);
            It != PhiDefs.end() && Seen.insert(Key).second)
          Live.push_back(It->second);
      }
    }
  }

  const MedOp *def(const MedVar &V) const {
    auto It = Defs.find(keyOf(V));
    return It == Defs.end() ? nullptr : It->second;
  }

  /// The one operation reading \p V, when nothing else (a PHI included) does.
  MedOp *soleUser(const MedVar &V) const {
    const ValueKey Key = keyOf(V);
    auto It = Users.find(Key);
    if (It == Users.end() || It->second.size() != 1 || PhiUsed.count(Key))
      return nullptr;
    return It->second.front();
  }

  /// True when the machine holds flag value \p V: no modelled instruction
  /// defined it since its root was entered (the incoming version, or copies
  /// of it), an unmodelled effect left it unspecified, or every path brings
  /// one of those.
  bool machineHolds(const MedVar &V, std::set<ValueKey> &Seen) const {
    if (V.Kind == MedVar::Unspecified)
      return true;
    if (V.Kind != MedVar::Flag)
      return false;
    if (!Seen.insert(keyOf(V)).second)
      return true;
    if (const MedOp *Op = def(V))
      return Op->Opcode == NdOp::COPY && Op->NumInputs == 1 &&
             machineHolds(Op->Inputs[0], Seen);
    if (auto It = PhiDefs.find(keyOf(V)); It != PhiDefs.end())
      return llvm::all_of(It->second->Args, [&](const auto &Arg) {
        return machineHolds(Arg.second, Seen);
      });
    return V.SSAVer == 0;
  }

  /// True when flag value \p V is zero on every path.
  bool isZero(const MedVar &V, std::set<ValueKey> &Seen) const {
    if (V.isConst())
      return V.ConstVal == 0;
    if (!Seen.insert(keyOf(V)).second)
      return true;
    if (const MedOp *Op = def(V))
      return Op->Opcode == NdOp::COPY && Op->NumInputs == 1 &&
             isZero(Op->Inputs[0], Seen);
    if (auto It = PhiDefs.find(keyOf(V)); It != PhiDefs.end())
      return llvm::all_of(It->second->Args, [&](const auto &Arg) {
        return isZero(Arg.second, Seen);
      });
    return false;
  }
};

bool isPushf(const MedOp &Op) {
  return !Op.Dead && Op.Opcode == NdOp::INTRINSIC && Op.NumInputs == 1 &&
         Op.Inputs[0].isConst() &&
         Op.Inputs[0].ConstVal == static_cast<uint64_t>(Intrinsic::Pushf) &&
         Op.Output.Kind == MedVar::Temp && Op.Output.Size != 0 &&
         Op.Output.Size <= 8;
}

std::optional<unsigned> eflagsBit(uint64_t Flag) {
  for (const x86reg::EFlagsBit &Entry : x86reg::EFlagsBits)
    if (Entry.Flag == Flag)
      return Entry.Bit;
  return std::nullopt;
}

bool isModelledBit(unsigned Bit) {
  return llvm::any_of(x86reg::EFlagsBits, [&](const x86reg::EFlagsBit &Entry) {
    return Entry.Bit == Bit;
  });
}

/// One modelled flag merged into a PUSHF image: `Next = Prev | zext(Flag) <<
/// Bit`.
struct FlagTerm {
  MedOp *Merge;
  MedVar Prev;
  MedVar Flag;
  unsigned Bit;
};

/// The lifter's merge of a modelled flag into \p Prev, or nullopt.
std::optional<FlagTerm> matchFlagTerm(const ValueGraph &Graph, MedOp &Merge,
                                      const MedVar &Prev) {
  if (Merge.Opcode != NdOp::INT_OR || Merge.NumInputs != 2 ||
      Merge.Output.Kind != MedVar::Temp)
    return std::nullopt;
  const bool PrevFirst = keyOf(Merge.Inputs[0]) == keyOf(Prev);
  if (!PrevFirst && keyOf(Merge.Inputs[1]) != keyOf(Prev))
    return std::nullopt;
  const MedOp *Shift = Graph.def(Merge.Inputs[PrevFirst ? 1 : 0]);
  if (!Shift || Shift->Opcode != NdOp::INT_LEFT || Shift->NumInputs != 2 ||
      !Shift->Inputs[1].isConst())
    return std::nullopt;
  const MedOp *Extend = Graph.def(Shift->Inputs[0]);
  if (!Extend || Extend->Opcode != NdOp::INT_ZEXT || Extend->NumInputs != 1)
    return std::nullopt;
  const MedVar &Flag = Extend->Inputs[0];
  if (Flag.Kind != MedVar::Flag && Flag.Kind != MedVar::Const &&
      Flag.Kind != MedVar::Unspecified)
    return std::nullopt;
  return FlagTerm{&Merge, Prev, Flag,
                  static_cast<unsigned>(Shift->Inputs[1].ConstVal)};
}

} // namespace

void foldMachineFlagsIntoPushfImages(MedFunc &Func) {
  std::vector<MedOp *> Pushfs;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops)
      if (isPushf(Op))
        Pushfs.push_back(&Op);
  if (Pushfs.empty())
    return;

  const ValueGraph Graph(Func);
  for (MedOp *Pushf : Pushfs) {
    const MedVar Machine = Pushf->Output;
    MedOp *Clear = Graph.soleUser(Machine);
    if (!Clear || Clear->Opcode != NdOp::INT_AND || Clear->NumInputs != 2 ||
        keyOf(Clear->Inputs[0]) != keyOf(Machine) ||
        !Clear->Inputs[1].isConst() || Clear->Output.Kind != MedVar::Temp ||
        Clear->Output.Size != Machine.Size)
      continue;

    std::vector<FlagTerm> Terms;
    MedVar Image = Clear->Output;
    while (MedOp *Merge = Graph.soleUser(Image)) {
      std::optional<FlagTerm> Term = matchFlagTerm(Graph, *Merge, Image);
      if (!Term)
        break;
      Image = Merge->Output;
      Terms.push_back(*Term);
    }

    const uint64_t SizeMask = Machine.Size == 8
                                  ? ~uint64_t{0}
                                  : (uint64_t{1} << (Machine.Size * 8)) - 1;
    uint64_t Kept = Clear->Inputs[1].ConstVal & SizeMask;
    for (const FlagTerm &Term : Terms) {
      // Only the lifter's own shape: a modelled flag at its EFLAGS position,
      // cleared from the machine value.
      if (Term.Flag.Kind == MedVar::Const || Term.Bit >= Machine.Size * 8)
        continue;
      const uint64_t Bit = uint64_t{1} << Term.Bit;
      if ((Term.Flag.Kind == MedVar::Flag
               ? eflagsBit(Term.Flag.RegOff) != Term.Bit
               : !isModelledBit(Term.Bit)) ||
          (Kept & Bit))
        continue;
      std::set<ValueKey> Seen;
      bool FromMachine;
      if (Term.Flag.Kind == MedVar::Flag && Term.Flag.RegOff == x86reg::DF)
        // C keeps DF clear outside its own string operations, as the
        // machine does whenever the modelled DF is zero.
        FromMachine = Graph.isZero(Term.Flag, Seen);
      else
        FromMachine = Graph.machineHolds(Term.Flag, Seen);
      if (!FromMachine)
        continue;
      Kept |= Bit;
      MedOp &Merge = *Term.Merge;
      Merge.Opcode = NdOp::COPY;
      Merge.Inputs[0] = Term.Prev;
      Merge.NumInputs = 1;
    }
    if (Kept == (Clear->Inputs[1].ConstVal & SizeMask))
      continue;
    if (Kept == SizeMask) {
      Clear->Opcode = NdOp::COPY;
      Clear->NumInputs = 1;
    } else {
      Clear->Inputs[1] = MedVar::makeConst(Kept, Machine.Size,
                                           ConstantAddressProvenance::Scalar);
    }
  }
}

} // namespace neverd
