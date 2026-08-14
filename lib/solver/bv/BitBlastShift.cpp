//===- BitBlastShift.cpp - Barrel shifters and rotates --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements shifting and rotating by an amount that may not be known.
///
/// A shift by a fixed amount is not a circuit at all — it is a renaming of
/// wires, and costs nothing.  That case is worth separating out because it is
/// overwhelmingly the common one in recovered code, and because the variable
/// case is built out of it: one stage per bit of the amount, each stage either
/// applying a fixed power-of-two shift or passing its input through.  The
/// depth is logarithmic in the width rather than linear, and a stage whose
/// amount bit is already decided disappears.
///
/// Two boundary behaviours are defined by the expression language rather than
/// by any machine, and both are encoded here rather than left to the caller.
/// A shift by at least the width produces the fill value — zero, or the sign
/// for an arithmetic right shift — which the staged shifts already give for
/// amounts the stages cover, and which an explicit guard gives for amounts
/// above that.  A rotation is taken modulo the width, which is free when the
/// width is a power of two and needs a division when it is not.
///
//===----------------------------------------------------------------------===//

#include "BitBlastDetail.h"

#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::solver::detail {

namespace {

bool isRotate(ShiftKind Kind) {
  return Kind == ShiftKind::RotateLeft || Kind == ShiftKind::RotateRight;
}

/// Stages a barrel needs: the number of amount bits that can name a shift
/// below the width.  A one-bit value needs none, because every nonzero amount
/// already shifts everything out.
size_t stageCount(size_t Width) {
  size_t Stages = 0;
  while ((size_t(1) << Stages) < Width)
    ++Stages;
  return Stages;
}

} // namespace

std::optional<uint64_t> constantAmount(const CnfEncoder &E, LitSpan Amount,
                                       uint64_t Limit) {
  uint64_t Value = 0;
  bool Saturated = false;

  for (size_t I = 0, N = Amount.size(); I < N; ++I) {
    SatLit L = Amount[I];
    if (!E.isConstant(L))
      return std::nullopt;
    if (!E.isTrueLit(L))
      continue;
    if (I >= 64) {
      Saturated = true;
      continue;
    }
    Value |= uint64_t(1) << I;
  }

  return Saturated || Value > Limit ? Limit : Value;
}

std::optional<uint64_t> constantResidue(const CnfEncoder &E, LitSpan Amount,
                                        uint64_t Modulus) {
  assert(Modulus != 0 && "a residue modulo zero");

  uint64_t Residue = 0;
  uint64_t Place = 1 % Modulus;

  for (SatLit L : Amount) {
    if (!E.isConstant(L))
      return std::nullopt;
    if (E.isTrueLit(L))
      Residue = (Residue + Place) % Modulus;
    Place = (Place * 2) % Modulus;
  }

  return Residue;
}

void shiftBitsByConstant(CnfEncoder &E, LitSpan A, uint64_t Amount,
                         ShiftKind Kind, LitVec &Out) {
  const size_t Width = A.size();
  assert(Width != 0 && "a shift of a zero-width value");

  Out.clear();
  Out.reserve(Width);

  if (isRotate(Kind)) {
    const size_t Step = static_cast<size_t>(Amount % Width);
    for (size_t I = 0; I < Width; ++I) {
      size_t From = Kind == ShiftKind::RotateLeft ? (I + Width - Step) % Width
                                                  : (I + Step) % Width;
      Out.push_back(A[From]);
    }
    return;
  }

  // Everything at or beyond the width leaves nothing of the operand, so the
  // amount can be clamped and the arithmetic below kept in range.
  const size_t Step = static_cast<size_t>(std::min<uint64_t>(Amount, Width));

  switch (Kind) {
  case ShiftKind::Left:
    for (size_t I = 0; I < Width; ++I)
      Out.push_back(I < Step ? E.falseLit() : A[I - Step]);
    return;
  case ShiftKind::LogicalRight:
    for (size_t I = 0; I < Width; ++I)
      Out.push_back(I + Step < Width ? A[I + Step] : E.falseLit());
    return;
  case ShiftKind::ArithmeticRight:
    for (size_t I = 0; I < Width; ++I)
      Out.push_back(I + Step < Width ? A[I + Step] : A[Width - 1]);
    return;
  case ShiftKind::RotateLeft:
  case ShiftKind::RotateRight:
    break;
  }
}

void shiftBits(CnfEncoder &E, LitSpan A, LitSpan Amount, ShiftKind Kind,
               LitVec &Out) {
  const size_t Width = A.size();
  assert(Width != 0 && "a shift of a zero-width value");

  const bool Rotate = isRotate(Kind);

  if (std::optional<uint64_t> Fixed =
          Rotate ? constantResidue(E, Amount, Width)
                 : constantAmount(E, Amount, Width)) {
    shiftBitsByConstant(E, A, *Fixed, Kind, Out);
    return;
  }

  const size_t Stages = stageCount(Width);

  // A rotation is modulo the width.  When the width is a power of two the
  // amount's low bits already are that residue; otherwise it has to be
  // computed, and the only exact way to do that is the division the definition
  // names.  The reduction is skipped when the amount is too narrow to reach
  // the width in the first place.
  LitSpan Steps = Amount;
  LitVec Reduced;
  const bool NeedsReduction =
      Rotate && (Width & (Width - 1)) != 0 &&
      (Amount.size() >= 64 || (uint64_t(1) << Amount.size()) > Width);

  if (NeedsReduction) {
    LitVec Modulus;
    Modulus.reserve(Amount.size());
    for (size_t I = 0, N = Amount.size(); I < N; ++I)
      Modulus.push_back(E.constant(I < 64 && ((Width >> I) & 1) != 0));
    divideBits(E, Amount, Modulus, /*Quotient=*/nullptr, &Reduced);
    Steps = Reduced;
  }

  LitVec Current(A.begin(), A.end());
  LitVec Stage;
  LitVec Selected;

  const size_t Usable = std::min(Stages, Steps.size());
  for (size_t K = 0; K < Usable; ++K) {
    SatLit Step = Steps[K];
    if (E.isFalseLit(Step))
      continue;

    shiftBitsByConstant(E, Current, uint64_t(1) << K, Kind, Stage);
    if (E.isTrueLit(Step)) {
      Current = std::move(Stage);
      continue;
    }
    selectBits(E, Step, Stage, Current, Selected);
    Current = std::move(Selected);
  }

  if (!Rotate) {
    // Amount bits above the last stage can only mean an amount past the width.
    // Rotation needs no such guard: its amount was already reduced.
    SatLit Beyond = E.falseLit();
    for (size_t K = Stages, N = Amount.size(); K < N; ++K)
      Beyond = E.mkOr(Beyond, Amount[K]);

    if (!E.isFalseLit(Beyond)) {
      SatLit Fill =
          Kind == ShiftKind::ArithmeticRight ? A[Width - 1] : E.falseLit();
      LitVec Filled(Width, Fill);
      selectBits(E, Beyond, Filled, Current, Out);
      return;
    }
  }

  Out = std::move(Current);
}

} // namespace neverd::solver::detail
