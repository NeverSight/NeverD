//===- PipelineWideIntReturnEvidence.cpp - Register-pair evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Requires a high-word carrier on every return path, including independent
/// roots, before promoting a 32-bit function to a register-pair result.
///
//===----------------------------------------------------------------------===//

#include "PipelineWideIntReturnDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedReturnArguments.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;
ValueKey key(const MedVar &Value) {
  return {Value.Kind, Value.Id, Value.SSAVer};
}

bool isEntryIdentity(const MedOp &Op) {
  return Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
         Op.Output == Op.Inputs[0];
}

class PairReturnEvidence {
  const MedFunc &Func;
  const TargetRegInfo &TRI;
  const int EntryBlock;
  const bool ShiftReturnsInputOnZero;
  std::map<int, const MedBlock *> Blocks;
  std::map<ValueKey, std::pair<const MedOp *, int>> Definitions;
  std::map<ValueKey, std::pair<const PhiNode *, int>> Phis;

  bool isIncomingWord(const MedVar &V, uint64_t Reg) const {
    if (V.Kind != MedVar::Reg || V.RegOff != Reg || V.Size != TRI.PointerSize)
      return false;
    const auto It = Definitions.find(key(V));
    return It == Definitions.end()
               ? V.SSAVer == 0 && !Phis.count(key(V))
               : It->second.second == EntryBlock &&
                     isIncomingRegisterCopy(*It->second.first);
  }

  bool testsZeroCount(const MedOp &Branch, bool Taken) const {
    auto Definition = [&](const MedVar &V) -> const MedOp * {
      const auto It = Definitions.find(key(V));
      return It == Definitions.end() ? nullptr : It->second.first;
    };
    const MedOp *Cmp = Definition(Branch.Inputs[1]);
    if (Cmp && Cmp->Opcode == NdOp::BOOL_NOT && Cmp->NumInputs == 1) {
      Taken = !Taken;
      Cmp = Definition(Cmp->Inputs[0]);
    }
    if (!Cmp || Cmp->NumInputs != 2 ||
        Cmp->Opcode != (Taken ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL))
      return false;
    const MedVar *Count = &Cmp->Inputs[0], *Zero = &Cmp->Inputs[1];
    if (Count->isConst())
      std::swap(Count, Zero);
    return Zero->isConst() && Zero->ConstVal == 0 &&
           Zero->Size == Count->Size &&
           isIncomingWord(*Count, TRI.IntParamRegs[2]);
  }

  bool dominatesEdge(int Definition, int Predecessor) const {
    std::vector<int> Pending{Predecessor};
    std::set<int> Seen;
    bool ReachedDefinition = false;
    while (!Pending.empty()) {
      const int Id = Pending.back();
      Pending.pop_back();
      if (Id == Definition) {
        ReachedDefinition = true;
        continue;
      }
      if (!Seen.insert(Id).second)
        continue;
      const auto It = Blocks.find(Id);
      if (It == Blocks.end() || Id == EntryBlock || It->second->Preds.empty() ||
          !It->second->ExceptionalPreds.empty())
        return false;
      Pending.insert(Pending.end(), It->second->Preds.begin(),
                     It->second->Preds.end());
    }
    return ReachedDefinition;
  }

