//===- NdOpEmulatorFloatConvert.cpp - Concrete scalar FP conversions -----===//

#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"

namespace neverd {

namespace {

const llvm::fltSemantics *floatSemantics(uint16_t Size) {
  switch (Size) {
  case 4:
    return &llvm::APFloat::IEEEsingle();
  case 8:
    return &llvm::APFloat::IEEEdouble();
  default:
    return nullptr;
  }
}

llvm::APFloat::roundingMode mxcsrRoundingMode(uint32_t MXCSR) {
  switch ((MXCSR >> 13) & 3U) {
  case 1:
    return llvm::APFloat::rmTowardNegative;
  case 2:
    return llvm::APFloat::rmTowardPositive;
  case 3:
    return llvm::APFloat::rmTowardZero;
  default:
    return llvm::APFloat::rmNearestTiesToEven;
  }
}

bool invalidConversion(llvm::APFloat::opStatus Status) {
  return (static_cast<unsigned>(Status) &
          static_cast<unsigned>(llvm::APFloat::opInvalidOp)) != 0;
}

uint32_t conversionStatusFlags(llvm::APFloat::opStatus Status) {
  const unsigned Raw = static_cast<unsigned>(Status);
  uint32_t Flags = 0;
  if ((Raw & static_cast<unsigned>(llvm::APFloat::opInvalidOp)) != 0)
    Flags |= 1U << 0;
  if ((Raw & static_cast<unsigned>(llvm::APFloat::opInexact)) != 0)
    Flags |= 1U << 5;
  return Flags;
}

bool hasUnmaskedException(uint32_t MXCSR, uint32_t Raised) {
  return (Raised & ~(MXCSR >> 7) & 0x3fU) != 0;
}

} // namespace

bool NdOpEmulator::executeFloatConvert(const LowOp &Op) {
  if (Op.NumInputs != 1 || Op.Output.Size == 0 || Op.Output.Size > 8 ||
      Op.Inputs[0].Size == 0 || Op.Inputs[0].Size > 8)
    return false;

  if (Op.Opcode == NdOp::FLOAT_INT2FLOAT ||
      Op.Opcode == NdOp::FLOAT_UINT2FLOAT) {
    const llvm::fltSemantics *Semantics = floatSemantics(Op.Output.Size);
    if (!Semantics || (Op.Inputs[0].Size != 4 && Op.Inputs[0].Size != 8))
      return false;

    const unsigned InputBits = Op.Inputs[0].Size * 8;
    llvm::APFloat Result(*Semantics);
    const bool IsSigned = Op.Opcode == NdOp::FLOAT_INT2FLOAT;
    llvm::APFloat::roundingMode Rounding = llvm::APFloat::rmNearestTiesToEven;
    const bool IsX86 = Img.Arch == Arch::X86 || Img.Arch == Arch::X64;
    if (IsX86)
      Rounding = mxcsrRoundingMode(MXCSR);
    const llvm::APFloat::opStatus Status = Result.convertFromAPInt(
        llvm::APInt(InputBits, readOperand(Op.Inputs[0])), IsSigned, Rounding);
    if (IsX86) {
      const uint32_t Raised = conversionStatusFlags(Status);
      MXCSR |= Raised;
      if (hasUnmaskedException(MXCSR, Raised))
        return false;
    }
    writeOutput(Op.Output, Result.bitcastToAPInt().getZExtValue());
    return true;
  }

  if (Op.Opcode != NdOp::FLOAT_FLOAT2INT && Op.Opcode != NdOp::FLOAT_FLOAT2UINT)
    return false;

  const llvm::fltSemantics *Semantics = floatSemantics(Op.Inputs[0].Size);
  if (!Semantics || (Op.Output.Size != 4 && Op.Output.Size != 8))
    return false;

  const unsigned OutputBits = Op.Output.Size * 8;
  const bool IsUnsigned = Op.Opcode == NdOp::FLOAT_FLOAT2UINT;
  const bool IsX86 = Img.Arch == Arch::X86 || Img.Arch == Arch::X64;
  uint64_t SourceBits = readOperand(Op.Inputs[0]);
  uint32_t Raised = 0;
  if (IsX86) {
    const uint64_t ExponentMask = Op.Inputs[0].Size == 4
                                      ? UINT64_C(0x7f800000)
                                      : UINT64_C(0x7ff0000000000000);
    const uint64_t FractionMask = Op.Inputs[0].Size == 4
                                      ? UINT64_C(0x007fffff)
                                      : UINT64_C(0x000fffffffffffff);
    const uint64_t SignMask = Op.Inputs[0].Size == 4
                                  ? UINT64_C(0x80000000)
                                  : UINT64_C(0x8000000000000000);
    const bool Denormal =
        (SourceBits & ExponentMask) == 0 && (SourceBits & FractionMask) != 0;
    if (Denormal && (MXCSR & (1U << 6)) != 0)
      SourceBits &= SignMask;
    else if (Denormal)
      Raised |= 1U << 1;
  }
  llvm::APFloat Source(*Semantics,
                       llvm::APInt(Op.Inputs[0].Size * 8, SourceBits));
  llvm::APSInt Result(OutputBits, IsUnsigned);
  bool IsExact = false;
  const llvm::APFloat::opStatus Status =
      Source.convertToInteger(Result, llvm::APFloat::rmTowardZero, &IsExact);
  if (invalidConversion(Status)) {
    if (IsX86) {
      // Intel's signed indefinite is INT_MIN; AVX-512 unsigned conversion
      // instructions define the indefinite result as all one bits.
      Result = IsUnsigned ? llvm::APInt::getAllOnes(OutputBits)
                          : llvm::APInt::getSignedMinValue(OutputBits);
    } else if (Source.isNaN()) {
      Result = llvm::APInt(OutputBits, 0);
    } else if (IsUnsigned) {
      Result = Source.isNegative() ? llvm::APInt(OutputBits, 0)
                                   : llvm::APInt::getAllOnes(OutputBits);
    } else {
      Result = Source.isNegative() ? llvm::APInt::getSignedMinValue(OutputBits)
                                   : llvm::APInt::getSignedMaxValue(OutputBits);
    }
  }
  if (IsX86) {
    Raised |= conversionStatusFlags(Status);
    MXCSR |= Raised;
    if (hasUnmaskedException(MXCSR, Raised))
      return false;
  }
  writeOutput(Op.Output, Result.getZExtValue());
  return true;
}

} // namespace neverd
