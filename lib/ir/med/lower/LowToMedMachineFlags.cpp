//===- LowToMedMachineFlags.cpp - Machine flags in status images ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The x86 lifter builds a PUSHF image, and the ARM lifter the APSR word an
/// MRS reads, from the machine's status word (Pushf, ArmMrs): it clears each
/// modelled flag's bit and merges in that flag's value, which carries the
/// computation that set the flag and which a C translation cannot reproduce
/// in its own flags.  Where no modelled instruction has defined a flag since
/// its root was entered, or since an unmodelled effect left it, the machine
/// still holds that bit, so the image takes it from the machine value exactly
/// as __readeflags, or the MRS, returns it.  On entry the ARM procedure call
/// standard leaves the flags UNKNOWN, as the machine's are.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/lift/X86/X86Regs.h"

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
  /// one of those.  ARM's GE flags lie outside its flag range, as registers.
  bool machineHolds(const MedVar &V, std::set<ValueKey> &Seen) const {
    if (V.Kind == MedVar::Unspecified)
      return true;
    if (V.Kind != MedVar::Flag && V.Kind != MedVar::Reg)
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

/// A modelled flag and its bit in a status image.
struct ModelledBit {
  uint64_t Flag;
  unsigned Bit;
};

constexpr ModelledBit X86EFlagsBits[] = {
#define NEVERD_X86_EFLAGS_BIT(Flag, Bit) {x86reg::Flag, Bit},
#include "neverd/lift/X86/X86EFlags.def"
};

constexpr ModelledBit ARMApsrBits[] = {
#define NEVERD_ARM_APSR_BIT(Flag, Bit) {armreg::Flag, Bit},
#include "neverd/lift/ARM/ARMApsrFlags.def"
};

/// A status word an intrinsic reads from the machine, into which the lifter
/// merges the flags it models.
struct StatusImage {
  llvm::ArrayRef<ModelledBit> Bits;
  /// A flag C keeps clear, as the machine does whenever the modelled one is.
  std::optional<uint64_t> ClearFlag;

  std::optional<unsigned> bitOf(uint64_t Flag) const {
    for (const ModelledBit &Entry : Bits)
      if (Entry.Flag == Flag)
        return Entry.Bit;
    return std::nullopt;
  }
  bool isModelledBit(unsigned Bit) const {
    return llvm::any_of(
        Bits, [&](const ModelledBit &Entry) { return Entry.Bit == Bit; });
  }
};

/// The status image \p Op reads: x86's PUSHF, or an ARM MRS of APSR.
std::optional<StatusImage> statusImage(const MedOp &Op) {
  if (Op.Dead || Op.Opcode != NdOp::INTRINSIC || Op.NumInputs == 0 ||
      !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Temp ||
      Op.Output.Size == 0 || Op.Output.Size > 8)
    return std::nullopt;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
  if (Id == Intrinsic::Pushf && Op.NumInputs == 1)
    return StatusImage{X86EFlagsBits, x86reg::DF};
  if (Id == Intrinsic::ArmMrs && Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
      Op.Inputs[1].ConstVal == ArmMrsApsr && Op.Output.Size == 4)
    return StatusImage{ARMApsrBits, std::nullopt};
  return std::nullopt;
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
  if (Flag.Kind != MedVar::Flag && Flag.Kind != MedVar::Reg &&
      Flag.Kind != MedVar::Const && Flag.Kind != MedVar::Unspecified)
    return std::nullopt;
  return FlagTerm{&Merge, Prev, Flag,
                  static_cast<unsigned>(Shift->Inputs[1].ConstVal)};
}

} // namespace

void foldMachineFlagsIntoStatusImages(MedFunc &Func) {
  std::vector<std::pair<MedOp *, StatusImage>> Reads;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops)
      if (std::optional<StatusImage> Image = statusImage(Op))
        Reads.emplace_back(&Op, *Image);
  if (Reads.empty())
    return;

  const ValueGraph Graph(Func);
  for (const auto &[Read, Status] : Reads) {
    const MedVar Machine = Read->Output;
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
      // Only the lifter's own shape: a modelled flag at its position in the
      // image, cleared from the machine value.
      if (Term.Flag.Kind == MedVar::Const || Term.Bit >= Machine.Size * 8)
        continue;
      const uint64_t Bit = uint64_t{1} << Term.Bit;
      const bool Register =
          Term.Flag.Kind == MedVar::Flag || Term.Flag.Kind == MedVar::Reg;
      if ((Register ? Status.bitOf(Term.Flag.RegOff) != Term.Bit
                    : !Status.isModelledBit(Term.Bit)) ||
          (Kept & Bit))
        continue;
      std::set<ValueKey> Seen;
      bool FromMachine;
      if (Register && Term.Flag.RegOff == Status.ClearFlag)
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
