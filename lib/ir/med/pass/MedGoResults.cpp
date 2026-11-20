//===- MedGoResults.cpp - Go register ABI call results --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The results a call under Go's internal register ABI returns past its
/// first: which of them a function reads, and the rewrite that returns them.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedGoResults.h"

#include "../lower/LowToMedCallReturnDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/med/MedReturnValue.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

namespace {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;

ValueKey identity(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

/// A call's results already returned by Go's rule: RAX first, then the
/// registers after it in order.
int returnedLast(Arch A, const MedOp &Op) {
  if (!Op.RegisterReturnFields || Op.RegisterReturnFields->empty())
    return -1;
  const auto &Fields = *Op.RegisterReturnFields;
  for (size_t I = 0; I < Fields.size(); ++I)
    if (Fields[I].IsFP ||
        goResultPosition(A, Fields[I].RegOff) != static_cast<int>(I))
      return -1;
  return static_cast<int>(Fields.size()) - 1;
}

} // namespace

GoReturnedValue
goReturnedValue(const llvm::DenseMap<int, const MedBlock *> &ById,
                const MedBlock &Block, uint64_t RegOff, Arch A) {
  GoReturnedValue Found;
  const uint16_t Width = getTargetRegInfo(A).FullRegWidth;
  std::set<int> Seen;
  llvm::SmallVector<const MedBlock *, 8> Work{&Block};
  while (!Work.empty()) {
    const MedBlock *B = Work.pop_back_val();
    if (!Seen.insert(B->Id).second)
      continue;
    for (auto It = B->Ops.rbegin(); It != B->Ops.rend(); ++It) {
      if (It->Opcode != NdOp::RETURN && It->Output.Kind == MedVar::Reg &&
          It->Output.RegOff == RegOff && It->Output.Size == Width) {
        Found.What = It->Opcode == NdOp::COPY && It->NumInputs == 1 &&
                             identity(It->Inputs[0]) == identity(It->Output)
                         ? GoReturnedValue::Entry
                         : GoReturnedValue::Defined;
        Found.Value = &It->Output;
        return Found;
      }
      if ((It->Opcode == NdOp::CALL || It->Opcode == NdOp::INDIR_CALL) &&
          !It->PreservesCallerSaved &&
          !callPreservesRegisterBytes(A, It->CallPreservedGPRs, RegOff,
                                      Width)) {
        Found.What = GoReturnedValue::Call;
        Found.Caller = &*It;
        return Found;
      }
    }
    if (const PhiNode *Phi = aggregateReturnPhi(*B, RegOff);
        Phi && Phi->Output.Size == Width) {
      Found.What = GoReturnedValue::Phi;
      Found.Value = &Phi->Output;
      return Found;
    }
    if (B->Preds.empty()) {
      Found.What = GoReturnedValue::Entry;
      return Found;
    }
    // A loop's back edge leads only around the loop: try every edge.
    for (auto Pred = B->Preds.rbegin(); Pred != B->Preds.rend(); ++Pred)
      if (const auto It = ById.find(*Pred); It != ById.end())
        Work.push_back(It->second);
  }
  return Found;
}

int goResultPosition(Arch A, uint64_t RegOff) {
  const llvm::ArrayRef<uint64_t> Results = goIntegerArgumentRegisters(A);
  const auto *It = llvm::find(Results, RegOff);
  return It == Results.end() ? -1 : static_cast<int>(It - Results.begin());
}

std::map<uint32_t, int> goCallResultsRead(const MedFunc &Func, Arch A,
                                          const std::set<uint32_t> &Sites,
                                          llvm::ArrayRef<MedVar> Returned) {
  std::map<uint32_t, int> Last;
  if (Sites.empty())
    return Last;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Sites.count(Op.CallSiteId))
        if (const int Returning = returnedLast(A, Op); Returning > 0)
          Last[Op.CallSiteId] = Returning;
  std::map<uint32_t, std::vector<MedVar>> Clobbers;
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Sites.count(Clobber.CallSiteId) && Clobber.Value.Kind == MedVar::Reg &&
        goResultPosition(A, Clobber.Value.RegOff) > 0)
      Clobbers[Clobber.CallSiteId].push_back(Clobber.Value);
  if (Clobbers.empty())
    return Last;

  // The widths each value of a result register is read at, carried through
  // the PHIs it reaches.
  llvm::DenseMap<ValueKey, uint16_t> ReadWidths;
  llvm::DenseMap<ValueKey, const PhiNode *> PhiDefinitions;
  llvm::SmallVector<ValueKey, 16> Work;
  const auto Observe = [&](const MedVar &Value, uint16_t Bytes) {
    if (Value.Kind != MedVar::Reg || goResultPosition(A, Value.RegOff) <= 0)
      return;
    const ValueKey Key = identity(Value);
    Bytes = std::min(Bytes, Value.Size);
    uint16_t &Width = ReadWidths[Key];
    if (Bytes > Width) {
      Width = Bytes;
      Work.push_back(Key);
    }
  };
  const auto SelfView = [](const MedOp &Op) {
    return Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 1 &&
           Op.Inputs[0].Kind == MedVar::Reg &&
           Op.Inputs[0].RegOff == Op.Output.RegOff;
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      if (Phi.Output.Kind == MedVar::Reg &&
          goResultPosition(A, Phi.Output.RegOff) > 0)
        PhiDefinitions.try_emplace(identity(Phi.Output), &Phi);
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Read = Block.Ops[I];
      if (SelfView(Read) ||
          lowtomed_detail::keepsUpperBytesOfWrite(Block.Ops, I) ||
          ((Read.Opcode == NdOp::INT_XOR || Read.Opcode == NdOp::INT_SUB) &&
           Read.NumInputs == 2 && Read.Inputs[0].Kind == Read.Inputs[1].Kind &&
           Read.Inputs[0].Id == Read.Inputs[1].Id &&
           Read.Inputs[0].SSAVer == Read.Inputs[1].SSAVer))
        continue;
      for (uint8_t J = 0; J < Read.NumInputs; ++J)
        Observe(Read.Inputs[J], Read.Inputs[J].Size);
    }
  }
  for (const MedVar &Value : Returned)
    Observe(Value, Value.Size);
  while (!Work.empty()) {
    const ValueKey Key = Work.pop_back_val();
    const auto Found = PhiDefinitions.find(Key);
    if (Found == PhiDefinitions.end())
      continue;
    const PhiNode &Phi = *Found->second;
    const uint16_t Width = std::min(ReadWidths[Key], Phi.Output.Size);
    for (const auto &[Pred, Input] : Phi.Args)
      Observe(Input, Width);
  }
  for (const auto &[Site, Values] : Clobbers)
    for (const MedVar &Value : Values)
      if (ReadWidths.count(identity(Value))) {
        int &Position = Last[Site];
        Position = std::max(Position, goResultPosition(A, Value.RegOff));
      }
  return Last;
}

