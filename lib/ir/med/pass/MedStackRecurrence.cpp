//===- MedStackRecurrence.cpp - Counted stack recurrence bounds ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove finite unsigned iterations before bounding changing stack PHIs.

#include "MedStackRecurrence.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
namespace {

using Key = std::tuple<MedVar::VarKind, int, int>;
Key key(const MedVar &Value) { return {Value.Kind, Value.Id, Value.SSAVer}; }
bool same(const MedVar &A, const MedVar &B) {
  return key(A) == key(B) && A.Size == B.Size && A.TheArch == B.TheArch &&
         A.RenameTag == B.RenameTag &&
         (A.Kind != MedVar::Reg || A.RegOff == B.RegOff);
}
bool pure(const MedOp &Op) {
  return !Op.Dead && Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         Op.IntrinsicOutputs.empty() &&
         Op.RegistrationRoot == MedOp::RegistrationRootKind::None;
}
bool scalarConstant(const MedVar &Value, unsigned Width) {
  return Value.isConst() && Value.Size == Width &&
         !isAddressProvenance(Value.Provenance);
}

class RecurrenceProof {
  struct Definition {
    const MedOp *Op = nullptr;
    const MedVar *Output = nullptr;
    int Block = -1;
    bool Ambiguous = false;
  };
  const MedFunc &Func;
  size_t &Remaining;
  std::map<Key, Definition> Definitions;

  bool charge(size_t Count = 1) {
    if (Count > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Count;
    return true;
  }
  const Definition *definition(const MedVar &Value) const {
    const auto It = Definitions.find(key(Value));
    return It == Definitions.end() || It->second.Ambiguous ||
                   !same(*It->second.Output, Value)
               ? nullptr
               : &It->second;
  }
  // Only discard transfers that preserve every bit of the counter. This
  // includes the x86 low-dword / zero-extension copies around a 32-bit write.
  std::optional<MedVar> peel(MedVar Value, unsigned Width) {
    std::set<Key> Seen;
    while (true) {
      if (!charge() || Value.Size < Width || !Seen.insert(key(Value)).second)
        return {};
      const auto *Def = definition(Value);
      if (!Def || !Def->Op)
        return Value;
      const auto &Op = *Def->Op;
      if (!pure(Op))
        return {};
      if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT) &&
          Op.NumInputs == 1 && Op.Inputs[0].Size >= Width &&
          (Op.Opcode != NdOp::COPY || Op.Inputs[0].Size == Value.Size))
        Value = Op.Inputs[0];
      else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs == 2 &&
               Op.Inputs[1].isConst() && !Op.Inputs[1].ConstVal &&
               !isAddressProvenance(Op.Inputs[1].Provenance) &&
               Op.Inputs[0].Size >= Value.Size)
        Value = Op.Inputs[0];
      else
        return Value;
    }
  }

  std::optional<std::pair<const Definition *, uint64_t>>
  step(const MedVar &Back, const MedVar &Header) {
    const auto Value = peel(Back, Header.Size);
    const auto *Def = Value ? definition(*Value) : nullptr;
    if (!Def || !Def->Op || !pure(*Def->Op) || !Header.Size ||
        Header.Size > sizeof(uint64_t) || Def->Op->Output.Size != Header.Size)
      return {};
    const auto &Op = *Def->Op;
    if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
        Op.NumInputs != 2 || !same(Op.Inputs[0], Header) ||
        !scalarConstant(Op.Inputs[1], Header.Size))
      return {};
    const uint64_t Mask = UINT64_MAX >> ((8 - Header.Size) * 8);
    const uint64_t Delta =
        (Op.Opcode == NdOp::INT_ADD ? Op.Inputs[1].ConstVal
                                    : uint64_t{0} - Op.Inputs[1].ConstVal) &
        Mask;
    return std::pair{Def, Delta};
  }

