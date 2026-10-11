//===- HighTypeInference.cpp - Type inference for HighIR ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Type inference passes for HighIR: float-type propagation from operations,
/// return-size deduction from MedIR, and pointer-parameter detection from
/// register usage patterns.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <set>
#include <unordered_set>

namespace neverd {

//===----------------------------------------------------------------------===//
// inferTypes — propagate float types and assign default types to locals
//===----------------------------------------------------------------------===//

void MedToHighConverter::inferTypes(HighFunc &Func) {
  std::set<int64_t> FloatStackOffs;

  auto IsFloatOp = [](NdOp Op) {
    return Op == NdOp::FLOAT_ADD || Op == NdOp::FLOAT_SUB ||
           Op == NdOp::FLOAT_MULT || Op == NdOp::FLOAT_DIV ||
           Op == NdOp::FLOAT_NEG || Op == NdOp::FLOAT_SQRT ||
           Op == NdOp::FLOAT_ABS || Op == NdOp::FLOAT_CEIL ||
           Op == NdOp::FLOAT_FLOOR || Op == NdOp::FLOAT_MIN ||
           Op == NdOp::FLOAT_MAX || Op == NdOp::FLOAT_MINNUM ||
           Op == NdOp::FLOAT_MAXNUM;
  };

  HighExprSet Seen;
  std::function<void(const HighExpr &)> ScanExpr = [&](const HighExpr &E) {
    if (!Seen.insert(&E).second)
      return;
    if ((E.Kind == ExprKind::BinOp || E.Kind == ExprKind::UnaryOp) &&
        IsFloatOp(E.Op)) {
      for (auto &Child : E.Operands) {
        if (Child && Child->Kind == ExprKind::Var &&
            Child->Var.Kind == MedVar::Stack)
          FloatStackOffs.insert(Child->Var.StackOff);
      }
    }
    for (auto &Child : E.Operands)
      if (Child)
        ScanExpr(*Child);
  };

  std::function<void(const HighStmt &)> ScanStmt = [&](const HighStmt &S) {
    auto TryScan = [&](const ExprPtr &E) {
      if (E)
        ScanExpr(*E);
    };
    TryScan(S.StoreAddr);
    TryScan(S.StoreVal);
    TryScan(S.Val);
    TryScan(S.Dst);
    TryScan(S.Cond);
    TryScan(S.RetVal);
    TryScan(S.CallExpr);
    TryScan(S.SwitchExpr);
    for (auto &C : S.Body)
      ScanStmt(C);
    for (auto &C : S.ElseBody)
      ScanStmt(C);
    for (auto &SC : S.Cases)
      for (auto &C : SC.Body)
        ScanStmt(C);
    for (auto &C : S.DefaultBody)
      ScanStmt(C);
  };

  for (auto &S : Func.Body)
    ScanStmt(S);

  std::set<int> AddrParams;
  auto Unwrap = [](const HighExpr *E) -> const HighExpr * {
    unsigned Depth = 0;
    while (E && !E->Operands.empty() && Depth++ < 8 &&
           (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
            E->Kind == ExprKind::UnaryOp))
      E = E->Operands[0].get();
    return E;
  };
  auto NoteAddress = [&](const ExprPtr &Addr) {
    const HighExpr *E = Unwrap(Addr.get());
    if (!E)
      return;
    if (E->Kind == ExprKind::Var && E->Var.Kind == MedVar::Param)
      AddrParams.insert(E->Var.Id);
    if (E->Kind == ExprKind::BinOp &&
        (E->Op == NdOp::INT_ADD || E->Op == NdOp::INT_SUB)) {
      for (const ExprPtr &Op : E->Operands) {
        const HighExpr *Leaf = Unwrap(Op.get());
        if (Leaf && Leaf->Kind == ExprKind::Var &&
            Leaf->Var.Kind == MedVar::Param)
          AddrParams.insert(Leaf->Var.Id);
      }
    }
  };
  std::function<void(const HighStmt &)> NoteStmt = [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Store)
      NoteAddress(S.StoreAddr);
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Load &&
        !S.Dst->Operands.empty())
      NoteAddress(S.Dst->Operands[0]);
    if (S.Val && S.Val->Kind == ExprKind::Load && !S.Val->Operands.empty())
      NoteAddress(S.Val->Operands[0]);
    for (const HighStmt &Inner : S.Body)
      NoteStmt(Inner);
    for (const HighStmt &Inner : S.ElseBody)
      NoteStmt(Inner);
    for (const SwitchCase &Case : S.Cases)
      for (const HighStmt &Inner : Case.Body)
        NoteStmt(Inner);
    for (const HighStmt &Inner : S.DefaultBody)
      NoteStmt(Inner);
    for (const auto &ClauseBody : S.EHClauseBodies)
      for (const HighStmt &Inner : ClauseBody)
        NoteStmt(Inner);
  };
  for (const HighStmt &S : Func.Body)
    NoteStmt(S);
  for (size_t I = 0; I < Func.Params.size(); ++I) {
    HighParam &Param = Func.Params[I];
    // Address use is a heuristic, not authority to rewrite a source ABI.
    // Native context carriers may intentionally be declared as integer bits.
    if (Func.SourceTypeHint)
      continue;
    if (Param.Type && Param.Type->Kind == NdTypeKind::Ptr)
      continue;
    if (AddrParams.count(static_cast<int>(I)))
      Param.Type = NdType::makePtr();
  }

  for (auto &Local : Func.Locals) {
    if (FloatStackOffs.count(Local.StackOff)) {
      uint16_t Sz = Local.Type ? Local.Type->Size : 4;
      Local.Type = NdType::makeFloat(Sz);
      continue;
    }
    if (!Local.Type) {
      Local.Type = NdType::makeInt(4);
    }
  }
}

