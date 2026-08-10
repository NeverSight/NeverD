//===- Semantics.cpp - Shared scalar EVM semantics ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Semantics.h"

namespace neverd::evm {
namespace {

llvm::APInt zeroWord() { return llvm::APInt(kWordBits, 0); }

llvm::APInt boolWord(bool Value) {
  return llvm::APInt(kWordBits, Value ? 1 : 0);
}

llvm::APInt modularExponent(llvm::APInt Base, llvm::APInt Exponent) {
  llvm::APInt Result(kWordBits, 1);
  while (!Exponent.isZero()) {
    if (Exponent[0])
      Result *= Base;
    Exponent.lshrInPlace(1);
    Base *= Base;
  }
  return Result;
}

llvm::APInt signExtend(const llvm::APInt &ByteIndex, const llvm::APInt &Value) {
  if (ByteIndex.uge(kWordBytes))
    return Value;
  const unsigned Width =
      static_cast<unsigned>((ByteIndex.getZExtValue() + 1) * kBitsPerByte);
  return Value.trunc(Width).sext(kWordBits);
}

} // namespace

std::optional<llvm::APInt> evaluateALU(Opcode Op,
                                       llvm::ArrayRef<llvm::APInt> Inputs) {
  const auto Info = assignedOpcodeInfo(Op);
  if (!Info || !isALU(*Info) || Inputs.size() != Info->StackInputs)
    return std::nullopt;
  for (const llvm::APInt &Input : Inputs)
    if (Input.getBitWidth() != kWordBits)
      return std::nullopt;

  const llvm::APInt &A = Inputs.front();
  if (Op == Opcode::ISZERO)
    return boolWord(A.isZero());
  if (Op == Opcode::NOT)
    return ~A;
  if (Op == Opcode::CLZ)
    return llvm::APInt(kWordBits, A.countl_zero());

  const llvm::APInt &B = Inputs[1];
  switch (Op) {
  case Opcode::ADD:
    return A + B;
  case Opcode::MUL:
    return A * B;
  case Opcode::SUB:
    return A - B;
  case Opcode::DIV:
    return B.isZero() ? zeroWord() : A.udiv(B);
  case Opcode::SDIV:
    if (B.isZero())
      return zeroWord();
    if (A.isMinSignedValue() && B.isAllOnes())
      return A;
    return A.sdiv(B);
  case Opcode::MOD:
    return B.isZero() ? zeroWord() : A.urem(B);
  case Opcode::SMOD:
    return B.isZero() || (A.isMinSignedValue() && B.isAllOnes()) ? zeroWord()
                                                                 : A.srem(B);
  case Opcode::ADDMOD:
  case Opcode::MULMOD: {
    const llvm::APInt &Modulus = Inputs[2];
    if (Modulus.isZero())
      return zeroWord();
    const llvm::APInt Wide =
        Op == Opcode::ADDMOD ? A.zext(kWideWordBits) + B.zext(kWideWordBits)
                             : A.zext(kWideWordBits) * B.zext(kWideWordBits);
    return Wide.urem(Modulus.zext(kWideWordBits)).trunc(kWordBits);
  }
  case Opcode::EXP:
    return modularExponent(A, B);
  case Opcode::SIGNEXTEND:
    return signExtend(A, B);
  case Opcode::LT:
    return boolWord(A.ult(B));
  case Opcode::GT:
    return boolWord(A.ugt(B));
  case Opcode::SLT:
    return boolWord(A.slt(B));
  case Opcode::SGT:
    return boolWord(A.sgt(B));
  case Opcode::EQ:
    return boolWord(A == B);
  case Opcode::AND:
    return A & B;
  case Opcode::OR:
    return A | B;
  case Opcode::XOR:
    return A ^ B;
  case Opcode::BYTE: {
    if (A.uge(kWordBytes))
      return zeroWord();
    const unsigned Shift = static_cast<unsigned>(
        (kWordBytes - 1 - A.getZExtValue()) * kBitsPerByte);
    return llvm::APInt(kWordBits,
                       B.extractBitsAsZExtValue(kBitsPerByte, Shift));
  }
  case Opcode::SHL:
    return A.uge(kWordBits) ? zeroWord()
                            : B.shl(static_cast<unsigned>(A.getZExtValue()));
  case Opcode::SHR:
    return A.uge(kWordBits) ? zeroWord()
                            : B.lshr(static_cast<unsigned>(A.getZExtValue()));
  case Opcode::SAR:
    return A.uge(kWordBits)
               ? (B.isNegative() ? llvm::APInt::getAllOnes(kWordBits)
                                 : zeroWord())
               : B.ashr(static_cast<unsigned>(A.getZExtValue()));
  default:
    return std::nullopt;
  }
}

} // namespace neverd::evm
