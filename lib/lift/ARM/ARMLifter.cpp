//===- ARMLifter.cpp - ARM32 lifter dispatch & helpers -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Main dispatch for ARM32 instruction lifting plus the decode-time
/// instruction classification the decoder and function detector use.
/// Operand helpers live in ARMLiftOperands.cpp and flag/condition-code
/// emission in ARMLiftFlags.cpp.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {
namespace {

bool isPredicated(const cs_arm &ARM) {
  return ARM.cc >= ARMCC_EQ && ARM.cc <= ARMCC_LE;
}

bool operandIsPC(const cs_arm_op &Operand) {
  return Operand.type == ARM_OP_REG && Operand.reg == ARM_REG_PC;
}

bool operandWritesPC(const cs_arm_op &Operand) {
  return operandIsPC(Operand) && (Operand.access & CS_AC_WRITE) != 0;
}

bool anyOperandIsPC(const cs_arm &ARM, unsigned First) {
  for (unsigned I = First; I < ARM.op_count; ++I)
    if (operandIsPC(ARM.operands[I]))
      return true;
  return false;
}

bool isExplicitExceptionReturn(unsigned Id) {
  switch (Id) {
  case ARM_INS_ERET:
  case ARM_INS_RFEDA:
  case ARM_INS_RFEDB:
  case ARM_INS_RFEIA:
  case ARM_INS_RFEIB:
    return true;
  default:
    return false;
  }
}

bool hasA32UserRegisterTransferBit(const cs_insn *Insn) {
  if (!Insn || Insn->size != 4)
    return false;
  const uint32_t Encoding =
      uint32_t{Insn->bytes[0]} | (uint32_t{Insn->bytes[1]} << 8) |
      (uint32_t{Insn->bytes[2]} << 16) | (uint32_t{Insn->bytes[3]} << 24);
  constexpr uint32_t MultipleTransferClass = 0x08000000;
  constexpr uint32_t MultipleTransferMask = 0x0e000000;
  constexpr uint32_t UserRegisterTransfer = 1u << 22;
  return (Encoding & MultipleTransferMask) == MultipleTransferClass &&
         (Encoding & UserRegisterTransfer) != 0;
}

LowInstructionTargetMode aluTargetMode(InstructionMode SourceMode) {
  // ARMv8 A32 ALUWritePC performs address-based interworking.  T32
  // data-processing writes use BranchWritePC and retain Thumb state.
  return SourceMode == InstructionMode::Thumb
             ? LowInstructionTargetMode::Preserve
             : LowInstructionTargetMode::FromTargetBit0;
}

bool hasControlTerminator(const std::vector<LowOp> &Ops, size_t First) {
  for (size_t I = First; I < Ops.size(); ++I)
    switch (Ops[I].Opcode) {
    case NdOp::BRANCH:
    case NdOp::INDIR_BR:
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::RETURN:
      return true;
    default:
      break;
    }
  return false;
}

bool hasObservableEffect(const std::vector<LowOp> &Ops, size_t First) {
  for (size_t I = First; I < Ops.size(); ++I)
    switch (Ops[I].Opcode) {
    case NdOp::LOAD:
    case NdOp::STORE:
    case NdOp::ATOMIC_XCHG:
    case NdOp::INTRINSIC:
      return true;
    default:
      break;
    }
  return false;
}

} // namespace

// ===----------------------------------------------------------------------===//
// ARMLifter construction
// ===----------------------------------------------------------------------===//

ARMLifter::ARMLifter(Arch A) : TargetArch(A) {}

// ===----------------------------------------------------------------------===//
// Main dispatch
// ===----------------------------------------------------------------------===//

