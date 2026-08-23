//===- X86Lifter.cpp - x86/x64 lifter dispatch & helpers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Main dispatch for x86/x64 instruction lifting, plus operand helpers and
/// flag emission shared by every category file.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/X86Lifter.h"

#include "neverd/decode/Decoder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <limits>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

// ===----------------------------------------------------------------------===//
// X86Lifter construction
// ===----------------------------------------------------------------------===//

X86Lifter::X86Lifter(Arch A) : TargetArch(A) {}

// ===----------------------------------------------------------------------===//
// LiftState helpers
// ===----------------------------------------------------------------------===//

NdVar X86Lifter::LiftState::computeEA(const cs_x86_op &MemOp) {
  const uint16_t ArithmeticSize =
      AddressSize == 2 || AddressSize == 4 || AddressSize == 8 ? AddressSize
                                                               : uint16_t{8};
  NdVar EA = makeTemp(ArithmeticSize);
  bool First = true;
  bool ConsumedDisplacement = false;
  auto AtAddressWidth = [&](NdVar V) {
    if (V.Size == ArithmeticSize)
      return V;
    if (V.isConst()) {
      const uint64_t Original = V.Offset;
      if (ArithmeticSize < 8)
        V.Offset &= (uint64_t{1} << (ArithmeticSize * 8)) - 1;
      V.Size = ArithmeticSize;
      if (V.Offset != Original &&
          V.Provenance != ConstantAddressProvenance::Unknown &&
          V.Provenance != ConstantAddressProvenance::Scalar) {
        // Truncating an authenticated object address changes its identity.
        // Keep only the architectural-address role; the old container owner
        // must not be transferred to the wrapped numeric value.
        V.Provenance = ConstantAddressProvenance::Address;
        V.AddressOwnerVA = InvalidVA;
      }
      return V;
    }
    NdVar Converted = makeTemp(ArithmeticSize);
    if (V.Size < ArithmeticSize)
      emit(NdOp::INT_ZEXT, Converted, {V});
    else
      emit(NdOp::SUBBYTES, Converted, {V, NdVar::scalar(0, ArithmeticSize)});
    return Converted;
  };
  auto Acc = [&](NdVar V) {
    V = AtAddressWidth(V);
    if (First) {
      emit(NdOp::COPY, EA, {V});
      First = false;
    } else {
      emit(NdOp::INT_ADD, EA, {EA, V});
    }
  };
  if (MemOp.mem.base != X86_REG_INVALID) {
    if (MemOp.mem.base == X86_REG_RIP) {
      if (RelocatedDisplacement && RelocatedDisplacementIsCompleteAddress) {
        Acc(*RelocatedDisplacement);
        ConsumedDisplacement = true;
      } else {
        Acc(NdVar::address(Addr + InsnSize, 8));
      }
    } else if (MemOp.mem.base == X86_REG_EIP) {
      // In 64-bit mode an address-size override makes EIP-relative arithmetic
      // wrap at 32 bits and then zero-extend. Fold the displacement at that
      // architectural width; adding it to the full instruction VA preserves a
      // stale high half.
      if (RelocatedDisplacement && RelocatedDisplacementIsCompleteAddress) {
        Acc(*RelocatedDisplacement);
      } else {
        const uint32_t NextEIP = static_cast<uint32_t>(Addr + InsnSize);
        const uint32_t Effective =
            NextEIP + static_cast<uint32_t>(MemOp.mem.disp);
        Acc(NdVar::address(Effective, 8));
      }
      ConsumedDisplacement = true;
    } else {
      auto RI = mapCapstoneReg(static_cast<x86_reg>(MemOp.mem.base));
      Acc(NdVar::reg(RI.Offset, RI.Size));
    }
  }
  if (MemOp.mem.index != X86_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<x86_reg>(MemOp.mem.index));
    NdVar Idx = AtAddressWidth(NdVar::reg(RI.Offset, RI.Size));
    if (MemOp.mem.scale > 1) {
      NdVar Scaled = makeTemp(ArithmeticSize);
      emit(NdOp::INT_MULT, Scaled,
           {Idx, NdVar::scalar(MemOp.mem.scale, ArithmeticSize)});
      Acc(Scaled);
    } else {
      Acc(Idx);
    }
  }
  if ((MemOp.mem.disp != 0 || RelocatedDisplacement) && !ConsumedDisplacement) {
    const bool IsCompleteAbsoluteAddress =
        MemOp.mem.base == X86_REG_INVALID && MemOp.mem.index == X86_REG_INVALID;
    NdVar Displacement =
        RelocatedDisplacement ? *RelocatedDisplacement
        : IsCompleteAbsoluteAddress
            ? NdVar::address(static_cast<uint64_t>(MemOp.mem.disp),
                             ArithmeticSize)
            : NdVar::scalar(static_cast<uint64_t>(MemOp.mem.disp),
                            ArithmeticSize);
    Acc(Displacement);
  }
  if (First)
    Acc(NdVar::address(0, ArithmeticSize));
  if (ArithmeticSize == 8)
    return EA;
  NdVar ExtendedEA = makeTemp(8);
  emit(NdOp::INT_ZEXT, ExtendedEA, {EA});
  return ExtendedEA;
}

