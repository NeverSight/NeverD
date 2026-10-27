//===- AArch64ReturnProtection.cpp - Source return protection -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/AArch64/AArch64ReturnProtection.h"

#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/lift/AArch64/AArch64Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <tuple>
#include <vector>

namespace neverd {
namespace {
using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

Intrinsic intrinsic(const MedOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs && Op.Inputs[0].isConst()
             ? static_cast<Intrinsic>(Op.Inputs[0].ConstVal)
             : Intrinsic::None;
}
} // namespace

std::optional<AArch64ReturnProtectionProjection>
findAArch64ReturnProtection(const MedFunc &Func, BinaryFormat Format) {
  if (Func.Blocks.empty() || !Func.Blocks.front().Preds.empty() ||
      Func.SkippedSSA || Func.EntryKind != StackEntryKind::Call)
    return std::nullopt;

  const MedOp *Sign = nullptr;
  const auto &Entry = Func.Blocks.front();
  for (const MedOp &Op : Entry.Ops) {
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Output == Op.Inputs[0])
      continue;
    if ((intrinsic(Op) == Intrinsic::Pacia ||
         intrinsic(Op) == Intrinsic::Pacib) &&
        aarch64PAuthMedShapeIsValid(Op) &&
        isEntryLiveInValue(Func, Op.Inputs[1], a64reg::X30))
      Sign = &Op;
    break;
  }
  if (!Sign)
    return std::nullopt;

  std::map<Key, const MedOp *> Definitions;
  std::vector<MedVar> Values;
  std::vector<const MedOp *> Returns;
  bool Calls = false;
  for (const MedBlock &Block : Func.Blocks) {
    for (const MedOp &Op : Block.Ops) {
      if (Op.Output.Size && !Op.Output.isConst()) {
        Definitions.emplace(key(Op.Output), &Op);
        Values.push_back(Op.Output);
      }
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Values.push_back(Op.Inputs[I]);
      if (Op.Opcode == NdOp::RETURN)
        Returns.push_back(&Op);
      Calls |= Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
      // An indirect tail transfer is not proved by a normal RETURN recipe.
      if (Op.Opcode == NdOp::INDIR_BR)
        return std::nullopt;
    }
  }
  if (Returns.empty())
    return std::nullopt;
  const auto Offsets = entryStackOffsets(Func, Values, Arch::AArch64, Format);
  std::map<Key, int64_t> FrameOffsets;
  for (size_t I = 0; I < Values.size(); ++I)
    if (!Values[I].isConst() && Offsets[I])
      FrameOffsets.emplace(key(Values[I]), *Offsets[I]);
  const auto Offset = [&](const MedVar &V) -> std::optional<int64_t> {
    const auto It = FrameOffsets.find(key(V));
    return !V.isConst() && It != FrameOffsets.end()
               ? std::optional<int64_t>(It->second)
               : std::nullopt;
  };
  if (Offset(Sign->Inputs[2]) != 0)
    return std::nullopt;

  AArch64ReturnProtectionProjection Result;
  Result.Protection.BKey = intrinsic(*Sign) == Intrinsic::Pacib;
  Result.Protection.Leaf = !Calls;
  Result.Operations.insert(Sign);
  const auto Definition = [&](const MedVar &V) -> const MedOp * {
    const auto It = Definitions.find(key(V));
    return !V.isConst() && It != Definitions.end() ? It->second : nullptr;
  };
  const auto Uncopy = [&](MedVar V) -> const MedOp * {
    std::set<Key> Seen;
    while (Seen.insert(key(V)).second) {
      const MedOp *Def = Definition(V);
      if (!Def || Def->Opcode != NdOp::COPY || Def->NumInputs != 1 ||
          Def->Output.Size != 8 || Def->Inputs[0].Size != 8 ||
          Def->Output == Def->Inputs[0])
        return Def;
      Result.Operations.insert(Def);
      V = Def->Inputs[0];
    }
    return nullptr;
  };