void ARMLifter::lift(const cs_insn *Insn, std::vector<LowOp> &Ops) {
  auto *Detail = Insn->detail;
  if (!Detail)
    return;

  auto &ARM = Detail->arm;
  LiftState S(Insn->address, static_cast<uint16_t>(Insn->size), Ops);

  const ControlInfo Control = classifyControl(Insn, InstructionMode::Default);
  if (Control.Kind == ControlKind::ExceptionReturn ||
      hasA32UserRegisterTransferBit(Insn)) {
    // TODO: introduce a typed LowIR exception-return operation that restores
    // CPSR/SPSR and derives the destination mode from the restored T bit.  A
    // normal RETURN or INDIR_BR would silently discard privileged state, so
    // reject the instruction in strict mode and terminate conservatively in
    // permissive mode until that contract exists.
    if (Strict)
      throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
    S.emitIntrinsic(Intrinsic::ArmUdf);
    return;
  }

  // ARM32 pipeline: PC reads as current_addr + 8 during execution.
  S.emit(NdOp::COPY, NdVar::reg(armreg::PC, 4),
         {NdVar::cst(Insn->address + 8, 4)});

  // A genuinely predicated instruction carries a real condition (EQ..LE).
  // ARMCC_AL and the "execute always" sentinel capstone surfaces for some
  // unconditional forms (BL/BLX immediates and BX/BLX-register both report one
  // past ARMCC_AL) all mean unconditional, so the EQ..LE range test catches
  // every spelling.  Calls must be included: an unconditional BL wrapped in a
  // never-taken predicate guard emits a mid-instruction COND_BR, after which
  // the emitter drops the remaining ops — silently deleting the CALL itself
  // (e.g. a self-recursive `bl`).  The same guard would suppress a BX indirect
  // branch's computed-goto recovery.
  bool IsCond = isPredicated(ARM);
  NdVar ArmCondVar;
  bool IsControlInsn = Control.isControl();

  if (IsCond && IsControlInsn) {
    NdVar Cond = buildCondCode(ARM.cc, S);
    NdVar InvCond = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, InvCond, {Cond});
    va_t NextAddr = S.Addr + Insn->size;
    S.emit(NdOp::COND_BR, {}, {NdVar::cst(NextAddr, 4), InvCond});
  } else if (IsCond && !IsControlInsn) {
    ArmCondVar = buildCondCode(ARM.cc, S);
  }

  size_t CondOpsStart = Ops.size();

  bool Handled = liftCore(S, Insn, ARM) || liftCoreExt(S, Insn, ARM) ||
                 liftControl(S, Insn, ARM) || liftMem(S, Insn, ARM) ||
                 liftMul(S, Insn, ARM) || liftSIMD(S, Insn, ARM) ||
                 liftSIMDNEON(S, Insn, ARM);

  if (!Handled) {
    if (Strict) {
      Ops.resize(S.OpsStart);
      throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
    }
    LLVM_DEBUG(llvm::dbgs()
               << "ARM: unlifted " << Insn->mnemonic << " " << Insn->op_str
               << " at 0x" << llvm::utohexstr(S.Addr) << "\n");
    S.emit(NdOp::NOP, {}, {});
  }

  // A discarded load is still observable when it faults, touches MMIO, trips a
  // watchpoint, or participates in a race; a false store must not dirty even a
  // substitute stack address.  Represent every predicated instruction with a
  // memory or opaque architectural effect as an instruction-local micro-CFG:
  // `COND_BR next, !predicate` skips the entire same-address effect slice.
  // Pure register/flag instructions stay branchless and use SELECT below.
  bool UsesInstructionGuard = false;
  if (IsCond && !IsControlInsn && ArmCondVar.Size > 0 &&
      hasObservableEffect(Ops, CondOpsStart)) {
    const size_t GuardOpsStart = Ops.size();
    NdVar InvCond = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, InvCond, {ArmCondVar});
    S.emit(NdOp::COND_BR, {}, {NdVar::cst(S.Addr + Insn->size, 4), InvCond});
    std::rotate(Ops.begin() + static_cast<long>(CondOpsStart),
                Ops.begin() + static_cast<long>(GuardOpsStart), Ops.end());
    for (size_t I = S.OpsStart; I < Ops.size(); ++I)
      Ops[I].Seq = static_cast<int>(I - S.OpsStart);
    S.Seq = static_cast<int>(Ops.size() - S.OpsStart);
    UsesInstructionGuard = true;
  }

  if (IsCond && !IsControlInsn && ArmCondVar.Size > 0 &&
      !UsesInstructionGuard) {
    // Wrap each register/flag write `Out = expr` as `Out = cond ? expr : Out`.
    // The SELECT is inserted immediately after the defining op (not appended
    // at the end) so later ops in the same instruction that read `Out` observe
    // the predicated value — this matters for flag-setting instructions whose
    // N/Z flags read the result register.
    llvm::DenseSet<uint64_t> SeenRegs;
    for (size_t I = CondOpsStart; I < Ops.size(); ++I) {
      auto &Op = Ops[I];
      if (Op.Output.Space == VnodeSpace::REG && Op.Output.Size > 0 &&
          Op.Output.Offset < armreg::RegSpaceEnd &&
          SeenRegs.insert(Op.Output.Offset).second) {
        NdVar OldVal = NdVar::reg(Op.Output.Offset, Op.Output.Size);
        NdVar NewVal = S.makeTemp(Op.Output.Size);
        NdVar FinalDst = Op.Output;
        Op.Output = NewVal;
        LowOp Sel;
        Sel.Opcode = NdOp::SELECT;
        Sel.Addr = S.Addr;
        Sel.Seq = 0;
        Sel.Output = FinalDst;
        Sel.addInput(ArmCondVar);
        Sel.addInput(NewVal);
        Sel.addInput(OldVal);
        Ops.insert(Ops.begin() + static_cast<long>(I) + 1, Sel);
        ++I; // step over the inserted SELECT
      }
    }
  }

  // PC control is classified from the decoded instruction, not inferred from
  // whichever LowOp happened to write PC.  That keeps COPY (MOV), LOAD (LDR),
  // INT_NOT (MVN), and ordinary ALU writes on the same control path and lets a
  // predicated form share the guard emitted above.
  if (!hasControlTerminator(Ops, S.OpsStart)) {
    if (Control.Kind == ControlKind::IndirectBranch)
      S.emit(NdOp::INDIR_BR, {}, {NdVar::reg(armreg::PC, 4)});
    else if (Control.Kind == ControlKind::Return)
      S.emit(NdOp::RETURN, {}, {NdVar::reg(armreg::PC, 4)});
  }
}