void X86Lifter::LiftState::storeToMem(const cs_x86_op &MemOp, NdVar Val) {
  NdVar EA = computeEA(MemOp);
  emit(NdOp::STORE, {}, {EA, Val});
}

NdVar X86Lifter::LiftState::emitByteSwap(NdVar Src) {
  uint16_t Sz = Src.Size;
  NdVar Result = makeTemp(Sz);
  if (Sz == 2) {
    NdVar Lo = makeTemp(2);
    NdVar Hi = makeTemp(2);
    emit(NdOp::INT_AND, Lo, {Src, NdVar::scalar(0xFF, 2)});
    emit(NdOp::INT_LEFT, Lo, {Lo, NdVar::scalar(8, 2)});
    emit(NdOp::INT_RIGHT, Hi, {Src, NdVar::scalar(8, 2)});
    emit(NdOp::INT_OR, Result, {Lo, Hi});
  } else if (Sz == 4) {
    NdVar B0 = makeTemp(4), B1 = makeTemp(4);
    NdVar B2 = makeTemp(4), B3 = makeTemp(4);
    NdVar T0 = makeTemp(4), T1 = makeTemp(4);
    NdVar T2 = makeTemp(4), T3 = makeTemp(4);
    emit(NdOp::INT_AND, B0, {Src, NdVar::scalar(0xFF, 4)});
    emit(NdOp::INT_LEFT, T0, {B0, NdVar::scalar(24, 4)});
    emit(NdOp::INT_AND, B1, {Src, NdVar::scalar(0xFF00, 4)});
    emit(NdOp::INT_LEFT, T1, {B1, NdVar::scalar(8, 4)});
    emit(NdOp::INT_RIGHT, T2, {Src, NdVar::scalar(8, 4)});
    emit(NdOp::INT_AND, B2, {T2, NdVar::scalar(0xFF00, 4)});
    emit(NdOp::INT_RIGHT, T3, {Src, NdVar::scalar(24, 4)});
    emit(NdOp::INT_AND, B3, {T3, NdVar::scalar(0xFF, 4)});
    NdVar R01 = makeTemp(4), R23 = makeTemp(4);
    emit(NdOp::INT_OR, R01, {T0, T1});
    emit(NdOp::INT_OR, R23, {B2, B3});
    emit(NdOp::INT_OR, Result, {R01, R23});
  } else if (Sz == 8) {
    NdVar B0 = makeTemp(8), B1 = makeTemp(8);
    NdVar B2 = makeTemp(8), B3 = makeTemp(8);
    NdVar B4 = makeTemp(8), B5 = makeTemp(8);
    NdVar B6 = makeTemp(8), B7 = makeTemp(8);
    NdVar T0 = makeTemp(8), T1 = makeTemp(8);
    NdVar T2 = makeTemp(8), T3 = makeTemp(8);
    NdVar T4 = makeTemp(8), T5 = makeTemp(8);
    NdVar T6 = makeTemp(8), T7 = makeTemp(8);
    emit(NdOp::INT_AND, B0, {Src, NdVar::scalar(0xFF, 8)});
    emit(NdOp::INT_LEFT, T0, {B0, NdVar::scalar(56, 8)});
    emit(NdOp::INT_AND, B1, {Src, NdVar::scalar(0xFF00, 8)});
    emit(NdOp::INT_LEFT, T1, {B1, NdVar::scalar(40, 8)});
    emit(NdOp::INT_AND, B2, {Src, NdVar::scalar(0xFF0000, 8)});
    emit(NdOp::INT_LEFT, T2, {B2, NdVar::scalar(24, 8)});
    emit(NdOp::INT_AND, B3, {Src, NdVar::scalar(0xFF000000ULL, 8)});
    emit(NdOp::INT_LEFT, T3, {B3, NdVar::scalar(8, 8)});
    emit(NdOp::INT_RIGHT, T4, {Src, NdVar::scalar(8, 8)});
    emit(NdOp::INT_AND, B4, {T4, NdVar::scalar(0xFF000000ULL, 8)});
    emit(NdOp::INT_RIGHT, T5, {Src, NdVar::scalar(24, 8)});
    emit(NdOp::INT_AND, B5, {T5, NdVar::scalar(0xFF0000, 8)});
    emit(NdOp::INT_RIGHT, T6, {Src, NdVar::scalar(40, 8)});
    emit(NdOp::INT_AND, B6, {T6, NdVar::scalar(0xFF00, 8)});
    emit(NdOp::INT_RIGHT, T7, {Src, NdVar::scalar(56, 8)});
    emit(NdOp::INT_AND, B7, {T7, NdVar::scalar(0xFF, 8)});
    NdVar R01 = makeTemp(8), R23 = makeTemp(8);
    NdVar R45 = makeTemp(8), R67 = makeTemp(8);
    NdVar R03 = makeTemp(8), R47 = makeTemp(8);
    emit(NdOp::INT_OR, R01, {T0, T1});
    emit(NdOp::INT_OR, R23, {T2, T3});
    emit(NdOp::INT_OR, R45, {B4, B5});
    emit(NdOp::INT_OR, R67, {B6, B7});
    emit(NdOp::INT_OR, R03, {R01, R23});
    emit(NdOp::INT_OR, R47, {R45, R67});
    emit(NdOp::INT_OR, Result, {R03, R47});
  } else {
    emit(NdOp::COPY, Result, {Src});
  }
  return Result;
}