  std::optional<int64_t> SavedOffset;
  const MedOp *Save = nullptr;
  for (const MedOp *Ret : Returns) {
    if (Ret->NumInputs != 1 || Ret->Inputs[0].Size != 8)
      return std::nullopt;
    const MedOp *Auth = Uncopy(Ret->Inputs[0]);
    if (!Auth)
      return std::nullopt;
    const Intrinsic Id = intrinsic(*Auth);
    const bool Branch = Id == Intrinsic::A64_AuthBranch;
    const Intrinsic Expected =
        Result.Protection.BKey ? Intrinsic::Autib : Intrinsic::Autia;
    if (!aarch64PAuthMedShapeIsValid(*Auth) ||
        (Branch ? Auth->Inputs[2].ConstVal != (Result.Protection.BKey
                                                   ? kAArch64PAuthKeyIB
                                                   : kAArch64PAuthKeyIA)
                : Id != Expected) ||
        Offset(Auth->Inputs[Branch ? 3 : 2]) != 0)
      return std::nullopt;
    Result.Operations.insert(Auth);
    const MedOp *Signed = Uncopy(Auth->Inputs[1]);
    if (Signed == Sign)
      continue;
    if (!Signed || Signed->Opcode != NdOp::LOAD || Signed->NumInputs != 1 ||
        Signed->Output.Size != 8 ||
        Signed->MemoryOrdering != NdMemoryOrdering::None ||
        Signed->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    const auto Slot = Offset(Signed->Inputs[0]);
    if (!Slot || *Slot < -Func.FrameSize || *Slot > -8 ||
        (SavedOffset && *SavedOffset != *Slot))
      return std::nullopt;
    SavedOffset = Slot;
    Result.Operations.insert(Signed);
    if (Save)
      continue;
    // Saving before any transfer in the entry block dominates every return.
    for (const MedOp &Op : Entry.Ops) {
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::RETURN)
        break;
      if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
          Op.Inputs[1].Size == 8 && Offset(Op.Inputs[0]) == Slot &&
          Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        if (Uncopy(Op.Inputs[1]) != Sign)
          return std::nullopt;
        Save = &Op;
        Result.Operations.insert(Save);
        break;
      }
    }
    if (!Save)
      return std::nullopt;
  }

  // Every signed/authenticated value is used only by this recipe. In
  // particular, a signed LR returned in X0, passed to a call, stored elsewhere
  // or merged into a PHI must remain explicit in the source.
  std::set<Key> RemovedValues;
  for (const MedOp *Op : Result.Operations)
    if (Op->Output.Size)
      RemovedValues.insert(key(Op->Output));
  // Propagation can replace AUT's LR operand with the reload's temporary,
  // leaving the architectural restore COPY unused. It still belongs to the
  // recipe. Do not absorb copies to X0: RETURN's explicit operand is its
  // control target, while X0 is an implicit, observable result.
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
          Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == a64reg::X30 &&
          Op.Output.Size == 8 && Op.Inputs[0].Size == 8 &&
          RemovedValues.count(key(Op.Inputs[0]))) {
        Result.Operations.insert(&Op);
        RemovedValues.insert(key(Op.Output));
      }
  const auto Removed = [&](const MedVar &V) {
    return !V.isConst() && RemovedValues.count(key(V));
  };
  const auto EscapesSlot = [&](const MedVar &V) {
    const auto At = Offset(V);
    return SavedOffset && At && *At >= *SavedOffset && *At < *SavedOffset + 8;
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, V] : Phi.Args)
        if (Removed(V) || EscapesSlot(V))
          return std::nullopt;
    for (const MedOp &Op : Block.Ops) {
      if (Result.Operations.count(&Op))
        continue;
      if (Op.Opcode != NdOp::RETURN)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (Removed(Op.Inputs[I]))
            return std::nullopt;
      if (SavedOffset &&
          (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.NumInputs) {
        const auto At = Offset(Op.Inputs[0]);
        const unsigned Bytes =
            Op.Opcode == NdOp::LOAD ? Op.Output.Size : Op.Inputs[1].Size;
        if (At && *At < *SavedOffset + 8 &&
            *At > *SavedOffset - static_cast<int64_t>(Bytes))
          return std::nullopt;
      }
      if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
          EscapesSlot(Op.Inputs[1]))
        return std::nullopt;
    }
  }
  for (const MedCallInfo &Call : Func.CallInfos)
    for (const MedVar &Arg : Call.Args)
      if (Removed(Arg) || EscapesSlot(Arg))
        return std::nullopt;
  return Result;
}

} // namespace neverd
