//===- AArch64LiftAtomic.cpp - AArch64 atomic instruction lifter ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 atomic instruction handlers: LDCLR/LDEOR/LDSET, exclusive
/// load/store (LDXR/STXR), acquire/release (LDAR/STLR), compare-and-swap
/// (CAS/CASP), atomic load-op (LDADD/SWP), and RCW atomics.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-aarch64"

namespace neverd {

bool AArch64Lifter::liftAtomic(LiftState &S, const cs_insn *Insn,
                               const cs_aarch64 &ARM64) {
  // Byte/halfword variants (mnemonic suffix 'b'/'h') access 1/2 bytes even
  // though the register operands are always W/X.  The memory access width and
  // the value that participates in the op must use this size, not the register.
  auto accessSize = [&](uint16_t RegSz) -> uint16_t {
    const char *Mn = Insn->mnemonic;
    size_t L = Mn ? std::strlen(Mn) : 0;
    if (L && Mn[L - 1] == 'b')
      return 1;
    if (L && Mn[L - 1] == 'h')
      return 2;
    return RegSz;
  };
  // LSE ordering is encoded as an A/L/AL suffix immediately before an
  // optional B/H access-width suffix.  Deriving it once from the decoded
  // mnemonic keeps every scalar RMW family and its store aliases consistent.
  auto lseOrdering = [&]() {
    llvm::StringRef Mn(Insn->mnemonic);
    if (Mn.ends_with("b") || Mn.ends_with("h"))
      Mn = Mn.drop_back();
    if (Mn.ends_with("al"))
      return NdMemoryOrdering::AcquireRelease;
    if (Mn.ends_with("a"))
      return NdMemoryOrdering::Acquire;
    if (Mn.ends_with("l"))
      return NdMemoryOrdering::Release;
    return NdMemoryOrdering::Relaxed;
  };

  // Snapshot the source before writing the returned old value.  Rs==Rt is a
  // valid encoding, and reading Rs after the destination write would otherwise
  // feed the just-loaded value back into the RMW.  The ST* aliases decode with
  // two operands and discard the old value while retaining the memory effect.
  auto loadOpOperands = [&](NdVar &EA, NdVar &SrcN, bool &StoreForm) {
    NdVar SrcReg = operandRead(S, ARM64.operands[0]);
    NdVar Src = S.makeTemp(SrcReg.Size);
    S.emit(NdOp::COPY, Src, {SrcReg});
    StoreForm = ARM64.op_count < 3;
    unsigned MemIdx = StoreForm ? 1 : 2;
    EA = operandEffAddr(S, ARM64.operands[MemIdx]);
    uint16_t Asz = accessSize(Src.Size);
    SrcN = narrowToWidth(S, Src, Asz);
    return Asz;
  };

  auto writeLoadOpResult = [&](const NdVar &OldVal, bool StoreForm) {
    if (!StoreForm) {
      NdVar Dst = operandWrite(ARM64.operands[1]);
      if (OldVal.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {OldVal});
      else
        S.emit(NdOp::COPY, Dst, {OldVal});
    }
  };

  auto emitScalarAtomicRMW = [&](Intrinsic Id, const NdVar &EA,
                                 const NdVar &Operand, uint16_t Asz,
                                 bool StoreForm) {
    NdMemoryOrdering Ordering = lseOrdering();
    NdVar OldVal = S.makeTemp(Asz);
    S.emitIntrinsic(
        Id, OldVal,
        {Operand, EA, NdVar::cst(Asz, 2),
         NdVar::cst(static_cast<uint64_t>(Ordering), 1)},
        Ordering);
    writeLoadOpResult(OldVal, StoreForm);
  };

  switch (Insn->id) {

  // ========================================================================
  // FEAT_LSE128 LDCLRP — atomically clear a pair of 64-bit words and return
  // the old pair in the same two registers.  This cannot share the scalar
  // LDCLR prologue: the architectural memory access is one aligned i128 RMW.
  // ========================================================================
  case AARCH64_INS_LDCLRP:
  case AARCH64_INS_LDCLRPA:
  case AARCH64_INS_LDCLRPAL:
  case AARCH64_INS_LDCLRPL: {
    if (ARM64.op_count < 3)
      break;

    NdVar LowReg = operandRead(S, ARM64.operands[0]);
    NdVar HighReg = operandRead(S, ARM64.operands[1]);
    NdVar LowSrc = S.makeTemp(LowReg.Size);
    NdVar HighSrc = S.makeTemp(HighReg.Size);
    S.emit(NdOp::COPY, LowSrc, {LowReg});
    S.emit(NdOp::COPY, HighSrc, {HighReg});

    NdVar ClearPair = S.makeTemp(LowSrc.Size + HighSrc.Size);
    S.emit(NdOp::CONCAT, ClearPair, {HighSrc, LowSrc});
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    NdVar OldPair = S.makeTemp(ClearPair.Size);

    Intrinsic Id = Intrinsic::A64_Ldclrp;
    if (Insn->id == AARCH64_INS_LDCLRPA)
      Id = Intrinsic::A64_Ldclrpa;
    else if (Insn->id == AARCH64_INS_LDCLRPAL)
      Id = Intrinsic::A64_Ldclrpal;
    else if (Insn->id == AARCH64_INS_LDCLRPL)
      Id = Intrinsic::A64_Ldclrpl;
    NdMemoryOrdering Ordering = NdMemoryOrdering::Relaxed;
    if (Insn->id == AARCH64_INS_LDCLRPA)
      Ordering = NdMemoryOrdering::Acquire;
    else if (Insn->id == AARCH64_INS_LDCLRPAL)
      Ordering = NdMemoryOrdering::AcquireRelease;
    else if (Insn->id == AARCH64_INS_LDCLRPL)
      Ordering = NdMemoryOrdering::Release;
    S.emitIntrinsic(Id, OldPair, {ClearPair, EA}, Ordering);

    NdVar LowDst = operandWrite(ARM64.operands[0]);
    NdVar HighDst = operandWrite(ARM64.operands[1]);
    S.emit(NdOp::SUBBYTES, LowDst, {OldPair, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, HighDst, {OldPair, NdVar::cst(LowDst.Size, 4)});
    break;
  }

  // ========================================================================
  // Atomic LD{CLR,EOR,SET,SMAX,SMIN,UMAX,UMIN} — all ordering variants.
  // Pattern: old = *S.Addr; *S.Addr = op(old, Src); Dst = old.
  // ========================================================================
  case AARCH64_INS_LDCLR:
  case AARCH64_INS_LDCLRA:
  case AARCH64_INS_LDCLRAL:
  case AARCH64_INS_LDCLRL:
  case AARCH64_INS_LDCLRB:
  case AARCH64_INS_LDCLRAB:
  case AARCH64_INS_LDCLRALB:
  case AARCH64_INS_LDCLRLB:
  case AARCH64_INS_LDCLRH:
  case AARCH64_INS_LDCLRAH:
  case AARCH64_INS_LDCLRALH:
  case AARCH64_INS_LDCLRLH: {
    if (ARM64.op_count < 2)
      break;
    NdVar EA, SrcN;
    bool StoreForm = false;
    uint16_t Asz = loadOpOperands(EA, SrcN, StoreForm);
    NdVar Inv = S.makeTemp(Asz);
    S.emit(NdOp::INT_NOT, Inv, {SrcN});
    emitScalarAtomicRMW(Intrinsic::A64_AtomicAnd, EA, Inv, Asz, StoreForm);
    break;
  }

  case AARCH64_INS_LDEOR:
  case AARCH64_INS_LDEORA:
  case AARCH64_INS_LDEORAL:
  case AARCH64_INS_LDEORL:
  case AARCH64_INS_LDEORB:
  case AARCH64_INS_LDEORAB:
  case AARCH64_INS_LDEORALB:
  case AARCH64_INS_LDEORLB:
  case AARCH64_INS_LDEORH:
  case AARCH64_INS_LDEORAH:
  case AARCH64_INS_LDEORALH:
  case AARCH64_INS_LDEORLH: {
    if (ARM64.op_count < 2)
      break;
    NdVar EA, SrcN;
    bool StoreForm = false;
    uint16_t Asz = loadOpOperands(EA, SrcN, StoreForm);
    emitScalarAtomicRMW(Intrinsic::A64_AtomicXor, EA, SrcN, Asz, StoreForm);
    break;
  }

  // ========================================================================
  // FEAT_LSE128 LDSETP — atomically set a pair of 64-bit words and return the
  // old pair in the same two registers.
  // ========================================================================
  case AARCH64_INS_LDSETP:
  case AARCH64_INS_LDSETPA:
  case AARCH64_INS_LDSETPAL:
  case AARCH64_INS_LDSETPL: {
    if (ARM64.op_count < 3)
      break;

    NdVar LowReg = operandRead(S, ARM64.operands[0]);
    NdVar HighReg = operandRead(S, ARM64.operands[1]);
    NdVar LowSrc = S.makeTemp(LowReg.Size);
    NdVar HighSrc = S.makeTemp(HighReg.Size);
    S.emit(NdOp::COPY, LowSrc, {LowReg});
    S.emit(NdOp::COPY, HighSrc, {HighReg});

    NdVar SetPair = S.makeTemp(LowSrc.Size + HighSrc.Size);
    S.emit(NdOp::CONCAT, SetPair, {HighSrc, LowSrc});
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    NdVar OldPair = S.makeTemp(SetPair.Size);

    Intrinsic Id = Intrinsic::A64_Ldsetp;
    if (Insn->id == AARCH64_INS_LDSETPA)
      Id = Intrinsic::A64_Ldsetpa;
    else if (Insn->id == AARCH64_INS_LDSETPAL)
      Id = Intrinsic::A64_Ldsetpal;
    else if (Insn->id == AARCH64_INS_LDSETPL)
      Id = Intrinsic::A64_Ldsetpl;
    NdMemoryOrdering Ordering = NdMemoryOrdering::Relaxed;
    if (Insn->id == AARCH64_INS_LDSETPA)
      Ordering = NdMemoryOrdering::Acquire;
    else if (Insn->id == AARCH64_INS_LDSETPAL)
      Ordering = NdMemoryOrdering::AcquireRelease;
    else if (Insn->id == AARCH64_INS_LDSETPL)
      Ordering = NdMemoryOrdering::Release;
    S.emitIntrinsic(Id, OldPair, {SetPair, EA}, Ordering);

    NdVar LowDst = operandWrite(ARM64.operands[0]);
    NdVar HighDst = operandWrite(ARM64.operands[1]);
    S.emit(NdOp::SUBBYTES, LowDst, {OldPair, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, HighDst, {OldPair, NdVar::cst(LowDst.Size, 4)});
    break;
  }

  case AARCH64_INS_LDSET:
  case AARCH64_INS_LDSETA:
  case AARCH64_INS_LDSETAL:
  case AARCH64_INS_LDSETL:
  case AARCH64_INS_LDSETB:
  case AARCH64_INS_LDSETAB:
  case AARCH64_INS_LDSETALB:
  case AARCH64_INS_LDSETLB:
  case AARCH64_INS_LDSETH:
  case AARCH64_INS_LDSETAH:
  case AARCH64_INS_LDSETALH:
  case AARCH64_INS_LDSETLH: {
    if (ARM64.op_count < 2)
      break;
    NdVar EA, SrcN;
    bool StoreForm = false;
    uint16_t Asz = loadOpOperands(EA, SrcN, StoreForm);
    emitScalarAtomicRMW(Intrinsic::A64_AtomicOr, EA, SrcN, Asz, StoreForm);
    break;
  }

  case AARCH64_INS_LDSMAX:
  case AARCH64_INS_LDSMAXA:
  case AARCH64_INS_LDSMAXAL:
  case AARCH64_INS_LDSMAXL:
  case AARCH64_INS_LDSMAXB:
  case AARCH64_INS_LDSMAXAB:
  case AARCH64_INS_LDSMAXALB:
  case AARCH64_INS_LDSMAXLB:
  case AARCH64_INS_LDSMAXH:
  case AARCH64_INS_LDSMAXAH:
  case AARCH64_INS_LDSMAXALH:
  case AARCH64_INS_LDSMAXLH:
  case AARCH64_INS_LDSMIN:
  case AARCH64_INS_LDSMINA:
  case AARCH64_INS_LDSMINAL:
  case AARCH64_INS_LDSMINL:
  case AARCH64_INS_LDSMINB:
  case AARCH64_INS_LDSMINAB:
  case AARCH64_INS_LDSMINALB:
  case AARCH64_INS_LDSMINLB:
  case AARCH64_INS_LDSMINH:
  case AARCH64_INS_LDSMINAH:
  case AARCH64_INS_LDSMINALH:
  case AARCH64_INS_LDSMINLH:
  case AARCH64_INS_LDUMAX:
  case AARCH64_INS_LDUMAXA:
  case AARCH64_INS_LDUMAXAL:
  case AARCH64_INS_LDUMAXL:
  case AARCH64_INS_LDUMAXB:
  case AARCH64_INS_LDUMAXAB:
  case AARCH64_INS_LDUMAXALB:
  case AARCH64_INS_LDUMAXLB:
  case AARCH64_INS_LDUMAXH:
  case AARCH64_INS_LDUMAXAH:
  case AARCH64_INS_LDUMAXALH:
  case AARCH64_INS_LDUMAXLH:
  case AARCH64_INS_LDUMIN:
  case AARCH64_INS_LDUMINA:
  case AARCH64_INS_LDUMINAL:
  case AARCH64_INS_LDUMINL:
  case AARCH64_INS_LDUMINB:
  case AARCH64_INS_LDUMINAB:
  case AARCH64_INS_LDUMINALB:
  case AARCH64_INS_LDUMINLB:
  case AARCH64_INS_LDUMINH:
  case AARCH64_INS_LDUMINAH:
  case AARCH64_INS_LDUMINALH:
  case AARCH64_INS_LDUMINLH: {
    if (ARM64.op_count < 2)
      break;
    NdVar EA, SrcN;
    bool StoreForm = false;
    uint16_t Asz = loadOpOperands(EA, SrcN, StoreForm);
    const char *Mn = Insn->mnemonic;
    bool IsSigned = (std::strstr(Mn, "smax") != nullptr ||
                     std::strstr(Mn, "smin") != nullptr);
    bool IsMax = (std::strstr(Mn, "max") != nullptr);
    Intrinsic Id = IsSigned
                       ? (IsMax ? Intrinsic::A64_AtomicSmax
                                : Intrinsic::A64_AtomicSmin)
                       : (IsMax ? Intrinsic::A64_AtomicUmax
                                : Intrinsic::A64_AtomicUmin);
    emitScalarAtomicRMW(Id, EA, SrcN, Asz, StoreForm);
    break;
  }

  // CASP — compare-and-swap pair.  `CASP Xs,Xs+1, Xt,Xt+1, [Xn]`:
  //   {lo,hi} = *[Xn];  if {lo,hi}=={Xs,Xs+1} then *[Xn] = {Xt,Xt+1};
  //   {Xs,Xs+1} = {lo,hi}  (old pair written back to the comparison regs).
  // Keep the entire pair in one ATOMIC_CMPXCHG operation; separate scalar
  // loads and stores allow concurrent callers to lose updates.
  case AARCH64_INS_CASP:
  case AARCH64_INS_CASPA:
  case AARCH64_INS_CASPAL:
  case AARCH64_INS_CASPL: {
    if (ARM64.op_count < 5)
      break;
    NdVar ExpLo = operandRead(S, ARM64.operands[0]);
    NdVar ExpHi = operandRead(S, ARM64.operands[1]);
    NdVar DesLo = operandRead(S, ARM64.operands[2]);
    NdVar DesHi = operandRead(S, ARM64.operands[3]);
    NdVar EA = operandEffAddr(S, ARM64.operands[4]);
    NdVar DstLo = operandWrite(ARM64.operands[0]);
    NdVar DstHi = operandWrite(ARM64.operands[1]);
    NdVar ExpectedPair = S.makeTemp(ExpLo.Size + ExpHi.Size);
    NdVar DesiredPair = S.makeTemp(DesLo.Size + DesHi.Size);
    NdVar OldPair = S.makeTemp(ExpectedPair.Size);
    S.emit(NdOp::CONCAT, ExpectedPair, {ExpHi, ExpLo});
    S.emit(NdOp::CONCAT, DesiredPair, {DesHi, DesLo});

    NdMemoryOrdering Ordering = NdMemoryOrdering::Relaxed;
    if (Insn->id == AARCH64_INS_CASPA)
      Ordering = NdMemoryOrdering::Acquire;
    else if (Insn->id == AARCH64_INS_CASPAL)
      Ordering = NdMemoryOrdering::AcquireRelease;
    else if (Insn->id == AARCH64_INS_CASPL)
      Ordering = NdMemoryOrdering::Release;
    S.emit(NdOp::ATOMIC_CMPXCHG, OldPair, {EA, ExpectedPair, DesiredPair},
           Ordering);
    S.emit(NdOp::SUBBYTES, DstLo, {OldPair, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, DstHi, {OldPair, NdVar::cst(DstLo.Size, 4)});
    break;
  }

  // ========================================================================
  // RCW atomics (FEAT_LSE128 / FEAT_LRCPC3)
  // ========================================================================
  case AARCH64_INS_RCWCASP:
  case AARCH64_INS_RCWCASPA:
  case AARCH64_INS_RCWCASPAL:
  case AARCH64_INS_RCWCASPL:
  case AARCH64_INS_RCWSCASP:
  case AARCH64_INS_RCWSCASPA:
  case AARCH64_INS_RCWSCASPAL:
  case AARCH64_INS_RCWSCASPL: {
    // Pair syntax expands to five Capstone operands:
    //   expected-lo, expected-hi, desired-lo, desired-hi, [address].
    if (ARM64.op_count < 5)
      break;
    NdVar ExpLo = operandRead(S, ARM64.operands[0]);
    NdVar ExpHi = operandRead(S, ARM64.operands[1]);
    NdVar DesLo = operandRead(S, ARM64.operands[2]);
    NdVar DesHi = operandRead(S, ARM64.operands[3]);
    NdVar EA = operandEffAddr(S, ARM64.operands[4]);
    NdVar DstLo = operandWrite(ARM64.operands[0]);
    NdVar DstHi = operandWrite(ARM64.operands[1]);
    Intrinsic Id = Intrinsic::None;
    switch (Insn->id) {
    case AARCH64_INS_RCWCASP:
      Id = Intrinsic::A64_Rcwcasp;
      break;
    case AARCH64_INS_RCWCASPA:
      Id = Intrinsic::A64_Rcwcaspa;
      break;
    case AARCH64_INS_RCWCASPAL:
      Id = Intrinsic::A64_Rcwcaspal;
      break;
    case AARCH64_INS_RCWCASPL:
      Id = Intrinsic::A64_Rcwcaspl;
      break;
    case AARCH64_INS_RCWSCASP:
      Id = Intrinsic::A64_Rcwscasp;
      break;
    case AARCH64_INS_RCWSCASPA:
      Id = Intrinsic::A64_Rcwscaspa;
      break;
    case AARCH64_INS_RCWSCASPAL:
      Id = Intrinsic::A64_Rcwscaspal;
      break;
    case AARCH64_INS_RCWSCASPL:
      Id = Intrinsic::A64_Rcwscaspl;
      break;
    default:
      break;
    }

    NdVar ExpectedPair = S.makeTemp(16);
    S.emit(NdOp::CONCAT, ExpectedPair, {ExpHi, ExpLo});
    NdVar DesiredPair = S.makeTemp(16);
    S.emit(NdOp::CONCAT, DesiredPair, {DesHi, DesLo});
    NdVar OldPair = S.makeTemp(16);
    S.emitIntrinsic(Id, OldPair, {ExpectedPair, DesiredPair, EA});

    NdVar OldLo = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, OldLo, {OldPair, NdVar::cst(0, 4)});
    NdVar OldHi = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, OldHi, {OldPair, NdVar::cst(8, 4)});
    S.emit(NdOp::COPY, DstLo, {OldLo});
    S.emit(NdOp::COPY, DstHi, {OldHi});
    break;
  }
  case AARCH64_INS_RCWCAS:
  case AARCH64_INS_RCWCASA:
  case AARCH64_INS_RCWCASAL:
  case AARCH64_INS_RCWCASL:
  case AARCH64_INS_RCWSCAS:
  case AARCH64_INS_RCWSCASA:
  case AARCH64_INS_RCWSCASAL:
  case AARCH64_INS_RCWSCASL: {
    if (ARM64.op_count >= 3) {
      NdVar Expected = operandRead(S, ARM64.operands[0]);
      NdVar Desired = operandRead(S, ARM64.operands[1]);
      NdVar EA = operandEffAddr(S, ARM64.operands[2]);
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Loaded = S.makeTemp(Expected.Size);
      S.emit(NdOp::LOAD, Loaded, {EA});
      NdVar Cmp = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Cmp, {Loaded, Expected});
      S.emit(NdOp::SELECT, Dst, {Cmp, Desired, Loaded});
      S.emit(NdOp::STORE, {}, {EA, Dst});
    }
    break;
  }
  case AARCH64_INS_RCWCLR:
  case AARCH64_INS_RCWCLRA:
  case AARCH64_INS_RCWCLRAL:
  case AARCH64_INS_RCWCLRL:
  case AARCH64_INS_RCWCLRP:
  case AARCH64_INS_RCWCLRPA:
  case AARCH64_INS_RCWCLRPAL:
  case AARCH64_INS_RCWCLRPL:
  case AARCH64_INS_RCWSCLR:
  case AARCH64_INS_RCWSCLRA:
  case AARCH64_INS_RCWSCLRAL:
  case AARCH64_INS_RCWSCLRL:
  case AARCH64_INS_RCWSCLRP:
  case AARCH64_INS_RCWSCLRPA:
  case AARCH64_INS_RCWSCLRPAL:
  case AARCH64_INS_RCWSCLRPL: {
    // RCWCLR: atomically clear Bits — new = old & ~Src
    if (ARM64.op_count >= 3) {
      NdVar Src = operandRead(S, ARM64.operands[0]);
      NdVar Dst = operandWrite(ARM64.operands[1]);
      NdVar EA = operandEffAddr(S, ARM64.operands[2]);
      NdVar OldVal = S.makeTemp(Dst.Size);
      S.emit(NdOp::LOAD, OldVal, {EA});
      S.emit(NdOp::COPY, Dst, {OldVal});
      NdVar InvSrc = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, InvSrc, {Src});
      NdVar NewVal = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_AND, NewVal, {OldVal, InvSrc});
      S.emit(NdOp::STORE, {}, {EA, NewVal});
    }
    break;
  }
  case AARCH64_INS_RCWSET:
  case AARCH64_INS_RCWSETA:
  case AARCH64_INS_RCWSETAL:
  case AARCH64_INS_RCWSETL:
  case AARCH64_INS_RCWSETP:
  case AARCH64_INS_RCWSETPA:
  case AARCH64_INS_RCWSETPAL:
  case AARCH64_INS_RCWSETPL:
  case AARCH64_INS_RCWSSET:
  case AARCH64_INS_RCWSSETA:
  case AARCH64_INS_RCWSSETAL:
  case AARCH64_INS_RCWSSETL:
  case AARCH64_INS_RCWSSETP:
  case AARCH64_INS_RCWSSETPA:
  case AARCH64_INS_RCWSSETPAL:
  case AARCH64_INS_RCWSSETPL: {
    // RCWSET: atomically set Bits — new = old | Src
    if (ARM64.op_count >= 3) {
      NdVar Src = operandRead(S, ARM64.operands[0]);
      NdVar Dst = operandWrite(ARM64.operands[1]);
      NdVar EA = operandEffAddr(S, ARM64.operands[2]);
      NdVar OldVal = S.makeTemp(Dst.Size);
      S.emit(NdOp::LOAD, OldVal, {EA});
      S.emit(NdOp::COPY, Dst, {OldVal});
      NdVar NewVal = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_OR, NewVal, {OldVal, Src});
      S.emit(NdOp::STORE, {}, {EA, NewVal});
    }
    break;
  }
  case AARCH64_INS_RCWSWP:
  case AARCH64_INS_RCWSWPA:
  case AARCH64_INS_RCWSWPAL:
  case AARCH64_INS_RCWSWPL:
  case AARCH64_INS_RCWSWPP:
  case AARCH64_INS_RCWSWPPA:
  case AARCH64_INS_RCWSWPPAL:
  case AARCH64_INS_RCWSWPPL:
  case AARCH64_INS_RCWSSWP:
  case AARCH64_INS_RCWSSWPA:
  case AARCH64_INS_RCWSSWPAL:
  case AARCH64_INS_RCWSSWPL:
  case AARCH64_INS_RCWSSWPP:
  case AARCH64_INS_RCWSSWPPA:
  case AARCH64_INS_RCWSSWPPAL:
  case AARCH64_INS_RCWSSWPPL: {
    // RCWSWP: atomically swap — new = Src
    if (ARM64.op_count >= 3) {
      NdVar Src = operandRead(S, ARM64.operands[0]);
      NdVar Dst = operandWrite(ARM64.operands[1]);
      NdVar EA = operandEffAddr(S, ARM64.operands[2]);
      NdVar OldVal = S.makeTemp(Dst.Size);
      S.emit(NdOp::LOAD, OldVal, {EA});
      S.emit(NdOp::COPY, Dst, {OldVal});
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    break;
  }

  // WKDMC / WKDMD (WKdm compression)
  case AARCH64_INS_WKDMC:
    S.emitIntrinsic(Intrinsic::A64_Wkdmc);
    break;
  case AARCH64_INS_WKDMD:
    S.emitIntrinsic(Intrinsic::A64_Wkdmd);
    break;

    // ST2G (store allocation tag pair) — already handled above in MTE block
    // LDADDP (load-add pair) etc. handled via LDADD grouping

  // ========================================================================
  // Exclusive load/store (LDXR / STXR)
  // ========================================================================
  case AARCH64_INS_LDXR:
  case AARCH64_INS_LDXRB:
  case AARCH64_INS_LDXRH:
  case AARCH64_INS_LDAXR:
  case AARCH64_INS_LDAXRB:
  case AARCH64_INS_LDAXRH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
    uint16_t Asz = accessSize(Dst.Size);
    NdVar Loaded = S.makeTemp(Asz);
    bool Acquire = Insn->id == AARCH64_INS_LDAXR ||
                   Insn->id == AARCH64_INS_LDAXRB ||
                   Insn->id == AARCH64_INS_LDAXRH;
    S.emitIntrinsic(Acquire ? Intrinsic::A64_Ldaxr : Intrinsic::A64_Ldxr,
                    Loaded, {EA, NdVar::cst(Asz, 2)},
                    Acquire ? NdMemoryOrdering::Acquire
                            : NdMemoryOrdering::Relaxed);
    if (Asz < Dst.Size)
      S.emit(NdOp::INT_ZEXT, Dst, {Loaded});
    else
      S.emit(NdOp::COPY, Dst, {Loaded});
    break;
  }
  case AARCH64_INS_STXR:
  case AARCH64_INS_STXRB:
  case AARCH64_INS_STXRH:
  case AARCH64_INS_STLXR:
  case AARCH64_INS_STLXRB:
  case AARCH64_INS_STLXRH: {
    if (ARM64.op_count < 3)
      break;
    NdVar Status = operandWrite(ARM64.operands[0]);
    NdVar SrcReg = operandRead(S, ARM64.operands[1]);
    NdVar Src = S.makeTemp(SrcReg.Size);
    S.emit(NdOp::COPY, Src, {SrcReg});
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    NdVar SrcN = narrowToWidth(S, Src, accessSize(Src.Size));
    bool Release = Insn->id == AARCH64_INS_STLXR ||
                   Insn->id == AARCH64_INS_STLXRB ||
                   Insn->id == AARCH64_INS_STLXRH;
    S.emitIntrinsic(Release ? Intrinsic::A64_Stlxr : Intrinsic::A64_Stxr,
                    Status, {SrcN, EA, NdVar::cst(SrcN.Size, 2)},
                    Release ? NdMemoryOrdering::Release
                            : NdMemoryOrdering::Relaxed);
    break;
  }

  // ========================================================================
  // Acquire/release load/store (LDAR / STLR)
  // ========================================================================
  case AARCH64_INS_LDAR:
  case AARCH64_INS_LDARB:
  case AARCH64_INS_LDARH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
    uint16_t Asz = accessSize(Dst.Size);
    if (Asz < Dst.Size) {
      NdVar L = S.makeTemp(Asz);
      S.emit(NdOp::LOAD, L, {EA}, NdMemoryOrdering::Acquire);
      S.emit(NdOp::INT_ZEXT, Dst, {L});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA}, NdMemoryOrdering::Acquire);
    }
    break;
  }
  case AARCH64_INS_STLR:
  case AARCH64_INS_STLRB:
  case AARCH64_INS_STLRH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM64.operands[0]);
    NdVar EA = operandEffAddr(S, ARM64.operands[1]);
    NdVar SrcN = narrowToWidth(S, Src, accessSize(Src.Size));
    S.emit(NdOp::STORE, {}, {EA, SrcN}, NdMemoryOrdering::Release);
    break;
  }

  // ========================================================================
  // Exclusive pair load/store (LDXP / STXP / LDAXP / STLXP)
  // ========================================================================
  case AARCH64_INS_LDXP:
  case AARCH64_INS_LDAXP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst1 = operandWrite(ARM64.operands[0]);
    NdVar Dst2 = operandWrite(ARM64.operands[1]);
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    NdVar Pair = S.makeTemp(Dst1.Size + Dst2.Size);
    bool Acquire = Insn->id == AARCH64_INS_LDAXP;
    S.emitIntrinsic(Acquire ? Intrinsic::A64_Ldaxp : Intrinsic::A64_Ldxp, Pair,
                    {EA, NdVar::cst(Pair.Size, 2)},
                    Acquire ? NdMemoryOrdering::Acquire
                            : NdMemoryOrdering::Relaxed);
    S.emit(NdOp::SUBBYTES, Dst1, {Pair, NdVar::cst(0, 4)});
    S.emit(NdOp::SUBBYTES, Dst2, {Pair, NdVar::cst(Dst1.Size, 4)});
    break;
  }
  case AARCH64_INS_STXP:
  case AARCH64_INS_STLXP: {
    if (ARM64.op_count < 4)
      break;
    NdVar Status = operandWrite(ARM64.operands[0]);
    NdVar Src1Reg = operandRead(S, ARM64.operands[1]);
    NdVar Src2Reg = operandRead(S, ARM64.operands[2]);
    NdVar Src1 = S.makeTemp(Src1Reg.Size);
    NdVar Src2 = S.makeTemp(Src2Reg.Size);
    S.emit(NdOp::COPY, Src1, {Src1Reg});
    S.emit(NdOp::COPY, Src2, {Src2Reg});
    NdVar EA = operandEffAddr(S, ARM64.operands[3]);
    NdVar Pair = S.makeTemp(Src1.Size + Src2.Size);
    S.emit(NdOp::CONCAT, Pair, {Src2, Src1});
    bool Release = Insn->id == AARCH64_INS_STLXP;
    S.emitIntrinsic(Release ? Intrinsic::A64_Stlxp : Intrinsic::A64_Stxp,
                    Status, {Pair, EA, NdVar::cst(Pair.Size, 2)},
                    Release ? NdMemoryOrdering::Release
                            : NdMemoryOrdering::Relaxed);
    break;
  }

  // ========================================================================
  // Compare-and-swap (CAS / CASP, ARMv8.1)
  // ========================================================================
  case AARCH64_INS_CAS:
  case AARCH64_INS_CASA:
  case AARCH64_INS_CASAL:
  case AARCH64_INS_CASL:
  case AARCH64_INS_CASB:
  case AARCH64_INS_CASH:
  case AARCH64_INS_CASAB:
  case AARCH64_INS_CASAH:
  case AARCH64_INS_CASALB:
  case AARCH64_INS_CASALH:
  case AARCH64_INS_CASLB:
  case AARCH64_INS_CASLH: {
    if (ARM64.op_count < 3)
      break;
    NdVar Expected = operandRead(S, ARM64.operands[0]);
    NdVar Desired = operandRead(S, ARM64.operands[1]);
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    NdVar Dst = operandWrite(ARM64.operands[0]);
    uint16_t Asz = accessSize(Dst.Size);
    NdVar ExpN = narrowToWidth(S, Expected, Asz);
    NdVar DesN = narrowToWidth(S, Desired, Asz);
    NdVar Old = S.makeTemp(Asz);

    NdMemoryOrdering Ordering = lseOrdering();
    S.emit(NdOp::ATOMIC_CMPXCHG, Old, {EA, ExpN, DesN}, Ordering);
    if (Asz < Dst.Size)
      S.emit(NdOp::INT_ZEXT, Dst, {Old});
    else
      S.emit(NdOp::COPY, Dst, {Old});
    break;
  }

  // ========================================================================
  // Atomic load-op (LDADD / SWP, all ordering variants)
  // ========================================================================
  case AARCH64_INS_LDADD:
  case AARCH64_INS_LDADDA:
  case AARCH64_INS_LDADDAL:
  case AARCH64_INS_LDADDL:
  case AARCH64_INS_LDADDB:
  case AARCH64_INS_LDADDAB:
  case AARCH64_INS_LDADDALB:
  case AARCH64_INS_LDADDLB:
  case AARCH64_INS_LDADDH:
  case AARCH64_INS_LDADDAH:
  case AARCH64_INS_LDADDALH:
  case AARCH64_INS_LDADDLH: {
    if (ARM64.op_count < 2)
      break;

    NdVar SrcReg = operandRead(S, ARM64.operands[0]);
    NdVar Src = S.makeTemp(SrcReg.Size);
    S.emit(NdOp::COPY, Src, {SrcReg});
    bool StoreForm = ARM64.op_count < 3;
    unsigned MemIdx = StoreForm ? 1 : 2;
    NdVar EA = operandEffAddr(S, ARM64.operands[MemIdx]);
    uint16_t Asz = accessSize(Src.Size);
    NdVar SrcN = narrowToWidth(S, Src, Asz);
    NdVar OldVal = S.makeTemp(Asz);

    NdMemoryOrdering Ordering = lseOrdering();

    S.emit(NdOp::ATOMIC_ADD, OldVal, {EA, SrcN}, Ordering);
    if (!StoreForm) {
      NdVar Dst = operandWrite(ARM64.operands[1]);
      if (Asz < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {OldVal});
      else
        S.emit(NdOp::COPY, Dst, {OldVal});
    }
    break;
  }
  case AARCH64_INS_SWP:
  case AARCH64_INS_SWPA:
  case AARCH64_INS_SWPAL:
  case AARCH64_INS_SWPL:
  case AARCH64_INS_SWPB:
  case AARCH64_INS_SWPAB:
  case AARCH64_INS_SWPALB:
  case AARCH64_INS_SWPLB:
  case AARCH64_INS_SWPH:
  case AARCH64_INS_SWPAH:
  case AARCH64_INS_SWPALH:
  case AARCH64_INS_SWPLH: {
    if (ARM64.op_count < 3)
      break;
    NdVar EA, SrcN;
    bool StoreForm = false;
    uint16_t Asz = loadOpOperands(EA, SrcN, StoreForm);
    NdVar OldVal = S.makeTemp(Asz);
    S.emit(NdOp::ATOMIC_XCHG, OldVal, {EA, SrcN}, lseOrdering());
    writeLoadOpResult(OldVal, StoreForm);
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
