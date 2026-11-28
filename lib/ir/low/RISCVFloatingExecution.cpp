//===- RISCVFloatingExecution.cpp - Concrete scalar IEEE F/D semantics --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/RISCV/RISCVFloating.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"

#include <bit>

using namespace neverd;
using namespace neverd::riscv;
namespace {
using llvm::APFloat;
using llvm::APInt;
using Op = FPOperation;
constexpr unsigned NX = 1, UF = 2, OF = 4, DZ = 8, NV = 16;
unsigned accrued(APFloat::opStatus S) {
  return ((S & APFloat::opInvalidOp) ? NV : 0) |
         ((S & APFloat::opDivByZero) ? DZ : 0) |
         ((S & APFloat::opOverflow) ? OF : 0) |
         ((S & APFloat::opUnderflow) ? UF : 0) |
         ((S & APFloat::opInexact) ? NX : 0);
}
const llvm::fltSemantics &semantics(FPValue T) {
  return T == FPValue::D ? APFloat::IEEEdouble() : APFloat::IEEEsingle();
}
uint64_t bits(const APFloat &F) { return F.bitcastToAPInt().getZExtValue(); }
uint64_t canonical(const APFloat &F) {
  return F.isNaN() ? bits(APFloat::getQNaN(F.getSemantics())) : bits(F);
}
struct Answer {
  uint64_t Value;
  unsigned Flags;
};

APFloat widened(APFloat X) {
  bool Loses = false;
  X.convert(APFloat::IEEEquad(), APFloat::rmTowardZero, &Loses);
  return X;
}

// APFloat reports only inexact when a directed overflow saturates. A nearby
// inexact result can also round to the largest finite value without overflow.
// The next power of two is exactly representable in binary128. Rounding toward
// zero there keeps a value just below that boundary on the correct side, even
// for a fused product with a tiny addend.
template <typename WideResult>
Answer rounded(const APFloat &Value, APFloat::opStatus Status,
               APFloat::roundingMode Mode, WideResult Wide) {
  unsigned Flags = accrued(Status);
  const auto &Sem = Value.getSemantics();
  if (Status == APFloat::opInexact) {
    if (Value.isLargest() &&
        ilogb(Wide(APFloat::rmTowardZero)) > APFloat::semanticsMaxExponent(Sem))
      Flags |= OF;
    if (Value.isSmallestNormalized()) {
      // Tininess after rounding uses target precision with an unbounded
      // exponent. Scale the boundary to one so its lower neighbor is normal.
      // At their midpoint both nearest modes choose one. Truncating the wide
      // intermediate preserves which side of that midpoint selects one.
      const bool Nearest = Mode == APFloat::rmNearestTiesToEven ||
                           Mode == APFloat::rmNearestTiesToAway;
      APFloat X =
          scalbn(Wide(Nearest ? APFloat::rmTowardZero : Mode),
                 -APFloat::semanticsMinExponent(Sem), APFloat::rmTowardZero);
      bool Loses = false;
      X.convert(Sem, Mode, &Loses);
      if (ilogb(X) < 0)
        Flags |= UF;
    }
  }
  return {canonical(Value), Flags};
}

/// For a positive finite radicand, the square root is normal even for the
/// smallest subnormal S or D. Its discarded part is above one half ulp iff
/// N - floor(sqrt(N))^2 exceeds floor(sqrt(N)). A tie is impossible.
Answer squareRoot(const APFloat &A, unsigned Width, unsigned RM) {
  if (A.isNaN())
    return {canonical(A), A.isSignaling() ? NV : 0};
  if (A.isZero())
    return {bits(A), 0};
  if (A.isNegative())
    return {bits(APFloat::getQNaN(A.getSemantics())), NV};
  if (A.isInfinity())
    return {bits(A), 0};
  const unsigned Frac = Width == 64 ? 52 : 23, Bias = Width == 64 ? 1023 : 127;
  const uint64_t FracMask = (uint64_t(1) << Frac) - 1;
  const uint64_t B = bits(A);
  const unsigned Exp = B >> Frac;
  uint64_t Sig = B & FracMask;
  int E = int(Exp) - int(Bias);
  if (Exp)
    Sig |= uint64_t(1) << Frac;
  else {
    const unsigned Shift = Frac + 1 - std::bit_width(Sig);
    Sig <<= Shift;
    E = 1 - int(Bias) - int(Shift);
  }
  if (E % 2) {
    Sig <<= 1;
    --E;
  }
  const APInt N = APInt(128, Sig).shl(Frac);
  APInt Root = N.sqrt();
  if ((Root * Root).ugt(N))
    --Root;
  const APInt R = N - Root * Root;
  uint64_t Y = Root.getZExtValue();
  if (!R.isZero() && (RM == 3 || ((RM == 0 || RM == 4) && R.ugt(Root))))
    ++Y;
  int Out = E / 2 + int(Bias);
  if (Y == (uint64_t(1) << (Frac + 1))) {
    Y >>= 1;
    ++Out;
  }
  return {(uint64_t(Out) << Frac) | (Y & FracMask), R.isZero() ? 0u : NX};
}
unsigned classify(const APFloat &A) {
  if (A.isNaN())
    return A.isSignaling() ? 8 : 9;
  if (A.isInfinity())
    return A.isNegative() ? 0 : 7;
  if (A.isZero())
    return A.isNegative() ? 3 : 4;
  if (A.isDenormal())
    return A.isNegative() ? 2 : 5;
  return A.isNegative() ? 1 : 6;
}
Answer evaluate(const FPForm &F, llvm::ArrayRef<uint64_t> In, unsigned RM) {
  constexpr APFloat::roundingMode Modes[] = {
      APFloat::rmNearestTiesToEven, APFloat::rmTowardZero,
      APFloat::rmTowardNegative, APFloat::rmTowardPositive,
      APFloat::rmNearestTiesToAway};
  const auto Mode = Modes[RM];
  const auto Value = [&](unsigned I) {
    return APFloat(semantics(F.Inputs[I]),
                   APInt(fpBytes(F.Inputs[I], 8) * 8, In[I]));
  };
  if (!fpRegister(F.Inputs[0])) {
    APFloat R = APFloat::getZero(semantics(F.Result));
    const bool Signed = F.Inputs[0] == FPValue::W || F.Inputs[0] == FPValue::L;
    const auto S = R.convertFromAPInt(APInt(8 * fpBytes(F.Inputs[0], 8), In[0]),
                                      Signed, Mode);
    return {bits(R), accrued(S)};
  }
  APFloat A = Value(0);
  if (F.Operation == Op::Sqrt)
    return squareRoot(A, fpBytes(F.Result, 8) * 8, RM);
  if (F.Operation == Op::Class)
    return {uint64_t(1) << classify(A), 0};
  if (F.Operation == Op::Convert) {
    bool Loses = false;
    const auto S = A.convert(semantics(F.Result), Mode, &Loses);
    return rounded(A, S, Mode,
                   [&](APFloat::roundingMode) { return widened(Value(0)); });
  }
  if (F.Operation == Op::ToW || F.Operation == Op::ToWU ||
      F.Operation == Op::ToL || F.Operation == Op::ToLU) {
    const unsigned Width = 8 * fpBytes(F.Result, 8);
    const bool Unsigned = F.Result == FPValue::WU || F.Result == FPValue::LU;
    llvm::APSInt R(Width, Unsigned);
    bool Exact = false;
    const auto S = A.convertToInteger(R, Mode, &Exact);
    if (S & APFloat::opInvalidOp) {
      const bool Low = A.isNegative() && !A.isNaN();
      return {(Low ? llvm::APSInt::getMinValue(Width, Unsigned)
                   : llvm::APSInt::getMaxValue(Width, Unsigned))
                  .getZExtValue(),
              NV};
    }
    return {R.getZExtValue(), accrued(S)};
  }
  APFloat B = Value(1);
  const auto Binary = [&](auto Calculate) {
    const auto Status = (A.*Calculate)(B, Mode);
    return rounded(A, Status, Mode, [&](APFloat::roundingMode Rounding) {
      APFloat X = widened(Value(0));
      (X.*Calculate)(widened(B), Rounding);
      return X;
    });
  };
  switch (F.Operation) {
  case Op::Add:
    return Binary(&APFloat::add);
  case Op::Sub:
    return Binary(&APFloat::subtract);
  case Op::Mul:
    return Binary(&APFloat::multiply);
  case Op::Div:
    return Binary(&APFloat::divide);
  case Op::MAdd:
  case Op::MSub:
  case Op::NMSub:
  case Op::NMAdd: {
    APFloat C = Value(2);
    unsigned Extra =
        ((A.isZero() && B.isInfinity()) || (A.isInfinity() && B.isZero())) ? NV
                                                                           : 0;
    const auto Fused = [&](APFloat &X, const APFloat &Y, APFloat Z,
                           APFloat::roundingMode Rounding) {
      if (F.Operation == Op::NMSub || F.Operation == Op::NMAdd)
        X.changeSign();
      if (F.Operation == Op::MSub || F.Operation == Op::NMAdd)
        Z.changeSign();
      return X.fusedMultiplyAdd(Y, Z, Rounding);
    };
    const auto Status = Fused(A, B, C, Mode);
    auto Result = rounded(A, Status, Mode, [&](APFloat::roundingMode Rounding) {
      APFloat X = widened(Value(0));
      Fused(X, widened(B), widened(C), Rounding);
      return X;
    });
    Result.Flags |= Extra;
    return Result;
  }
  case Op::Min:
  case Op::Max: {
    unsigned Flags = A.isSignaling() || B.isSignaling() ? NV : 0;
    if (A.isNaN())
      return {canonical(B), Flags};
    if (B.isNaN())
      return {bits(A), Flags};
    const bool Less = A.compare(B) == APFloat::cmpLessThan ||
                      (A.isZero() && B.isZero() && A.isNegative());
    return {bits((Less == (F.Operation == Op::Min)) ? A : B), Flags};
  }
  case Op::Eq:
  case Op::Lt:
  case Op::Le: {
    if (A.isNaN() || B.isNaN())
      return {0, F.Operation != Op::Eq || A.isSignaling() || B.isSignaling()
                     ? NV
                     : 0};
    const auto C = A.compare(B);
    return {F.Operation == Op::Eq   ? C == APFloat::cmpEqual
            : F.Operation == Op::Lt ? C == APFloat::cmpLessThan
                                    : C != APFloat::cmpGreaterThan,
            0};
  }
  default:
    llvm_unreachable("unhandled RISC-V floating operation");
  }
}
} // namespace