// ===----------------------------------------------------------------------===//
// Operand read / write
// ===----------------------------------------------------------------------===//

NdVar X86Lifter::operandRead(LiftState &S, const cs_x86_op &Op) {
  uint16_t Sz = static_cast<uint16_t>(Op.size);
  switch (Op.type) {
  case X86_OP_REG: {
    auto RI = mapCapstoneReg(static_cast<x86_reg>(Op.reg));
    return NdVar::reg(RI.Offset, RI.Size);
  }
  case X86_OP_IMM:
    if (S.RelocatedImmediate) {
      NdVar Value = *S.RelocatedImmediate;
      Value.Size = Sz;
      return Value;
    }
    // An encoded immediate is role-neutral until the instruction handler or
    // an exact relocation occurrence establishes its semantics. In
    // particular, MOVABS may legitimately carry a function address.
    return NdVar::cst(static_cast<uint64_t>(Op.imm), Sz);
  case X86_OP_MEM: {
    NdVar EA = S.computeEA(Op);
    NdVar Result = S.makeTemp(Sz);
    S.emit(NdOp::LOAD, Result, {EA});
    return Result;
  }
  default:
    return NdVar::scalar(0, Sz);
  }
}

NdVar X86Lifter::operandWrite(const cs_x86_op &Op) {
  uint16_t Sz = static_cast<uint16_t>(Op.size);
  switch (Op.type) {
  case X86_OP_REG: {
    auto RI = mapCapstoneReg(static_cast<x86_reg>(Op.reg));
    return NdVar::reg(RI.Offset, RI.Size);
  }
  case X86_OP_MEM:
    return NdVar::ram(0, Sz);
  default:
    return NdVar::scalar(0, Sz);
  }
}

// ===----------------------------------------------------------------------===//
// Flag emission helpers
// ===----------------------------------------------------------------------===//

