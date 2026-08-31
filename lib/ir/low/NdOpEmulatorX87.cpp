//===- NdOpEmulatorX87.cpp - Exact x87 state emulation -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict concrete semantics for the x87 state operations used by iterative
/// FPREM/FPREM1 loops and by FNINIT/FNSTCW control-word probes.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace neverd {

namespace {

constexpr uint16_t X87DefaultControlWord = UINT16_C(0x037f);
constexpr uint16_t X87ConditionMask = UINT16_C(0x4700);
constexpr uint16_t X87ExceptionStatusMask = UINT16_C(0x80ff);

struct X87Encoding {
  uint64_t Significand = 0;
  uint16_t SignExponent = 0;

  unsigned exponent() const { return SignExponent & 0x7fffU; }
  bool integerBit() const { return (Significand >> 63) != 0; }
  bool isZero() const { return exponent() == 0 && Significand == 0; }
  bool isSubnormal() const {
    return exponent() == 0 && Significand != 0 && !integerBit();
  }
  bool isInfinity() const {
    return exponent() == 0x7fffU && Significand == UINT64_C(0x8000000000000000);
  }
  bool isNaN() const {
    return exponent() == 0x7fffU && Significand != UINT64_C(0x8000000000000000);
  }
  bool isCanonical() const {
    if (exponent() == 0)
      return !integerBit();
    return integerBit();
  }
};

X87Encoding decodeX87(llvm::ArrayRef<uint8_t> Bytes) {
  X87Encoding Value;
  std::memcpy(&Value.Significand, Bytes.data(), sizeof(Value.Significand));
  std::memcpy(&Value.SignExponent, Bytes.data() + sizeof(Value.Significand),
              sizeof(Value.SignExponent));
  return Value;
}

llvm::APInt x87Bits(llvm::ArrayRef<uint8_t> Bytes) {
  llvm::APInt Bits(80, 0);
  for (unsigned Index = 0; Index != 10; ++Index)
    Bits |= llvm::APInt(80, Bytes[Index]) << (Index * 8);
  return Bits;
}

std::array<uint8_t, 10> encodeX87(const llvm::APFloat &Value) {
  const llvm::APInt Bits = Value.bitcastToAPInt();
  std::array<uint8_t, 10> Bytes{};
  for (unsigned Index = 0; Index != Bytes.size(); ++Index)
    Bytes[Index] =
        static_cast<uint8_t>(Bits.extractBitsAsZExtValue(8, Index * 8));
  return Bytes;
}

/// Return the low three bits of the absolute integral quotient.  The x87
/// reports magnitude bits (matching Intel-compatible hardware and SoftFloat),
/// not the two's-complement low bits of a negative quotient.
uint8_t completedQuotientBits(const X87Encoding &Dividend,
                              const X87Encoding &Divisor,
                              bool RoundNearestEven) {
  const int ExponentDifference = static_cast<int>(Dividend.exponent()) -
                                 static_cast<int>(Divisor.exponent());

  // With normalized significands in [1,2), a negative exponent difference
  // always gives |dividend/divisor| < 1.  Only D=-1 can cross the one-half
  // threshold used by FPREM1; avoiding a fixed-width left shift here also
  // keeps the full x87 exponent range well-defined.
  if (ExponentDifference < 0) {
    if (!RoundNearestEven || ExponentDifference < -1)
      return 0;
    return Dividend.Significand > Divisor.Significand ? 1U : 0U;
  }

  llvm::APInt Numerator(128, Dividend.Significand);
  llvm::APInt Denominator(128, Divisor.Significand);
  Numerator <<= static_cast<unsigned>(ExponentDifference);

  llvm::APInt Quotient = Numerator.udiv(Denominator);
  if (RoundNearestEven) {
    const llvm::APInt Remainder = Numerator.urem(Denominator);
    const llvm::APInt TwiceRemainder = Remainder.shl(1);
    if (TwiceRemainder.ugt(Denominator) ||
        (TwiceRemainder == Denominator && Quotient[0]))
      ++Quotient;
  }
  return static_cast<uint8_t>(Quotient.getZExtValue() & 7U);
}

uint16_t quotientConditionCodes(uint8_t Quotient) {
  uint16_t Status = 0;
  if ((Quotient & 4U) != 0)
    Status |= UINT16_C(1) << x86reg::FPU_SW_C0_BIT;
  if ((Quotient & 1U) != 0)
    Status |= UINT16_C(1) << x86reg::FPU_SW_C1_BIT;
  if ((Quotient & 2U) != 0)
    Status |= UINT16_C(1) << x86reg::FPU_SW_C3_BIT;
  return Status;
}

} // namespace

NdOpEmulator::NdOpEmulator(const BinaryImage &Image) : Img(Image) {
  resetX87State();
}

void NdOpEmulator::resetX87State() {
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64)
    return;
  // These pseudo-registers are complete 16-bit architectural containers, not
  // subregister writes into a wider GPR alias.
  Registers[x86reg::FPU_CW] = X87DefaultControlWord;
  Registers[x86reg::FPU_SW] = 0;
  WideRegisters.erase(x86reg::FPU_CW);
  WideRegisters.erase(x86reg::FPU_SW);
}