bool NdOpEmulator::executeRISCVFloating(const LowOp &Op) {
  if (!isRISCV(Img.Arch) || !RISCVFCSR || Op.NumInputs < 2 ||
      !Op.Inputs[1].isConst() || (!Op.Output.isReg() && !Op.Output.isTemp()))
    return false;
  const unsigned XLen = Img.getPointerSize();
  const uint64_t Selector = Op.Inputs[1].Offset;
  if (Op.Inputs[0].Offset == unsigned(Intrinsic::RISCVFPControl)) {
    const auto C = fpControl(Selector);
    if (!C || Op.Output.Size != XLen || Op.NumInputs != (C->Operation ? 3 : 2))
      return false;
    const unsigned Old = (*RISCVFCSR >> C->shift()) & C->mask();
    if (C->Operation) {
      if (Op.Inputs[2].Size != XLen ||
          !knownOperandBytes(Op.Inputs[2], 0, XLen))
        return false;
      const unsigned Mask = readOperand(Op.Inputs[2]) & C->mask();
      const unsigned New = C->Operation == 1   ? Mask
                           : C->Operation == 2 ? Old | Mask
                                               : Old & ~Mask;
      *RISCVFCSR =
          (*RISCVFCSR & ~(C->mask() << C->shift())) | (New << C->shift());
    }
    writeOutput(Op.Output, Old);
    return true;
  }
  const auto *F = floatingForm(Selector, XLen);
  if (!F || Op.NumInputs != 2 + F->arity() ||
      Op.Output.Size != fpBytes(F->Result, XLen))
    return false;
  const unsigned RM = (Selector & 7) == 7 ? *RISCVFCSR >> 5 : Selector & 7;
  if (RM > 4)
    return false;
  llvm::SmallVector<uint64_t, 3> Inputs;
  for (unsigned I = 0; I < F->arity(); ++I) {
    const auto &V = Op.Inputs[2 + I];
    if (V.Size != fpBytes(F->Inputs[I], XLen) ||
        !knownOperandBytes(V, 0, V.Size))
      return false;
    Inputs.push_back(readOperand(V));
  }
  const Answer R = evaluate(*F, Inputs, RM);
  writeOutput(Op.Output, R.Value);
  *RISCVFCSR |= R.Flags;
  return true;
}