//===----------------------------------------------------------------------===//
// inferReturnType — deduce the return value's type from RETURN predecessors
//===----------------------------------------------------------------------===//

TypeRef inferReturnType(const MedFunc &Med) {
  for (auto &Blk : Med.Blocks) {
    for (auto RIt = Blk.Ops.rbegin(); RIt != Blk.Ops.rend(); ++RIt) {
      if (RIt->Opcode != NdOp::RETURN)
        continue;
      for (auto RIt2 = RIt + 1; RIt2 != Blk.Ops.rend(); ++RIt2) {
        if (RIt2->Output.Kind != MedVar::Reg || RIt2->Output.RegOff != 0 ||
            RIt2->Output.Size == 0)
          continue;
        if (RIt2->Opcode == NdOp::INT_ZEXT && RIt2->NumInputs >= 1) {
          // A byte or halfword zero-extended into the register is unsigned:
          // a signed return type would sign-extend it.  A 32-bit value
          // zero-extended into a 64-bit register is how those targets write
          // any 32-bit result, signed or not.
          const uint16_t Bytes = RIt2->Inputs[0].Size;
          return Bytes < 4 ? NdType::makeInt(Bytes, /*S=*/false)
                           : NdType::makeInt(Bytes);
        }
        return NdType::makeInt(RIt2->Output.Size);
      }
    }
  }
  return NdType::makeInt(4);
}

//===----------------------------------------------------------------------===//
// detectPtrParamRegs — find register offsets used as pointer-typed params
//===----------------------------------------------------------------------===//