public:
  RecurrenceProof(const MedFunc &Func, size_t &Remaining)
      : Func(Func), Remaining(Remaining) {}

  std::optional<CountedStackRecurrence> prove(const PhiNode &Stack) {
    if (Func.SkippedSSA || Func.AnalystStackFrame || Stack.ExceptionalEntry ||
        Stack.Args.size() != 2 || Stack.Output.Kind != MedVar::Reg ||
        (Stack.Output.Size != 4 && Stack.Output.Size != 8) ||
        Stack.Output.TheArch == Arch::Unknown)
      return {};
    const auto &TRI = getTargetRegInfo(Stack.Output.TheArch);
    if (Stack.Output.RegOff != TRI.StackPointer ||
        Stack.Output.Size != TRI.PointerSize || !charge(Func.Blocks.size()))
      return {};
    const MedBlock *Header = nullptr;
    for (const auto &Block : Func.Blocks) {
      if (!charge(Block.Phis.size() + Block.Ops.size() + 1) || Block.Id < 0 ||
          size_t(Block.Id) >= Func.Blocks.size() ||
          &Func.Blocks[Block.Id] != &Block)
        return {};
      const auto Add = [&](const MedVar &Value, const MedOp *Op) {
        if (!Value.Size || Value.isConst())
          return;
        auto [It, New] = Definitions.emplace(
            key(Value), Definition{Op, &Value, Block.Id, false});
        if (!New)
          It->second.Ambiguous = true;
      };
      for (const auto &Phi : Block.Phis) {
        Add(Phi.Output, nullptr);
        if (&Phi == &Stack)
          Header = &Block;
      }
      for (const auto &Op : Block.Ops)
        Add(Op.Output, &Op);
    }
    if (!Header || Header->Preds.size() != 2 || Header->Succs.size() != 2 ||
        !Header->ExceptionalPreds.empty() || Header->Ops.empty() ||
        !definition(Stack.Output))
      return {};
    const auto &Branch = Header->Ops.back();
    if (Branch.Opcode != NdOp::COND_BR || Branch.Dead ||
        Branch.NumInputs != 2 || !Branch.Inputs[0].isConst())
      return {};
    const auto *Guard = definition(Branch.Inputs[1]);
    if (!Guard || !Guard->Op || Guard->Block != Header->Id ||
        !pure(*Guard->Op) || Guard->Op->NumInputs != 2)
      return {};
    const auto &Compare = *Guard->Op;
    const bool ContinueOnTrue = Compare.Opcode == NdOp::INT_LESS;
    if (!ContinueOnTrue && Compare.Opcode != NdOp::INT_LESSEQUAL)
      return {};
    const MedVar &Counter = Compare.Inputs[ContinueOnTrue ? 0 : 1];
    const MedVar &Limit = Compare.Inputs[ContinueOnTrue ? 1 : 0];
    if (!Counter.Size || Counter.Size > sizeof(uint64_t) ||
        !scalarConstant(Limit, Counter.Size))
      return {};
    const auto CountPhi = std::find_if(
        Header->Phis.begin(), Header->Phis.end(),
        [&](const auto &Phi) { return same(Phi.Output, Counter); });
    if (CountPhi == Header->Phis.end() || CountPhi->ExceptionalEntry ||
        CountPhi->Args.size() != 2 || !definition(Counter))
      return {};
    const uint64_t Mask = UINT64_MAX >> ((8 - Counter.Size) * 8);
    const uint64_t Bound = Limit.ConstVal & Mask;
    if (!Bound)
      return {};
    for (unsigned Back = 0; Back != 2; ++Back) {
      const auto StackStep = step(Stack.Args[Back].second, Stack.Output);
      if (!StackStep)
        continue;
      const int BackId = Stack.Args[Back].first;
      const int EntryId = Stack.Args[Back ^ 1u].first;
      if (BackId < 0 || EntryId < 0 || BackId == EntryId ||
          size_t(BackId) >= Func.Blocks.size() ||
          size_t(EntryId) >= Func.Blocks.size() || BackId == Header->Id ||
          EntryId == Header->Id ||
          std::set<int>(Header->Preds.begin(), Header->Preds.end()) !=
              std::set<int>{BackId, EntryId})
        return {};
      const auto Arg = [&](int Pred) -> const MedVar * {
        const MedVar *Found = nullptr;
        for (const auto &[P, V] : CountPhi->Args)
          if (P == Pred) {
            if (Found)
              return nullptr;
            Found = &V;
          }
        return Found;
      };
      const auto *Incoming = Arg(EntryId);
      const auto *Changed = Arg(BackId);
      if (!Incoming || !Changed)
        return {};
      const auto Initial = peel(*Incoming, Counter.Size);
      const auto CountStep = step(*Changed, Counter);
      if (!Initial || !scalarConstant(*Initial, Counter.Size) || !CountStep ||
          !CountStep->second || CountStep->second > Mask - (Bound - 1))
        return {};
      const uint64_t First = Initial->ConstVal & Mask;
      if (First >= Bound)
        return {};
      // Rounding up without overflow. The last header visit may already fail
      // the guard; the guard still bounds the preceding update's operands.
      const uint64_t Trips = (Bound - First - 1) / CountStep->second + 1;
      const int64_t Delta =
          llvm::APInt(Stack.Output.Size * 8, StackStep->second).getSExtValue();
      int64_t Change;
      if (!Delta || Trips > INT64_MAX ||
          llvm::MulOverflow(Delta, int64_t(Trips), Change))
        return {};

      std::vector<std::set<int>> Preds(Func.Blocks.size());
      for (const auto &Block : Func.Blocks) {
        if (!charge(Block.Succs.size() + Block.Preds.size() + 1))
          return {};
        std::set<int> Unique;
        for (int Succ : Block.Succs) {
          if (Succ < 0 || size_t(Succ) >= Func.Blocks.size() ||
              !Unique.insert(Succ).second)
            return {};
          Preds[Succ].insert(Block.Id);
        }
      }
      for (const auto &Block : Func.Blocks)
        if (Preds[Block.Id] !=
                std::set<int>(Block.Preds.begin(), Block.Preds.end()) ||
            Preds[Block.Id].size() != Block.Preds.size())
          return {};
      std::set<int> Body;
      std::vector<int> Pending{BackId};
      while (!Pending.empty()) {
        const int Id = Pending.back();
        Pending.pop_back();
        if (!charge() || Id == EntryId || !Id)
          return {};
        if (Id == Header->Id || !Body.insert(Id).second)
          continue;
        const auto &Block = Func.Blocks[Id];
        if (!Block.ExceptionalPreds.empty() || Block.Preds.empty() ||
            !charge(Block.Preds.size()))
          return {};
        Pending.insert(Pending.end(), Block.Preds.begin(), Block.Preds.end());
      }
      for (va_t Root : Func.ModuleAnalysisRoots) {
        if (!charge() || Root == Header->StartAddr)
          return {};
        for (int Id : Body) {
          if (!charge() || Root == Func.Blocks[Id].StartAddr)
            return {};
        }
      }
      int Continue = -1;
      bool SawTarget = false;
      for (int Succ : Header->Succs) {
        const bool Taken =
            Func.Blocks[Succ].StartAddr == Branch.Inputs[0].ConstVal;
        SawTarget |= Taken;
        if (Taken == ContinueOnTrue)
          Continue = Succ;
        if (Body.count(Succ) != size_t(Taken == ContinueOnTrue))
          return {};
      }
      if (!SawTarget || Continue < 0 || !Body.count(Continue))
        return {};
      const auto OnceOnEveryBackedge = [&](int Update) {
        if (!Body.count(Update))
          return false;
        std::set<int> Seen;
        std::vector<int> Work{BackId};
        while (!Work.empty()) {
          const int Id = Work.back();
          Work.pop_back();
          if (!charge() || Id == Header->Id)
            return false;
          if (Id == Update || !Seen.insert(Id).second)
            continue;
          if (!charge(Func.Blocks[Id].Preds.size()))
            return false;
          Work.insert(Work.end(), Func.Blocks[Id].Preds.begin(),
                      Func.Blocks[Id].Preds.end());
        }
        Seen.clear();
        Work = Func.Blocks[Update].Succs;
        while (!Work.empty()) {
          const int Id = Work.back();
          Work.pop_back();
          if (!charge() || Id == Update)
            return false;
          if (!Body.count(Id) || !Seen.insert(Id).second)
            continue;
          if (!charge(Func.Blocks[Id].Succs.size()))
            return false;
          Work.insert(Work.end(), Func.Blocks[Id].Succs.begin(),
                      Func.Blocks[Id].Succs.end());
        }
        return true;
      };
      if (!OnceOnEveryBackedge(StackStep->first->Block) ||
          !OnceOnEveryBackedge(CountStep->first->Block))
        return {};
      return CountedStackRecurrence{Stack.Args[Back ^ 1u].second,
                                    std::min(Change, int64_t{0}),
                                    std::max(Change, int64_t{0})};
    }
    return {};
  }
};

} // namespace

std::optional<CountedStackRecurrence>
countedStackRecurrence(const MedFunc &Func, const PhiNode &Stack,
                       size_t &Remaining) {
  return RecurrenceProof(Func, Remaining).prove(Stack);
}

} // namespace neverd