  bool phiHasDefinedInputs(const PhiNode &Phi, int BlockId,
                           bool MayForwardInput) const {
    const PhiNode *FirstWord = nullptr;
    if (MayForwardInput)
      for (const PhiNode &P : Blocks.at(BlockId)->Phis)
        if (P.Output.Kind == MedVar::Reg &&
            P.Output.RegOff == TRI.IntReturnReg &&
            P.Output.Size == TRI.PointerSize && !P.ExceptionalEntry)
          FirstWord = &P;
    std::vector<std::pair<MedVar, int>> Pending{{Phi.Output, BlockId}};
    std::set<ValueKey> Seen;
    bool HasDefinition = false;
    while (!Pending.empty()) {
      const auto [Value, Predecessor] = Pending.back();
      Pending.pop_back();
      if (Value.Size < TRI.PointerSize)
        return false;
      if (Value.isConst()) {
        HasDefinition = true;
        continue;
      }
      if (const auto It = Phis.find(key(Value)); It != Phis.end()) {
        const auto [InputPhi, Owner] = It->second;
        if (!dominatesEdge(Owner, Predecessor) || InputPhi->Args.empty() ||
            InputPhi->ExceptionalEntry)
          return false;
        if (!Seen.insert(key(Value)).second)
          continue;
        for (const auto &[Pred, Input] : InputPhi->Args) {
          if (FirstWord && InputPhi == &Phi &&
              isIncomingWord(Input, TRI.IntReturnReg2) &&
              llvm::any_of(FirstWord->Args, [&](const auto &Arg) {
                return Arg.first == Pred &&
                       isIncomingWord(Arg.second, TRI.IntReturnReg);
              })) {
            const auto It = Blocks.find(Pred);
            if (It != Blocks.end() &&
                forwardsInputOnZero(*It->second, It->second->Ops.size(),
                                    Blocks.at(Owner)))
              continue;
          }
          Pending.push_back({Input, Pred});
        }
        continue;
      }
      const auto It = Definitions.find(key(Value));
      if (It == Definitions.end() ||
          !dominatesEdge(It->second.second, Predecessor))
        return false;
      const MedOp &Definition = *It->second.first;
      if (isEntryIdentity(Definition) ||
          (Definition.Opcode == NdOp::COPY && Definition.NumInputs == 1 &&
           Definition.Inputs[0].Kind == MedVar::Unspecified))
        return false;
      HasDefinition = true;
    }
    return HasDefinition;
  }

public:
  PairReturnEvidence(const MedFunc &Func, const TargetRegInfo &TRI,
                     bool ShiftReturnsInputOnZero)
      : Func(Func), TRI(TRI),
        EntryBlock(Func.Blocks.empty() ? -1 : Func.Blocks.front().Id),
        ShiftReturnsInputOnZero(ShiftReturnsInputOnZero &&
                                TRI.IntParamRegs.size() >= 3 &&
                                TRI.IntParamRegs[0] == TRI.IntReturnReg &&
                                TRI.IntParamRegs[1] == TRI.IntReturnReg2) {
    for (const MedBlock &Block : Func.Blocks) {
      Blocks.emplace(Block.Id, &Block);
      for (const MedOp &Op : Block.Ops)
        if (Op.Output.Size)
          Definitions.emplace(key(Op.Output), std::pair{&Op, Block.Id});
      for (const PhiNode &Phi : Block.Phis)
        Phis.emplace(key(Phi.Output), std::pair{&Phi, Block.Id});
    }
  }

  // RV32 branches to a shared return. PPC32 can place a conditional return
  // in the block itself. Both must test the incoming count and preserve both
  // argument words along the returning edge.
  bool forwardsInputOnZero(const MedBlock &Block, size_t End,
                           const MedBlock *Next = nullptr) const {
    if (!ShiftReturnsInputOnZero)
      return false;
    while (End && Block.Ops[End - 1].Opcode != NdOp::COND_BR) {
      if (Next)
        return false;
      --End;
    }
    if (!End)
      return false;
    const MedOp &Branch = Block.Ops[End - 1];
    if (Branch.NumInputs != 2 || !Branch.Inputs[0].isConst())
      return false;
    const MedBlock *Taken = nullptr;
    for (int Id : Block.Succs) {
      const auto It = Blocks.find(Id);
      if (It != Blocks.end() && It->second != &Block &&
          It->second->StartAddr == Branch.Inputs[0].ConstVal) {
        if (Taken)
          return false;
        Taken = It->second;
      }
    }
    if (!Taken ||
        (Next && (Block.Succs.size() != 2 ||
                  std::find(Block.Succs.begin(), Block.Succs.end(), Next->Id) ==
                      Block.Succs.end())) ||
        !testsZeroCount(Branch, Next == Taken))
      return false;
    return registerUntouchedFromEntry(Func, Block, TRI.IntReturnReg,
                                      TRI.PointerSize, TRI.TheArch) &&
           registerUntouchedFromEntry(Func, Block, TRI.IntReturnReg2,
                                      TRI.PointerSize, TRI.TheArch);
  }