void X86Lifter::emitPF(LiftState &S, NdVar Result) {
  NdVar LoByte = S.makeTemp(1);
  S.emit(NdOp::SUBBYTES, LoByte, {Result, NdVar::scalar(0, 4)});
  NdVar PopCnt = S.makeTemp(1);
  S.emit(NdOp::POPCOUNT, PopCnt, {LoByte});
  NdVar ParBit = S.makeTemp(1);
  S.emit(NdOp::INT_AND, ParBit, {PopCnt, NdVar::scalar(1, 1)});
  S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::PF, 1),
         {ParBit, NdVar::scalar(0, 1)});
}

void X86Lifter::emitZSPF(LiftState &S, NdVar Result) {
  S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
         {Result, NdVar::scalar(0, Result.Size)});
  S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
         {Result, NdVar::scalar(0, Result.Size)});
  emitPF(S, Result);
}

void X86Lifter::emitFlagsLogic(LiftState &S, NdVar Result) {
  emitZSPF(S, Result);
  S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::scalar(0, 1)});
  S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
}

// AF (auxiliary carry) reflects a carry/borrow out of bit 3, identical for
// add and subtract: AF = bit 4 of (A ^ B ^ Result).  Previously unmodeled, so
// LAHF/PUSHF read a stale 0 for it.
void X86Lifter::emitAF(LiftState &S, NdVar Result, NdVar A, NdVar B) {
  uint16_t Sz = Result.Size;
  NdVar AxB = S.makeTemp(Sz);
  S.emit(NdOp::INT_XOR, AxB, {A, B});
  NdVar AxBxR = S.makeTemp(Sz);
  S.emit(NdOp::INT_XOR, AxBxR, {AxB, Result});
  NdVar AfBit = S.makeTemp(Sz);
  S.emit(NdOp::INT_AND, AfBit, {AxBxR, NdVar::scalar(0x10, Sz)});
  S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::AF, 1),
         {AfBit, NdVar::scalar(0, Sz)});
}

void X86Lifter::emitFlagsArith(LiftState &S, NdVar Result, NdVar A, NdVar B,
                               bool IsSub) {
  emitZSPF(S, Result);
  emitAF(S, Result, A, B);
  if (IsSub) {
    S.emit(NdOp::INT_LESS, NdVar::reg(x86reg::CF, 1), {A, B});
    S.emit(NdOp::INT_SBOR, NdVar::reg(x86reg::OF, 1), {A, B});
  } else {
    S.emit(NdOp::INT_CARRY, NdVar::reg(x86reg::CF, 1), {A, B});
    S.emit(NdOp::INT_SOVF, NdVar::reg(x86reg::OF, 1), {A, B});
  }
}

NdVar X86Lifter::extractBit(LiftState &S, NdVar Val, unsigned BitPos) {
  uint16_t Sz = Val.Size;
  NdVar Shifted = S.makeTemp(Sz);
  S.emit(NdOp::INT_RIGHT, Shifted, {Val, NdVar::scalar(BitPos, Sz)});
  NdVar Bit = S.makeTemp(1);
  S.emit(NdOp::SUBBYTES, Bit, {Shifted, NdVar::scalar(0, 4)});
  S.emit(NdOp::INT_AND, Bit, {Bit, NdVar::scalar(1, 1)});
  return Bit;
}

void X86Lifter::emitShiftRotateOF(LiftState &S, NdVar Cnt, NdVar OfBit) {
  NdVar IsOne = S.makeTemp(1);
  S.emit(NdOp::INT_EQUAL, IsOne, {Cnt, NdVar::scalar(1, Cnt.Size)});
  NdVar NewOF = S.makeTemp(1);
  S.emit(NdOp::SELECT, NewOF, {IsOne, OfBit, NdVar::reg(x86reg::OF, 1)});
  S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NewOF});
}

void X86Lifter::emitZeroCountFlagGuard(
    LiftState &S, NdVar Cnt,
    std::initializer_list<std::pair<int, NdVar>> Flags) {
  NdVar IsZero = S.makeTemp(1);
  S.emit(NdOp::INT_EQUAL, IsZero, {Cnt, NdVar::scalar(0, Cnt.Size)});
  for (const auto &F : Flags) {
    NdVar Cur = S.makeTemp(1);
    S.emit(NdOp::COPY, Cur, {NdVar::reg(F.first, 1)});
    NdVar Restored = S.makeTemp(1);
    S.emit(NdOp::SELECT, Restored, {IsZero, F.second, Cur});
    S.emit(NdOp::COPY, NdVar::reg(F.first, 1), {Restored});
  }
}

