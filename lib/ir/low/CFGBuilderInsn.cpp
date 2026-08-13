//===- CFGBuilderInsn.cpp - Instruction classification and rewriting -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-instruction classification and control-flow rewriting: setting the
/// branch/call/return flags from an instruction's lifted ops, recognizing a
/// direct branch to another function or a no-return libc call, and rewriting
/// direct and indirect tail calls into an explicit call + return pair so ABI
/// recovery sees the arguments set up before the branch.
///
/// See CFGBuilder.cpp for the exploration that drives this classification.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/libc/LibCNames.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// convertIndirectTailCalls — model `bx reg`/`br reg`/`jmp *reg` (function
// pointer, not a jump table) as an indirect call + return.
//===----------------------------------------------------------------------===//

void CFGBuilder::convertIndirectTailCalls(LowFunc &Func) {
  bool Changed = false;
  for (auto &[Addr, Rec] : Insns) {
    // Only an unconditional indirect branch with no resolved jump-table targets
    // is a function-pointer tail call.  A resolved INDIR_BR (switch / computed
    // goto) keeps its successors; a conditional one and a return are
    // unaffected.
    if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall || Rec.IsRet ||
        Rec.IsCond || !Rec.JumpTableTargets.empty())
      continue;
    rewriteAsIndirectTailCall(Rec);
    Changed = true;
  }
  if (Changed)
    rebuildBlocks(Func);
}

//===----------------------------------------------------------------------===//
// classifyInsn — set control-flow flags from decoded ops
//===----------------------------------------------------------------------===//

void CFGBuilder::classifyInsn(InsnRecord &Rec) {
  for (auto &Op : Rec.Ops) {
    switch (Op.Opcode) {
    case NdOp::BRANCH:
      Rec.IsBranch = true;
      if (Op.Inputs[0].isConst())
        Rec.BranchTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::COND_BR:
      Rec.IsBranch = true;
      Rec.IsCond = true;
      if (Op.Inputs[0].isConst())
        Rec.BranchTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::INDIR_BR:
      Rec.IsBranch = true;
      Rec.IsIndirect = true;
      break;
    case NdOp::CALL:
      Rec.IsCall = true;
      if (Op.Inputs[0].isConst())
        CallTargets.insert(Op.Inputs[0].Offset);
      break;
    case NdOp::INDIR_CALL:
      Rec.IsCall = true;
      Rec.IsIndirect = true;
      break;
    case NdOp::RETURN:
      Rec.IsRet = true;
      break;
    default:
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
// isTailCallTarget / rewriteAsTailCall — model `jmp other_func` as call + ret
//===----------------------------------------------------------------------===//

bool CFGBuilder::isTailCallTarget(va_t Target) const {
  if (Target == InvalidVA || Target == CurrentFuncEntry)
    return false;
  if (KnownFuncEntries && KnownFuncEntries->count(Target) > 0)
    return true;
  // A direct branch landing on a registered import veneer is a tail call to
  // that external function.  Without this the veneer is followed and inlined
  // into the caller as an untyped indirect call, dropping import identity and
  // ABI.  BinaryImage resolves both the format-native IAT address and exact
  // executable stubs, so this also covers COFF thunks whose IATAddr stays a
  // data slot.
  if (CurrentImg && CurrentImg->findImportAt(Target))
    return true;
  return false;
}

bool CFGBuilder::isNoReturnCall(const InsnRecord &Rec) const {
  if (!CurrentImg)
    return false;
  va_t Target = InvalidVA;
  for (const auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
        Op.Inputs[0].isConst()) {
      Target = Op.Inputs[0].Offset;
      break;
    }
  if (Target == InvalidVA)
    return false;
  // External libc through any registered import address or executable veneer.
  if (const Import *Imp = CurrentImg->findImportAt(Target))
    if (libc::isNoReturnFunction(Imp->Name))
      return true;
  // Statically-linked / internal: the target is the function entry itself.
  if (const Symbol *Sym = CurrentImg->findSymbolAt(Target))
    if (libc::isNoReturnFunction(Sym->Name))
      return true;
  return false;
}

void CFGBuilder::rewriteAsTailCall(InsnRecord &Rec) {
  if (!CurrentImg)
    return;
  const auto &TRI = getTargetRegInfo(CurrentImg->Arch);
  NdVar RetReg = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  va_t Target = Rec.BranchTarget;
  va_t At = Rec.Addr;

  // The instruction's only effect is the control transfer, so drop every op it
  // lifted to (e.g. x86 emits a lone BRANCH; ARM also emits a `COPY PC, target`
  // pipeline model whose leftover constant would otherwise pollute
  // call-argument recovery) and replace them with CALL(retReg, target) +
  // RETURN(retReg), mirroring a real `call target; ret`.  Downstream ABI
  // recovery then recovers the call arguments set up before the branch.
  Rec.Ops.clear();

  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = RetReg;
  Call.addInput(NdVar::cst(Target, TRI.PointerSize));
  Call.Addr = At;
  Rec.Ops.push_back(Call);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(RetReg);
  Ret.Addr = At;
  Rec.Ops.push_back(Ret);

  Rec.IsBranch = false;
  Rec.IsCond = false;
  Rec.IsIndirect = false;
  Rec.IsCall = true;
  Rec.IsRet = true;
  Rec.BranchTarget = InvalidVA;
  CallTargets.insert(Target);
}

void CFGBuilder::rewriteAsIndirectTailCall(InsnRecord &Rec) {
  if (!CurrentImg)
    return;

  // Capture the indirect branch target (the function-pointer register/temp)
  // before clearing the instruction's lifted ops.
  NdVar Target;
  bool Found = false;
  for (auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      Target = Op.Inputs[0];
      Found = true;
      break;
    }
  if (!Found)
    return;

  const auto &TRI = getTargetRegInfo(CurrentImg->Arch);
  NdVar RetReg = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  va_t At = Rec.Addr;

  // Replace `bx reg` (+ any ARM `COPY PC, reg` pipeline model) with an indirect
  // call to the target followed by a return of the result, mirroring a real
  // `blx reg; bx lr`.  Downstream ABI recovery threads the call arguments set
  // up before the branch into the INDIR_CALL.
  Rec.Ops.clear();

  LowOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Output = RetReg;
  Call.addInput(Target);
  Call.Addr = At;
  Rec.Ops.push_back(Call);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(RetReg);
  Ret.Addr = At;
  Rec.Ops.push_back(Ret);

  Rec.IsBranch = false;
  Rec.IsCond = false;
  Rec.IsIndirect = true;
  Rec.IsCall = true;
  Rec.IsRet = true;
  Rec.BranchTarget = InvalidVA;
}

} // namespace neverd
