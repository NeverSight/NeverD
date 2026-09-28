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
#include <functional>
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
  struct ScaledQuotient {
    MedVar Value;
    uint64_t Factor;
  };
  // Compilers can factor D*q as (q + (q << 1)) << 1 for D=6. Recover
  // the exact coefficient from a small same-width DAG; every leaf must be
  // the same quotient, with no extra variable or relocatable constant.
  std::function<std::optional<ScaledQuotient>(const MedVar &, unsigned)>
      scaleFactor = [&](const MedVar &V,
                        unsigned Depth) -> std::optional<ScaledQuotient> {
    if (V.Size != 4 || Depth > 8)
      return std::nullopt;
    const MedOp *Def = LookupDef(V);
    if (!Def || Def->Output.Size != 4)
      return std::nullopt;
    if (Def->Opcode == NdOp::INT_RIGHT && Def->NumInputs == 2)
      return ScaledQuotient{V, 1};
    if (Def->NumInputs != 2 || Def->Inputs[0].Size != 4 ||
        Def->Inputs[1].Size != 4)
      return std::nullopt;
    if (Def->Opcode == NdOp::INT_LEFT) {
      auto Amount = TraceConstant(Def->Inputs[1]);
      if (!Amount || *Amount >= 32)
        return std::nullopt;
      auto Input = scaleFactor(Def->Inputs[0], Depth + 1);
      if (!Input || Input->Factor > (limits::kMaxJumpTableEntries >> *Amount))
        return std::nullopt;
      Input->Factor <<= *Amount;
      return Input;
    }
    if (Def->Opcode == NdOp::INT_MULT) {
      for (unsigned I = 0; I < 2; ++I) {
        auto Factor = TraceConstant(Def->Inputs[I]);
        if (!Factor || *Factor == 0 || *Factor > limits::kMaxJumpTableEntries)
          continue;
        auto Input = scaleFactor(Def->Inputs[I ^ 1u], Depth + 1);
        if (Input && Input->Factor <= limits::kMaxJumpTableEntries / *Factor) {
          Input->Factor *= *Factor;
          return Input;
        }
      }
      return std::nullopt;
    }
    if (Def->Opcode == NdOp::INT_ADD) {
      auto Left = scaleFactor(Def->Inputs[0], Depth + 1);
      auto Right = scaleFactor(Def->Inputs[1], Depth + 1);
      if (!Left || !Right || !sameValue(Left->Value, Right->Value) ||
          Right->Factor > limits::kMaxJumpTableEntries - Left->Factor)
        return std::nullopt;
      Left->Factor += Right->Factor;
      return Left;
    }
    return std::nullopt;
  };
  auto Scaled = scaleFactor(Subtract->Inputs[1], 0);
  if (!Scaled || Scaled->Factor < 2)
    return false;
  const MedVar Quotient = Scaled->Value;
  const uint64_t Divisor = Scaled->Factor;
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