// ===----------------------------------------------------------------------===//
// Decode-time instruction classification
// ===----------------------------------------------------------------------===//

void ARMLifter::fixupDecodedInsn(cs_insn * /*I*/) {
  // No capstone decode-id quirks to correct for ARM (yet).
}

bool ARMLifter::isFunctionTerminator(const cs_insn *I) {
  const ControlInfo Control = classifyControl(I, InstructionMode::Default);
  if (Control.IsConditional)
    return false;

  switch (Control.Kind) {
  case ControlKind::DirectBranch:
  case ControlKind::IndirectBranch:
  case ControlKind::Return:
  case ControlKind::ExceptionReturn:
    return true;
  case ControlKind::None:
  case ControlKind::DirectCall:
  case ControlKind::IndirectCall:
    return false;
  }
  return false;
}

va_t ARMLifter::directCallTarget(const cs_insn *I) {
  if (!I->detail)
    return InvalidVA;
  const cs_arm &A = I->detail->arm;
  if ((I->id == ARM_INS_BL || I->id == ARM_INS_BLX) && A.op_count >= 1 &&
      A.operands[0].type == ARM_OP_IMM)
    return static_cast<va_t>(A.operands[0].imm);
  return InvalidVA;
}

ARMLifter::ControlInfo ARMLifter::classifyControl(const cs_insn *I,
                                                  InstructionMode SourceMode) {
  ControlInfo Result;
  if (!I)
    return Result;

  if (isExplicitExceptionReturn(I->id)) {
    Result.Kind = ControlKind::ExceptionReturn;
    return Result;
  }

  // Detail-free callers can still classify instructions whose control kind is
  // fully determined by the opcode.  Operand-dependent ARM forms deliberately
  // remain unknown; function discovery enables detail for ARM before asking.
  if (!I->detail) {
    switch (I->id) {
    case ARM_INS_B:
      Result.Kind = ControlKind::DirectBranch;
      break;
    case ARM_INS_BL:
      Result.Kind = ControlKind::DirectCall;
      break;
    case ARM_INS_BX:
      Result.Kind = ControlKind::IndirectBranch;
      break;
    case ARM_INS_BLX:
      Result.Kind = ControlKind::IndirectCall;
      break;
    case ARM_INS_CBZ:
    case ARM_INS_CBNZ:
      Result.Kind = ControlKind::DirectBranch;
      Result.IsConditional = true;
      break;
    case ARM_INS_TBB:
    case ARM_INS_TBH:
      Result.Kind = ControlKind::IndirectBranch;
      break;
    default:
      break;
    }
    return Result;
  }

  const cs_arm &ARM = I->detail->arm;
  Result.IsConditional = isPredicated(ARM);

  switch (I->id) {
  case ARM_INS_B:
    Result.Kind = ControlKind::DirectBranch;
    return Result;
  case ARM_INS_BL:
    Result.Kind = ControlKind::DirectCall;
    return Result;
  case ARM_INS_BX: {
    Result.Kind = ARM.op_count >= 1 && ARM.operands[0].type == ARM_OP_REG &&
                          ARM.operands[0].reg == ARM_REG_LR
                      ? ControlKind::Return
                      : ControlKind::IndirectBranch;
    if (ARM.op_count >= 1 && ARM.operands[0].type == ARM_OP_REG)
      Result.TargetMode = LowInstructionTargetMode::FromTargetBit0;
    return Result;
  }
  case ARM_INS_BLX:
    Result.Kind = ARM.op_count >= 1 && ARM.operands[0].type == ARM_OP_REG
                      ? ControlKind::IndirectCall
                      : ControlKind::DirectCall;
    if (ARM.op_count < 1)
      return Result;
    if (ARM.operands[0].type == ARM_OP_REG)
      Result.TargetMode = LowInstructionTargetMode::FromTargetBit0;
    else if (ARM.operands[0].type == ARM_OP_IMM)
      Result.TargetMode = SourceMode == InstructionMode::Thumb
                              ? LowInstructionTargetMode::ARM
                              : LowInstructionTargetMode::Thumb;
    return Result;
  case ARM_INS_CBZ:
  case ARM_INS_CBNZ:
    Result.Kind = ControlKind::DirectBranch;
    Result.IsConditional = true;
    return Result;
  case ARM_INS_TBB:
  case ARM_INS_TBH:
    Result.Kind = ControlKind::IndirectBranch;
    return Result;
  case ARM_INS_POP:
    if (anyOperandIsPC(ARM, 0)) {
      Result.Kind = ControlKind::Return;
      Result.TargetMode = LowInstructionTargetMode::FromTargetBit0;
    }
    return Result;
  case ARM_INS_LDR:
    if (ARM.op_count >= 1 && operandIsPC(ARM.operands[0])) {
      Result.Kind = isAliasMnemonic(I, "pop") ? ControlKind::Return
                                              : ControlKind::IndirectBranch;
      Result.TargetMode = LowInstructionTargetMode::FromTargetBit0;
    }
    return Result;
  case ARM_INS_LDM:
  case ARM_INS_LDMIB:
  case ARM_INS_LDMDA:
  case ARM_INS_LDMDB: {
    const bool IsPop = isAliasMnemonic(I, "pop");
    const unsigned FirstLoadedReg = IsPop ? 0 : 1;
    if (!anyOperandIsPC(ARM, FirstLoadedReg))
      return Result;
    if (ARM.usermode || hasA32UserRegisterTransferBit(I)) {
      Result.Kind = ControlKind::ExceptionReturn;
      return Result;
    }
    const bool UsesSP = !IsPop && ARM.op_count >= 1 &&
                        ARM.operands[0].type == ARM_OP_REG &&
                        ARM.operands[0].reg == ARM_REG_SP;
    Result.Kind =
        IsPop || UsesSP ? ControlKind::Return : ControlKind::IndirectBranch;
    Result.TargetMode = LowInstructionTargetMode::FromTargetBit0;
    return Result;
  }
  default:
    break;
  }

  if (ARM.op_count < 1 || !operandWritesPC(ARM.operands[0]))
    return Result;

  // Data-processing instructions with S=1 and PC as destination perform an
  // architectural exception return, not an ordinary flag-setting branch.
  if (ARM.update_flags) {
    Result.Kind = ControlKind::ExceptionReturn;
    return Result;
  }

  const bool IsPlainMovLR = I->id == ARM_INS_MOV && ARM.op_count == 2 &&
                            ARM.operands[1].type == ARM_OP_REG &&
                            ARM.operands[1].reg == ARM_REG_LR &&
                            ARM.operands[1].shift.type == ARM_SFT_INVALID;
  Result.Kind =
      IsPlainMovLR ? ControlKind::Return : ControlKind::IndirectBranch;
  Result.TargetMode = aluTargetMode(SourceMode);
  return Result;
}

LowInstructionTargetMode
ARMLifter::controlTargetMode(const cs_insn *I, InstructionMode SourceMode) {
  return classifyControl(I, SourceMode).TargetMode;
}

} // namespace neverd