// ===----------------------------------------------------------------------===//
// Main dispatch
// ===----------------------------------------------------------------------===//

void X86Lifter::lift(const cs_insn *Insn, std::vector<LowOp> &Ops,
                     llvm::ArrayRef<RelocatedAddressOperand> Relocs) {
  LastGetPcOccurrence.reset();
  auto *Detail = Insn->detail;
  if (!Detail)
    return;

  auto &X86 = Detail->x86;
  LiftState S(Insn->address, static_cast<uint16_t>(Insn->size), Ops);
  S.AddressSize = X86.addr_size != 0
                      ? static_cast<uint16_t>(X86.addr_size)
                      : static_cast<uint16_t>(TargetArch == Arch::X64 ? 8 : 4);
  bool ConflictingDisplacementOwners = false;
  bool ConflictingImmediateOwners = false;
  const cs_x86_op *ImmediateOperand = nullptr;
  for (uint8_t I = 0; I < X86.op_count; ++I)
    if (X86.operands[I].type == X86_OP_IMM) {
      ImmediateOperand = &X86.operands[I];
      break;
    }
  for (const RelocatedAddressOperand &Reloc : Relocs) {
    if (Reloc.Width == 0)
      continue;
    const unsigned Bits = Reloc.Width * 8;
    const uint64_t Mask = Bits >= 64 ? std::numeric_limits<uint64_t>::max()
                                     : (uint64_t(1) << Bits) - 1;
    if (!Reloc.PCRelativeFromInstructionEnd &&
        (Reloc.TargetVA & Mask) != (Reloc.EncodedValue & Mask))
      continue;
    NdVar Candidate{VnodeSpace::CONST, Reloc.TargetVA, 8, Reloc.Provenance,
                    Reloc.TargetOwnerVA};
    if (X86.encoding.disp_size != 0 && Reloc.Width == X86.encoding.disp_size &&
        Reloc.FieldVA == Insn->address + X86.encoding.disp_offset &&
        (static_cast<uint64_t>(X86.disp) & Mask) ==
            (Reloc.EncodedValue & Mask)) {
      if (S.RelocatedDisplacement &&
          (*S.RelocatedDisplacement != Candidate ||
           S.RelocatedDisplacementIsCompleteAddress !=
               Reloc.PCRelativeFromInstructionEnd))
        ConflictingDisplacementOwners = true;
      else {
        S.RelocatedDisplacement = Candidate;
        S.RelocatedDisplacementIsCompleteAddress =
            Reloc.PCRelativeFromInstructionEnd;
      }
    }
    if ((!Reloc.PCRelativeFromInstructionEnd ||
         Reloc.Provenance == ConstantAddressProvenance::CodeAddress) &&
        ImmediateOperand && X86.encoding.imm_size != 0 &&
        Reloc.Width == X86.encoding.imm_size &&
        Reloc.FieldVA == Insn->address + X86.encoding.imm_offset &&
        (Reloc.PCRelativeFromInstructionEnd ||
         (static_cast<uint64_t>(ImmediateOperand->imm) & Mask) ==
             (Reloc.EncodedValue & Mask))) {
      if (S.RelocatedImmediate && *S.RelocatedImmediate != Candidate)
        ConflictingImmediateOwners = true;
      else
        S.RelocatedImmediate = Candidate;
    }
  }
  // Two loader owners for the same encoded field are corrupt/ambiguous
  // provenance.  Falling back to a raw immediate would freeze the old VA, so
  // reject the instruction before emitting any LowIR instead.
  if (ConflictingDisplacementOwners || ConflictingImmediateOwners)
    throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);

  // Cleared per instruction; the FNINIT/FNCLEX handler sets it so the CFG
  // builder treats this as an absolute x87 TOP reset, not a relative push/pop.
  FpuReset = false;

  // get-PC thunk: capture whether the previous instruction was a `call $+5`, so
  // the POP handler can resolve this instruction's destination to the constant
  // PC.  Cleared each instruction; the CALL handler re-arms it for the next.
  GetPcArmedThisInsn = GetPcPending;
  GetPcPending = false;

  // Track CQO/CDQ/CWD → IDIV/DIV pattern.
  unsigned Id = Insn->id;
  if (Id == X86_INS_CQO) {
    LastRdxState = RdxState::SignExtRAX;
    LastRdxSize = 8;
  } else if (Id == X86_INS_CDQ) {
    LastRdxState = RdxState::SignExtRAX;
    LastRdxSize = 4;
  } else if (Id == X86_INS_CWD) {
    LastRdxState = RdxState::SignExtRAX;
    LastRdxSize = 2;
  } else if (Id != X86_INS_IDIV && Id != X86_INS_DIV) {
    // Check for XOR RDX,RDX (zero) pattern before invalidating.
    bool SetZero = false;
    if (Id == X86_INS_XOR && X86.op_count == 2 &&
        X86.operands[0].type == X86_OP_REG &&
        X86.operands[1].type == X86_OP_REG) {
      auto R0 = mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      auto R1 = mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
      if (R0.Offset == x86reg::RDX && R1.Offset == x86reg::RDX) {
        LastRdxState = RdxState::Zero;
        // On x86-64, writing a 32-bit register implicitly zeros the upper 32
        // bits, so XOR EDX,EDX effectively zeros all of RDX.
        LastRdxSize = (TargetArch == Arch::X64 && R0.Size == 4) ? 8 : R0.Size;
        SetZero = true;
      }
    }
    if (!SetZero) {
      for (uint8_t I = 0; I < X86.op_count; ++I) {
        if (X86.operands[I].type == X86_OP_REG) {
          auto RI = mapCapstoneReg(static_cast<x86_reg>(X86.operands[I].reg));
          if (RI.Offset == x86reg::RDX)
            LastRdxState = RdxState::Unknown;
        }
      }
    }
  }

  bool Handled = liftCore(S, Insn, X86) || liftControl(S, Insn, X86) ||
                 liftAtomic(S, Insn, X86) || liftString(S, Insn, X86) ||
                 liftFPU(S, Insn, X86) || liftExt(S, Insn, X86) ||
                 liftSIMD(S, Insn, X86) || liftSIMDAVX(S, Insn, X86) ||
                 liftSIMDLegacy(S, Insn, X86);

  if (!Handled) {
    if (Strict)
      throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
    S.emit(NdOp::NOP, {}, {});
    LLVM_DEBUG(llvm::dbgs()
               << "Unlifted instruction: " << Insn->mnemonic << " "
               << Insn->op_str << " @ 0x" << llvm::utohexstr(S.Addr) << "\n");
  }

  // x86-64: writing to a 32-bit register implicitly zero-extends to 64 bits.
  if (TargetArch == Arch::X64) {
    size_t End = Ops.size();
    for (size_t I = S.OpsStart; I < End; ++I) {
      auto &Op = Ops[I];
      if (Op.Output.Space != VnodeSpace::REG)
        continue;
      if (Op.Output.Size != 4)
        continue;
      uint64_t ROffs = Op.Output.Offset;
      if (ROffs >= x86reg::GPRSpaceEnd)
        continue;
      if (ROffs == x86reg::RSP || ROffs == x86reg::RBP)
        continue;

      S.emit(NdOp::INT_ZEXT, NdVar::reg(ROffs, 8), {NdVar::reg(ROffs, 4)});
    }
  }
}

