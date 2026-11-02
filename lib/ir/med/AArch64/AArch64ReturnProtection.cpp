//===- AArch64ReturnProtection.cpp - Source return protection -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/AArch64/AArch64ReturnProtection.h"

#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/lift/AArch64/AArch64Regs.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

namespace neverd {
namespace {
using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }
using Position = std::pair<int, size_t>;

Intrinsic intrinsic(const MedOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs && Op.Inputs[0].isConst()
             ? static_cast<Intrinsic>(Op.Inputs[0].ConstVal)
             : Intrinsic::None;
}

bool transport(const MedVar &V) {
  // Other architectural registers can be implicit call/return operands even
  // when no MedOp reads them. In particular, do not absorb a COPY to X0.
  return V.Size == 8 && (V.Kind == MedVar::Temp ||
                         (V.Kind == MedVar::Reg && V.RegOff == a64reg::X30));
}

class ReturnProtectionProof {
  const MedFunc &Func;
  BinaryFormat Format;
  std::map<int, const MedBlock *> Blocks;
  std::map<Key, const MedOp *> Definitions;
  std::map<Key, std::pair<const PhiNode *, int>> Phis;
  std::map<const MedOp *, Position> Positions;
  std::map<Key, int64_t> Offsets;
  std::set<const MedOp *> Signs, Auths;
  std::map<int64_t, const MedOp *> Saves;
  AArch64ReturnProtectionProjection Result;

  std::optional<int64_t> offset(const MedVar &V) const {
    const auto It = Offsets.find(key(V));
    return !V.isConst() && It != Offsets.end()
               ? std::optional<int64_t>(It->second)
               : std::nullopt;
  }

  std::vector<int> predecessors(const MedBlock &Block) const {
    auto Preds = Block.Preds;
    for (const auto &Edge : Block.ExceptionalPreds)
      Preds.push_back(Edge.BlockId);
    return Preds;
  }

  std::vector<int> successors(const MedBlock &Block) const {
    auto Succs = Block.Succs;
    for (const auto &Edge : Block.ExceptionalSuccs)
      Succs.push_back(Edge.BlockId);
    return Succs;
  }

  bool reaches(Position From, Position To) const {
    if (From.first == To.first && From.second < To.second)
      return true;
    std::vector<int> Pending = successors(*Blocks.at(From.first));
    std::set<int> Seen;
    while (!Pending.empty()) {
      const int Id = Pending.back();
      Pending.pop_back();
      if (Id == To.first)
        return true;
      if (!Seen.insert(Id).second || !Blocks.count(Id))
        continue;
      const auto Next = successors(*Blocks.at(Id));
      Pending.insert(Pending.end(), Next.begin(), Next.end());
    }
    return false;
  }

  bool signedBefore(Position At) const {
    return std::any_of(Signs.begin(), Signs.end(), [&](const MedOp *Sign) {
      return reaches(Positions.at(Sign), At);
    });
  }

  bool saveDominates(const MedOp &Save, const MedOp &Load) const {
    const auto S = Positions.at(&Save), L = Positions.at(&Load);
    if (S.first == L.first)
      return S.second < L.second;
    // The save precedes every transfer in its block, including any call that
    // could dispatch to a handler. Avoiding that block must make LOAD
    // unreachable from every independent entry.
    for (size_t I = 0; I < S.second; ++I) {
      const auto Op = Blocks.at(S.first)->Ops[I].Opcode;
      if (Op == NdOp::CALL || Op == NdOp::INDIR_CALL || Op == NdOp::COND_BR ||
          Op == NdOp::BRANCH || Op == NdOp::INDIR_BR || Op == NdOp::RETURN)
        return false;
    }
    // Address-taken labels and exception landing pads retain this invocation's
    // frame. The entry save runs before any call can enter a handler; those
    // analysis roots are not independent native calls that bypass its prologue.
    if (S.first == Func.Blocks.front().Id)
      return true;
    std::vector<int> Pending{Func.Blocks.front().Id};
    for (const auto &[Id, Block] : Blocks)
      if (Func.ModuleAnalysisRoots.count(Block->StartAddr))
        Pending.push_back(Id);
    std::set<int> Seen;
    while (!Pending.empty()) {
      const int Id = Pending.back();
      Pending.pop_back();
      if (Id == S.first || !Seen.insert(Id).second)
        continue;
      if (Id == L.first || !Blocks.count(Id))
        return false;
      const auto Next = successors(*Blocks.at(Id));
      Pending.insert(Pending.end(), Next.begin(), Next.end());
    }
    return true;
  }

