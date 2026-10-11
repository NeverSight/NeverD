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

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedReturnValue.h"

#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>

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

/// A SUBBYTES that re-views the low bytes of the register it writes (`w0`
/// from `x0` after `add w0`, kept for a loop that reads w0 again) is a view
/// of the register's value, not its definition: a result wider than the view
/// is the register it was taken from.
bool isRegisterLowView(const MedOp &Op) {
  return Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 1 &&
         Op.Inputs[0].Kind == MedVar::Reg &&
         isSameRegisterLowSlice(Op.Output.RegOff, Op.Output.Size,
                                Op.Inputs[0].RegOff, Op.Inputs[0].Size,
                                Op.NumInputs < 2 || !Op.Inputs[1].isConst()
                                    ? 0
                                    : Op.Inputs[1].ConstVal);
}

const MedBlock *onlyPredecessor(const MedFunc &Med, const MedBlock &Block) {
  if (Block.Preds.size() != 1)
    return nullptr;
  const int Id = Block.Preds.front();
  auto It = std::find_if(
      Med.Blocks.begin(), Med.Blocks.end(),
      [Id](const MedBlock &Candidate) { return Candidate.Id == Id; });
  return It == Med.Blocks.end() ? nullptr : &*It;
}
// Whether \p V is the value a register holds at entry to \p Med: named by
// the identity copy that begins a function, or defined nowhere.
bool isEntryIdentity(const MedFunc &Med, const MedVar &V) {
  auto Same = [&](const MedVar &W) {
    return W.Kind == V.Kind && W.RegOff == V.RegOff && W.Id == V.Id &&
           W.SSAVer == V.SSAVer;
  };
  for (const auto &Blk : Med.Blocks) {
    for (const auto &Phi : Blk.Phis)
      if (Same(Phi.Output))
        return false;
    for (const auto &Op : Blk.Ops)
      if (Same(Op.Output))
        return Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
               Same(Op.Inputs[0]);
  }
  return true;
}

} // namespace