std::set<uint64_t> detectPtrParamRegs(const MedFunc &Med) {
  std::map<std::pair<int, int>, const MedOp *> DefMap;
  for (auto &Blk : Med.Blocks)
    for (auto &Op : Blk.Ops)
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        DefMap[{Op.Output.Id, Op.Output.SSAVer}] = &Op;

  std::set<uint64_t> PtrRegs;
  std::set<uint64_t> SegmentOffsetRegs;
  auto recordAddressRegs = [&](const MedVar &AddrVar,
                               std::set<uint64_t> &Roles) {
    if (AddrVar.Kind == MedVar::Reg && AddrVar.Id >= 0 && AddrVar.SSAVer == 0)
      Roles.insert(AddrVar.RegOff);
    if (AddrVar.Kind != MedVar::Temp || AddrVar.Id < 0)
      return;
    auto DIt = DefMap.find({AddrVar.Id, AddrVar.SSAVer});
    if (DIt == DefMap.end() || DIt->second->Opcode != NdOp::INT_ADD ||
        DIt->second->NumInputs < 2)
      return;
    for (uint8_t I = 0; I < DIt->second->NumInputs; ++I)
      if (DIt->second->Inputs[I].Kind == MedVar::Reg &&
          DIt->second->Inputs[I].SSAVer == 0)
        Roles.insert(DIt->second->Inputs[I].RegOff);
  };
  for (auto &Blk : Med.Blocks) {
    for (auto &Op : Blk.Ops) {
      if (Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::INDIR_CALL) {
        if (Op.NumInputs >= 1 && Op.Inputs[0].Kind == MedVar::Reg)
          PtrRegs.insert(Op.Inputs[0].RegOff);
      }
      const MedVar *MemoryAddress = nullptr;
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
           Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
           Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
          Op.NumInputs >= 1)
        MemoryAddress = &Op.Inputs[0];
      else if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs >= 2 &&
               Op.Inputs[0].isConst() &&
               intrinsicSupportsMemoryAddressSpace(
                   static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) &&
               !isX86StringIntrinsic(
                   static_cast<Intrinsic>(Op.Inputs[0].ConstVal)))
        MemoryAddress = &Op.Inputs[1];
      if (MemoryAddress) {
        recordAddressRegs(*MemoryAddress,
                          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default
                              ? PtrRegs
                              : SegmentOffsetRegs);
      }
    }
  }
  for (uint64_t Reg : SegmentOffsetRegs)
    PtrRegs.erase(Reg);
  return PtrRegs;
}

std::set<int> detectPtrParamIds(const MedFunc &Med) {
  std::map<std::pair<int, int>, const MedOp *> DefMap;
  for (const MedBlock &Blk : Med.Blocks)
    for (const MedOp &Op : Blk.Ops)
      if (Op.Output.Id >= 0 && Op.Output.Size > 0)
        DefMap[{Op.Output.Id, Op.Output.SSAVer}] = &Op;

  std::set<int> PtrParams;
  auto recordAddressParams = [&](const MedVar &AddrVar) {
    if (AddrVar.Kind == MedVar::Param && AddrVar.SSAVer == 0)
      PtrParams.insert(AddrVar.Id);
    if (AddrVar.Kind != MedVar::Temp || AddrVar.Id < 0)
      return;
    auto DIt = DefMap.find({AddrVar.Id, AddrVar.SSAVer});
    if (DIt == DefMap.end() || DIt->second->NumInputs < 1)
      return;
    const MedOp &Def = *DIt->second;
    if (Def.Opcode != NdOp::INT_ADD && Def.Opcode != NdOp::COPY)
      return;
    for (uint8_t I = 0; I < Def.NumInputs; ++I)
      if (Def.Inputs[I].Kind == MedVar::Param && Def.Inputs[I].SSAVer == 0)
        PtrParams.insert(Def.Inputs[I].Id);
  };
  for (const MedBlock &Blk : Med.Blocks) {
    for (const MedOp &Op : Blk.Ops) {
      const MedVar *MemoryAddress = nullptr;
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
           Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
           Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
          Op.NumInputs >= 1)
        MemoryAddress = &Op.Inputs[0];
      else if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs >= 2 &&
               Op.Inputs[0].isConst() &&
               intrinsicSupportsMemoryAddressSpace(
                   static_cast<Intrinsic>(Op.Inputs[0].ConstVal)))
        MemoryAddress = &Op.Inputs[1];
      if (MemoryAddress &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        recordAddressParams(*MemoryAddress);
    }
  }
  return PtrParams;
}

} // namespace neverd