  bool returnedLink(const MedVar &Value, Position At) const {
    std::vector<std::pair<MedVar, Position>> Pending{{Value, At}};
    std::set<std::tuple<Key, Position>> Seen;
    bool Anchored = false;
    while (!Pending.empty()) {
      const auto [V, Use] = Pending.back();
      Pending.pop_back();
      if (!Seen.emplace(key(V), Use).second)
        continue;
      if (V.Size != 8 || V.isConst())
        return false;
      const auto Def = Definitions.find(key(V));
      if (Def != Definitions.end() && Auths.count(Def->second)) {
        Anchored = true;
        continue;
      }
      if (isEntryLiveInValue(Func, V, a64reg::X30)) {
        // An unsigned early return is legal only on the arm that never signed
        // LR. Do not bless a signed arm that bypasses authentication.
        if (signedBefore(Use))
          return false;
        Anchored = true;
        continue;
      }
      if (Def != Definitions.end() && Def->second->Opcode == NdOp::COPY &&
          Def->second->NumInputs == 1 && Def->second->Inputs[0] != V) {
        Pending.emplace_back(Def->second->Inputs[0], Use);
        continue;
      }
      const auto Phi = Phis.find(key(V));
      if (Phi == Phis.end() || Phi->second.first->Args.empty() ||
          Phi->second.first->ExceptionalEntry)
        return false;
      // A PHI selects along its incoming edges. A later signing operation
      // cannot be evaded by following an old unsigned input back past it.
      for (const MedOp *Sign : Signs)
        if (reaches({Phi->second.second, 0}, Positions.at(Sign)) &&
            reaches(Positions.at(Sign), Use))
          return false;
      for (const auto &[Pred, Arg] : Phi->second.first->Args) {
        if (!Blocks.count(Pred))
          return false;
        Pending.emplace_back(Arg, Position{Pred, Blocks.at(Pred)->Ops.size()});
      }
    }
    return Anchored;
  }

  template <typename Predicate>
  bool registerBefore(uint64_t Register, Position At, Predicate Accept) const {
    std::vector<Position> Pending{At};
    std::set<Position> Seen;
    bool Anchored = false;
    while (!Pending.empty()) {
      const auto Point = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(Point).second)
        continue;
      const auto &Block = *Blocks.at(Point.first);
      const MedVar *Value = nullptr;
      for (size_t I = Point.second; I-- > 0;) {
        const auto &Op = Block.Ops[I];
        if (Register == a64reg::X30 &&
            (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL))
          return false;
        if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == Register) {
          Value = &Op.Output;
          break;
        }
      }
      if (!Value)
        for (const auto &Phi : Block.Phis)
          if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == Register) {
            Value = &Phi.Output;
            break;
          }
      if (Value) {
        if (!Accept(*Value, Point))
          return false;
        Anchored = true;
        continue;
      }
      const auto Preds = predecessors(Block);
      if (Preds.empty())
        return false;
      for (int Pred : Preds) {
        if (!Blocks.count(Pred))
          return false;
        Pending.emplace_back(Pred, Blocks.at(Pred)->Ops.size());
      }
    }
    return Anchored;
  }

  bool restoredBefore(const MedOp &Op) const {
    const auto At = Positions.at(&Op);
    return registerBefore(
               a64reg::SP, At,
               [&](const MedVar &V, Position) { return offset(V) == 0; }) &&
           registerBefore(a64reg::X30, At, [&](const MedVar &V, Position Use) {
             return returnedLink(V, Use);
           });
  }

