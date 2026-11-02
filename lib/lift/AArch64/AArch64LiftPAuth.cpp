//===- AArch64LiftPAuth.cpp - AArch64 pointer authentication ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Armv8.3 pointer signing, authentication, stripping and authenticated
/// control flow, including the implicit-register HINT encodings used by arm64e.
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BranchEncoding.h"

namespace neverd {

bool liftPAuth(AArch64Lifter &L, AArch64Lifter::LiftState &S,
               const cs_insn *Insn, const cs_aarch64 &A) {
  const auto reject = [&]() {
    throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
  };
  // Match complete words before the generic HINT/NOP handler. Capstone's
  // alias and non-alias presentations consume the same implicit operands.
  const NdVar ZeroModifier = NdVar::cst(0, kAArch64PAuthPointerBytes);
  const NdVar SPModifier = NdVar::reg(a64reg::SP, kAArch64PAuthPointerBytes);
  const NdVar X16Modifier = NdVar::reg(a64reg::X16, kAArch64PAuthPointerBytes);
  const uint32_t Word = readLE<uint32_t>(Insn->bytes);
  if (Word == kAArch64PACMEncoding) {
    S.emitIntrinsic(Intrinsic::A64_PACMPending, {}, {});
    return true;
  }
  const NdVar X15Modifier = NdVar::reg(a64reg::X15, kAArch64PAuthPointerBytes);
  const NdVar PCModifier = NdVar::cst(S.Addr, kAArch64PAuthPointerBytes);
  const NdVar PCRelModifier = NdVar::cst(
      S.Addr - ((Word >> kAArch64PAuthLRImmShift) & kAArch64PAuthLRImmMask) *
                   kAArch64PAuthLRScale,
      kAArch64PAuthPointerBytes);
  const auto registerModifier = [&](unsigned Number) {
    return Number == kAArch64PAuthLRRegMask
               ? ZeroModifier
               : NdVar::reg(a64reg::X0 + Number * kAArch64PAuthPointerBytes,
                            kAArch64PAuthPointerBytes);
  };
  const NdVar RnModifier = registerModifier((Word >> kAArch64PAuthLRImmShift) &
                                            kAArch64PAuthLRRegMask);
  const NdVar RmModifier = registerModifier(Word & kAArch64PAuthLRRegMask);
#define NEVERD_A64_PAUTH_LR_INSTRUCTION(Mask, Encoding, Id, Pointer, Mod,      \
                                        Mod2)                                  \
  if ((Word & Mask) == Encoding) {                                             \
    const NdVar Target =                                                       \
        NdVar::reg(a64reg::Pointer, kAArch64PAuthPointerBytes);                \
    S.emitIntrinsic(Intrinsic::Id, Target,                                     \
                    {Target, Mod##Modifier, Mod2##Modifier});                  \
    return true;                                                               \
  }
#define NEVERD_A64_PAUTH_LR_RETURN(Mask, Encoding, Id, Mod2)                   \
  if ((Word & Mask) == Encoding && Insn->id != AARCH64_INS_RETAA &&            \
      Insn->id != AARCH64_INS_RETAB) {                                         \
    const NdVar Target = S.makeTemp(kAArch64PAuthPointerBytes);                \
    S.emitIntrinsic(Intrinsic::Id, Target,                                     \
                    {NdVar::reg(a64reg::X30, kAArch64PAuthPointerBytes),       \
                     SPModifier, Mod2##Modifier});                             \
    S.emit(NdOp::RETURN, {}, {Target});                                        \
    return true;                                                               \
  }
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLR.def"
  switch (Word) {
#define NEVERD_A64_PAUTH_HINT(Insn, Word, Id, Pointer, Mod)                    \
  case Word: {                                                                 \
    const NdVar Target =                                                       \
        NdVar::reg(a64reg::Pointer, kAArch64PAuthPointerBytes);                \
    S.emitIntrinsic(Intrinsic::Id, Target, {Target, Mod##Modifier});           \
    return true;                                                               \
  }
#define NEVERD_A64_PAUTH_STRIP_HINT(Insn, Word, Id, Pointer)                   \
  case Word: {                                                                 \
    const NdVar Target =                                                       \
        NdVar::reg(a64reg::Pointer, kAArch64PAuthPointerBytes);                \
    S.emitIntrinsic(Intrinsic::Id, Target, {Target});                          \
    return true;                                                               \
  }
#define NEVERD_A64_PAUTH_UNSUPPORTED_HINT(Insn, Word)                          \
  case Word:                                                                   \
    reject();                                                                  \
    return false;
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.def"
  default:
    break;
  }

  Intrinsic Id;
  bool Zero = false;
  bool Strip = false;
  switch (Insn->id) {
#define NEVERD_A64_PAUTH_OPERATION(Intr, Insn, Mnemonic, Key, IsStrip, Auth)   \
  case AARCH64_INS_##Insn:                                                     \
    Id = Intrinsic::Intr;                                                      \
    Strip = IsStrip;                                                           \
    break;
#define NEVERD_A64_PAUTH_ZERO(Insn, Intr)                                      \
  case AARCH64_INS_##Insn:                                                     \
    Id = Intrinsic::Intr;                                                      \
    Zero = true;                                                               \
    break;
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.def"
  default: {
    NdOp Transfer;
    unsigned Key;
    switch (Insn->id) {
#define NEVERD_A64_PAUTH_BRANCH(Insn, Mask, Word, KeyName, IsZero, Kind)       \
  case AARCH64_INS_##Insn:                                                     \
    Transfer = NdOp::Kind;                                                     \
    Key = kAArch64PAuthKey##KeyName;                                           \
    Zero = IsZero;                                                             \
    break;
#define NEVERD_A64_PAUTH_UNSUPPORTED(Insn) case AARCH64_INS_##Insn:
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.def"
      reject();
      return false;
    default:
      return false;
    }
    const bool Return = Transfer == NdOp::RETURN;
    const bool Call = Transfer == NdOp::INDIR_CALL;
    if ((Return && A.op_count != 0) ||
        (!Return && (A.op_count != (Zero ? 1 : 2) ||
                     A.operands[0].type != AARCH64_OP_REG ||
                     (!Zero && A.operands[1].type != AARCH64_OP_REG))))
      reject();
    NdVar Pointer = Return ? NdVar::reg(a64reg::X30, kAArch64PAuthPointerBytes)
                           : L.operandRead(S, A.operands[0]);
    NdVar Modifier = Return ? SPModifier
                     : Zero ? ZeroModifier
                            : L.operandRead(S, A.operands[1]);
    NdVar Target = S.makeTemp(kAArch64PAuthPointerBytes);
    S.emitIntrinsic(
        Intrinsic::A64_AuthBranch, Target,
        {Pointer, NdVar::cst(Key, kAArch64PAuthKeyBytes), Modifier});
    // Capture/authenticate the old target and modifier before writing LR:
    // either source register of BLRAA/BLRAB can itself be x30.
    if (Call)
      S.emit(NdOp::COPY, NdVar::reg(a64reg::X30, kAArch64PAuthPointerBytes),
             {NdVar::cst(S.Addr + branch::A64InstructionBytes,
                         kAArch64PAuthPointerBytes)});
    S.emit(Transfer,
           Call ? NdVar::reg(a64reg::X0, kAArch64PAuthPointerBytes) : NdVar{},
           {Target});
    return true;
  }
  }
  if (A.op_count != (Zero || Strip ? 1 : 2) ||
      A.operands[0].type != AARCH64_OP_REG ||
      (!Zero && !Strip && A.operands[1].type != AARCH64_OP_REG))
    reject();
  NdVar Pointer = L.operandRead(S, A.operands[0]);
  NdVar Dst = L.operandWrite(A.operands[0]);
  if (Strip)
    S.emitIntrinsic(Id, Dst, {Pointer});
  else
    S.emitIntrinsic(
        Id, Dst,
        {Pointer, Zero ? ZeroModifier : L.operandRead(S, A.operands[1])});
  return true;
}

} // namespace neverd
