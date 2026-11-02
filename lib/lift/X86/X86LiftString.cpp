//===- X86LiftString.cpp - x86/x64 string & system instruction lifter ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches the x86/x64 string operations, and lifts the compare/scan forms
/// (CMPS/SCAS and the CMPSD/CMPSS disambiguation) here because they reach the
/// shared REP completion helper and the local direction-flag step helper.
/// MOVS/STOS/LODS/XLATB are in X86LiftStringMove.cpp and the
/// trap/serializing/system-call instructions in X86LiftSystem.cpp.
///
//===----------------------------------------------------------------------===//

#include "X86FPMaskCompareState.h"
#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/Support/Debug.h"

#include <iterator>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

// Element size (bytes) of a MOVS/STOS/LODS/SCAS/CMPS variant.
unsigned stringElemSize(unsigned InsnId) {
  switch (InsnId) {
  case X86_INS_MOVSW:
  case X86_INS_STOSW:
  case X86_INS_LODSW:
  case X86_INS_SCASW:
  case X86_INS_CMPSW:
    return 2;
  case X86_INS_MOVSD:
  case X86_INS_STOSD:
  case X86_INS_LODSD:
  case X86_INS_SCASD:
    return 4;
  case X86_INS_MOVSQ:
  case X86_INS_STOSQ:
  case X86_INS_LODSQ:
  case X86_INS_SCASQ:
  case X86_INS_CMPSQ:
    return 8;
  default:
    return 1;
  }
}

NdMemoryAddressSpace stringSourceAddressSpace(const cs_x86 &X86) {
  for (uint8_t I = 0; I < X86.op_count; ++I) {
    const cs_x86_op &Op = X86.operands[I];
    if (Op.type != X86_OP_MEM)
      continue;
    NdMemoryAddressSpace AddressSpace =
        X86Lifter::LiftState::memoryAddressSpace(Op);
    if (AddressSpace != NdMemoryAddressSpace::Default)
      return AddressSpace;
  }

  // Capstone does not materialize XLAT's implicit [RBX + AL] operand.  Keep
  // the prefix fallback here as part of the same source-segment policy rather
  // than duplicating instruction-byte tests in the individual handlers.
  for (uint8_t Prefix : X86.prefix) {
    if (Prefix == X86_PREFIX_FS)
      return NdMemoryAddressSpace::X86FS;
    if (Prefix == X86_PREFIX_GS)
      return NdMemoryAddressSpace::X86GS;
  }
  return NdMemoryAddressSpace::Default;
}

namespace {

// True when a REPNE/REPNZ prefix (0xF2) is attached.  On CMPS/SCAS this selects
// the "repeat while equal fails" loop (`memchr` / `strlen` idioms); REP/REPE
// (0xF3) selects the "repeat while equal" loop (`memcmp`).
bool hasRepnePrefix(const cs_x86 &X86) {
  for (int I = 0; I < 4; ++I)
    if (X86.prefix[I] == X86_PREFIX_REPNE)
      return true;
  return false;
}

// True when any repeat prefix is attached: REP/REPE (0xF3, == X86_PREFIX_REPE)
// or REPNE/REPNZ (0xF2).  CMPS/SCAS use REPE for `==`-terminated loops and
// REPNE for `!=`-terminated loops (memcmp / strlen / memchr idioms).
bool hasRepOrRepnePrefix(const cs_x86 &X86) {
  for (int I = 0; I < 4; ++I)
    if (X86.prefix[I] == X86_PREFIX_REP || X86.prefix[I] == X86_PREFIX_REPNE)
      return true;
  return false;
}

} // namespace

// Direction-aware pointer step for single string ops: +ElemSz when DF=0
// (forward) and -ElemSz when DF=1 (backward).  Folds to a constant whenever the
// direction flag is statically known (e.g. after CLD/STD), so the common
// forward case keeps its original zero-cost lowering.
NdVar X86Lifter::dirStep(LiftState &S, unsigned ElemSz) {
  const uint16_t AddrSz = S.AddressSize;
  NdVar Step = S.makeTemp(AddrSz);
  S.emit(NdOp::SELECT, Step,
         {NdVar::reg(x86reg::DF, 1),
          NdVar::scalar(static_cast<uint64_t>(-static_cast<int64_t>(ElemSz)),
                        AddrSz),
          NdVar::scalar(ElemSz, AddrSz)});
  return Step;
}

