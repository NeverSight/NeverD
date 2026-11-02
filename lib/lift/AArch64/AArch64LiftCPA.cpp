//===- AArch64LiftCPA.cpp - Checked pointer arithmetic ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Scalar and scalable CPA read runtime checking controls. Preserve an
/// instruction-level operation instead of replacing it with wrapping integer
/// arithmetic.
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {

bool liftCPA(AArch64Lifter &L, AArch64Lifter::LiftState &S, const cs_insn *Insn,
             const cs_aarch64 &A) {
  const auto reject = [&]() {
    throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
  };
  const uint32_t Word = readLE<uint32_t>(Insn->bytes);
  if (Insn->id == AARCH64_INS_MOVPRFX) {
    using namespace aarch64_sve;
    const auto Prefix = cpaPrefix(Word, S.FollowingWord, S.Addr);
    if (!Prefix || Insn->size != kPrefixInstructionBytes ||
        A.op_count != Prefix->RawCount)
      reject();
    const auto Dst =
        NdVar::reg(a64reg::Z(Prefix->Destination), kVectorStorageBytes);
    const auto Src = NdVar::reg(a64reg::Z(Prefix->Source), kVectorStorageBytes);
    if (Prefix->Form == PrefixForm::Unpredicated) {
      S.emit(NdOp::COPY, Dst, {Src});
    } else {
      const auto Pred =
          NdVar::reg(a64reg::P(Prefix->Predicate), kPredicateStorageBytes);
      const auto Element = NdVar::cst(kCPAElementBytes, kElementOperandBytes);
      if (Prefix->Form == PrefixForm::Merging)
        S.emitIntrinsic(Intrinsic::A64_SveMovprfxMerge, Dst,
                        {Pred, Src, Dst, Element});
      else
        S.emitIntrinsic(Intrinsic::A64_SveMovprfxZero, Dst,
                        {Pred, Src, Element});
    }
    return true;
  }
  std::optional<aarch64_sve::CPAOperation> Scalable;
#define NEVERD_A64_SVE_CPA_OPERATION(Id, InsnId, Name, Form, Mask, Encoding,   \
                                     ...)                                      \
  if (Insn->id == AARCH64_INS_##InsnId && (Word & Mask) == Encoding)           \
    Scalable = aarch64_sve::cpaOperation(Intrinsic::Id);
#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.def"
  if (Scalable) {
    using namespace aarch64_sve;
    if (A.op_count != cpaRoles(Scalable->Form).RawCount)
      reject();
    // The original word owns register roles, including the implicit old
    // destination of MADPT/MLAPT and the tied merging ADDPT/SUBPT operand.
    const auto vector = [&](unsigned Shift) {
      return NdVar::reg(a64reg::Z((Word >> Shift) & kCPARegisterMask),
                        kVectorStorageBytes);
    };
    const auto Dst = vector(0);
    const auto Second = vector(kCPASecondShift);
    const auto Third = vector(kCPAThirdShift);
    const auto Element = NdVar::cst(kCPAElementBytes, kElementOperandBytes);
    switch (Scalable->Form) {
    case CPAForm::Binary:
      S.emitIntrinsic(Scalable->Id, Dst, {Second, Third, Element});
      break;
    case CPAForm::Merging: {
      const auto Pred = NdVar::reg(
          a64reg::P((Word >> kCPAPredicateShift) & kCPAPredicateMask),
          kPredicateStorageBytes);
      S.emitIntrinsic(Scalable->Id, Dst, {Pred, Dst, Second, Element});
      break;
    }
    case CPAForm::Multiply:
      if (Scalable->Id == Intrinsic::A64_SveMadpt)
        S.emitIntrinsic(Scalable->Id, Dst, {Dst, Third, Second, Element});
      else
        S.emitIntrinsic(Scalable->Id, Dst, {Dst, Second, Third, Element});
      break;
    }
    return true;
  }
  Intrinsic Id;
  switch (Insn->id) {
#define NEVERD_A64_CPA(IdValue, Insn, ...)                                     \
  case AARCH64_INS_##Insn:                                                     \
    Id = Intrinsic::IdValue;                                                   \
    break;
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.def"
  case AARCH64_INS_MADPT:
  case AARCH64_INS_MLAPT:
    // No other multiply form has a defined checked-pointer contract.
    reject();
    return false;
  default:
    return false;
  }
  const auto Operation = *aarch64CPAOperation(Id);
  if ((readLE<uint32_t>(Insn->bytes) & Operation.Mask) != Operation.Encoding ||
      A.op_count != Operation.Sources + 1)
    reject();
  for (unsigned I = 0; I < A.op_count; ++I) {
    const auto &Operand = A.operands[I];
    if (Operand.type != AARCH64_OP_REG ||
        mapCapstoneReg(static_cast<aarch64_reg>(Operand.reg)).Size !=
            kAArch64CPAPointerBytes ||
        Operand.ext != AARCH64_EXT_INVALID || Operand.vector_index >= 0 ||
        (Operand.shift.type != AARCH64_SFT_INVALID &&
         (Operation.Sources != 2 || I != 2 ||
          Operand.shift.type != AARCH64_SFT_LSL ||
          Operand.shift.value > kAArch64CPAMaxShift)))
      reject();
  }
  const NdVar Dst = L.operandWrite(A.operands[0]);
  const NdVar First = L.operandRead(S, A.operands[1]);
  const NdVar Second = L.operandRead(S, A.operands[2]);
  if (Operation.Sources == 2)
    S.emitIntrinsic(Id, Dst, {First, Second});
  else
    S.emitIntrinsic(Id, Dst, {First, Second, L.operandRead(S, A.operands[3])});
  return true;
}

} // namespace neverd
