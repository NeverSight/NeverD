//===- NdOpReturnLowering.cpp - RETURN lowering to HighIR -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// RETURN statement lowering: return-value recovery from registers, call
/// outputs, phi nodes, and predecessor blocks.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

namespace neverd {

namespace {
/// A bare RET does not syntactically read the return-value register. A runtime
/// declaration can still describe a source value occupying that incoming ABI
/// slot (notably Objective-C self on AArch64). Bind it only when the complete
/// function leaves that carrier untouched; a call, partial write, or PHI makes
/// this fallback unavailable. This is a source hint, never safety evidence.
ExprPtr unchangedDeclaredReturnParameter(const MedFunc &Med,
                                         const TargetRegInfo &TRI,
                                         uint64_t ReturnReg) {
  if (!Med.SourceTypeHint || !Med.SourceTypeHint->ReturnType ||
      Med.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void)
    return nullptr;
  auto Overlaps = [&](const MedVar &Value) {
    if (Value.Kind != MedVar::Reg || !Value.Size)
      return false;
    return Value.RegOff >= ReturnReg
               ? Value.RegOff - ReturnReg < TRI.FullRegWidth
               : ReturnReg - Value.RegOff < Value.Size;
  };
  for (const auto &Block : Med.Blocks) {
    for (const auto &Phi : Block.Phis)
      if (Overlaps(Phi.Output))
        return nullptr;
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC)
        return nullptr;
      if (!Overlaps(Op.Output))
        continue;
      const bool SelfCopy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                            Op.Inputs[0] == Op.Output &&
                            Op.Inputs[0].RegOff == Op.Output.RegOff &&
                            Op.Inputs[0].Size == Op.Output.Size;
      if (!SelfCopy)
        return nullptr;
    }
  }
  for (size_t I = 0; I < Med.Params.size(); ++I) {
    if (Med.Params[I].RegOff != ReturnReg || I >= Med.TypedParams.size())
      continue;
    MedVar Parameter = Med.Params[I];
    Parameter.Kind = MedVar::Param;
    Parameter.Id = static_cast<int>(I);
    return HighExpr::makeVar(Parameter, Med.TypedParams[I].Type);
  }
  return nullptr;
}
} // namespace

void MedToHighConverter::lowerReturn(HighFunc &Func, const MedBlock &CurBlock,
                                     const MedOp &CurOp, const MedFunc &Med) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.Addr = CurOp.Addr;

  ExprPtr RetVal;
  const auto &TRI = getTargetRegInfo(TargetArch);
  const bool UsesFPReturnReg = Func.ReturnType &&
                               Func.ReturnType->Kind == NdTypeKind::Float &&
                               TRI.hasFPReturnReg() && !Med.FPReturnViaX87;
  const uint64_t ReturnReg =
      UsesFPReturnReg ? TRI.FPReturnReg : TRI.IntReturnReg;

  if (CurOp.NumInputs >= 1 && CurOp.Inputs[0].Id >= 0 &&
      CurOp.Inputs[0].Kind == MedVar::Reg) {
    uint64_t RO = CurOp.Inputs[0].RegOff;
    if (!TRI.isFrameOrLinkReg(RO))
      RetVal = medvarToExpr(CurOp.Inputs[0]);
  }

  if (!RetVal) {
    for (auto RIt = CurBlock.Ops.rbegin(); RIt != CurBlock.Ops.rend(); ++RIt) {
      if (RIt->Opcode == NdOp::RETURN)
        continue;
      if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
          RIt->Output.RegOff == ReturnReg) {
        if (RIt->Opcode == NdOp::CALL || RIt->Opcode == NdOp::INDIR_CALL ||
            RIt->Opcode == NdOp::INTRINSIC)
          RetVal = HighExpr::makeVar(RIt->Output);
        else if (RIt->Opcode == NdOp::LOAD ||
                 RIt->MemoryOrdering != NdMemoryOrdering::None ||
                 RIt->MemoryAddressSpace != NdMemoryAddressSpace::Default)
          RetVal = HighExpr::makeVar(RIt->Output);
        else {
          bool FromCallind = false;
          if (RIt->NumInputs >= 1) {
            auto InKey =
                std::make_pair(RIt->Inputs[0].Id, RIt->Inputs[0].SSAVer);
            if (CallOutputs.count(InKey)) {
              FromCallind = true;
              for (auto SIt = Func.Body.rbegin(); SIt != Func.Body.rend();
                   ++SIt) {
                if (SIt->Kind == StmtKind::Call && SIt->CallExpr) {
                  RetVal = SIt->CallExpr;
                  Func.Body.erase(std::next(SIt).base());
                  break;
                }
              }
            }
          }
          if (!FromCallind)
            RetVal = medOpToExpr(*RIt);
        }
        if (RetVal)
          RetVal = forceInlineExpr(RetVal);
        break;
      }
    }
  }

  if (!RetVal) {
    for (auto &Phi : CurBlock.Phis) {
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.RegOff == ReturnReg) {
        RetVal = HighExpr::makeVar(Phi.Output);
        break;
      }
    }
  }

  if (!RetVal) {
    for (int PI : CurBlock.Preds) {
      if (PI < 0 || PI >= static_cast<int>(Med.Blocks.size()))
        continue;
      auto &Pred = Med.Blocks[PI];
      for (auto RIt = Pred.Ops.rbegin(); RIt != Pred.Ops.rend(); ++RIt) {
        if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
            RIt->Output.RegOff == ReturnReg) {
          RetVal = medvarToExpr(RIt->Output);
          goto FoundRet;
        }
      }
    }
  FoundRet:;
  }

  if (!RetVal)
    RetVal = unchangedDeclaredReturnParameter(Med, TRI, ReturnReg);

  if (!RetVal) {
    uint16_t RetSz = Func.ReturnType && Func.ReturnType->Size
                         ? Func.ReturnType->Size
                         : TRI.FullRegWidth;
    MedVar RV;
    RV.Kind = MedVar::Reg;
    RV.RegOff = ReturnReg;
    RV.Size = RetSz;
    RV.Id = -1;
    RV.SSAVer = 0;
    RetVal = HighExpr::makeVar(RV);
  }

  S.RetVal = RetVal;
  Func.Body.push_back(std::move(S));
}

} // namespace neverd
