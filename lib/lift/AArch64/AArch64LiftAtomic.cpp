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
  // Shared prologue for the atomic load-ops: load the (access-sized) old value
  // from the memory operand's *effective address* — operandEffAddr, NOT
  // operandRead, which would dereference the pointer a second time and use the
  // loaded value as the address.  Zero-extend the old value into the register
  // destination and narrow the source to the access width.
  //
  // The "store" aliases (STADD/STCLR/STSET/STEOR/STSMAX/.../STUMIN, WZR
  // destination) decode to the same LD* instruction id but with op_count==2
  // (Src, [Xn]) and no destination register — the old value is discarded but
  // memory must still be updated.  Handle both forms here.
  auto loadOpPrologue = [&](NdVar &EA, NdVar &OldVal, NdVar &SrcN) {
    NdVar SrcReg = operandRead(S, ARM64.operands[0]);
    // Snapshot the source value up front.  For SWP/LD<op> with Rs==Rt (a valid
    // encoding, e.g. `swp x0,x0,[x1]`) the destination write below stores the
    // loaded memory value into the *same* register that holds the source — so a
    // later read of the source would see the clobbered value and store the
    // wrong datum back to memory.  Copying to a temp decouples it from the
    // register.
    NdVar Src = S.makeTemp(SrcReg.Size);
    S.emit(NdOp::COPY, Src, {SrcReg});
    bool StoreForm = (ARM64.op_count < 3); // STADD/ST* alias: no dest register
    unsigned MemIdx = StoreForm ? 1 : 2;
    EA = operandEffAddr(S, ARM64.operands[MemIdx]);
    uint16_t Asz = accessSize(Src.Size);
    OldVal = S.makeTemp(Asz);
    S.emit(NdOp::LOAD, OldVal, {EA});
    if (!StoreForm) {
      NdVar Dst = operandWrite(ARM64.operands[1]);
      if (Asz < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {OldVal});
      else
        S.emit(NdOp::COPY, Dst, {OldVal});
    }
    SrcN = narrowToWidth(S, Src, Asz);
  };

  switch (Insn->id) {

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
  case AARCH64_INS_LDCLRLH:
  case AARCH64_INS_LDCLRP:
  case AARCH64_INS_LDCLRPA:
  case AARCH64_INS_LDCLRPAL:
  case AARCH64_INS_LDCLRPL: {
    if (ARM64.op_count < 2)
      break;
    NdVar EA, OldVal, SrcN;
    loadOpPrologue(EA, OldVal, SrcN);
    NdVar Inv = S.makeTemp(OldVal.Size);
    S.emit(NdOp::INT_NOT, Inv, {SrcN});
    NdVar NV = S.makeTemp(OldVal.Size);
    S.emit(NdOp::INT_AND, NV, {OldVal, Inv});
    S.emit(NdOp::STORE, {}, {EA, NV});
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
    NdVar EA, OldVal, SrcN;
    loadOpPrologue(EA, OldVal, SrcN);
    NdVar NV = S.makeTemp(OldVal.Size);
    S.emit(NdOp::INT_XOR, NV, {OldVal, SrcN});
    S.emit(NdOp::STORE, {}, {EA, NV});
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
  case AARCH64_INS_LDSETLH:
  case AARCH64_INS_LDSETP:
  case AARCH64_INS_LDSETPA:
  case AARCH64_INS_LDSETPAL:
  case AARCH64_INS_LDSETPL: {
    if (ARM64.op_count < 2)
      break;
    NdVar EA, OldVal, SrcN;
    loadOpPrologue(EA, OldVal, SrcN);
    NdVar NV = S.makeTemp(OldVal.Size);
    S.emit(NdOp::INT_OR, NV, {OldVal, SrcN});
    S.emit(NdOp::STORE, {}, {EA, NV});
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
    // new = {s,u}{max,min}(old, Src).  The old handler wrongly emitted an
    // INT_OR into the destination register (which must receive the *old*
    // value) and then stored the unchanged old value back to memory, so the
    // operation, the returned value and the memory update were all wrong.
    if (ARM64.op_count < 2)
      break;
    NdVar EA, OldVal, SrcN;
    loadOpPrologue(EA, OldVal, SrcN);
    const char *Mn = Insn->mnemonic;
    // Signed = LDSMAX/LDSMIN (or store aliases STSMAX/STSMIN); checking for the
    // "smax"/"smin" substring covers both the ld* and st* prefixes.
    bool IsSigned = (std::strstr(Mn, "smax") != nullptr ||
                     std::strstr(Mn, "smin") != nullptr);
    bool IsMax = (std::strstr(Mn, "max") != nullptr);
    NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
    NdVar Cmp = S.makeTemp(1);
    if (IsMax)
      S.emit(CmpOp, Cmp, {SrcN, OldVal}); // (Src < old) ? old : Src = max
    else
      S.emit(CmpOp, Cmp, {OldVal, SrcN}); // (old < Src) ? old : Src = min
    NdVar NV = S.makeTemp(OldVal.Size);
    S.emit(NdOp::SELECT, NV, {Cmp, OldVal, SrcN});
    S.emit(NdOp::STORE, {}, {EA, NV});
    break;
  }

  // CASP — compare-and-swap pair.  `CASP Xs,Xs+1, Xt,Xt+1, [Xn]`:
  //   {lo,hi} = *[Xn];  if {lo,hi}=={Xs,Xs+1} then *[Xn] = {Xt,Xt+1};
  //   {Xs,Xs+1} = {lo,hi}  (old pair written back to the comparison regs).
  // The old handler was a single COPY placeholder (no compare/swap/writeback).
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
    uint16_t Rsz = ExpLo.Size; // 4 (W pair) or 8 (X pair)
    NdVar LoadedLo = S.makeTemp(Rsz);
    S.emit(NdOp::LOAD, LoadedLo, {EA});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Rsz, 8)});
    NdVar LoadedHi = S.makeTemp(Rsz);
    S.emit(NdOp::LOAD, LoadedHi, {EA2});
    NdVar CmpLo = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, CmpLo, {LoadedLo, ExpLo});
    NdVar CmpHi = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, CmpHi, {LoadedHi, ExpHi});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::INT_AND, Eq, {CmpLo, CmpHi});
    NdVar NewLo = S.makeTemp(Rsz);
    S.emit(NdOp::SELECT, NewLo, {Eq, DesLo, LoadedLo});
    NdVar NewHi = S.makeTemp(Rsz);
    S.emit(NdOp::SELECT, NewHi, {Eq, DesHi, LoadedHi});
    S.emit(NdOp::STORE, {}, {EA, NewLo});
    S.emit(NdOp::STORE, {}, {EA2, NewHi});
    S.emit(NdOp::COPY, DstLo, {LoadedLo});
    S.emit(NdOp::COPY, DstHi, {LoadedHi});
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
    if (Asz < Dst.Size) {
      NdVar L = S.makeTemp(Asz);
      S.emit(NdOp::LOAD, L, {EA});
      S.emit(NdOp::INT_ZEXT, Dst, {L});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA});
    }
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
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    NdVar SrcN = narrowToWidth(S, Src, accessSize(Src.Size));
    S.emit(NdOp::STORE, {}, {EA, SrcN});
    S.emit(NdOp::COPY, Status, {NdVar::cst(0, Status.Size)});
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
    S.emit(NdOp::LOAD, Dst1, {EA});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Dst1.Size, 8)});
    S.emit(NdOp::LOAD, Dst2, {EA2});
    break;
  }
  case AARCH64_INS_STXP:
  case AARCH64_INS_STLXP: {
    if (ARM64.op_count < 4)
      break;
    NdVar Status = operandWrite(ARM64.operands[0]);
    NdVar Src1 = operandRead(S, ARM64.operands[1]);
    NdVar Src2 = operandRead(S, ARM64.operands[2]);
    NdVar EA = operandEffAddr(S, ARM64.operands[3]);
    S.emit(NdOp::STORE, {}, {EA, Src1});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Src1.Size, 8)});
    S.emit(NdOp::STORE, {}, {EA2, Src2});
    S.emit(NdOp::COPY, Status, {NdVar::cst(0, Status.Size)});
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
    NdVar Loaded = S.makeTemp(Asz);
    S.emit(NdOp::LOAD, Loaded, {EA});
    // Compute the compare/conditional-store BEFORE writing Dst.  Dst is the
    // SAME register as Expected (Rs is both the compare value and the
    // load-back destination); writing Dst=loaded first would make Expected read
    // back as `loaded`, turning the compare into `loaded==loaded` (always true)
    // and storing Desired unconditionally even on a mismatch.
    NdVar ExpN = narrowToWidth(S, Expected, Asz);
    NdVar DesN = narrowToWidth(S, Desired, Asz);
    NdVar Cmp = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, Cmp, {Loaded, ExpN});
    NdVar NewMem = S.makeTemp(Asz);
    S.emit(NdOp::SELECT, NewMem, {Cmp, DesN, Loaded});
    S.emit(NdOp::STORE, {}, {EA, NewMem});
    // Write the old value back into Rs last.
    if (Asz < Dst.Size)
      S.emit(NdOp::INT_ZEXT, Dst, {Loaded});
    else
      S.emit(NdOp::COPY, Dst, {Loaded});
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
    NdVar EA, OldVal, SrcN;
    loadOpPrologue(EA, OldVal, SrcN);
    NdVar NewVal = S.makeTemp(OldVal.Size);
    S.emit(NdOp::INT_ADD, NewVal, {OldVal, SrcN});
    S.emit(NdOp::STORE, {}, {EA, NewVal});
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
    NdVar EA, OldVal, SrcN;
    loadOpPrologue(EA, OldVal, SrcN);
    S.emit(NdOp::STORE, {}, {EA, SrcN});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