  bool highWordReaches(const MedBlock &ReturnBlock, size_t ReturnIndex) const {
    const MedBlock *Block = &ReturnBlock;
    size_t End = ReturnIndex;
    std::set<int> Seen;
    while (Seen.insert(Block->Id).second) {
      for (size_t I = End; I-- > 0;) {
        const MedOp &Op = Block->Ops[I];
        if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
            !Op.PreservesCallerSaved)
          return false;
        if (Op.Output.Kind != MedVar::Reg ||
            Op.Output.RegOff != TRI.IntReturnReg2 || !Op.Output.Size)
          continue;
        const bool HighSlice = Op.Opcode == NdOp::SUBBYTES &&
                               Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
                               Op.Inputs[1].ConstVal == TRI.PointerSize;
        if (Op.Opcode == NdOp::SUBBYTES && !HighSlice)
          continue;
        if (Op.Output.Size < TRI.PointerSize || isEntryIdentity(Op) ||
            (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
             Op.Inputs[0].Kind == MedVar::Unspecified))
          return false;
        // A pointer calculation can leave its base in the high register.
        // Reading that base to form the low result is not a second result.
        for (size_t J = I + 1; J < End; ++J) {
          const MedOp &Use = Block->Ops[J];
          if (Use.Output.Kind != MedVar::Reg ||
              Use.Output.RegOff != TRI.IntReturnReg)
            continue;
          for (uint8_t K = 0; K < Use.NumInputs; ++K)
            if (Use.Inputs[K] == Op.Output)
              return false;
        }
        return true;
      }
      for (const PhiNode &Phi : Block->Phis)
        if (Phi.Output.Kind == MedVar::Reg &&
            Phi.Output.RegOff == TRI.IntReturnReg2 &&
            Phi.Output.Size >= TRI.PointerSize)
          return phiHasDefinedInputs(
              Phi, Block->Id,
              ShiftReturnsInputOnZero && Block == &ReturnBlock &&
                  std::none_of(Block->Ops.begin(), Block->Ops.begin() + End,
                               [&](const MedOp &Op) {
                                 return Op.Output.Kind == MedVar::Reg &&
                                        Op.Output.RegOff == TRI.IntReturnReg &&
                                        Op.Output.Size;
                               }));
      if (Block->Preds.size() != 1 || !Block->ExceptionalPreds.empty())
        return false;
      const auto Pred = Blocks.find(Block->Preds.front());
      if (Pred == Blocks.end())
        return false;
      Block = Pred->second;
      End = Block->Ops.size();
    }
    return false;
  }
};

} // namespace

bool returnsRegisterPairI64(
    const MedFunc &Func, const TargetRegInfo &TRI,
    llvm::function_ref<bool(const MedOp &)> TailReturnsPair,
    bool ShiftReturnsInputOnZero) {
  if (TRI.PointerSize != 4 || !TRI.IntReturnReg2)
    return false;
  PairReturnEvidence Evidence(Func, TRI, ShiftReturnsInputOnZero);
  bool SawReturn = false;
  for (const MedBlock &Block : Func.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Return = Block.Ops[I];
      if (Return.Opcode != NdOp::RETURN)
        continue;
      SawReturn = true;
      if (Evidence.highWordReaches(Block, I) ||
          Evidence.forwardsInputOnZero(Block, I))
        continue;
      if (!I)
        return false;
      const MedOp &Call = Block.Ops[I - 1];
      if ((Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
          Call.Addr != Return.Addr || !TailReturnsPair(Call))
        return false;
    }
  return SawReturn;
}

bool returnsRegisterPairI64(const MedFunc &Func, const TargetRegInfo &TRI) {
  return returnsRegisterPairI64(Func, TRI, [](const MedOp &) { return false; });
}

} // namespace neverd