bool NdOpEmulator::executeX87(const LowOp &Op) {
  if (Img.Arch != Arch::X86 && Img.Arch != Arch::X64)
    return false;
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;

  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::X87Fninit) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return false;

    // FNINIT makes every x87 tag empty.  The emulator has no separate tag
    // array, so invalidate the cached payloads: retaining them would let a
    // later malformed stack read fold a stale pre-reset value.
    for (unsigned Index = 0; Index != x86reg::FPUStackDepth; ++Index) {
      const uint64_t Offset =
          x86reg::ST0 + static_cast<uint64_t>(Index) * x86reg::FPURegStride;
      Registers.erase(Offset);
      WideRegisters.erase(Offset);
    }
    resetX87State();
    return true;
  }

  if (Id == Intrinsic::X87Fnclex) {
    if (Op.NumInputs != 1 || Op.Output.Size != 0)
      return false;
    const uint16_t Status = static_cast<uint16_t>(
        getRegister(x86reg::FPU_SW).value_or(0) & ~X87ExceptionStatusMask);
    Registers[x86reg::FPU_SW] = Status;
    WideRegisters.erase(x86reg::FPU_SW);
    return true;
  }

  if (Id == Intrinsic::X87ReadStatus) {
    if (Op.NumInputs != 1 || Op.Output.Size != 2 ||
        (!Op.Output.isReg() && !Op.Output.isTemp()))
      return false;
    writeOutput(Op.Output, getRegister(x86reg::FPU_SW).value_or(0));
    return true;
  }

  if (Id != Intrinsic::X87Fprem && Id != Intrinsic::X87Fprem1)
    return false;
  if (Op.NumInputs != 3 || Op.Output.Size != x86reg::FPURegSize ||
      Op.Inputs[1].Size != x86reg::FPURegSize ||
      Op.Inputs[2].Size != x86reg::FPURegSize ||
      (!Op.Output.isReg() && !Op.Output.isTemp()))
    return false;

  auto knownX87Bytes =
      [&](const NdVar &Operand) -> std::optional<std::array<uint8_t, 10>> {
    if ((!Operand.isReg() && !Operand.isTemp()) ||
        Operand.Size != x86reg::FPURegSize)
      return std::nullopt;
    const auto It = WideRegisters.find(Operand.Offset);
    if (It == WideRegisters.end() || It->second.size() < x86reg::FPURegSize)
      return std::nullopt;
    std::array<uint8_t, 10> Bytes{};
    std::copy_n(It->second.begin(), Bytes.size(), Bytes.begin());
    return Bytes;
  };

  const auto DividendBytes = knownX87Bytes(Op.Inputs[1]);
  const auto DivisorBytes = knownX87Bytes(Op.Inputs[2]);
  if (!DividendBytes || !DivisorBytes)
    return false;
  const X87Encoding Dividend = decodeX87(*DividendBytes);
  const X87Encoding Divisor = decodeX87(*DivisorBytes);

  // Invalid encodings and operands that raise x87 exceptions need the tag
  // word, exception masks, and sticky exception fields to decide whether the
  // destination commits.  Those are deliberately outside this lightweight
  // emulator, so reject them instead of returning a plausible quiet NaN.
  if (!Dividend.isCanonical() || !Divisor.isCanonical() ||
      Dividend.isSubnormal() || Divisor.isSubnormal() || Dividend.isNaN() ||
      Divisor.isNaN() || Dividend.isInfinity() || Divisor.isZero())
    return false;

  uint16_t ConditionCodes = 0;
  std::array<uint8_t, 10> ResultBytes = *DividendBytes;
  if (!Dividend.isZero() && !Divisor.isInfinity()) {
    llvm::APFloat Result(llvm::APFloat::x87DoubleExtended(),
                         x87Bits(*DividendBytes));
    const llvm::APFloat DivisorValue(llvm::APFloat::x87DoubleExtended(),
                                     x87Bits(*DivisorBytes));
    const int ExponentDifference = static_cast<int>(Dividend.exponent()) -
                                   static_cast<int>(Divisor.exponent());
    llvm::APFloat::opStatus ArithmeticStatus = llvm::APFloat::opOK;
    if (ExponentDifference < 64) {
      ArithmeticStatus = Id == Intrinsic::X87Fprem
                             ? Result.mod(DivisorValue)
                             : Result.remainder(DivisorValue);
      const uint8_t Quotient =
          completedQuotientBits(Dividend, Divisor, Id == Intrinsic::X87Fprem1);
      ConditionCodes = quotientConditionCodes(Quotient);
    } else {
      // Intel permits an implementation-selected N in [32,63].  Modern
      // 32-bit-and-later x87 implementations clear C0/C1/C3 on an incomplete
      // reduction; use the deterministic maximal step also used by Unicorn.
      constexpr int PartialReductionBits = 63;
      llvm::APFloat ScaledDivisor =
          llvm::scalbn(DivisorValue, ExponentDifference - PartialReductionBits,
                       llvm::APFloat::rmNearestTiesToEven);
      if (!ScaledDivisor.isFinite() || ScaledDivisor.isZero())
        return false;
      ArithmeticStatus = Result.mod(ScaledDivisor);
      ConditionCodes = UINT16_C(1) << x86reg::FPU_SW_C2_BIT;
    }
    if (ArithmeticStatus != llvm::APFloat::opOK)
      return false;
    ResultBytes = encodeX87(Result);
    const X87Encoding EncodedResult = decodeX87(ResultBytes);
    if (!EncodedResult.isCanonical() || EncodedResult.isSubnormal() ||
        EncodedResult.isNaN() || EncodedResult.isInfinity())
      return false;
  }

  const uint16_t OldStatus =
      static_cast<uint16_t>(getRegister(x86reg::FPU_SW).value_or(0));
  const uint16_t NewStatus =
      static_cast<uint16_t>((OldStatus & ~X87ConditionMask) | ConditionCodes);
  writeOutputBytes(Op.Output, ResultBytes);
  Registers[x86reg::FPU_SW] = NewStatus;
  WideRegisters.erase(x86reg::FPU_SW);
  return true;
}

} // namespace neverd