void MedToHighConverter::lowerReturn(HighFunc &Func, const MedBlock &CurBlock,
                                     const MedOp &InputOp, const MedFunc &Med) {
  MedOp CurOp = InputOp;
  if (lowerX86RegistrationCatchReturn(Func, CurBlock, CurOp, Med))
    return;
  if (Med.SourceParametersBound && Med.SourceTypeHint)
    if (const auto Error = sourceABIErrorResult(*Med.SourceTypeHint)) {
      if (!SwiftErrorEntryInput || !CurOp.HasSourceErrorResult ||
          !CurOp.NumInputs || CurOp.Inputs[CurOp.NumInputs - 1].Size != 8)
        throw std::invalid_argument("Swift entry has no error-output operand");
      MedVar Slot;
      Slot.Kind = MedVar::Param;
      Slot.Id = static_cast<int>(Error->ParameterIndex);
      Slot.RegOff = Error->Location.RegisterOffset;
      Slot.Size = 8;
      Slot.TheArch = TargetArch;
      HighStmt Store;
      Store.Kind = StmtKind::Store;
      Store.Addr = CurOp.Addr;
      Store.StoreAddr = HighExpr::makeVar(
          Slot, Med.SourceTypeHint->Parameters[Error->ParameterIndex].Type);
      Store.StoreVal =
          sourceScalarValue(CurOp.Inputs[--CurOp.NumInputs], Error->Type);
      Func.Body.push_back(std::move(Store));
    }
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.Addr = CurOp.Addr;
  if (Med.CxxContinuationExitAnalysisComplete && InputOp.NumInputs == 1)
    for (const auto &Exit : Med.CxxContinuationExits)
      if (Exit.ReturnAddr == InputOp.Addr &&
          Exit.ReturnSeq == InputOp.OriginSeq && Exit.BlockId == CurBlock.Id &&
          Exit.ReturnValue == InputOp.Inputs[0])
        if (Exit.Complete)
          S.CxxContinuationReturnTargets = Exit.Targets;
  if (!S.CxxContinuationReturnTargets.empty()) {
    // This is a native pointer-valued dispatch result, even when a generic
    // return-width heuristic found a narrower integer in another register.
    S.RetVal = medvarToExpr(InputOp.Inputs[0]);
    Func.ReturnType = NdType::makeInt(InputOp.Inputs[0].Size, false);
    Func.Body.push_back(std::move(S));
    return;
  }

  // An established void declaration has no return carrier. Searching the
  // machine return register here would invent a value (and may move a prior
  // call into that value), despite the source ABI already proving it absent.
  if (Med.SourceParametersBound && Func.SourceTypeHint &&
      Func.SourceTypeHint->HasExplicitABI && Func.SourceTypeHint->ReturnType &&
      Func.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void &&
      Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Void) {
    Func.Body.push_back(std::move(S));
    return;
  }

  if (Med.SourceParametersBound && Func.SourceTypeHint &&
      Func.ReturnType->Kind == NdTypeKind::Struct) {
    const auto Members = sourceAggregateMembers(Func.ReturnType);
    std::vector<ExprPtr> Leaves;
    if (CurOp.NumInputs == Members.size())
      for (size_t I = 0; I < Members.size(); ++I)
        Leaves.push_back(sourceScalarValue(CurOp.Inputs[I], Members[I].Type));
    S.RetVal = HighExpr::makeRecord(Func.ReturnType, std::move(Leaves));
    Func.Body.push_back(std::move(S));
    return;
  }

  ExprPtr RetVal;
  if (Med.ExplicitX87ReturnValue && Med.FPReturnViaX87 && CurOp.NumInputs == 1)
    RetVal = sourceFloatValue(CurOp.Inputs[0], 10);
  const auto &TRI = getTargetRegInfo(TargetArch);
  const bool UsesFPReturnReg = Func.ReturnType &&
                               Func.ReturnType->Kind == NdTypeKind::Float &&
                               TRI.hasFPReturnReg() && !Med.FPReturnViaX87;
  const uint64_t ReturnReg =
      UsesFPReturnReg ? TRI.FPReturnReg : TRI.IntReturnReg;
  const bool ExplicitABI =
      Med.SourceTypeHint && Med.SourceTypeHint->HasExplicitABI;
  auto ValueFromDefinition = [&](const MedOp &Definition) -> ExprPtr {
    ExprPtr Value;
    if (Definition.Opcode == NdOp::CALL ||
        Definition.Opcode == NdOp::INDIR_CALL ||
        Definition.Opcode == NdOp::INTRINSIC ||
        Definition.Opcode == NdOp::LOAD ||
        Definition.MemoryOrdering != NdMemoryOrdering::None ||
        Definition.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      Value = HighExpr::makeVar(Definition.Output);
    else
      Value = medOpToExpr(Definition);
    return Value ? forceInlineExpr(Value) : nullptr;
  };

  if (ExplicitABI && Med.SourceParametersBound &&
      !Med.SourceTypeHint->ReturnComponents.empty() && CurOp.NumInputs == 1 &&
      CurOp.Inputs[0].Size == Func.ReturnType->Size)
    RetVal = medvarToExpr(CurOp.Inputs[0]);

  if (!RetVal && !ExplicitABI && !UsesFPReturnReg && !Med.FPReturnViaX87 &&
      Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Int &&
      hasPropagatedIntegerReturnValue(CurOp, TargetArch, Func.ReturnType->Size))
    RetVal = medvarToExpr(CurOp.Inputs[0]);

  if (!RetVal && CurOp.NumInputs >= 1 && CurOp.Inputs[0].Id >= 0 &&
      CurOp.Inputs[0].Kind == MedVar::Reg) {
    uint64_t RO = CurOp.Inputs[0].RegOff;
    // A floating-point result lives in the FP return register; the RETURN's
    // default integer-register operand does not carry it.
    if (!TRI.isReturnControlReg(RO) &&
        (!(ExplicitABI || UsesFPReturnReg) || RO == ReturnReg))
      RetVal = medvarToExpr(CurOp.Inputs[0]);
  }

  if (!RetVal) {
    for (auto RIt = CurBlock.Ops.rbegin(); RIt != CurBlock.Ops.rend(); ++RIt) {
      if (RIt->Opcode == NdOp::RETURN)
        continue;
      if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
          RIt->Output.RegOff == ReturnReg && !isRegisterLowView(*RIt)) {
        // Calls and memory reads materialize once. Other definitions can be
        // followed to their right-hand side so a register-only COPY need not
        // become a source variable with no emitted assignment.
        RetVal = ValueFromDefinition(*RIt);
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
    // A bare RET block inherits the value established on its only incoming
    // edge: the last definition on the chain of single predecessors, or the
    // join that begins a block of it (a conditional return out of a loop).
    // Reconstruct the defining expression rather than naming the otherwise
    // unused register SSA output. Multiple incoming edges need an explicit
    // PHI; choosing one predecessor would invent semantics.
    std::set<const MedBlock *> Seen{&CurBlock};
    for (const MedBlock *Pred = onlyPredecessor(Med, CurBlock);
         Pred && !RetVal && Seen.insert(Pred).second;
         Pred = onlyPredecessor(Med, *Pred)) {
      for (auto RIt = Pred->Ops.rbegin(); RIt != Pred->Ops.rend(); ++RIt) {
        if (RIt->Output.Kind != MedVar::Reg || RIt->Output.Size == 0 ||
            RIt->Output.RegOff != ReturnReg || isRegisterLowView(*RIt))
          continue;
        RetVal = ValueFromDefinition(*RIt);
        break;
      }
      for (const auto &Phi : Pred->Phis)
        if (!RetVal && Phi.Output.Kind == MedVar::Reg &&
            Phi.Output.RegOff == ReturnReg)
          RetVal = HighExpr::makeVar(Phi.Output);
    }
  }

  if (!RetVal && CurBlock.Preds.size() > 1) {
    // A join whose predecessors all leave the same version of the register
    // needs no PHI: the value was established before they parted, as when an
    // ARM predicated store or a branch around a store sits between the result
    // and the RETURN.  Predecessors that leave different versions need the
    // PHI, and the value a register holds at entry is none computed.
    auto LeftBy = [&](int PredId, MedVar &Out) {
      const auto Pred = std::find_if(Med.Blocks.begin(), Med.Blocks.end(),
                                     [PredId](const MedBlock &Candidate) {
                                       return Candidate.Id == PredId;
                                     });
      if (Pred == Med.Blocks.end())
        return false;
      for (auto RIt = Pred->Ops.rbegin(); RIt != Pred->Ops.rend(); ++RIt)
        if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
            RIt->Output.RegOff == ReturnReg && !isRegisterLowView(*RIt)) {
          Out = RIt->Output;
          return true;
        }
      return reachingRegAtBlockEntry(*Pred, ReturnReg, Out);
    };
    std::optional<MedVar> Common;
    bool Agree = true;
    for (const int PredId : CurBlock.Preds) {
      MedVar Left;
      if (!LeftBy(PredId, Left) ||
          (Common &&
           (Common->Id != Left.Id || Common->SSAVer != Left.SSAVer))) {
        Agree = false;
        break;
      }
      Common = Left;
    }
    if (Agree && Common && !isEntryIdentity(Med, *Common)) {
      for (const auto &Blk : Med.Blocks)
        for (const auto &Op : Blk.Ops)
          if (!RetVal && Op.Output.Kind == MedVar::Reg &&
              Op.Output.Id == Common->Id && Op.Output.SSAVer == Common->SSAVer)
            RetVal = ValueFromDefinition(Op);
      if (!RetVal)
        RetVal = HighExpr::makeVar(*Common);
    }
  }

  // Copy propagation can leave the integer return register's value as the
  // temporary it was copied from (`CONCAT t = rax.hi, 0` for `xor al, al`)
  // when no definition above names the register.  Where the RETURN operand
  // is that register (TargetRegInfo::ReturnOperandIsValue), the temporary is
  // the value returned and the register's incoming value is not.
  if (!RetVal && TRI.ReturnOperandIsValue && CurOp.NumInputs >= 1 &&
      CurOp.Inputs[0].Id >= 0 && CurOp.Inputs[0].Kind == MedVar::Temp &&
      !ExplicitABI && !UsesFPReturnReg)
    RetVal = medvarToExpr(CurOp.Inputs[0]);
  if (!RetVal)
    RetVal = unchangedDeclaredReturnParameter(Med, TRI, ReturnReg);

  // A proven i64 result on a 32-bit target occupies both integer return
  // registers.  The RETURN operand names only the low register, so recover
  // the high register from the same reaching definitions before constructing
  // the source return value.  Keep the low and high SSA values separate until
  // this boundary; an unrelated earlier write to the high register is not a
  // substitute for the value reaching this RETURN.
  if (Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Int &&
      Func.ReturnType->Size == 2 * TRI.PointerSize && TRI.PointerSize == 4 &&
      TRI.IntReturnReg2 != 0 && RetVal && RetVal->Type &&
      RetVal->Type->Size == TRI.PointerSize) {
    ExprPtr High;
    for (auto RIt = CurBlock.Ops.rbegin(); RIt != CurBlock.Ops.rend(); ++RIt) {
      if (RIt->Opcode == NdOp::RETURN)
        continue;
      if (RIt->Output.Kind == MedVar::Reg && RIt->Output.Size > 0 &&
          RIt->Output.RegOff == TRI.IntReturnReg2) {
        High = ValueFromDefinition(*RIt);
        break;
      }
    }
    if (!High)
      for (const auto &Phi : CurBlock.Phis)
        if (Phi.Output.Kind == MedVar::Reg &&
            Phi.Output.RegOff == TRI.IntReturnReg2) {
          High = HighExpr::makeVar(Phi.Output);
          break;
        }
    if (!High) {
      if (const MedBlock *Pred = onlyPredecessor(Med, CurBlock)) {
        for (auto RIt = Pred->Ops.rbegin(); RIt != Pred->Ops.rend(); ++RIt) {
          if (RIt->Output.Kind != MedVar::Reg || RIt->Output.Size == 0 ||
              RIt->Output.RegOff != TRI.IntReturnReg2)
            continue;
          High = ValueFromDefinition(*RIt);
          break;
        }
      }
    }
    // A join whose predecessors all carry the same version of the high
    // register needs no PHI: its value is the one reaching the block.  The
    // incoming value at entry, named by an identity copy, is none the
    // function computed.
    if (!High) {
      MedVar Reaching;
      if (reachingRegAtBlockEntry(CurBlock, TRI.IntReturnReg2, Reaching) &&
          Reaching.Size > 0 && !isEntryIdentity(Med, Reaching))
        High = medvarToExpr(Reaching);
    }
    if (High) {
      auto Pair = HighExpr::makeBinop(
          NdOp::CONCAT, sourceBitSlice(High, 0, TRI.PointerSize),
          sourceBitSlice(RetVal, 0, TRI.PointerSize));
      Pair->Type = Func.ReturnType;
      RetVal = std::move(Pair);
    } else {
      throw std::runtime_error(
          "cannot recover the high register of a wide integer return");
    }
  }

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

  // The FP return register holds the scalar's bits in its low lane.
  if (UsesFPReturnReg &&
      (!RetVal->Type || RetVal->Type->Kind != NdTypeKind::Float)) {
    const auto Bytes = ExplicitABI
                           ? Med.SourceTypeHint->ReturnLocation.ValueBytes
                           : Func.ReturnType->Size;
    if (Bytes == Func.ReturnType->Size)
      RetVal = HighExpr::makeBitCast(sourceBitSlice(RetVal, 0, Bytes),
                                     Func.ReturnType);
  }

  // A result the type pass bounded by the bytes every path defines (a `bool`
  // left in AL over an undefined RAX) is the low bytes of the register value;
  // the bytes above belong to no result.
  if (!UsesFPReturnReg && Med.DefinedReturnBytes && Func.ReturnType &&
      Func.ReturnType->Kind == NdTypeKind::Int && Func.ReturnType->Size &&
      Func.ReturnType->Size <= Med.DefinedReturnBytes && RetVal->Type &&
      RetVal->Type->Kind == NdTypeKind::Int &&
      Func.ReturnType->Size < RetVal->Type->Size)
    RetVal = sourceBitSlice(RetVal, 0, Func.ReturnType->Size);

  S.RetVal = RetVal;
  Func.Body.push_back(std::move(S));
}

} // namespace neverd
