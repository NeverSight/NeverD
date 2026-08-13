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

#include "neverd/lift/ARMLifter.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

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
  bool IsCond = (ARM.cc >= ARMCC_EQ && ARM.cc <= ARMCC_LE);
  NdVar ArmCondVar;
  bool IsBranchInsn = (Insn->id == ARM_INS_B || Insn->id == ARM_INS_BL ||
                       Insn->id == ARM_INS_BX || Insn->id == ARM_INS_BLX ||
                       Insn->id == ARM_INS_CBZ || Insn->id == ARM_INS_CBNZ);

  if (IsCond && IsBranchInsn) {
    NdVar Cond = buildCondCode(ARM.cc, S);
    NdVar InvCond = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, InvCond, {Cond});
    va_t NextAddr = S.Addr + Insn->size;
    S.emit(NdOp::COND_BR, {}, {NdVar::cst(NextAddr, 4), InvCond});
  } else if (IsCond && !IsBranchInsn) {
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

  // Conditional non-branch wrapping.  Every predicated effect is modeled
  // branchlessly, so no COND_BR appears in the middle of an instruction's op
  // list: the CFG builder works at instruction granularity and cannot split a
  // block at such a COND_BR, so any op after it (e.g. a predicated store) would
  // be silently dropped by the emitter.
  //   * register/flag write `Out = expr`  →  `Out = cond ? expr : Out`
  //   * memory access `M[addr]`             →  `M[cond ? addr : SP]`
  //   * memory store `M[addr] = val`        →  `M[safe] = cond ? val : M[safe]`
  //     (read-modify-write)
  // A false predicated load/store must not even dereference its architectural
  // address: ARM code commonly leaves that address invalid on the untaken path
  // (for example `ldrne r2, [r0, #-60]` where r0 is unrelated when Z is set).
  // Keep the branchless representation, but redirect the speculative access to
  // the ABI-valid stack pointer.  The register SELECT below discards a false
  // load and the read-modify-write preserves the fallback word for a false
  // store.
  if (IsCond && !IsBranchInsn && ArmCondVar.Size > 0) {
    // Guard the address of every memory effect before wrapping its result.  Do
    // this as a separate pass so insertion does not interfere with the
    // register/store transformations below.
    for (size_t I = CondOpsStart; I < Ops.size(); ++I) {
      auto &Op = Ops[I];
      if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
          Op.NumInputs == 0)
        continue;
      NdVar SafeEA = S.makeTemp(4);
      LowOp SelEA;
      SelEA.Opcode = NdOp::SELECT;
      SelEA.Addr = S.Addr;
      SelEA.Output = SafeEA;
      SelEA.addInput(ArmCondVar);
      SelEA.addInput(Op.Inputs[0]);
      SelEA.addInput(NdVar::reg(armreg::SP, 4));
      Op.Inputs[0] = SafeEA;
      Ops.insert(Ops.begin() + static_cast<long>(I), SelEA);
      ++I;
    }

    // Wrap each register/flag write `Out = expr` as `Out = cond ? expr : Out`.
    // The SELECT is inserted immediately after the defining op (not appended
    // at the end) so later ops in the same instruction that read `Out` observe
    // the predicated value — this matters for flag-setting instructions whose
    // N/Z flags read the result register.
    llvm::DenseSet<uint64_t> SeenRegs;
    for (size_t I = CondOpsStart; I < Ops.size(); ++I) {
      auto &Op = Ops[I];
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2) {
        NdVar EA = Op.Inputs[0];
        NdVar Val = Op.Inputs[1];
        NdVar Old = S.makeTemp(Val.Size);
        NdVar Merged = S.makeTemp(Val.Size);
        Op.Inputs[1] = Merged;
        LowOp LoadOld;
        LoadOld.Opcode = NdOp::LOAD;
        LoadOld.Addr = S.Addr;
        LoadOld.Output = Old;
        LoadOld.addInput(EA);
        LowOp SelVal;
        SelVal.Opcode = NdOp::SELECT;
        SelVal.Addr = S.Addr;
        SelVal.Output = Merged;
        SelVal.addInput(ArmCondVar);
        SelVal.addInput(Val);
        SelVal.addInput(Old);
        Ops.insert(Ops.begin() + static_cast<long>(I), SelVal);
        Ops.insert(Ops.begin() + static_cast<long>(I), LoadOld);
        I += 2; // step over the inserted LOAD + SELECT
        continue;
      }
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

  // An arithmetic write to PC (e.g. `add pc, base, table[idx]`) is an
  // indirect branch (jump-table dispatch).  COPY-to-PC (`mov pc, lr`) and
  // LOAD-to-PC (`ldr pc, ...`) are returns handled by their own lifters, so
  // only a computed PC write becomes an INDIR_BR.
  if (!IsCond) {
    bool HasTerminator = false;
    bool ArithPCWrite = false;
    for (auto &Op : Ops) {
      switch (Op.Opcode) {
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
      case NdOp::RETURN:
        HasTerminator = true;
        break;
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_OR:
      case NdOp::INT_XOR:
      case NdOp::INT_AND:
      case NdOp::INT_MULT:
      case NdOp::INT_LEFT:
      case NdOp::INT_RIGHT:
      case NdOp::INT_ASHR:
        if (Op.Output.isReg() && Op.Output.Offset == armreg::PC)
          ArithPCWrite = true;
        break;
      default:
        break;
      }
    }
    if (ArithPCWrite && !HasTerminator)
      S.emit(NdOp::INDIR_BR, {}, {NdVar::reg(armreg::PC, 4)});
  }
}

// ===----------------------------------------------------------------------===//
// Decode-time instruction classification
// ===----------------------------------------------------------------------===//

void ARMLifter::fixupDecodedInsn(cs_insn * /*I*/) {
  // No capstone decode-id quirks to correct for ARM (yet).
}

bool ARMLifter::isFunctionTerminator(const cs_insn *I) {
  switch (I->id) {
  case ARM_INS_BX:
  case ARM_INS_B:
  case ARM_INS_POP:
    return true;
  default:
    return false;
  }
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

} // namespace neverd
