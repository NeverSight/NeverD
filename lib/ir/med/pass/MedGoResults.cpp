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

#include "neverd/ir/NdTypes.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/GoCallContract.h"
#include "neverd/ir/med/MedReturnValue.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"

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
  size_t Integers = 0;
  while (Integers < Fields.size() && !Fields[Integers].IsFP) {
    if (goResultPosition(A, Fields[Integers].RegOff) !=
        static_cast<int>(Integers))
      return -1;
    ++Integers;
  }
  if (Integers == 0)
    return -1;
  // The floating results the record carries follow, in Go's order.
  for (size_t I = Integers; I < Fields.size(); ++I)
    if (!Fields[I].IsFP || goFloatResultPosition(A, Fields[I].RegOff) !=
                               static_cast<int>(I - Integers))
      return -1;
  return static_cast<int>(Integers) - 1;
}

/// The register of Go's floating result at \p Position on \p A.
uint64_t goFloatResultRegister(Arch A, int Position) {
  return A == Arch::X64
             ? x86reg::VectorBase + uint64_t(Position) * x86reg::VectorRegStride
             : a64reg::V0 + uint64_t(Position) * (a64reg::V(1) - a64reg::V0);
}

/// The floating results \p Op already returns: the position of the last, or
/// -1 (returnedLast).
int returnedLastFloat(Arch A, const MedOp &Op) {
  if (returnedLast(A, Op) < 0)
    return -1;
  int Last = -1;
  for (const MedReturnReg &Field : *Op.RegisterReturnFields)
    if (Field.IsFP)
      Last = std::max(Last, goFloatResultPosition(A, Field.RegOff));
  return Last;
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

int goFloatResultPosition(Arch A, uint64_t RegOff) {
  uint64_t Base = 0, Stride = 0, Count = 0;
  if (A == Arch::X64) {
    Base = x86reg::VectorBase;
    Stride = x86reg::VectorRegStride;
    Count = 15;
  } else if (A == Arch::AArch64) {
    Base = a64reg::V0;
    Stride = a64reg::V(1) - a64reg::V0;
    Count = 16;
  } else {
    return -1;
  }
  if (RegOff < Base || (RegOff - Base) % Stride != 0 ||
      (RegOff - Base) / Stride >= Count)
    return -1;
  return static_cast<int>((RegOff - Base) / Stride);
}

std::map<uint32_t, GoCallShape>
goCallResultsRead(const MedFunc &Func, Arch A, const std::set<uint32_t> &Sites,
                  llvm::ArrayRef<MedVar> Returned, bool Floats) {
  // The result registers a reader may take past the first: Go's integer
  // ones after RAX and, with Floats, its floating ones.
  const auto ResultRegister = [&](uint64_t RegOff) {
    return goResultPosition(A, RegOff) > 0 ||
           (Floats && goFloatResultPosition(A, RegOff) >= 0);
  };
  std::map<uint32_t, GoCallShape> Shapes;
  if (Sites.empty())
    return Shapes;
  const uint16_t Word = getTargetRegInfo(A).PointerSize;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Sites.count(Op.CallSiteId))
        if (const int Returning = returnedLast(A, Op); Returning > 0) {
          GoCallShape &Shape = Shapes[Op.CallSiteId];
          Shape.Last = Returning;
          for (const MedReturnReg &Field : *Op.RegisterReturnFields)
            if (!Field.IsFP)
              Shape.Widths.push_back(Field.Size);
          Shape.LastFloat = returnedLastFloat(A, Op);
          for (const MedReturnReg &Field : *Op.RegisterReturnFields)
            if (Field.IsFP)
              Shape.FloatWidths.push_back(Field.Size);
        }
  std::map<uint32_t, std::vector<MedVar>> Clobbers;
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Sites.count(Clobber.CallSiteId) && Clobber.Value.Kind == MedVar::Reg &&
        ResultRegister(Clobber.Value.RegOff))
      Clobbers[Clobber.CallSiteId].push_back(Clobber.Value);
  if (Clobbers.empty())
    return Shapes;

  // The widths each value of a result register is read at, carried through
  // the PHIs it reaches.
  const uint16_t Whole = getTargetRegInfo(A).FullRegWidth;
  llvm::DenseMap<ValueKey, uint16_t> ReadWidths;
  // The bytes of a value the views taken of it cover.
  llvm::DenseMap<ValueKey, uint16_t> ViewWidths;
  llvm::DenseMap<ValueKey, const PhiNode *> PhiDefinitions;
  llvm::SmallVector<ValueKey, 16> Work;
  const auto Observe = [&](const MedVar &Value, uint16_t Bytes) {
    if (Value.Kind != MedVar::Reg || !ResultRegister(Value.RegOff))
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
      if (Phi.Output.Kind == MedVar::Reg && ResultRegister(Phi.Output.RegOff))
        PhiDefinitions.try_emplace(identity(Phi.Output), &Phi);
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Read = Block.Ops[I];
      if (SelfView(Read)) {
        const uint64_t Offset = Read.NumInputs >= 2 && Read.Inputs[1].isConst()
                                    ? Read.Inputs[1].ConstVal
                                    : 0;
        uint16_t &Covered = ViewWidths[identity(Read.Inputs[0])];
        Covered = static_cast<uint16_t>(
            std::max<uint64_t>(Covered, Offset + Read.Output.Size));
        continue;
      }
      if (lowtomed_detail::keepsUpperBytesOfWrite(Block.Ops, I) ||
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
  // A narrower view's PHI is a value of its own, which a write of the
  // whole register leaves stale: an arm64 `mov x11, #45` keeps W11's
  // version from before a call, so the call seemed to return W11.  An arm
  // of one is current where the whole register, at the end of the edge's
  // block, still holds that value: the clobber of the same call, or a PHI
  // of the whole register beside a PHI of the view.
  std::map<ValueKey, uint32_t> ClobberSites;
  for (const auto &[Site, Values] : Clobbers)
    for (const MedVar &Value : Values)
      ClobberSites.emplace(identity(Value), Site);
  llvm::DenseMap<int, const MedBlock *> ById;
  const auto Current = [&](int Pred, const MedVar &Input, uint64_t RegOff) {
    if (ById.empty())
      for (const MedBlock &Block : Func.Blocks)
        ById.try_emplace(Block.Id, &Block);
    const auto Block = ById.find(Pred);
    if (Block == ById.end())
      return false;
    const GoReturnedValue Holds =
        goReturnedValue(ById, *Block->second, RegOff, A);
    if (const auto Site = ClobberSites.find(identity(Input));
        Site != ClobberSites.end())
      return Holds.What == GoReturnedValue::Call &&
             Holds.Caller->CallSiteId == Site->second;
    if (PhiDefinitions.count(identity(Input)))
      return Holds.What == GoReturnedValue::Phi;
    return true;
  };
  while (!Work.empty()) {
    const ValueKey Key = Work.pop_back_val();
    const auto Found = PhiDefinitions.find(Key);
    if (Found == PhiDefinitions.end())
      continue;
    const PhiNode &Phi = *Found->second;
    const uint16_t Width = std::min(ReadWidths[Key], Phi.Output.Size);
    for (const auto &[Pred, Input] : Phi.Args)
      if (Phi.Output.Size >= Whole || Current(Pred, Input, Phi.Output.RegOff))
        Observe(Input, Width);
  }
  for (const auto &[Site, Values] : Clobbers) {
    int Last = -1, LastFloat = -1;
    for (const MedVar &Value : Values)
      if (ReadWidths.count(identity(Value))) {
        Last = std::max(Last, goResultPosition(A, Value.RegOff));
        LastFloat = std::max(LastFloat, goFloatResultPosition(A, Value.RegOff));
      }
    const auto Known = Shapes.find(Site);
    const int Returning = Known == Shapes.end() ? 0 : Known->second.Last;
    const int ReturningFloat =
        Known == Shapes.end() ? -1 : Known->second.LastFloat;
    // Go's floating results: a float32 or a float64 each, as wide as its
    // widest read takes; one no reader takes, before the last, a float64.
    if (Floats && LastFloat > ReturningFloat) {
      GoCallShape &Shape = Shapes[Site];
      if (Shape.Widths.empty())
        Shape.Widths.push_back(Word);
      Shape.Last = std::max(Shape.Last, 0);
      Shape.LastFloat = LastFloat;
      Shape.FloatWidths.resize(LastFloat + 1, 0);
      for (const MedVar &Value : Values) {
        const int Position = goFloatResultPosition(A, Value.RegOff);
        if (Position <= ReturningFloat)
          continue;
        uint16_t Bytes = 0;
        if (const auto Read = ReadWidths.find(identity(Value));
            Read != ReadWidths.end())
          Bytes = Read->second;
        Shape.FloatWidths[Position] =
            std::max(Shape.FloatWidths[Position], Bytes);
      }
      for (int Position = ReturningFloat + 1; Position <= LastFloat;
           ++Position) {
        uint16_t &Width = Shape.FloatWidths[Position];
        Width = Width == 4 ? 4 : 8;
      }
    }
    if (Last <= Returning)
      continue;
    GoCallShape &Shape = Shapes[Site];
    if (Shape.Widths.empty())
      Shape.Widths.push_back(Word);
    Shape.Widths.resize(Last + 1, 0);
    Shape.Last = Last;
    // A result is as wide as the widest read of it or view taken of it, in
    // the register widths C has; one no reader takes is a whole word.
    for (const MedVar &Value : Values) {
      const int Position = goResultPosition(A, Value.RegOff);
      if (Position <= Returning || Position > Last)
        continue;
      uint16_t Bytes = 0;
      if (const auto Read = ReadWidths.find(identity(Value));
          Read != ReadWidths.end())
        Bytes = Read->second;
      if (const auto View = ViewWidths.find(identity(Value));
          View != ViewWidths.end())
        Bytes = std::max(Bytes, View->second);
      Shape.Widths[Position] = std::max(Shape.Widths[Position], Bytes);
    }
    for (int Position = Returning + 1; Position <= Last; ++Position) {
      uint16_t &Width = Shape.Widths[Position];
      uint16_t Rounded = 1;
      while (Rounded < Width && Rounded < Word)
        Rounded *= 2;
      Width = Width ? std::min(Rounded, Word) : Word;
    }
  }
  return Shapes;
}

void defineGoCallResults(MedFunc &Func, Arch A,
                         const std::map<uint32_t, GoCallShape> &Shapes) {
  const llvm::ArrayRef<uint64_t> Results = goIntegerArgumentRegisters(A);
  if (Shapes.empty())
    return;
  const uint16_t Word = getTargetRegInfo(A).PointerSize;
  const auto Wanted = [&](const MedOp &Op) -> const GoCallShape * {
    if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
      return nullptr;
    const auto Found = Shapes.find(Op.CallSiteId);
    if (Found == Shapes.end())
      return nullptr;
    const GoCallShape &Shape = Found->second;
    const bool Floats = Shape.LastFloat >= 0;
    if ((Shape.Last <= 0 && !Floats) || Shape.Last < 0 ||
        Shape.Last >= static_cast<int>(Results.size()) ||
        Shape.Widths.size() != static_cast<size_t>(Shape.Last) + 1 ||
        Shape.Widths[0] != Word ||
        Shape.FloatWidths.size() != static_cast<size_t>(Shape.LastFloat + 1) ||
        Shape.Widths.size() + Shape.FloatWidths.size() >
            kSourceMaxRegisterReturnMembers)
      return nullptr;
    const int Returning = returnedLast(A, Op);
    if (Returning < 0 &&
        (Op.Output.Kind != MedVar::Reg || Op.Output.RegOff != Results[0]))
      return nullptr;
    // A call that returns floating results already keeps its record: the
    // integer ones it would take past them come first in it.
    if (returnedLastFloat(A, Op) >= 0)
      return nullptr;
    if (Returning >= Shape.Last && !Floats)
      return nullptr;
    // The results a call returns already are words, which the packed value
    // keeps where it has them.
    for (int Position = 0; Position <= Returning; ++Position)
      if ((*Op.RegisterReturnFields)[Position].Size != Word ||
          Shape.Widths[Position] != Word)
        return nullptr;
    return &Shape;
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
    if (Shapes.count(Clobber.CallSiteId) && Clobber.Value.Kind == MedVar::Reg &&
        (goResultPosition(A, Clobber.Value.RegOff) > 0 ||
         goFloatResultPosition(A, Clobber.Value.RegOff) >= 0))
      Left[Clobber.CallSiteId].push_back(Clobber.Value);
  // The positions each call now defines, and the packed values that grow.
  std::map<uint32_t, std::pair<int, int>> Defined;
  std::map<uint32_t, int> DefinedFloat;
  llvm::DenseMap<ValueKey, uint16_t> Widened;
  for (MedBlock &Block : Func.Blocks) {
    if (llvm::none_of(Block.Ops, [&](const MedOp &Op) { return Wanted(Op); }))
      continue;
    std::vector<MedOp> Ops;
    Ops.reserve(Block.Ops.size());
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const GoCallShape *Wants = Wanted(Block.Ops[I]);
      // The record of the results lays them out: each field naturally
      // aligned, as C declares the record (multiRegisterReturnType).
      TypeRef Record;
      if (Wants) {
        std::vector<TypeRef> Members;
        for (const uint16_t Width : Wants->Widths)
          Members.push_back(NdType::makeInt(Width, false));
        for (const uint16_t Width : Wants->FloatWidths)
          Members.push_back(NdType::makeFloat(Width));
        Record = NdType::makeStruct(std::move(Members));
        if (!Record || Record->FieldOffsets.size() !=
                           Wants->Widths.size() + Wants->FloatWidths.size())
          Wants = nullptr;
      }
      Ops.push_back(std::move(Block.Ops[I]));
      if (!Wants)
        continue;
      const int Last = Wants->Last;
      MedOp &Op = Ops.back();
      const int Returning = returnedLast(A, Op);
      const bool Fresh = Returning < 0;
      const int FirstNew = Fresh ? 0 : Returning + 1;
      const uint16_t Bytes = Record->Size;
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
        const uint16_t Width = Wants->Widths[Position];
        Shape->push_back({Register, Width, false});
        MedVar Field;
        if (Position == 0) {
          Field = First;
        } else {
          const MedVar *Exact = nullptr;
          for (const MedVar &Value : Values)
            if (Value.RegOff == Register &&
                (!Exact || (Value.Size == Width && Exact->Size != Width)))
              Exact = &Value;
          if (Exact && Exact->Size == Width) {
            Field = *Exact;
          } else {
            Field.Kind = MedVar::Reg;
            Field.TheArch = A;
            Field.RegOff = Register;
            Field.Id = NextId++;
            Field.SSAVer = NextVer++;
          }
        }
        Field.Size = Width;
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Addr = Op.Addr;
        Extract.Output = Field;
        Extract.addInput(Packed);
        Extract.addInput(
            MedVar::makeConst(Record->FieldOffsets[Position], Word));
        Fields.push_back(Extract);
        // The register at a width no reader takes past the field holds the
        // field in its low bytes; what is above them no read observes.
        for (const MedVar &Value : Values) {
          if (Value.RegOff != Register || identity(Value) == identity(Field))
            continue;
          MedOp View;
          View.Addr = Op.Addr;
          View.Output = Value;
          View.Opcode = Value.Size < Width   ? NdOp::SUBBYTES
                        : Value.Size > Width ? NdOp::INT_ZEXT
                                             : NdOp::COPY;
          View.addInput(Field);
          if (View.Opcode == NdOp::SUBBYTES)
            View.addInput(MedVar::makeConst(0, Word));
          Views.push_back(View);
        }
      }
      // The floating results follow, each from its register's low bytes.
      for (int Position = 0; Position <= Wants->LastFloat; ++Position) {
        const uint64_t Register = goFloatResultRegister(A, Position);
        const uint16_t Width = Wants->FloatWidths[Position];
        Shape->push_back({Register, Width, true});
        MedVar Field;
        const MedVar *Exact = nullptr;
        for (const MedVar &Value : Values)
          if (Value.RegOff == Register && Value.Size == Width)
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
        Field.Size = Width;
        MedOp Extract;
        Extract.Opcode = NdOp::SUBBYTES;
        Extract.Addr = Op.Addr;
        Extract.Output = Field;
        Extract.addInput(Packed);
        Extract.addInput(MedVar::makeConst(
            Record->FieldOffsets[Wants->Widths.size() + Position], Word));
        Fields.push_back(Extract);
        for (const MedVar &Value : Values) {
          if (Value.RegOff != Register || identity(Value) == identity(Field))
            continue;
          MedOp View;
          View.Addr = Op.Addr;
          View.Output = Value;
          View.Opcode = Value.Size < Width   ? NdOp::SUBBYTES
                        : Value.Size > Width ? NdOp::INT_ZEXT
                                             : NdOp::COPY;
          View.addInput(Field);
          if (View.Opcode == NdOp::SUBBYTES)
            View.addInput(MedVar::makeConst(0, Word));
          Views.push_back(View);
        }
      }
      if (Wants->LastFloat >= 0)
        DefinedFloat[Site] = Wants->LastFloat;
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
    if (Clobber.Value.Kind == MedVar::Reg)
      if (const auto Floats = DefinedFloat.find(Clobber.CallSiteId);
          Floats != DefinedFloat.end()) {
        const int Position = goFloatResultPosition(A, Clobber.Value.RegOff);
        if (Position >= 0 && Position <= Floats->second)
          return true;
      }
    const auto Range = Defined.find(Clobber.CallSiteId);
    if (Range == Defined.end() || Clobber.Value.Kind != MedVar::Reg)
      return false;
    const int Position = goResultPosition(A, Clobber.Value.RegOff);
    return Position > 0 && Position >= Range->second.first &&
           Position <= Range->second.second;
  });
}

} // namespace neverd
