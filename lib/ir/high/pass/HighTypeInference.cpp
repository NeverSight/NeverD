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

  std::unordered_set<const HighExpr *> Seen;
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
// inferReturnSize — deduce return value width from RETURN predecessors
//===----------------------------------------------------------------------===//

uint16_t inferReturnSize(const MedFunc &Med) {
  for (auto &Blk : Med.Blocks) {
    for (auto RIt = Blk.Ops.rbegin(); RIt != Blk.Ops.rend(); ++RIt) {
      if (RIt->Opcode != NdOp::RETURN)
        continue;
      for (auto RIt2 = RIt + 1; RIt2 != Blk.Ops.rend(); ++RIt2) {
        if (RIt2->Output.Kind != MedVar::Reg || RIt2->Output.RegOff != 0 ||
            RIt2->Output.Size == 0)
          continue;
        if (RIt2->Opcode == NdOp::INT_ZEXT && RIt2->NumInputs >= 1)
          return RIt2->Inputs[0].Size;
        return RIt2->Output.Size;
      }
    }
  }
  return 4;
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
    if (AddrVar.Kind == MedVar::Reg && AddrVar.Id >= 0 &&
        AddrVar.SSAVer == 0)
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
           Op.Opcode == NdOp::ATOMIC_XCHG ||
           Op.Opcode == NdOp::ATOMIC_ADD ||
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
        recordAddressRegs(
            *MemoryAddress,
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

} // namespace neverd
