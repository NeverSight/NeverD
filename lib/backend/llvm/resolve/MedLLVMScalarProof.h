//===- MedLLVMScalarProof.h - Bounded scalar expression proofs -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_SCALAR_PROOF_H
#define NEVERD_BACKEND_LLVM_SCALAR_PROOF_H

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/APInt.h"

#include <cstdint>
#include <optional>

namespace neverd::detail {

// Prove the complete 32-bit result of x - D * floor(x / D). The quotient is
// recovered from a 64-bit unsigned multiply, high-word extraction and logical
// shift. This proof is independent of the origin of x: even a relocatable
// pointer bit-pattern cannot make the remainder exceed D - 1.
template <typename LookupDefFn, typename TraceConstantFn>
bool provesUnsignedMagicRemainder(const MedVar &Value, LookupDefFn &&LookupDef,
                                  TraceConstantFn &&TraceConstant) {
  const MedOp *Subtract = LookupDef(Value);
  if (!Subtract || Subtract->Opcode != NdOp::INT_SUB ||
      Subtract->NumInputs != 2 || Subtract->Output.Size != 4 ||
      Subtract->Inputs[0].Size != 4 || Subtract->Inputs[1].Size != 4)
    return false;
  const MedVar Dividend = Subtract->Inputs[0];
  const MedOp *Scale = LookupDef(Subtract->Inputs[1]);
  if (!Scale || Scale->NumInputs != 2 || Scale->Output.Size != 4 ||
      Scale->Inputs[0].Size != 4 || Scale->Inputs[1].Size != 4)
    return false;
  auto sameValue = [](const MedVar &A, const MedVar &B) {
    if (A.isConst() || B.isConst() || A.Kind != B.Kind ||
        A.TheArch != B.TheArch || A.RenameTag != B.RenameTag || A.Id != B.Id ||
        A.SSAVer != B.SSAVer || A.Size != B.Size)
      return false;
    if (A.Kind == MedVar::Reg || A.Kind == MedVar::Param)
      return A.RegOff == B.RegOff;
    if (A.Kind == MedVar::Stack)
      return A.StackOff == B.StackOff;
    return true;
  };
  MedVar Quotient;
  uint64_t Divisor = 0;
  if (Scale->Opcode == NdOp::INT_MULT) {
    for (unsigned I = 0; I < 2; ++I)
      if (auto C = TraceConstant(Scale->Inputs[I]);
          C && *C >= 2 && *C <= limits::kMaxJumpTableEntries) {
        Quotient = Scale->Inputs[I ^ 1u];
        Divisor = *C;
        break;
      }
  } else if (Scale->Opcode == NdOp::INT_ADD) {
    for (unsigned I = 0; I < 2; ++I) {
      const MedVar Candidate = Scale->Inputs[I];
      const MedOp *Shift = LookupDef(Scale->Inputs[I ^ 1u]);
      if (!Shift || Shift->Opcode != NdOp::INT_LEFT || Shift->NumInputs != 2 ||
          Shift->Output.Size != 4 || Shift->Inputs[0].Size != 4 ||
          Shift->Inputs[1].Size != 4 || !sameValue(Shift->Inputs[0], Candidate))
        continue;
      auto Amount = TraceConstant(Shift->Inputs[1]);
      if (!Amount || *Amount >= 31)
        continue;
      const uint64_t D = uint64_t{1} + (uint64_t{1} << *Amount);
      if (D < 2 || D > limits::kMaxJumpTableEntries)
        continue;
      Quotient = Candidate;
      Divisor = D;
      break;
    }
  }
  if (!Divisor || Quotient.Size != 4)
    return false;
  const MedOp *Shift = LookupDef(Quotient);
  if (!Shift || Shift->Opcode != NdOp::INT_RIGHT || Shift->NumInputs != 2 ||
      Shift->Output.Size != 4 || Shift->Inputs[0].Size != 4 ||
      Shift->Inputs[1].Size != 4)
    return false;
  auto ExtraShift = TraceConstant(Shift->Inputs[1]);
  if (!ExtraShift || *ExtraShift >= 32)
    return false;
  const MedOp *HighHalf = LookupDef(Shift->Inputs[0]);
  if (!HighHalf || HighHalf->Opcode != NdOp::SUBBYTES ||
      HighHalf->NumInputs != 2 || HighHalf->Output.Size != 4 ||
      HighHalf->Inputs[0].Size != 8 || TraceConstant(HighHalf->Inputs[1]) != 4)
    return false;
  const MedOp *Product = LookupDef(HighHalf->Inputs[0]);
  if (!Product || Product->Opcode != NdOp::INT_MULT ||
      Product->NumInputs != 2 || Product->Output.Size != 8 ||
      Product->Inputs[0].Size != 8 || Product->Inputs[1].Size != 8)
    return false;
  uint64_t Multiplier = 0;
  bool MatchedDividend = false;
  for (unsigned I = 0; I < 2; ++I) {
    const MedOp *Widen = LookupDef(Product->Inputs[I]);
    if (!Widen || Widen->Opcode != NdOp::INT_ZEXT || Widen->NumInputs != 1 ||
        Widen->Output.Size != 8 || Widen->Inputs[0].Size != 4 ||
        !sameValue(Widen->Inputs[0], Dividend))
      continue;
    const MedOp *MagicWiden = LookupDef(Product->Inputs[I ^ 1u]);
    if (!MagicWiden || MagicWiden->Opcode != NdOp::INT_ZEXT ||
        MagicWiden->NumInputs != 1 || MagicWiden->Output.Size != 8 ||
        MagicWiden->Inputs[0].Size != 4)
      continue;
    auto Magic = TraceConstant(MagicWiden->Inputs[0]);
    if (!Magic || *Magic > UINT32_MAX)
      continue;
    Multiplier = *Magic;
    MatchedDividend = true;
    break;
  }
  if (!MatchedDividend)
    return false;

  constexpr unsigned Width = 32;
  const unsigned ShiftBits = Width + static_cast<unsigned>(*ExtraShift);
  const llvm::APInt Power = llvm::APInt(128, 1) << ShiftBits;
  const llvm::APInt D(128, Divisor);
  const llvm::APInt M(128, Multiplier);
  if (M != (Power + D - 1).udiv(D))
    return false;
  // x = D*q + r, 0 <= r < D. If the upper bound stays below 2^K, then
  // floor(x*M / 2^K) is exactly q for all 2^32 input values.
  const llvm::APInt Error = D * M - Power;
  const llvm::APInt MaxDividend = (llvm::APInt(128, 1) << Width) - 1;
  const llvm::APInt MaxQuotient = MaxDividend.udiv(D);
  return (MaxQuotient * Error + (D - 1) * M).ult(Power);
}

} // namespace neverd::detail

#endif // NEVERD_BACKEND_LLVM_SCALAR_PROOF_H