bool X86Lifter::liftString(LiftState &S, const cs_insn *Insn,
                           const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // CMPSD/CMPSS (string variant vs SSE scalar disambiguation)
  case X86_INS_CMPSD:
  case X86_INS_CMPSS: {
    bool HasXmm = false;
    for (uint8_t N = 0; N < X86.op_count; ++N) {
      if (X86.operands[N].type == X86_OP_REG &&
          X86.operands[N].reg >= X86_REG_XMM0 &&
          X86.operands[N].reg <= X86_REG_XMM15) {
        HasXmm = true;
        break;
      }
    }
    if (HasXmm || InsnId == X86_INS_CMPSS ||
        hasLegacyFPMaskCompareOpcode(Insn, TargetArch)) {
      return liftFPMaskCompareState(*this, S, Insn, X86,
                                    InsnId == X86_INS_CMPSD, true, false);
    } else if (!hasRepOrRepnePrefix(X86)) {
      // String CMPSD (AT&T `cmpsl`, no XMM operand): compare DS:[RSI] with
      // ES:[RDI] as a 4-byte CMP and step both pointers per DF.
      // Single form only; the REP form falls to the placeholder below.
      unsigned ElemSz = 4;
      const uint16_t AddrSz = S.AddressSize;
      NdVar SiVal = S.makeTemp(ElemSz);
      S.emit(NdOp::LOAD, SiVal, {NdVar::reg(x86reg::RSI, AddrSz)},
             NdMemoryOrdering::None, stringSourceAddressSpace(X86));
      NdVar DiVal = S.makeTemp(ElemSz);
      S.emit(NdOp::LOAD, DiVal, {NdVar::reg(x86reg::RDI, AddrSz)});
      NdVar Res = S.makeTemp(ElemSz);
      S.emit(NdOp::INT_SUB, Res, {SiVal, DiVal});
      emitFlagsArith(S, Res, SiVal, DiVal, /*IsSub=*/true);
      NdVar Step = dirStep(S, ElemSz);
      NdVar NewSi = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, AddrSz), Step});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, AddrSz), {NewSi});
      NdVar NewDi = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, AddrSz), Step});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, AddrSz), {NewDi});
    } else {
      // String CMPSD (`cmpsl`) under REP/REPE/REPNE.  stringElemSize cannot
      // classify X86_INS_CMPSD (it doubles as the SSE scalar compare), so the
      // 4-byte element size is passed explicitly.
      liftRepState(*this, S, X86, Intrinsic::X86RepCmpsState, 4,
                   hasRepnePrefix(X86));
    }
    break;
  }

  // --- Compare/scan string ops (CMPS/SCAS) ---
  // SCAS compares the accumulator (AL/AX/EAX/RAX) with ES:[RDI]; CMPS compares
  // DS:[RSI] with ES:[RDI].  Both set the status flags exactly like CMP (a SUB
  // whose result is discarded) and advance the pointer(s) by the element size.
  //
  // The single (non-repeated) form has no memory write and a deterministic
  // pointer step, so the whole effect is modelled directly in MedIR (like
  // LODS) — reusing emitFlagsArith makes the flags bit-identical to a real CMP.
  // Pointers step +/-ElemSz per the direction flag (see dirStep), matching the
  // single MOVS/STOS/LODS handlers above.
  //
  // The REP/REPE/REPNE forms terminate on a data-dependent ZF transition, so
  // the final RCX/RDI/RSI come back from the hardware loop the backend emits
  // for the intrinsic; see liftRepState.
  case X86_INS_CMPSB:
  case X86_INS_CMPSW:
  case X86_INS_CMPSQ:
  case X86_INS_SCASB:
  case X86_INS_SCASW:
  case X86_INS_SCASD:
  case X86_INS_SCASQ: {
    bool IsScas = (InsnId == X86_INS_SCASB || InsnId == X86_INS_SCASW ||
                   InsnId == X86_INS_SCASD || InsnId == X86_INS_SCASQ);
    if (!hasRepOrRepnePrefix(X86)) {
      unsigned ElemSz = stringElemSize(InsnId);
      const uint16_t AddrSz = S.AddressSize;
      NdVar DiVal = S.makeTemp(ElemSz);
      S.emit(NdOp::LOAD, DiVal, {NdVar::reg(x86reg::RDI, AddrSz)});
      NdVar CmpA, CmpB = DiVal;
      if (IsScas) {
        // result = AL/AX/EAX/RAX - ES:[RDI]
        CmpA = NdVar::reg(x86reg::RAX, ElemSz);
      } else {
        // result = DS:[RSI] - ES:[RDI]
        NdVar SiVal = S.makeTemp(ElemSz);
        S.emit(NdOp::LOAD, SiVal, {NdVar::reg(x86reg::RSI, AddrSz)},
               NdMemoryOrdering::None, stringSourceAddressSpace(X86));
        CmpA = SiVal;
      }
      NdVar Res = S.makeTemp(ElemSz);
      S.emit(NdOp::INT_SUB, Res, {CmpA, CmpB});
      emitFlagsArith(S, Res, CmpA, CmpB, /*IsSub=*/true);
      // Step the pointer(s) by the element size per DF (forward/backward).
      NdVar Step = dirStep(S, ElemSz);
      if (!IsScas) {
        NdVar NewSi = S.makeTemp(AddrSz);
        S.emit(NdOp::INT_ADD, NewSi, {NdVar::reg(x86reg::RSI, AddrSz), Step});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::RSI, AddrSz), {NewSi});
      }
      NdVar NewDi = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_ADD, NewDi, {NdVar::reg(x86reg::RDI, AddrSz), Step});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::RDI, AddrSz), {NewDi});
      break;
    }
    // REP/REPE/REPNE form: data-dependent loop, lowered to the hardware
    // instruction (see liftRepState).
    liftRepState(*this, S, X86,
                 IsScas ? Intrinsic::X86RepScasState
                        : Intrinsic::X86RepCmpsState,
                 stringElemSize(InsnId), hasRepnePrefix(X86));
    break;
  }

  default:
    return liftStringMove(*this, S, Insn, X86) ||
           liftSystem(*this, S, Insn, X86);
  }
  return true;
}

} // namespace neverd
