//===- X86DecimalAdjustAudit.h - Decimal byte and flag audit ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate implicit decimal operations from their original instruction
/// bytes, independently of decoded immediate and flag annotations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86DECIMALADJUSTAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86DECIMALADJUSTAUDIT_H

#include "X86LegacyScalarPrefixes.h"
#include "X86UndefinedFlagAudit.h"

#include "neverd/ir/X86/X86DecimalAdjust.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/ADT/SmallVector.h"

#include <array>
#include <optional>

namespace neverd::decimaladjust {

inline bool isInstruction(unsigned Id) {
  return Id == X86_INS_AAA || Id == X86_INS_AAS || Id == X86_INS_DAA ||
         Id == X86_INS_DAS || Id == X86_INS_AAM || Id == X86_INS_AAD;
}

inline std::optional<X86DecimalAdjustInstruction> encoding(const cs_insn *I,
                                                           Arch Target) {
  if (!I || !I->detail || !isInstruction(I->id) || Target != Arch::X86 ||
      I->size == 0 || I->size > 15)
    return std::nullopt;
  legacyvalidation::ScalarPrefixes P;
  if (!legacyvalidation::scalarPrefixes(I, false, P) || P.F3)
    return std::nullopt;
  const auto Op = x86DecimalAdjustOperation(I->bytes[P.OpcodeOffset]);
  if (!Op)
    return std::nullopt;
  using Kind = X86DecimalAdjustOperation;
  const unsigned Id = *Op == Kind::AAA   ? X86_INS_AAA
                      : *Op == Kind::AAS ? X86_INS_AAS
                      : *Op == Kind::DAA ? X86_INS_DAA
                      : *Op == Kind::DAS ? X86_INS_DAS
                      : *Op == Kind::AAM ? X86_INS_AAM
                                         : X86_INS_AAD;
  X86DecimalAdjustInstruction F{*Op};
  if (I->id != Id || P.OpcodeOffset + (F.hasBase() ? 2 : 1) != I->size)
    return std::nullopt;
  if (F.hasBase())
    F.Base = I->bytes[P.OpcodeOffset + 1];
  return F;
}

inline llvm::SmallVector<unsigned, 4>
flags(const X86DecimalAdjustInstruction &F) {
  constexpr std::array<unsigned, 6> Offsets{x86reg::CF, x86reg::PF, x86reg::AF,
                                            x86reg::ZF, x86reg::SF, x86reg::OF};
  constexpr std::array<unsigned, 6> Bits{0, 2, 4, 6, 7, 11};
  llvm::SmallVector<unsigned, 4> Flags;
  for (unsigned I = 0; I != Bits.size(); ++I)
    if (F.undefinedFlagsMask() & (1u << Bits[I]))
      Flags.push_back(Offsets[I]);
  return Flags;
}

inline void record(X86Lifter::LiftState &S,
                   const X86DecimalAdjustInstruction &F) {
  for (const auto Flag : flags(F))
    S.recordUndefinedBits(NdVar::reg(Flag, 1), 0, 1);
}

inline bool form(const cs_insn *I, Arch Target) {
  const auto F = encoding(I, Target);
  return F && !F->raisesDivideError();
}

inline bool matches(const cs_insn *I, Arch Target, llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  const auto F = encoding(I, Target);
  return F && !F->raisesDivideError() &&
         undefinedflags::matches(flags(*F), Ops, Effects);
}

} // namespace neverd::decimaladjust

#endif // NEVERD_LIB_LIFT_X86_X86DECIMALADJUSTAUDIT_H
