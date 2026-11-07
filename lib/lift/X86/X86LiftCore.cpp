//===- X86LiftCore.cpp - x86/x64 core instruction lifter ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the core x86/x64 integer ALU instructions to the per-family
/// handlers in X86LiftCore*.cpp. DIV/IDIV and bit tests retain their shared
/// mode-dependent width and addressing semantics here. Also defines the carry
/// snapshot helper that ADC/SBB and RCL/RCR share.
///
//===----------------------------------------------------------------------===//

#include "X86BitTestUndefined.h"
#include "X86DivisionUndefined.h"
#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Lifter.h"

namespace neverd {

bool isSameRegisterOperand(const cs_x86_op &A, const cs_x86_op &B) {
  return A.type == X86_OP_REG && B.type == X86_OP_REG && A.reg == B.reg;
}

NdVar emitByteSplat(X86Lifter::LiftState &S, uint16_t Bytes, uint8_t Byte) {
  const uint64_t Word = Byte * 0x0101010101010101ULL;
  if (Bytes <= 8)
    return NdVar::cst(
        Bytes == 8 ? Word : Word & ((uint64_t{1} << (8 * Bytes)) - 1), Bytes);
  NdVar Acc = NdVar::cst(Word, 8);
  for (uint16_t Done = 8; Done < Bytes; Done += 8) {
    NdVar Next = S.makeTemp(Done + 8);
    S.emit(NdOp::CONCAT, Next, {NdVar::cst(Word, 8), Acc});
    Acc = Next;
  }
  return Acc;
}

NdVar snapshotCarryAtWidth(X86Lifter::LiftState &S, uint16_t Size) {
  NdVar Carry = NdVar::reg(x86reg::CF, 1);
  NdVar Snapshot = S.makeTemp(Size);
  // ADC/SBB still consume the incoming carry while computing OF after they
  // have written the new CF. Keep a stable temp even for byte operations;
  // only the width conversion disappears in that case.
  S.emit(Size == 1 ? NdOp::COPY : NdOp::INT_ZEXT, Snapshot, {Carry});
  return Snapshot;
}

bool X86Lifter::liftCore(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  const auto RequireDivPrecondition = [&](NdVar Dividend, NdVar Divisor,
                                          X86DivKind Kind) {
    S.emitIntrinsic(
        Intrinsic::X86RequireDivPrecondition, {},
        {Dividend, Divisor, NdVar::scalar(static_cast<uint64_t>(Kind), 1)});
  };
  switch (InsnId) {

  case X86_INS_DIV:
  case X86_INS_IDIV: {
    if (X86.op_count < 1)
      break;
    const bool Signed = InsnId == X86_INS_IDIV;
    const bool Audited = divisionundefined::form(Insn, TargetArch);
    const NdVar Src = operandRead(S, X86.operands[0]);
    const uint16_t Sz = Src.Size;
    const NdOp Extend = Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
    NdVar Dividend = NdVar::reg(x86reg::RAX, 2);
    if (Sz != 1) {
      // Preserve the actual double-width dividend for every instruction.
      // Only subsequent dataflow may prove a narrower value; lift-call order
      // does not establish the incoming DX/EDX/RDX of another instruction.
      const NdVar ExtRAX = S.makeTemp(Sz * 2);
      const NdVar ExtRDX = S.makeTemp(Sz * 2);
      S.emit(NdOp::INT_ZEXT, ExtRAX, {NdVar::reg(x86reg::RAX, Sz)});
      S.emit(Extend, ExtRDX, {NdVar::reg(x86reg::RDX, Sz)});
      const NdVar HiShifted = S.makeTemp(Sz * 2);
      S.emit(NdOp::INT_LEFT, HiShifted,
             {ExtRDX, NdVar::scalar(Sz * 8, Sz * 2)});
      Dividend = S.makeTemp(Sz * 2);
      S.emit(NdOp::INT_OR, Dividend, {HiShifted, ExtRAX});
    }
    const NdVar ExtSrc = S.makeTemp(Sz * 2);
    S.emit(Extend, ExtSrc, {Src});
    const NdVar Quot = S.makeTemp(Sz * 2), Rem = S.makeTemp(Sz * 2);
    RequireDivPrecondition(Dividend, Src,
                           Signed ? X86DivKind::Signed : X86DivKind::Unsigned);
    S.emit(Signed ? NdOp::INT_SDIV : NdOp::INT_DIV, Quot, {Dividend, ExtSrc});
    S.emit(Signed ? NdOp::INT_SREM : NdOp::INT_REM, Rem, {Dividend, ExtSrc});
    if (Sz == 1) {
      const NdVar QuotByte = S.makeTemp(1), RemByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, QuotByte, {Quot, NdVar::scalar(0, 4)});
      S.emit(NdOp::SUBBYTES, RemByte, {Rem, NdVar::scalar(0, 4)});
      const NdVar AxResult = S.makeTemp(2);
      S.emit(NdOp::CONCAT, AxResult, {RemByte, QuotByte});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RAX, 2), {AxResult});
    } else {
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RAX, Sz),
             {Quot, NdVar::scalar(0, 4)});
      S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::RDX, Sz),
             {Rem, NdVar::scalar(0, 4)});
    }
    if (Audited)
      divisionundefined::record(S);
    break;
  }

  // --- Bit test/set/reset/complement ---
  // The bit offset's meaning depends on the bit-base operand and offset kind:
  //   * register base, or any immediate offset: the offset is taken modulo the
  //     operand width (16/32/64), so `bt eax,33` tests bit 1.
  //   * register offset on a MEMORY base: the offset is a signed bit index into
  //     a bit string — the accessed operand-size chunk is at
  //     EA + ((sext(idx) >> log2(bits)) << log2(bytes)), and the in-chunk bit
  //     is idx & (bits-1).  (Matches the QEMU/x86 reference for `bt mem,reg`.)
  case X86_INS_BT:
  case X86_INS_BTS:
  case X86_INS_BTR:
  case X86_INS_BTC: {
    if (X86.op_count < 2)
      break;
    uint16_t Sz = static_cast<uint16_t>(X86.operands[0].size);
    if (Sz == 0)
      Sz = (TargetArch == Arch::X64) ? 8 : 4;
    uint16_t Bits = Sz * 8;
    uint64_t LogSz = (Sz == 8) ? 3 : (Sz == 4) ? 2 : (Sz == 2) ? 1 : 0;
    uint16_t PtrSz = (TargetArch == Arch::X64) ? 8 : 4;
    uint16_t AddrSz =
        S.AddressSize == 2 || S.AddressSize == 4 || S.AddressSize == 8
            ? S.AddressSize
            : PtrSz;
    const bool Locked = X86.prefix[0] == X86_PREFIX_LOCK;
    if (Locked && !bitundefined::form(Insn, TargetArch))
      return false;
    bool MemBase = (X86.operands[0].type == X86_OP_MEM);
    bool RegOffset = (X86.operands[1].type == X86_OP_REG);

    NdVar IdxRaw = operandRead(S, X86.operands[1]);

    // Locate the operand-size value containing the target bit, and (for memory)
    // the byte address to load/store.
    NdVar Base;
    NdVar ByteAddr;
    if (MemBase) {
      ByteAddr = S.computeEA(X86.operands[0]);
      if (RegOffset) {
        if (ByteAddr.Size != AddrSz) {
          NdVar Wrapped = S.makeTemp(AddrSz);
          S.emit(NdOp::SUBBYTES, Wrapped, {ByteAddr, NdVar::scalar(0, AddrSz)});
          ByteAddr = Wrapped;
        }
        const uint16_t IndexArithmeticSize =
            std::max<uint16_t>(IdxRaw.Size, AddrSz);
        NdVar IdxExt = S.makeTemp(IndexArithmeticSize);
        if (IdxRaw.Size < IndexArithmeticSize)
          S.emit(NdOp::INT_SEXT, IdxExt, {IdxRaw});
        else
          S.emit(NdOp::COPY, IdxExt, {IdxRaw});
        NdVar ChunkOff = S.makeTemp(IndexArithmeticSize);
        S.emit(NdOp::INT_ASHR, ChunkOff,
               {IdxExt, NdVar::scalar(LogSz + 3, IndexArithmeticSize)});
        NdVar WideByteOff = S.makeTemp(IndexArithmeticSize);
        S.emit(NdOp::INT_LEFT, WideByteOff,
               {ChunkOff, NdVar::scalar(LogSz, IndexArithmeticSize)});
        NdVar ByteOff = WideByteOff;
        if (IndexArithmeticSize > AddrSz) {
          ByteOff = S.makeTemp(AddrSz);
          S.emit(NdOp::SUBBYTES, ByteOff,
                 {WideByteOff, NdVar::scalar(0, AddrSz)});
        }
        NdVar Adj = S.makeTemp(AddrSz);
        S.emit(NdOp::INT_ADD, Adj, {ByteAddr, ByteOff});
        ByteAddr = Adj;
        if (AddrSz < 8) {
          NdVar Extended = S.makeTemp(8);
          S.emit(NdOp::INT_ZEXT, Extended, {ByteAddr});
          ByteAddr = Extended;
        }
      }
      Base = S.makeTemp(Sz);
      if (!Locked)
        S.emit(NdOp::LOAD, Base, {ByteAddr}, NdMemoryOrdering::None,
               LiftState::memoryAddressSpace(X86.operands[0]));
    } else {
      Base = operandRead(S, X86.operands[0]);
    }

    // In-chunk bit position, taken modulo the operand width.
    NdVar Idx = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Idx, {IdxRaw, NdVar::scalar(Bits - 1, Sz)});

    if (Locked) {
      const auto Id = InsnId == X86_INS_BTS   ? Intrinsic::X86LockedBts
                      : InsnId == X86_INS_BTR ? Intrinsic::X86LockedBtr
                                              : Intrinsic::X86LockedBtc;
      // The completed transaction owns both the old bit and the write.
      // Do not precede it with an unlocked read, even when CF is dead.
      S.emitIntrinsic(Id, NdVar::reg(x86reg::CF, 1), {ByteAddr, Idx},
                      NdMemoryOrdering::SequentiallyConsistent,
                      LiftState::memoryAddressSpace(X86.operands[0]));
      bitundefined::record(S);
      break;
    }

    // CF = (Base >> Idx) & 1
    NdVar Shifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Shifted, {Base, Idx});
    NdVar Masked = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Masked, {Shifted, NdVar::scalar(1, Sz)});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
           {Masked, NdVar::scalar(0, Sz)});

    if (InsnId != X86_INS_BT) {
      NdVar Mask = S.makeTemp(Sz);
      S.emit(NdOp::INT_LEFT, Mask, {NdVar::scalar(1, Sz), Idx});
      NdVar Result = (MemBase || X86.operands[0].type == X86_OP_MEM)
                         ? S.makeTemp(Sz)
                         : operandWrite(X86.operands[0]);
      if (InsnId == X86_INS_BTS)
        S.emit(NdOp::INT_OR, Result, {Base, Mask});
      else if (InsnId == X86_INS_BTR) {
        NdVar Inv = S.makeTemp(Sz);
        S.emit(NdOp::INT_NOT, Inv, {Mask});
        S.emit(NdOp::INT_AND, Result, {Base, Inv});
      } else { // BTC
        S.emit(NdOp::INT_XOR, Result, {Base, Mask});
      }
      if (MemBase)
        S.emit(NdOp::STORE, {}, {ByteAddr, Result}, NdMemoryOrdering::None,
               LiftState::memoryAddressSpace(X86.operands[0]));
    }
    bitundefined::record(S);
    break;
  }

  default:
    return liftCoreMove(*this, S, Insn, X86) ||
           liftCoreArith(*this, S, Insn, X86) ||
           liftCoreShift(*this, S, Insn, X86) ||
           liftCoreBit(*this, S, Insn, X86);
  }
  return true;
}

} // namespace neverd