// ===----------------------------------------------------------------------===//
// Decode-time instruction classification
// ===----------------------------------------------------------------------===//

namespace {

// Capstone 6.0-alpha derives the SSE/SSE2 FP-compare pseudo-op ids (cmpeqps,
// cmpleps, cmpnleps, ...) as a base plus the predicate index; the result
// collides with unrelated enum entries (e.g. cmpleps -> X86_INS_CMPSB, the
// string-compare byte op).  Routing such an id to the string lifter emits a
// `cmps` that reads [rsi]/[rdi] -> bogus memory access.  All of these share
// opcode 0F C2 with the predicate in the trailing immediate byte; the mandatory
// prefix selects the width.  Re-derive the real instruction id from the opcode
// so the lifter dispatches to the correct compare handler.
void fixupCompareId(cs_insn *I) {
  if (!I->detail)
    return;
  const cs_x86 &X = I->detail->x86;
  if (X.opcode[0] != 0x0F || X.opcode[1] != 0xC2)
    return;
  uint8_t Mand = 0;
  for (uint16_t N = 0; N < I->size; ++N) {
    uint8_t B = I->bytes[N];
    if (B == 0x0F)
      break;
    if (B == 0x66 || B == 0xF2 || B == 0xF3)
      Mand = B;
  }
  switch (Mand) {
  case 0xF3:
    I->id = X86_INS_CMPSS;
    break;
  case 0xF2:
    I->id = X86_INS_CMPSD;
    break;
  case 0x66:
    I->id = X86_INS_CMPPD;
    break;
  default:
    I->id = X86_INS_CMPPS;
    break;
  }
}

// The VEX/EVEX FP-compare pseudo-ops (vcmpltps, vcmpleps, ...) all decode to
// the generic X86_INS_VCMP id; capstone keeps the width only in the mnemonic
// suffix. Re-derive the width-specific id so the lifter can pick the right lane
// size, reading the predicate from the trailing immediate byte.
void fixupVexCompareId(cs_insn *I) {
  const char *M = I->mnemonic;
  if (M[0] != 'v' || M[1] != 'c' || M[2] != 'm' || M[3] != 'p')
    return;
  size_t L = std::strlen(M);
  if (L < 6)
    return;
  char Wide = M[L - 2], Last = M[L - 1];
  if (Last == 's')
    I->id = (Wide == 'p') ? X86_INS_VCMPPS : X86_INS_VCMPSS;
  else if (Last == 'd')
    I->id = (Wide == 'p') ? X86_INS_VCMPPD : X86_INS_VCMPSD;
}

} // anonymous namespace