void defineGoCallResults(MedFunc &Func, Arch A,
                         const std::map<uint32_t, int> &Lasts) {
  const llvm::ArrayRef<uint64_t> Results = goIntegerArgumentRegisters(A);
  if (Lasts.empty())
    return;
  const auto Wanted = [&](const MedOp &Op) -> int {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      return 0;
    const auto Found = Lasts.find(Op.CallSiteId);
    if (Found == Lasts.end() || Found->second <= 0 ||
        Found->second >= static_cast<int>(Results.size()))
      return 0;
    const int Returning = returnedLast(A, Op);
    if (Returning < 0 &&
        (Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != Results[0]))
      return 0;
    return Returning < Found->second ? Found->second : 0;
  };

  int NextVer = 1, NextId = 1;
  const auto Bump = [&](const MedVar &V) {
    NextVer = std::max(NextVer, V.SSAVer + 1);
    NextId = std::max(NextId, V.Id + 1);
  };
  for (const MedBlock &B : Func.Blocks) {
    for (const MedOp &O : B.Ops) {
      Bump(O.Output);
      for (uint8_t I = 0; I < O.NumInputs; ++I)
        Bump(O.Inputs[I]);
    }
    for (const PhiNode &Phi : B.Phis) {
      Bump(Phi.Output);
      for (const auto &[Pred, Input] : Phi.Args)
        Bump(Input);
    }
  }
  std::map<uint32_t, std::vector<MedVar>> Left;
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Lasts.count(Clobber.CallSiteId) && Clobber.Value.Kind == MedVar::Reg &&
        goResultPosition(A, Clobber.Value.RegOff) > 0)
      Left[Clobber.CallSiteId].push_back(Clobber.Value);

  const uint16_t Word = getTargetRegInfo(A).PointerSize;
  // The positions each call now defines, and the packed values that grow.
  std::map<uint32_t, std::pair<int, int>> Defined;
  llvm::DenseMap<ValueKey, uint16_t> Widened;
  for (MedBlock &Block : Func.Blocks) {
    if (llvm::none_of(Block.Ops, [&](const MedOp &Op) { return Wanted(Op); }))
      continue;
    std::vector<MedOp> Ops;
    Ops.reserve(Block.Ops.size());
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const int Last = Wanted(Block.Ops[I]);
      Ops.push_back(std::move(Block.Ops[I]));
      if (!Last)
        continue;
      MedOp &Op = Ops.back();
      const int Returning = returnedLast(A, Op);
      const bool Fresh = Returning < 0;
      const int FirstNew = Fresh ? 0 : Returning + 1;
      const uint16_t Bytes = static_cast<uint16_t>(Word * (Last + 1));
      // The fields a call returns follow it together, before the views the
      // narrower reads take of them: a call's shape is read from that run
      // (recoverStructReturnFromCallers).  A call that returns some already
      // gives up its run to the whole one.
      std::vector<MedOp> Fields, Views;
      MedVar Packed;
      if (Fresh) {
        Packed.Kind = MedVar::Temp;
        Packed.TheArch = A;
        Packed.Id = NextId++;
        Packed.SSAVer = 0;
      } else {
        Packed = Op.Output;
        Widened[identity(Packed)] = Bytes;
        std::set<ValueKey> Defines;
        while (I + 1 < Block.Ops.size() && Block.Ops[I + 1].Addr == Op.Addr) {
          MedOp &Next = Block.Ops[I + 1];
          if (Next.Opcode == NdOp::SUBBYTES && Next.NumInputs == 2 &&
              identity(Next.Inputs[0]) == identity(Packed)) {
            Defines.insert(identity(Next.Output));
            Fields.push_back(std::move(Next));
          } else if (Next.NumInputs >= 1 &&
                     Defines.count(identity(Next.Inputs[0]))) {
            Views.push_back(std::move(Next));
          } else {
            break;
          }
          ++I;
        }
      }
      Packed.Size = Bytes;
      const MedVar First = Op.Output;
      Op.Output = Packed;
      const uint32_t Site = Op.CallSiteId;
      Defined[Site] = {FirstNew, Last};
      const std::vector<MedVar> &Values = Left[Site];
      auto Shape = std::make_shared<std::vector<MedReturnReg>>(
          Fresh ? std::vector<MedReturnReg>{} : *Op.RegisterReturnFields);
      for (int Position = FirstNew; Position <= Last; ++Position) {
        const uint64_t Register = Results[Position];
        Shape->push_back({Register, Word, false});
        MedVar Field;
        if (Position == 0) {
          Field = First;
        } else {
          const MedVar *Exact = nullptr;
          for (const MedVar &Value : Values)
            if (Value.RegOff == Register && (!Exact || Value.Size == Word))
              Exact = &Value;
          if (Exact) {
            Field = *Exact;
          } else {
            Field.Kind = MedVar::Reg;
            Field.TheArch = A;
            Field.RegOff = Register;
            Field.Id = NextId++;
            Field.SSAVer = NextVer++;
          }
        }
        Field.Size = Word;
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Addr = Op.Addr;
        Extract.Output = Field;
        Extract.addInput(Packed);
        Extract.addInput(MedVar::makeConst(Word * Position, Word));
        Fields.push_back(Extract);
        for (const MedVar &Value : Values) {
          if (Value.RegOff != Register || identity(Value) == identity(Field))
            continue;
          MedOp View;
          View.Addr = Op.Addr;
          View.Output = Value;
          View.Opcode = Value.Size < Word   ? NdOp::SUBBYTES
                        : Value.Size > Word ? NdOp::INT_ZEXT
                                            : NdOp::COPY;
          View.addInput(Field);
          if (View.Opcode == NdOp::SUBBYTES)
            View.addInput(MedVar::makeConst(0, Word));
          Views.push_back(View);
        }
      }
      Op.RegisterReturnFields = std::move(Shape);
      for (MedOp &Field : Fields)
        Ops.push_back(std::move(Field));
      for (MedOp &View : Views)
        Ops.push_back(std::move(View));
    }
    Block.Ops = std::move(Ops);
  }
  if (!Widened.empty())
    for (MedBlock &B : Func.Blocks)
      for (MedOp &O : B.Ops)
        for (uint8_t I = 0; I < O.NumInputs; ++I)
          if (const auto Grown = Widened.find(identity(O.Inputs[I]));
              Grown != Widened.end())
            O.Inputs[I].Size = Grown->second;
  llvm::erase_if(Func.CallClobbers, [&](const MedCallClobber &Clobber) {
    const auto Range = Defined.find(Clobber.CallSiteId);
    if (Range == Defined.end() || Clobber.Value.Kind != MedVar::Reg)
      return false;
    const int Position = goResultPosition(A, Clobber.Value.RegOff);
    return Position > 0 && Position >= Range->second.first &&
           Position <= Range->second.second;
  });
}

} // namespace neverd