public:
  ReturnProtectionProof(const MedFunc &F, BinaryFormat Fmt)
      : Func(F), Format(Fmt) {}

  std::optional<AArch64ReturnProtectionProjection> run() {
    if (Func.Blocks.empty() || !Func.Blocks.front().Preds.empty() ||
        Func.SkippedSSA || Func.EntryKind != StackEntryKind::Call)
      return std::nullopt;
    if (std::none_of(Func.Blocks.begin(), Func.Blocks.end(), [](const auto &B) {
          return std::any_of(B.Ops.begin(), B.Ops.end(), [](const auto &Op) {
            return intrinsic(Op) == Intrinsic::Pacia ||
                   intrinsic(Op) == Intrinsic::Pacib;
          });
        }))
      return std::nullopt;
    std::vector<MedVar> Values;
    bool Calls = false;
    for (const auto &Block : Func.Blocks) {
      Blocks.emplace(Block.Id, &Block);
      for (const auto &Phi : Block.Phis) {
        Phis.emplace(key(Phi.Output), std::make_pair(&Phi, Block.Id));
        Values.push_back(Phi.Output);
        for (const auto &[Pred, V] : Phi.Args)
          Values.push_back(V);
      }
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const auto &Op = Block.Ops[I];
        Positions.emplace(&Op, Position{Block.Id, I});
        if (Op.Output.Size && !Op.Output.isConst()) {
          Definitions.emplace(key(Op.Output), &Op);
          Values.push_back(Op.Output);
        }
        for (unsigned J = 0; J < Op.NumInputs; ++J)
          Values.push_back(Op.Inputs[J]);
        Calls |= (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
                 !Op.IsTailCall;
      }
    }
    for (const auto &Call : Func.CallInfos)
      Values.insert(Values.end(), Call.Args.begin(), Call.Args.end());
    const auto Stack = entryStackOffsets(Func, Values, Arch::AArch64, Format);
    for (size_t I = 0; I < Values.size(); ++I)
      if (!Values[I].isConst() && Stack[I])
        Offsets.emplace(key(Values[I]), *Stack[I]);

    std::set<Key> SignedValues;
    for (const auto &Block : Func.Blocks)
      for (const auto &Op : Block.Ops)
        if ((intrinsic(Op) == Intrinsic::Pacia ||
             intrinsic(Op) == Intrinsic::Pacib) &&
            aarch64PAuthMedShapeIsValid(Op) && transport(Op.Output) &&
            isEntryLiveInValue(Func, Op.Inputs[1], a64reg::X30) &&
            offset(Op.Inputs[2]) == 0) {
          const bool BKey = intrinsic(Op) == Intrinsic::Pacib;
          if (!Signs.empty() && Result.Protection.BKey != BKey)
            return std::nullopt;
          Result.Protection.BKey = BKey;
          Signs.insert(&Op);
          SignedValues.insert(key(Op.Output));
          Result.Operations.insert(&Op);
        }
    if (Signs.empty())
      return std::nullopt;
    Result.Protection.Leaf = !Calls;

    // Follow the saved word, rather than requiring the save in block zero.
    // Each reload must have a dominating private store, including through a
    // conditional prologue; a missing initialization arm cannot supply it.
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const auto &Block : Func.Blocks) {
        for (const auto &Phi : Block.Phis)
          if (transport(Phi.Output) && !Phi.Args.empty() &&
              !Phi.ExceptionalEntry &&
              std::all_of(Phi.Args.begin(), Phi.Args.end(), [&](const auto &A) {
                return SignedValues.count(key(A.second));
              }))
            Changed |= SignedValues.insert(key(Phi.Output)).second;
        for (const auto &Op : Block.Ops) {
          bool Signed = false;
          if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              Op.Inputs[0].Size == 8 && transport(Op.Output))
            Signed = SignedValues.count(key(Op.Inputs[0]));
          if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
              Op.Inputs[1].Size == 8 && SignedValues.count(key(Op.Inputs[1])) &&
              Op.MemoryOrdering == NdMemoryOrdering::None &&
              Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
            const auto Slot = offset(Op.Inputs[0]);
            if (!Slot || *Slot < -Func.FrameSize || *Slot > -8)
              return std::nullopt;
            const auto [It, Inserted] = Saves.emplace(*Slot, &Op);
            if (!Inserted && It->second != &Op)
              return std::nullopt;
            Changed |= Inserted;
            Result.Operations.insert(&Op);
          }
          if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
              transport(Op.Output) &&
              Op.MemoryOrdering == NdMemoryOrdering::None &&
              Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
            const auto Slot = offset(Op.Inputs[0]);
            const auto Save = Slot ? Saves.find(*Slot) : Saves.end();
            Signed = Save != Saves.end() && saveDominates(*Save->second, Op);
          }
          if (Signed) {
            Changed |= SignedValues.insert(key(Op.Output)).second;
            Result.Operations.insert(&Op);
          }
        }
      }
    }
    for (const auto &Block : Func.Blocks)
      for (const auto &Op : Block.Ops) {
        const auto Id = intrinsic(Op);
        const bool Branch = Id == Intrinsic::A64_AuthBranch;
        const auto Expected =
            Result.Protection.BKey ? Intrinsic::Autib : Intrinsic::Autia;
        if ((Id != Expected && !Branch) || !aarch64PAuthMedShapeIsValid(Op) ||
            (Branch && Op.Inputs[2].ConstVal != (Result.Protection.BKey
                                                     ? kAArch64PAuthKeyIB
                                                     : kAArch64PAuthKeyIA)) ||
            offset(Op.Inputs[Branch ? 3 : 2]) != 0 ||
            !SignedValues.count(key(Op.Inputs[1])) || !transport(Op.Output))
          continue;
        Auths.insert(&Op);
        Result.Operations.insert(&Op);
      }

    bool Exit = false;
    for (const auto &Block : Func.Blocks) {
      bool Terminated = false;
      for (const auto &Op : Block.Ops) {
        if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
            Op.IsTailCall && !Op.DoesNotReturn) {
          if (!restoredBefore(Op))
            return std::nullopt;
          Exit = Terminated = true;
        } else if (Op.Opcode == NdOp::RETURN) {
          if (!Terminated && (Op.NumInputs != 1 ||
                              !returnedLink(Op.Inputs[0], Positions.at(&Op)) ||
                              !registerBefore(a64reg::SP, Positions.at(&Op),
                                              [&](const MedVar &V, Position) {
                                                return offset(V) == 0;
                                              })))
            return std::nullopt;
          Exit = Terminated = true;
        } else if (Op.Opcode == NdOp::INDIR_BR && Block.Succs.empty()) {
          // This proves only removal of the LR recipe. An unresolved branch
          // stays unresolved: source projection cannot classify a tail call.
          if (!restoredBefore(Op))
            return std::nullopt;
          Exit = Terminated = true;
        } else if (((Op.Opcode == NdOp::CALL ||
                     Op.Opcode == NdOp::INDIR_CALL) &&
                    Op.DoesNotReturn) ||
                   isArchitecturalNoReturn(Op)) {
          Exit = Terminated = true;
        }
      }
      if (!Terminated && Block.Succs.empty())
        return std::nullopt;
    }
    if (!Exit)
      return std::nullopt;

    // LR PHIs often outlive their last actual consumer. Propagate through
    // transports to find the real readers; merely appearing on a PHI edge is
    // not an observable use. The entire machine IR remains unchanged.
    std::set<Key> Removed;
    for (const auto *Op : Result.Operations)
      if (Op->Output.Size)
        Removed.insert(key(Op->Output));
    Changed = true;
    while (Changed) {
      Changed = false;
      for (const auto &Block : Func.Blocks) {
        for (const auto &Phi : Block.Phis)
          if (transport(Phi.Output) && !Phi.ExceptionalEntry &&
              std::any_of(Phi.Args.begin(), Phi.Args.end(), [&](const auto &A) {
                return Removed.count(key(A.second));
              }))
            Changed |= Removed.insert(key(Phi.Output)).second;
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              transport(Op.Output) && Op.Inputs[0].Size == 8 &&
              Removed.count(key(Op.Inputs[0]))) {
            Changed |= Removed.insert(key(Op.Output)).second;
            Result.Operations.insert(&Op);
          }
      }
    }
    const auto EscapesSlot = [&](const MedVar &V) {
      const auto At = offset(V);
      return At && std::any_of(Saves.begin(), Saves.end(), [&](const auto &S) {
               return *At >= S.first && *At < S.first + 8;
             });
    };
    std::set<Key> Observed;
    for (const auto &Block : Func.Blocks) {
      for (const auto &Phi : Block.Phis)
        if (!transport(Phi.Output))
          Observed.insert(key(Phi.Output));
      for (const auto &Op : Block.Ops)
        if (!Result.Operations.count(&Op) &&
            !(Op.Opcode == NdOp::COPY && transport(Op.Output)))
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            Observed.insert(key(Op.Inputs[I]));
    }
    for (const auto &Call : Func.CallInfos)
      for (const auto &Arg : Call.Args)
        Observed.insert(key(Arg));
    Changed = true;
    while (Changed) {
      Changed = false;
      for (const auto &Block : Func.Blocks) {
        for (const auto &Phi : Block.Phis)
          if (Observed.count(key(Phi.Output)))
            for (const auto &[Pred, V] : Phi.Args)
              Changed |= Observed.insert(key(V)).second;
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              Observed.count(key(Op.Output)))
            Changed |= Observed.insert(key(Op.Inputs[0])).second;
      }
    }
    for (const auto &Block : Func.Blocks) {
      for (const auto &Phi : Block.Phis)
        for (const auto &[Pred, V] : Phi.Args)
          if ((Removed.count(key(V)) && !Removed.count(key(Phi.Output))) ||
              (EscapesSlot(V) && Observed.count(key(Phi.Output))))
            return std::nullopt;
      for (const auto &Op : Block.Ops) {
        if (Result.Operations.count(&Op))
          continue;
        if (Op.Opcode != NdOp::RETURN)
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            if (Removed.count(key(Op.Inputs[I])))
              return std::nullopt;
        if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
            Op.NumInputs) {
          const auto At = offset(Op.Inputs[0]);
          const unsigned Bytes =
              Op.Opcode == NdOp::LOAD ? Op.Output.Size : Op.Inputs[1].Size;
          for (const auto &[Slot, Save] : Saves)
            if (At && *At < Slot + 8 &&
                *At > Slot - static_cast<int64_t>(Bytes))
              return std::nullopt;
        }
        if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
            EscapesSlot(Op.Inputs[1]))
          return std::nullopt;
      }
    }
    for (const auto &Call : Func.CallInfos)
      for (const auto &Arg : Call.Args)
        if (Removed.count(key(Arg)) || EscapesSlot(Arg))
          return std::nullopt;
    for (const auto &Block : Func.Blocks)
      for (const auto &Phi : Block.Phis)
        if (Removed.count(key(Phi.Output)))
          Result.Phis.insert(&Phi);
    return Result;
  }
};
} // namespace

std::optional<AArch64ReturnProtectionProjection>
findAArch64ReturnProtection(const MedFunc &Func, BinaryFormat Format) {
  return ReturnProtectionProof(Func, Format).run();
}
} // namespace neverd