void X86Lifter::fixupDecodedInsn(cs_insn *I) {
  fixupCompareId(I);
  fixupVexCompareId(I);
}

bool X86Lifter::isFunctionTerminator(const cs_insn *I) {
  switch (I->id) {
  case X86_INS_RET:
  case X86_INS_RETF:
  case X86_INS_JMP:
  case X86_INS_INT3:
  case X86_INS_UD2:
  case X86_INS_IRET:
  case X86_INS_IRETD:
  case X86_INS_IRETQ:
    return true;
  default:
    return false;
  }
}

bool X86Lifter::isResumableTrap(const cs_insn *I) {
  return I->id == X86_INS_INT3;
}

va_t X86Lifter::directCallTarget(const cs_insn *I) {
  if (!I->detail)
    return InvalidVA;
  const cs_x86 &X = I->detail->x86;
  if (I->id == X86_INS_CALL && X.op_count >= 1 &&
      X.operands[0].type == X86_OP_IMM)
    return static_cast<va_t>(X.operands[0].imm);
  return InvalidVA;
}

std::optional<uint64_t> X86Lifter::returnImmediate(const cs_insn *I) {
  if (!I->detail)
    return std::nullopt;
  if (I->id != X86_INS_RET && I->id != X86_INS_RETF && I->id != X86_INS_RETFQ)
    return std::nullopt;
  const cs_x86 &X = I->detail->x86;
  if (X.op_count < 1 || X.operands[0].type != X86_OP_IMM)
    return std::nullopt;
  return static_cast<uint64_t>(X.operands[0].imm);
}

va_t X86Lifter::pcRelCodeRefTarget(const cs_insn *I) {
  if (!I->detail || I->id != X86_INS_LEA)
    return InvalidVA;
  const cs_x86 &X = I->detail->x86;
  if (X.op_count < 2 || X.operands[1].type != X86_OP_MEM)
    return InvalidVA;
  const auto &M = X.operands[1].mem;
  // Only a pure RIP/EIP base with no index is a single fixed address-of; an
  // index register means a runtime-indexed table, not a function pointer.
  if ((M.base != X86_REG_RIP && M.base != X86_REG_EIP) ||
      M.index != X86_REG_INVALID)
    return InvalidVA;
  if (M.base == X86_REG_EIP) {
    const uint32_t NextEIP = static_cast<uint32_t>(I->address + I->size);
    return static_cast<va_t>(NextEIP + static_cast<uint32_t>(M.disp));
  }
  return static_cast<va_t>(I->address + I->size + static_cast<int64_t>(M.disp));
}

} // namespace neverd
