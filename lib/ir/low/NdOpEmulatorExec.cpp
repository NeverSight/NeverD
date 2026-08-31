//===- NdOpEmulatorExec.cpp - NdOp opcode execution ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The per-opcode execution handlers of NdOpEmulator, grouped by operation
/// family: integer arithmetic and shifts, memory load and store, copy-like
/// reshapes (extend, subpiece, concat, negate), integer comparisons and
/// flag predicates, boolean logic, and the remaining select/bit-field
/// intrinsics.  See NdOpEmulator.cpp for the machine state these read and
/// write and for the step dispatcher that routes each opcode here.
///
//===----------------------------------------------------------------------===//

#include "X86ApproxReference.h"

#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace neverd {

namespace {
uint8_t aesMul(uint8_t A, uint8_t B) {
  uint8_t R = 0;
  while (B) {
    if (B & 1)
      R ^= A;
    A = static_cast<uint8_t>((A << 1) ^ ((A & 0x80) ? 0x1b : 0));
    B >>= 1;
  }
  return R;
}

uint8_t aesSbox(uint8_t X) {
  uint8_t Inv = 0;
  if (X)
    for (unsigned Candidate = 1; Candidate != 256; ++Candidate)
      if (aesMul(X, static_cast<uint8_t>(Candidate)) == 1) {
        Inv = static_cast<uint8_t>(Candidate);
        break;
      }
  return static_cast<uint8_t>(Inv ^ std::rotl(Inv, 1) ^ std::rotl(Inv, 2) ^
                              std::rotl(Inv, 3) ^ std::rotl(Inv, 4) ^ 0x63);
}

uint8_t aesInvSbox(uint8_t X) {
  for (unsigned Candidate = 0; Candidate != 256; ++Candidate)
    if (aesSbox(static_cast<uint8_t>(Candidate)) == X)
      return static_cast<uint8_t>(Candidate);
  return 0;
}

void aesShift(uint8_t *S, bool Inverse) {
  uint8_t T[16];
  std::memcpy(T, S, 16);
  for (unsigned C = 0; C != 4; ++C)
    for (unsigned R = 0; R != 4; ++R) {
      const unsigned SourceColumn = Inverse ? (C + 4 - R) & 3 : (C + R) & 3;
      S[4 * C + R] = T[4 * SourceColumn + R];
    }
}

void aesMix(uint8_t *S, bool Inverse) {
  for (unsigned C = 0; C != 4; ++C) {
    uint8_t *P = S + 4 * C;
    const uint8_t A = P[0], B = P[1], Cc = P[2], D = P[3];
    if (!Inverse) {
      P[0] = aesMul(A, 2) ^ aesMul(B, 3) ^ Cc ^ D;
      P[1] = A ^ aesMul(B, 2) ^ aesMul(Cc, 3) ^ D;
      P[2] = A ^ B ^ aesMul(Cc, 2) ^ aesMul(D, 3);
      P[3] = aesMul(A, 3) ^ B ^ Cc ^ aesMul(D, 2);
    } else {
      P[0] = aesMul(A, 14) ^ aesMul(B, 11) ^ aesMul(Cc, 13) ^ aesMul(D, 9);
      P[1] = aesMul(A, 9) ^ aesMul(B, 14) ^ aesMul(Cc, 11) ^ aesMul(D, 13);
      P[2] = aesMul(A, 13) ^ aesMul(B, 9) ^ aesMul(Cc, 14) ^ aesMul(D, 11);
      P[3] = aesMul(A, 11) ^ aesMul(B, 13) ^ aesMul(Cc, 9) ^ aesMul(D, 14);
    }
  }
}
} // namespace

bool NdOpEmulator::executeX86Crypto(const LowOp &Op) {
  if (Op.NumInputs < 3 || !Op.Inputs[0].isConst() || Op.Output.Size == 0 ||
      (Op.Output.Size != 16 && Op.Output.Size != 32 && Op.Output.Size != 64))
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  const auto Left = readOperandBytes(Op.Inputs[1]);
  const auto Right = readOperandBytes(Op.Inputs[2]);
  if (Left.size() != Op.Output.Size || Right.size() != Op.Output.Size)
    return false;
  std::vector<uint8_t> Result(Op.Output.Size);
  for (size_t Lane = 0; Lane != Op.Output.Size; Lane += 16) {
    if (Id == Intrinsic::Pclmulqdq) {
      if (Op.NumInputs < 4)
        return false;
      const unsigned Select = static_cast<unsigned>(readOperand(Op.Inputs[3]));
      uint64_t A = 0, B = 0, Low = 0, High = 0;
      std::memcpy(&A, Left.data() + Lane + ((Select & 1) ? 8 : 0), 8);
      std::memcpy(&B, Right.data() + Lane + ((Select & 0x10) ? 8 : 0), 8);
      for (unsigned Bit = 0; Bit != 64; ++Bit)
        if ((B >> Bit) & 1) {
          Low ^= A << Bit;
          if (Bit)
            High ^= A >> (64 - Bit);
        }
      std::memcpy(Result.data() + Lane, &Low, 8);
      std::memcpy(Result.data() + Lane + 8, &High, 8);
      continue;
    }
    std::memcpy(Result.data() + Lane, Left.data() + Lane, 16);
    uint8_t *State = Result.data() + Lane;
    const bool Decrypt = Id == Intrinsic::AesDec || Id == Intrinsic::AesDecLast;
    const bool Last =
        Id == Intrinsic::AesEncLast || Id == Intrinsic::AesDecLast;
    aesShift(State, Decrypt);
    for (unsigned I = 0; I != 16; ++I)
      State[I] = Decrypt ? aesInvSbox(State[I]) : aesSbox(State[I]);
    if (!Last)
      aesMix(State, Decrypt);
    for (unsigned I = 0; I != 16; ++I)
      State[I] ^= Right[Lane + I];
  }
  writeOutputBytes(Op.Output, Result);
  return true;
}

bool NdOpEmulator::executeArith(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return false;
  const bool IsBitwise = Op.Opcode == NdOp::INT_AND ||
                         Op.Opcode == NdOp::INT_OR ||
                         Op.Opcode == NdOp::INT_XOR;
  if (IsBitwise && (Op.Output.Size > sizeof(uint64_t) ||
                    Op.Inputs[0].Size > sizeof(uint64_t) ||
                    Op.Inputs[1].Size > sizeof(uint64_t))) {
    if (Op.Output.Size == 0 || Op.Inputs[0].Size != Op.Output.Size ||
        Op.Inputs[1].Size != Op.Output.Size)
      return false;
    const std::vector<uint8_t> A = readOperandBytes(Op.Inputs[0]);
    const std::vector<uint8_t> B = readOperandBytes(Op.Inputs[1]);
    std::vector<uint8_t> Result(Op.Output.Size);
    for (size_t Index = 0; Index < Result.size(); ++Index) {
      if (Op.Opcode == NdOp::INT_AND)
        Result[Index] = A[Index] & B[Index];
      else if (Op.Opcode == NdOp::INT_OR)
        Result[Index] = A[Index] | B[Index];
      else
        Result[Index] = A[Index] ^ B[Index];
    }
    writeOutputBytes(Op.Output, Result);
    return true;
  }

  const bool IsWide = Op.Output.Size > sizeof(uint64_t) ||
                      Op.Inputs[0].Size > sizeof(uint64_t) ||
                      Op.Inputs[1].Size > sizeof(uint64_t);
  if (IsWide) {
    // Do not silently truncate an unimplemented wide arithmetic opcode to its
    // low host word.  These operations are required by the x86 double-width
    // MUL/IMUL and RDX:RAX DIV/IDIV families.  Every operand and the result
    // must have the same width: accepting a narrower divisor or output here
    // would make the operation look successful after silently truncating it.
    const bool IsSupportedWide =
        Op.Opcode == NdOp::INT_MULT || Op.Opcode == NdOp::INT_DIV ||
        Op.Opcode == NdOp::INT_SDIV || Op.Opcode == NdOp::INT_REM ||
        Op.Opcode == NdOp::INT_SREM || Op.Opcode == NdOp::INT_LEFT ||
        Op.Opcode == NdOp::INT_RIGHT || Op.Opcode == NdOp::INT_ASHR;
    if (!IsSupportedWide || Op.Output.Size == 0 ||
        Op.Inputs[0].Size != Op.Output.Size ||
        Op.Inputs[1].Size != Op.Output.Size)
      return false;

    const unsigned BitWidth = static_cast<unsigned>(Op.Output.Size) * 8;
    const auto ToAPInt = [BitWidth](const std::vector<uint8_t> &Bytes) {
      std::vector<uint64_t> Words(llvm::APInt::getNumWords(BitWidth), 0);
      for (size_t Index = 0; Index != Bytes.size(); ++Index)
        Words[Index / sizeof(uint64_t)] |= static_cast<uint64_t>(Bytes[Index])
                                           << ((Index % sizeof(uint64_t)) * 8);
      return llvm::APInt(BitWidth, Words);
    };

    const llvm::APInt A = ToAPInt(readOperandBytes(Op.Inputs[0]));
    const llvm::APInt B = ToAPInt(readOperandBytes(Op.Inputs[1]));
    if ((Op.Opcode == NdOp::INT_DIV || Op.Opcode == NdOp::INT_SDIV ||
         Op.Opcode == NdOp::INT_REM || Op.Opcode == NdOp::INT_SREM) &&
        B.isZero())
      return false;
    if ((Op.Opcode == NdOp::INT_SDIV || Op.Opcode == NdOp::INT_SREM) &&
        A.isMinSignedValue() && B.isAllOnes())
      return false;

    llvm::APInt WideResult(BitWidth, 0);
    const uint64_t Shift =
        B.getLimitedValue(UINT64_MAX) & static_cast<uint64_t>(BitWidth - 1);
    switch (Op.Opcode) {
    case NdOp::INT_MULT:
      WideResult = A * B;
      break;
    case NdOp::INT_DIV:
      WideResult = A.udiv(B);
      break;
    case NdOp::INT_SDIV:
      WideResult = A.sdiv(B);
      break;
    case NdOp::INT_REM:
      WideResult = A.urem(B);
      break;
    case NdOp::INT_SREM:
      WideResult = A.srem(B);
      break;
    case NdOp::INT_LEFT:
      WideResult = A.shl(Shift);
      break;
    case NdOp::INT_RIGHT:
      WideResult = A.lshr(Shift);
      break;
    case NdOp::INT_ASHR:
      WideResult = A.ashr(Shift);
      break;
    default:
      return false;
    }

    std::vector<uint8_t> Result(Op.Output.Size);
    for (size_t Index = 0; Index != Result.size(); ++Index)
      Result[Index] =
          static_cast<uint8_t>(WideResult.extractBitsAsZExtValue(8, Index * 8));
    writeOutputBytes(Op.Output, Result);
    return true;
  }

  uint64_t A = readOperand(Op.Inputs[0]);
  uint64_t B = readOperand(Op.Inputs[1]);
  uint64_t Result = 0;

  switch (Op.Opcode) {
  case NdOp::INT_ADD:
    Result = A + B;
    break;
  case NdOp::INT_SUB:
    Result = A - B;
    break;
  case NdOp::INT_MULT:
    Result = A * B;
    break;
  case NdOp::INT_AND:
    Result = A & B;
    break;
  case NdOp::INT_OR:
    Result = A | B;
    break;
  case NdOp::INT_XOR:
    Result = A ^ B;
    break;
  case NdOp::INT_LEFT:
    Result = A << (B & 63);
    break;
  case NdOp::INT_RIGHT:
    Result = A >> (B & 63);
    break;
  case NdOp::INT_ASHR: {
    uint64_t SignExtended = A;
    const uint16_t InputSize = Op.Inputs[0].Size;
    if (InputSize > 0 && InputSize < sizeof(SignExtended)) {
      const unsigned Bits = InputSize * 8;
      const uint64_t Mask = (1ULL << Bits) - 1;
      SignExtended &= Mask;
      if (SignExtended & (1ULL << (Bits - 1)))
        SignExtended |= ~Mask;
    }
    int64_t SA = std::bit_cast<int64_t>(SignExtended);
    Result = static_cast<uint64_t>(SA >> (B & 63));
    break;
  }
  case NdOp::INT_DIV:
    if (Op.Output.Size == 0 || Op.Output.Size > sizeof(uint64_t) ||
        Op.Inputs[0].Size != Op.Output.Size ||
        Op.Inputs[1].Size != Op.Output.Size || B == 0)
      return false;
    Result = A / B;
    break;
  case NdOp::INT_SDIV: {
    if (Op.Output.Size == 0 || Op.Output.Size > sizeof(uint64_t) ||
        Op.Inputs[0].Size != Op.Output.Size ||
        Op.Inputs[1].Size != Op.Output.Size)
      return false;
    const unsigned Bits = Op.Output.Size * 8;
    const uint64_t Mask = Bits == 64 ? UINT64_MAX
                                     : (UINT64_C(1) << Bits) - 1;
    const uint64_t Sign = UINT64_C(1) << (Bits - 1);
    const auto Signed = [Mask, Sign](uint64_t Value) {
      Value &= Mask;
      if (Value & Sign)
        Value |= ~Mask;
      return std::bit_cast<int64_t>(Value);
    };
    const int64_t SignedA = Signed(A);
    const int64_t SignedB = Signed(B);
    if (SignedB == 0 || ((A & Mask) == Sign && (B & Mask) == Mask))
      return false;
    Result = static_cast<uint64_t>(SignedA / SignedB);
    break;
  }
  case NdOp::INT_REM:
    if (Op.Output.Size == 0 || Op.Output.Size > sizeof(uint64_t) ||
        Op.Inputs[0].Size != Op.Output.Size ||
        Op.Inputs[1].Size != Op.Output.Size || B == 0)
      return false;
    Result = A % B;
    break;
  case NdOp::INT_SREM: {
    if (Op.Output.Size == 0 || Op.Output.Size > sizeof(uint64_t) ||
        Op.Inputs[0].Size != Op.Output.Size ||
        Op.Inputs[1].Size != Op.Output.Size)
      return false;
    const unsigned Bits = Op.Output.Size * 8;
    const uint64_t Mask = Bits == 64 ? UINT64_MAX
                                     : (UINT64_C(1) << Bits) - 1;
    const uint64_t Sign = UINT64_C(1) << (Bits - 1);
    const auto Signed = [Mask, Sign](uint64_t Value) {
      Value &= Mask;
      if (Value & Sign)
        Value |= ~Mask;
      return std::bit_cast<int64_t>(Value);
    };
    const int64_t SignedA = Signed(A);
    const int64_t SignedB = Signed(B);
    if (SignedB == 0 || ((A & Mask) == Sign && (B & Mask) == Mask))
      return false;
    Result = static_cast<uint64_t>(SignedA % SignedB);
    break;
  }
  default:
    return false;
  }

  writeOutput(Op.Output, Result);
  return true;
}

bool NdOpEmulator::executeFloatArith(const LowOp &Op) {
  if (Op.Opcode == NdOp::FLOAT_LESS) {
    if (Op.NumInputs < 2 || Op.Output.Size != 1 ||
        Op.Inputs[0].Size != Op.Inputs[1].Size ||
        (Op.Inputs[0].Size != sizeof(float) &&
         Op.Inputs[0].Size != sizeof(double)))
      return false;

    const uint64_t A = readOperand(Op.Inputs[0]);
    const uint64_t B = readOperand(Op.Inputs[1]);
    bool Result = false;
    if (Op.Inputs[0].Size == sizeof(float)) {
      const uint32_t LeftBits = static_cast<uint32_t>(A);
      const uint32_t RightBits = static_cast<uint32_t>(B);
      const auto IsNaN = [](uint32_t Bits) {
        return (Bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) &&
               (Bits & UINT32_C(0x007fffff)) != 0;
      };
      if (!IsNaN(LeftBits) && !IsNaN(RightBits))
        Result =
            std::bit_cast<float>(LeftBits) < std::bit_cast<float>(RightBits);
    } else {
      const auto IsNaN = [](uint64_t Bits) {
        return (Bits & UINT64_C(0x7ff0000000000000)) ==
                   UINT64_C(0x7ff0000000000000) &&
               (Bits & UINT64_C(0x000fffffffffffff)) != 0;
      };
      if (!IsNaN(A) && !IsNaN(B))
        Result = std::bit_cast<double>(A) < std::bit_cast<double>(B);
    }
    writeOutput(Op.Output, Result ? 1 : 0);
    return true;
  }

  if (Op.NumInputs < 1 || Op.Output.Size != Op.Inputs[0].Size ||
      (Op.Output.Size != sizeof(float) && Op.Output.Size != sizeof(double)))
    return false;

  const uint64_t A = readOperand(Op.Inputs[0]);
  if (Op.Opcode == NdOp::FLOAT_NEG) {
    writeOutput(Op.Output, A ^ (Op.Output.Size == sizeof(float)
                                    ? UINT64_C(0x0000000080000000)
                                    : UINT64_C(0x8000000000000000)));
    return true;
  }

  if (Op.Opcode == NdOp::FLOAT_SQRT) {
    if (Op.Output.Size == sizeof(float)) {
      const uint32_t Bits = static_cast<uint32_t>(A);
      const uint32_t Magnitude = Bits & UINT32_C(0x7fffffff);
      const uint32_t Exponent = Magnitude & UINT32_C(0x7f800000);
      const uint32_t Fraction = Magnitude & UINT32_C(0x007fffff);
      if (Exponent == UINT32_C(0x7f800000) && Fraction != 0) {
        // x86 quiets a signaling NaN without discarding its sign or payload.
        writeOutput(Op.Output, Bits | UINT32_C(0x00400000));
      } else if ((Bits & UINT32_C(0x80000000)) != 0 && Magnitude != 0) {
        // A negative nonzero operand produces the x86 indefinite QNaN.
        writeOutput(Op.Output, UINT32_C(0xffc00000));
      } else if (Magnitude == 0 || Magnitude == UINT32_C(0x7f800000)) {
        writeOutput(Op.Output, Bits);
      } else {
        const float Input = std::bit_cast<float>(Bits);
        writeOutput(Op.Output, std::bit_cast<uint32_t>(std::sqrt(Input)));
      }
    } else {
      const uint64_t Magnitude = A & UINT64_C(0x7fffffffffffffff);
      const uint64_t Exponent = Magnitude & UINT64_C(0x7ff0000000000000);
      const uint64_t Fraction = Magnitude & UINT64_C(0x000fffffffffffff);
      if (Exponent == UINT64_C(0x7ff0000000000000) && Fraction != 0) {
        writeOutput(Op.Output, A | UINT64_C(0x0008000000000000));
      } else if ((A & UINT64_C(0x8000000000000000)) != 0 && Magnitude != 0) {
        writeOutput(Op.Output, UINT64_C(0xfff8000000000000));
      } else if (Magnitude == 0 || Magnitude == UINT64_C(0x7ff0000000000000)) {
        writeOutput(Op.Output, A);
      } else {
        const double Input = std::bit_cast<double>(A);
        writeOutput(Op.Output, std::bit_cast<uint64_t>(std::sqrt(Input)));
      }
    }
    return true;
  }

  if (Op.Opcode == NdOp::FLOAT_FMA) {
    if (Op.NumInputs < 3 || Op.Output.Size != Op.Inputs[1].Size ||
        Op.Output.Size != Op.Inputs[2].Size)
      return false;
    const uint64_t B = readOperand(Op.Inputs[1]);
    const uint64_t C = readOperand(Op.Inputs[2]);
    if (Op.Output.Size == sizeof(float)) {
      const float Left = std::bit_cast<float>(static_cast<uint32_t>(A));
      const float Right = std::bit_cast<float>(static_cast<uint32_t>(B));
      const float Addend = std::bit_cast<float>(static_cast<uint32_t>(C));
      writeOutput(Op.Output,
                  std::bit_cast<uint32_t>(std::fma(Left, Right, Addend)));
    } else {
      writeOutput(Op.Output,
                  std::bit_cast<uint64_t>(std::fma(std::bit_cast<double>(A),
                                                   std::bit_cast<double>(B),
                                                   std::bit_cast<double>(C))));
    }
    return true;
  }

  if (Op.NumInputs < 2 || Op.Output.Size != Op.Inputs[1].Size)
    return false;
  const uint64_t B = readOperand(Op.Inputs[1]);
  if (Op.Output.Size == sizeof(float)) {
    const float Left = std::bit_cast<float>(static_cast<uint32_t>(A));
    const float Right = std::bit_cast<float>(static_cast<uint32_t>(B));
    float Result = 0.0f;
    switch (Op.Opcode) {
    case NdOp::FLOAT_ADD:
      Result = Left + Right;
      break;
    case NdOp::FLOAT_SUB:
      Result = Left - Right;
      break;
    case NdOp::FLOAT_MULT:
      Result = Left * Right;
      break;
    case NdOp::FLOAT_DIV:
      Result = Left / Right;
      break;
    default:
      return false;
    }
    writeOutput(Op.Output, std::bit_cast<uint32_t>(Result));
    return true;
  }

  const double Left = std::bit_cast<double>(A);
  const double Right = std::bit_cast<double>(B);
  double Result = 0.0;
  switch (Op.Opcode) {
  case NdOp::FLOAT_ADD:
    Result = Left + Right;
    break;
  case NdOp::FLOAT_SUB:
    Result = Left - Right;
    break;
  case NdOp::FLOAT_MULT:
    Result = Left * Right;
    break;
  case NdOp::FLOAT_DIV:
    Result = Left / Right;
    break;
  default:
    return false;
  }
  writeOutput(Op.Output, std::bit_cast<uint64_t>(Result));
  return true;
}

bool NdOpEmulator::executeLoad(const LowOp &Op) {
  const LowMemoryOperandView Memory = lowMemoryOperands(Op);
  if (!Memory.Complete)
    return false;
  const auto Addr = resolveMemoryAddress(Op, readOperand(*Memory.Address));
  if (!Addr)
    return false;

  if (Op.Output.Size == 16 || Op.Output.Size == 32 || Op.Output.Size == 64) {
    const auto Bytes = loadMemoryBytes(*Addr, Op.Output.Size);
    if (!Bytes)
      return false;
    if (CollectLoads &&
        static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
      LoadLog.push_back({*Addr, Op.Output.Size});
    writeOutputBytes(Op.Output, *Bytes);
    return true;
  }

  auto Val = loadMemory(*Addr, Op.Output.Size);
  if (!Val)
    return false;
  if (CollectLoads &&
      static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
    LoadLog.push_back({*Addr, Op.Output.Size});
  writeOutput(Op.Output, *Val);
  return true;
}

bool NdOpEmulator::executeStore(const LowOp &Op) {
  const LowMemoryOperandView Memory = lowMemoryOperands(Op);
  if (!Memory.Complete)
    return false;
  const auto Addr = resolveMemoryAddress(Op, readOperand(*Memory.Address));
  if (!Addr)
    return false;
  const uint16_t Size = Memory.AccessSize;
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8 && Size != 16 &&
      Size != 32 && Size != 64)
    return false;
  const std::vector<uint8_t> Value = readOperandBytes(*Memory.StoredValue);
  if (Value.size() < Size)
    return false;
  if (!storeMemoryBytes(*Addr, llvm::ArrayRef(Value).take_front(Size)))
    return !strictMode();
  return true;
}

bool NdOpEmulator::executeIntrinsic(const LowOp &Op) {
  if (Op.NumInputs < 1 || !Op.Inputs[0].isConst())
    return false;
  const Intrinsic Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::X86FPArith)
    return executeX86FPArith(Op);
  if (Id == Intrinsic::X86FPConvert)
    return executeX86FPConvert(Op);
  if (Id == Intrinsic::X86FPRoundTransform)
    return executeX86FPRoundTransform(Op);
  if (Id == Intrinsic::X86FPExtract)
    return executeX86FPExtract(Op);
  if (Id == Intrinsic::X86FPRange)
    return executeX86FPRange(Op);
  if (Id == Intrinsic::X86FPFixup)
    return executeX86FPFixup(Op);
  if (Id == Intrinsic::X86FPScale)
    return executeX86FPScale(Op);
  if (Id == Intrinsic::X86FPCompare)
    return executeX86FPCompare(Op);
  if (Id == Intrinsic::AesEnc || Id == Intrinsic::AesEncLast ||
      Id == Intrinsic::AesDec || Id == Intrinsic::AesDecLast ||
      Id == Intrinsic::Pclmulqdq)
    return executeX86Crypto(Op);
  if (Id == Intrinsic::X86RequireDivPrecondition) {
    if (!intrinsicX86DivPreconditionShapeIsValid(
            Id, x86DivPreconditionLowShape(Op, Img.Arch)))
      return false;
    const auto IsKnown = [&](const NdVar &Value) {
      if (Value.isConst())
        return true;
      if ((!Value.isReg() && !Value.isTemp()) ||
          !Registers.contains(Value.Offset))
        return false;
      if (Value.Size <= sizeof(uint64_t))
        return true;
      const auto Wide = WideRegisters.find(Value.Offset);
      return Wide != WideRegisters.end() && Wide->second.size() >= Value.Size;
    };
    const NdVar &DividendValue = Op.Inputs[1];
    const NdVar &DivisorValue = Op.Inputs[2];
    if (!IsKnown(DividendValue) || !IsKnown(DivisorValue))
      return false;

    const unsigned FullBits = DividendValue.Size * 8;
    const unsigned HalfBits = DivisorValue.Size * 8;
    const auto ToAPInt = [](const std::vector<uint8_t> &Bytes,
                            unsigned BitWidth) {
      std::vector<uint64_t> Words(llvm::APInt::getNumWords(BitWidth), 0);
      for (size_t Index = 0; Index != Bytes.size(); ++Index)
        Words[Index / sizeof(uint64_t)] |= static_cast<uint64_t>(Bytes[Index])
                                           << ((Index % sizeof(uint64_t)) * 8);
      return llvm::APInt(BitWidth, Words);
    };
    const llvm::APInt Dividend =
        ToAPInt(readOperandBytes(DividendValue), FullBits);
    const llvm::APInt Divisor =
        ToAPInt(readOperandBytes(DivisorValue), HalfBits);
    if (Divisor.isZero())
      return false;

    const bool IsSigned =
        Op.Inputs[3].Offset == static_cast<uint64_t>(X86DivKind::Signed);
    if (!IsSigned) {
      const llvm::APInt High = Dividend.lshr(HalfBits).trunc(HalfBits);
      return High.ult(Divisor);
    }

    const bool DividendNegative = Dividend.isNegative();
    const bool DivisorNegative = Divisor.isNegative();
    const llvm::APInt DividendMagnitude =
        DividendNegative ? -Dividend : Dividend;
    const llvm::APInt ExtendedDivisor = Divisor.sext(FullBits);
    const llvm::APInt DivisorMagnitude =
        DivisorNegative ? -ExtendedDivisor : ExtendedDivisor;
    llvm::APInt QuotientLimit(FullBits, 1);
    QuotientLimit <<= HalfBits - 1;
    if (DividendNegative != DivisorNegative)
      ++QuotientLimit;
    const llvm::APInt Threshold = DivisorMagnitude * QuotientLimit;
    return DividendMagnitude.ult(Threshold);
  }
  if (Id == Intrinsic::RequireAligned) {
    if (!isKnownMemoryAddressSpace(Op.MemoryAddressSpace) ||
        !intrinsicSupportsMemoryAddressSpace(Id) ||
        !intrinsicMemoryAddressSpaceShapeIsValid(
            Id, Op.NumInputs, Op.Output.Size,
            Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
            Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
            Op.NumInputs > 3 ? Op.Inputs[3].Size : 0) ||
        !Op.Inputs[2].isConst())
      return false;
    const uint64_t Alignment = readOperand(Op.Inputs[2]);
    if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
      return false;
    if (Op.NumInputs == 4 && readOperand(Op.Inputs[3]) == 0)
      return true;
    const auto Address = resolveMemoryAddress(Op, readOperand(Op.Inputs[1]));
    return Address && ((*Address & (Alignment - 1)) == 0);
  }

  if (Id == Intrinsic::Enqcmd || Id == Intrinsic::Enqcmds) {
    const auto IsKnownScalar = [&](const NdVar &Value) {
      return Value.isConst() ||
             ((Value.isReg() || Value.isTemp()) &&
              Registers.contains(Value.Offset));
    };
    if (Img.Arch != Arch::X64 || Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.NumInputs != 3 || Op.Inputs[0].Size != 2 ||
        (!Op.Output.isReg() && !Op.Output.isTemp()) ||
        !intrinsicSupportsMemoryAddressSpace(Id) ||
        !intrinsicMemoryAddressSpaceShapeIsValid(
            Id, Op.NumInputs, Op.Output.Size, Op.Inputs[1].Size,
            Op.Inputs[2].Size, 0) ||
        !IsKnownScalar(Op.Inputs[1]) || !IsKnownScalar(Op.Inputs[2]) ||
        !X86CurrentPrivilegeLevel || !X86IA32Pasid ||
        !X86LinearAddressBits)
      return false;

    // These feature-specific checks precede even the ordinary source load.
    // Requiring explicit configuration keeps static BinaryImage emulation
    // from inventing either current privilege or per-thread PASID state.
    if (Id == Intrinsic::Enqcmds) {
      if (*X86CurrentPrivilegeLevel != 0)
        return false;
    } else if ((*X86IA32Pasid & UINT32_C(0x80000000)) == 0) {
      return false;
    }

    const auto IsCanonical = [&](uint64_t Address) {
      const unsigned Bits = *X86LinearAddressBits;
      const uint64_t LowMask = (UINT64_C(1) << Bits) - 1;
      const uint64_t HighMask = ~LowMask;
      const bool Sign = ((Address >> (Bits - 1)) & 1) != 0;
      return (Address & HighMask) == (Sign ? HighMask : 0);
    };
    const auto IsCanonicalRange = [&](uint64_t Address, uint64_t Size) {
      return Size != 0 && Size - 1 <= UINT64_MAX - Address &&
             IsCanonical(Address) && IsCanonical(Address + Size - 1);
    };

    const auto SourceAddress =
        resolveMemoryAddress(Op, readOperand(Op.Inputs[1]));
    if (!SourceAddress || !IsCanonicalRange(*SourceAddress, 64))
      return false;
    const auto Command = loadMemoryBytes(*SourceAddress, 64);
    if (!Command)
      return false;
    if (CollectLoads &&
        static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
      LoadLog.push_back({*SourceAddress, 64});

    uint32_t Header = 0;
    std::memcpy(&Header, Command->data(), sizeof(Header));
    if ((Id == Intrinsic::Enqcmd && Header != 0) ||
        (Id == Intrinsic::Enqcmds &&
         (Header & UINT32_C(0x7ff00000)) != 0))
      return false;

    const uint64_t PortalAddress = readOperand(Op.Inputs[2]);
    if ((PortalAddress & 63) != 0 ||
        !IsCanonicalRange(PortalAddress, 64))
      return false;
    for (uint64_t Offset = 0; Offset != 64; ++Offset) {
      const Segment *Mapped = Img.getSegmentFor(PortalAddress + Offset);
      if (!Mapped || !Mapped->isWritable())
        return false;
    }

    // BinaryImage has no authenticated enqueue-register portal.  Intel
    // defines an ordinary RAM/MMIO destination as retry: the enqueue store is
    // dropped, no bytes are written, and ZF receives one.
    writeOutput(Op.Output, 1);
    return true;
  }

  const bool IsApxRao = Id == Intrinsic::ApxRaoAdd ||
                        Id == Intrinsic::ApxRaoAnd ||
                        Id == Intrinsic::ApxRaoOr || Id == Intrinsic::ApxRaoXor;
  const bool IsApxCmpccXadd = Id == Intrinsic::ApxCmpccXadd;
  if (IsApxRao || IsApxCmpccXadd) {
    if (!isKnownMemoryAddressSpace(Op.MemoryAddressSpace) ||
        !intrinsicSupportsMemoryAddressSpace(Id) ||
        !intrinsicApxAtomicShapeIsValid(Id, apxAtomicLowShape(Op, Img.Arch)))
      return false;

    const uint16_t Width = IsApxRao ? Op.Inputs[2].Size : Op.Output.Size;

    const auto Address = resolveMemoryAddress(Op, readOperand(Op.Inputs[1]));
    if (!Address || (*Address & (Width - 1)) != 0)
      return false;

    // Atomic accesses are stricter than ordinary tracing loads/stores.  Check
    // the complete underlying mapping and both permissions before consulting
    // the write-back overlay: an overlay must not make a write-only or
    // truncated image mapping appear readable.  All checks use the final
    // linear address, after an FS/GS base has been applied.
    const Segment *Mapped = Img.getSegmentFor(*Address);
    if (!Mapped || !Mapped->isReadable() || !Mapped->isWritable() ||
        *Address < Mapped->VA)
      return false;
    const uint64_t Offset = *Address - Mapped->VA;
    if (Offset > Mapped->Size || Width > Mapped->Size - Offset ||
        Offset > Mapped->Data.size() || Width > Mapped->Data.size() - Offset)
      return false;

    // Reserve the store slot before reading or exposing any architectural
    // result.  CMPccXADD performs a write even when its condition is false.
    if (!MemStore.contains(*Address) &&
        static_cast<int>(MemStore.size()) >= limits::kMaxEmulatorStoreEntries) {
      ++Skips.DroppedStores;
      return false;
    }

    const std::optional<uint64_t> Loaded = loadMemory(*Address, Width);
    if (!Loaded)
      return false;
    const uint64_t Mask =
        Width == 8 ? UINT64_MAX : (UINT64_C(1) << (Width * 8)) - UINT64_C(1);
    const uint64_t Old = *Loaded & Mask;
    uint64_t Stored = Old;
    if (IsApxRao) {
      const uint64_t Source = readOperand(Op.Inputs[2]) & Mask;
      if (Id == Intrinsic::ApxRaoAdd)
        Stored = (Old + Source) & Mask;
      else if (Id == Intrinsic::ApxRaoAnd)
        Stored = Old & Source;
      else if (Id == Intrinsic::ApxRaoOr)
        Stored = Old | Source;
      else
        Stored = Old ^ Source;
    } else {
      const uint64_t Add = readOperand(Op.Inputs[2]) & Mask;
      const uint64_t Compare = readOperand(Op.Inputs[3]) & Mask;
      const uint64_t Difference = (Old - Compare) & Mask;
      const uint64_t Sign = UINT64_C(1) << (Width * 8 - 1);
      const bool CF = Old < Compare;
      const bool PF =
          (std::popcount(static_cast<unsigned>(Difference & 0xff)) & 1) == 0;
      const bool ZF = Difference == 0;
      const bool SF = (Difference & Sign) != 0;
      const bool OF = ((Old ^ Compare) & (Old ^ Difference) & Sign) != 0;
      bool Condition = false;
      switch (Op.Inputs[4].Offset) {
      case 0:
        Condition = OF;
        break;
      case 1:
        Condition = !OF;
        break;
      case 2:
        Condition = CF;
        break;
      case 3:
        Condition = !CF;
        break;
      case 4:
        Condition = ZF;
        break;
      case 5:
        Condition = !ZF;
        break;
      case 6:
        Condition = CF || ZF;
        break;
      case 7:
        Condition = !CF && !ZF;
        break;
      case 8:
        Condition = SF;
        break;
      case 9:
        Condition = !SF;
        break;
      case 10:
        Condition = PF;
        break;
      case 11:
        Condition = !PF;
        break;
      case 12:
        Condition = SF != OF;
        break;
      case 13:
        Condition = SF == OF;
        break;
      case 14:
        Condition = ZF || SF != OF;
        break;
      case 15:
        Condition = !ZF && SF == OF;
        break;
      }
      if (Condition)
        Stored = (Old + Add) & Mask;
    }

    // Commit the memory result before the load record or output.  Every
    // failure above therefore leaves memory, load-log, registers, and the
    // following lifted flag operations untouched as one transaction.
    if (!storeMemory(*Address, Width, Stored))
      return false;
    if (CollectLoads &&
        static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
      LoadLog.push_back({*Address, Width});
    if (IsApxCmpccXadd)
      writeOutput(Op.Output, Old);
    return true;
  }

  if (isPdepPextIntrinsic(Id)) {
    if (!intrinsicPdepPextShapeIsValid(Id, pdepPextLowShape(Op)))
      return false;

    const uint64_t Source = readOperand(Op.Inputs[1]);
    const uint64_t Mask = readOperand(Op.Inputs[2]);
    const unsigned BitWidth = Op.Output.Size * 8;
    uint64_t Result = 0;
    unsigned PackedBit = 0;
    for (unsigned Bit = 0; Bit != BitWidth; ++Bit) {
      if (((Mask >> Bit) & 1) == 0)
        continue;
      if (Id == Intrinsic::Pdep)
        Result |= ((Source >> PackedBit) & 1) << Bit;
      else
        Result |= ((Source >> Bit) & 1) << PackedBit;
      ++PackedBit;
    }
    writeOutput(Op.Output, Result);
    return true;
  }

  if (Id == Intrinsic::Mpsadbw) {
    if (Op.NumInputs != 4 || (Op.Output.Size != 16 && Op.Output.Size != 32) ||
        Op.Inputs[1].Size != Op.Output.Size ||
        Op.Inputs[2].Size != Op.Output.Size || !Op.Inputs[3].isConst() ||
        Op.Inputs[3].Size != 1 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;

    const std::vector<uint8_t> Left = readOperandBytes(Op.Inputs[1]);
    const std::vector<uint8_t> Right = readOperandBytes(Op.Inputs[2]);
    const uint8_t Immediate = static_cast<uint8_t>(readOperand(Op.Inputs[3]));
    std::vector<uint8_t> Result(Op.Output.Size, 0);

    for (size_t Lane = 0; Lane < Result.size() / 16; ++Lane) {
      const unsigned Control = (Immediate >> (Lane * 3)) & 7;
      const size_t LaneOffset = Lane * 16;
      const size_t LeftOffset = LaneOffset + (Control & 4);
      const size_t RightOffset = LaneOffset + (Control & 3) * 4;
      for (size_t Word = 0; Word < 8; ++Word) {
        uint16_t Sum = 0;
        for (size_t Byte = 0; Byte < 4; ++Byte) {
          const uint8_t A = Left[LeftOffset + Word + Byte];
          const uint8_t B = Right[RightOffset + Byte];
          Sum += A >= B ? static_cast<uint16_t>(A - B)
                        : static_cast<uint16_t>(B - A);
        }
        const size_t OutputOffset = LaneOffset + Word * 2;
        Result[OutputOffset] = static_cast<uint8_t>(Sum);
        Result[OutputOffset + 1] = static_cast<uint8_t>(Sum >> 8);
      }
    }

    writeOutputBytes(Op.Output, Result);
    return true;
  }

  if (Id == Intrinsic::Vdbpsadbw) {
    if (Op.NumInputs != 4 ||
        (Op.Output.Size != 16 && Op.Output.Size != 32 &&
         Op.Output.Size != 64) ||
        Op.Inputs[1].Size != Op.Output.Size ||
        Op.Inputs[2].Size != Op.Output.Size || !Op.Inputs[3].isConst() ||
        Op.Inputs[3].Size != 1 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    const std::vector<uint8_t> A = readOperandBytes(Op.Inputs[1]);
    const std::vector<uint8_t> B = readOperandBytes(Op.Inputs[2]);
    const uint8_t Immediate = static_cast<uint8_t>(readOperand(Op.Inputs[3]));
    std::vector<uint8_t> Permuted(B.size()), Result(B.size());
    for (size_t Lane = 0; Lane < B.size(); Lane += 16)
      for (size_t Dword = 0; Dword < 4; ++Dword) {
        const size_t Source = Lane + ((Immediate >> (2 * Dword)) & 3) * 4;
        std::copy_n(B.begin() + Source, 4, Permuted.begin() + Lane + Dword * 4);
      }
    for (size_t Base = 0; Base < A.size(); Base += 8)
      for (size_t Word = 0; Word < 4; ++Word) {
        const size_t ABase = Base + (Word >= 2 ? 4 : 0);
        const size_t BBase = Base + Word;
        uint16_t Sum = 0;
        for (size_t Byte = 0; Byte < 4; ++Byte) {
          const uint8_t Left = A[ABase + Byte];
          const uint8_t Right = Permuted[BBase + Byte];
          Sum += Left >= Right ? static_cast<uint16_t>(Left - Right)
                               : static_cast<uint16_t>(Right - Left);
        }
        Result[Base + Word * 2] = static_cast<uint8_t>(Sum);
        Result[Base + Word * 2 + 1] = static_cast<uint8_t>(Sum >> 8);
      }
    writeOutputBytes(Op.Output, Result);
    return true;
  }

  if (Id == Intrinsic::X86FourFMA) {
    if (Op.NumInputs != 6 || (Op.Output.Size != 16 && Op.Output.Size != 64) ||
        Op.Inputs[1].Size == 0 || Op.Inputs[1].Size > 8 ||
        Op.Inputs[2].Size != Op.Output.Size || !Op.Inputs[3].isConst() ||
        Op.Inputs[3].Size != 1 ||
        (Op.Inputs[4].Size != 1 && Op.Inputs[4].Size != 2) ||
        !Op.Inputs[5].isConst() || Op.Inputs[5].Size != 1)
      return false;
    const uint8_t GroupBase = static_cast<uint8_t>(readOperand(Op.Inputs[3]));
    const uint8_t Control = static_cast<uint8_t>(readOperand(Op.Inputs[5]));
    if ((Control & ~7u) != 0)
      return false;
    const bool Scalar = (Control & 1) != 0;
    const bool Negative = (Control & 2) != 0;
    const bool ZeroMask = (Control & 4) != 0;
    if (Scalar != (Op.Output.Size == 16) ||
        Op.Inputs[4].Size != (Scalar ? 1 : 2) || GroupBase > 28 ||
        (GroupBase & 3) != 0)
      return false;
    const uint64_t Mask = readOperand(Op.Inputs[4]);
    const uint64_t ActiveMask = Mask & (Scalar ? 1 : 0xffff);
    std::array<uint32_t, 4> MemoryWords{};
    if (ActiveMask != 0) {
      const auto Address = resolveMemoryAddress(Op, readOperand(Op.Inputs[1]));
      if (!Address)
        return false;
      for (unsigned I = 0; I < 4; ++I) {
        const auto Word = loadMemory(*Address + I * 4, 4);
        if (!Word)
          return false;
        MemoryWords[I] = static_cast<uint32_t>(*Word);
      }
      if (CollectLoads &&
          static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
        LoadLog.push_back({*Address, 16});
    }
    const std::vector<uint8_t> Old = readOperandBytes(Op.Inputs[2]);
    std::array<std::vector<uint8_t>, 4> Sources;
    if (ActiveMask != 0)
      for (unsigned I = 0; I < 4; ++I) {
        const x86_reg Reg = static_cast<x86_reg>(
            (Scalar ? X86_REG_XMM0 : X86_REG_ZMM0) + GroupBase + I);
        const RegInfo Info = mapCapstoneReg(Reg);
        const auto Value = getRegisterBytes(Info.Offset);
        if (!Value || Value->size() < Op.Output.Size)
          return false;
        Sources[I].assign(Value->begin(), Value->begin() + Op.Output.Size);
      }
    std::vector<uint8_t> Result = Old;
    const unsigned Lanes = Scalar ? 1 : 16;
    std::vector<uint32_t> Accumulators(Lanes);
    for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
      const unsigned Offset = Lane * 4;
      if (((ActiveMask >> Lane) & 1) == 0) {
        if (ZeroMask)
          std::fill_n(Result.begin() + Offset, 4, 0);
        continue;
      }
      std::memcpy(&Accumulators[Lane], Old.data() + Offset, 4);
    }
    auto Rounding = [&]() {
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
    }();
    auto StatusFlags = [](llvm::APFloat::opStatus Status) {
      const unsigned Raw = static_cast<unsigned>(Status);
      uint32_t Flags = 0;
      if (Raw & llvm::APFloat::opInvalidOp)
        Flags |= 1U << 0;
      if (Raw & llvm::APFloat::opDivByZero)
        Flags |= 1U << 2;
      if (Raw & llvm::APFloat::opOverflow)
        Flags |= 1U << 3;
      if (Raw & llvm::APFloat::opUnderflow)
        Flags |= 1U << 4;
      if (Raw & llvm::APFloat::opInexact)
        Flags |= 1U << 5;
      return Flags;
    };
    auto IsNaN = [](uint32_t V) {
      return (V & 0x7f800000U) == 0x7f800000U && (V & 0x007fffffU) != 0;
    };
    auto IsSignalingNaN = [&](uint32_t V) {
      return IsNaN(V) && (V & 0x00400000U) == 0;
    };
    auto IsInf = [](uint32_t V) { return (V & 0x7fffffffU) == 0x7f800000U; };
    auto IsZero = [](uint32_t V) { return (V & 0x7fffffffU) == 0; };
    auto IsDenormal = [](uint32_t V) {
      return (V & 0x7f800000U) == 0 && (V & 0x007fffffU) != 0;
    };
    auto ApplyDaz = [&](uint32_t Bits) {
      return (MXCSR & (1U << 6)) != 0 && IsDenormal(Bits) ? Bits & 0x80000000U
                                                          : Bits;
    };
    for (unsigned I = 0; I < 4; ++I) {
      uint32_t PreRaised = 0;
      std::vector<uint32_t> ArithmeticSources(Lanes);
      std::vector<uint32_t> Multipliers(Lanes);
      std::vector<uint32_t> Addends(Lanes);
      std::vector<uint8_t> NeedsPostComputation(Lanes, 0);
      for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
        if (((ActiveMask >> Lane) & 1) == 0)
          continue;
        const unsigned Offset = Lane * 4;
        uint32_t OriginalSource = 0;
        std::memcpy(&OriginalSource, Sources[I].data() + Offset, 4);
        const uint32_t OriginalMultiplier = MemoryWords[I];
        const uint32_t OriginalAddend = Accumulators[Lane];

        // Intel's pre-computation priority is NaN/other-invalid, then
        // denormal.  Select a source NaN from its original architectural bits:
        // V4FNMADD's arithmetic negation must not change its sign or payload.
        if (IsNaN(OriginalSource) || IsNaN(OriginalMultiplier) ||
            IsNaN(OriginalAddend)) {
          if (IsSignalingNaN(OriginalSource) ||
              IsSignalingNaN(OriginalMultiplier) ||
              IsSignalingNaN(OriginalAddend))
            PreRaised |= 1U << 0;
          const uint32_t Selected = IsNaN(OriginalSource) ? OriginalSource
                                    : IsNaN(OriginalMultiplier)
                                        ? OriginalMultiplier
                                        : OriginalAddend;
          Accumulators[Lane] = Selected | 0x00400000U;
          continue;
        }

        uint32_t SourceBits = ApplyDaz(OriginalSource);
        const uint32_t MultiplierBits = ApplyDaz(OriginalMultiplier);
        const uint32_t AddendBits = ApplyDaz(OriginalAddend);
        if (Negative)
          SourceBits ^= 0x80000000U;
        if ((IsZero(SourceBits) && IsInf(MultiplierBits)) ||
            (IsInf(SourceBits) && IsZero(MultiplierBits))) {
          Accumulators[Lane] = 0xffc00000U;
          PreRaised |= 1U << 0;
          continue;
        }
        const bool InfiniteProduct = IsInf(SourceBits) || IsInf(MultiplierBits);
        const uint32_t ProductInf =
            0x7f800000U | ((SourceBits ^ MultiplierBits) & 0x80000000U);
        if (InfiniteProduct && IsInf(AddendBits) &&
            ((ProductInf ^ AddendBits) & 0x80000000U)) {
          Accumulators[Lane] = 0xffc00000U;
          PreRaised |= 1U << 0;
          continue;
        }

        if ((MXCSR & (1U << 6)) == 0 &&
            (IsDenormal(OriginalSource) || IsDenormal(OriginalMultiplier) ||
             IsDenormal(OriginalAddend)))
          PreRaised |= 1U << 1;
        if (InfiniteProduct) {
          Accumulators[Lane] = ProductInf;
          continue;
        }
        if (IsInf(AddendBits)) {
          Accumulators[Lane] = AddendBits;
          continue;
        }

        ArithmeticSources[Lane] = SourceBits;
        Multipliers[Lane] = MultiplierBits;
        Addends[Lane] = AddendBits;
        NeedsPostComputation[Lane] = 1;
      }

      // Packed lanes report the union of their pre-computation exceptions.
      // An unmasked pre fault suppresses post-computation for every lane in
      // this iteration, rather than allowing a different lane to add O/U/P.
      MXCSR |= PreRaised;
      if ((PreRaised & ~(MXCSR >> 7) & 0x03U) != 0)
        return false;

      uint32_t PostRaised = 0;
      for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
        if (!NeedsPostComputation[Lane])
          continue;
        uint32_t Raised = 0;
        llvm::APFloat Product(llvm::APFloat::IEEEsingle(),
                              llvm::APInt(32, ArithmeticSources[Lane]));
        const llvm::APFloat Multiplier(llvm::APFloat::IEEEsingle(),
                                       llvm::APInt(32, Multipliers[Lane]));
        const llvm::APFloat Addend(llvm::APFloat::IEEEsingle(),
                                   llvm::APInt(32, Addends[Lane]));
        Raised |=
            StatusFlags(Product.fusedMultiplyAdd(Multiplier, Addend, Rounding));
        Accumulators[Lane] =
            static_cast<uint32_t>(Product.bitcastToAPInt().getZExtValue());
        const bool Tiny = IsDenormal(Accumulators[Lane]);
        const bool UnderflowMasked = (MXCSR & (1U << 11)) != 0;
        if (Tiny && !UnderflowMasked) {
          // Exact tiny results still fault when underflow is unmasked.  FTZ is
          // ignored on this path and must not synthesize precision.
          Raised |= 1U << 4;
        } else if (Tiny && (MXCSR & (1U << 15)) != 0) {
          // With underflow masked, FTZ flushes even an exact tiny result and
          // reports both underflow and precision.
          Accumulators[Lane] &= 0x80000000U;
          Raised |= (1U << 4) | (1U << 5);
        }
        PostRaised |= Raised;
      }
      MXCSR |= PostRaised;
      if ((PostRaised & ~(MXCSR >> 7) & 0x3fU) != 0)
        return false;
    }
    for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
      if (((ActiveMask >> Lane) & 1) == 0)
        continue;
      const unsigned Offset = Lane * 4;
      const uint32_t Bits = Accumulators[Lane];
      Result[Offset] = static_cast<uint8_t>(Bits);
      Result[Offset + 1] = static_cast<uint8_t>(Bits >> 8);
      Result[Offset + 2] = static_cast<uint8_t>(Bits >> 16);
      Result[Offset + 3] = static_cast<uint8_t>(Bits >> 24);
    }
    writeOutputBytes(Op.Output, Result);
    return true;
  }

  if (isX86VP4DPIntrinsic(Id)) {
    // [id, effective address, old ZMM destination, source-block base,
    // compact k mask, zeroing bit].  The four-register source block and the
    // m128 Tuple1_4X are architectural: do not model this as one vector add.
    if (!intrinsicX86VP4DPShapeIsValid(
            Id, Op.NumInputs, Op.Output.Size,
            Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
            Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
            Op.NumInputs > 3 ? Op.Inputs[3].Size : 0,
            Op.NumInputs > 4 ? Op.Inputs[4].Size : 0,
            Op.NumInputs > 5 ? Op.Inputs[5].Size : 0) ||
        !Op.Inputs[3].isConst() ||
        (!Op.Inputs[4].isConst() && !Op.Inputs[4].isReg()) ||
        !Op.Inputs[5].isConst())
      return false;
    const uint64_t GroupBase = readOperand(Op.Inputs[3]);
    const uint64_t Mask = readOperand(Op.Inputs[4]) & UINT64_C(0xffff);
    const bool ZeroMask = (readOperand(Op.Inputs[5]) & 1) != 0;
    if (GroupBase > 28 || (GroupBase & 3) != 0)
      return false;

    // Tuple1_4X is all-or-nothing: if any destination lane is active, read
    // all four dwords before observing or publishing any destination result.
    std::array<uint32_t, 4> MemoryWords{};
    if (Mask != 0) {
      const auto Address = resolveMemoryAddress(Op, readOperand(Op.Inputs[1]));
      if (!Address)
        return false;
      const auto Bytes = loadMemoryBytes(*Address, 16);
      if (!Bytes)
        return false;
      for (unsigned I = 0; I < 4; ++I)
        std::memcpy(&MemoryWords[I], Bytes->data() + I * 4, 4);
      if (CollectLoads &&
          static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
        LoadLog.push_back({*Address, 16});
    }

    const std::vector<uint8_t> Old = readOperandBytes(Op.Inputs[2]);
    if (Old.size() != 64)
      return false;
    std::array<std::vector<uint8_t>, 4> Sources;
    if (Mask != 0)
      for (unsigned I = 0; I < 4; ++I) {
        const RegInfo Info = mapCapstoneReg(static_cast<x86_reg>(
            X86_REG_ZMM0 + static_cast<unsigned>(GroupBase) + I));
        const auto Value = getRegisterBytes(Info.Offset);
        if (Info.Offset == UINT64_C(0xffff) || !Value || Value->size() < 64)
          return false;
        Sources[I].assign(Value->begin(), Value->begin() + 64);
      }

    std::vector<uint8_t> Result = Old;
    auto readDword = [](const std::vector<uint8_t> &Bytes, size_t Offset) {
      uint32_t Value = 0;
      std::memcpy(&Value, Bytes.data() + Offset, sizeof(Value));
      return Value;
    };
    auto writeDword = [](std::vector<uint8_t> &Bytes, size_t Offset,
                         uint32_t Value) {
      std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
    };
    auto readWord = [](const std::vector<uint8_t> &Bytes, size_t Offset) {
      uint16_t Value = 0;
      std::memcpy(&Value, Bytes.data() + Offset, sizeof(Value));
      return static_cast<int32_t>(static_cast<int16_t>(Value));
    };
    const bool Saturating = Id == Intrinsic::X86VP4DPWSSDS;
    for (unsigned Lane = 0; Lane < 16; ++Lane) {
      if (((Mask >> Lane) & 1) == 0) {
        if (ZeroMask)
          writeDword(Result, Lane * 4, 0);
        continue;
      }
      uint32_t Acc = readDword(Old, Lane * 4);
      for (unsigned I = 0; I < 4; ++I) {
        const int64_t Product =
            static_cast<int64_t>(readWord(Sources[I], Lane * 4)) *
                static_cast<int64_t>(static_cast<int16_t>(
                    MemoryWords[I] & UINT32_C(0xffff))) +
            static_cast<int64_t>(readWord(Sources[I], Lane * 4 + 2)) *
                static_cast<int64_t>(static_cast<int16_t>(
                    MemoryWords[I] >> 16));
        const int64_t Sum = static_cast<int64_t>(static_cast<int32_t>(Acc)) +
                            Product;
        if (Saturating) {
          const int64_t Clamped =
              std::max<int64_t>(-2147483648LL,
                                std::min<int64_t>(2147483647LL, Sum));
          Acc = static_cast<uint32_t>(static_cast<int32_t>(Clamped));
        } else {
          // VP4DPWSSD wraps after every one of the four sequential updates.
          Acc = static_cast<uint32_t>(Sum);
        }
      }
      writeDword(Result, Lane * 4, Acc);
    }
    writeOutputBytes(Op.Output, Result);
    return true;
  }

  if (Id == Intrinsic::F16CConvert) {
    if (Op.NumInputs != 4 || !Op.Inputs[1].isConst() ||
        Op.Inputs[1].Size != 1 || !Op.Inputs[3].isConst() ||
        Op.Inputs[3].Size != 1 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    const uint64_t RawKind = readOperand(Op.Inputs[1]);
    if (RawKind > static_cast<uint64_t>(
                      F16CConvertKind::SingleToHalfSuppressExceptions))
      return false;
    const auto Kind = static_cast<F16CConvertKind>(RawKind);
    const bool HalfToSingle = Kind == F16CConvertKind::HalfToSingle ||
                              Kind == F16CConvertKind::
                                          HalfToSingleSuppressExceptions;
    const bool SuppressExceptions =
        Kind == F16CConvertKind::HalfToSingleSuppressExceptions ||
        Kind == F16CConvertKind::SingleToHalfSuppressExceptions;
    if (HalfToSingle) {
      if ((Op.Output.Size != 16 && Op.Output.Size != 32 &&
           Op.Output.Size != 64) ||
          Op.Inputs[2].Size < Op.Output.Size / 2 ||
          Op.Inputs[2].Size > 32)
        return false;
    } else if ((Op.Output.Size != 16 && Op.Output.Size != 32) ||
               (Op.Inputs[2].Size != 16 && Op.Inputs[2].Size != 32 &&
                Op.Inputs[2].Size != 64) ||
               Op.Output.Size != std::max<uint16_t>(16, Op.Inputs[2].Size / 2)) {
      return false;
    }

    auto readWord = [](llvm::ArrayRef<uint8_t> Bytes, size_t Offset) {
      return static_cast<uint16_t>(Bytes[Offset]) |
             (static_cast<uint16_t>(Bytes[Offset + 1]) << 8);
    };
    auto readDword = [](llvm::ArrayRef<uint8_t> Bytes, size_t Offset) {
      return static_cast<uint32_t>(Bytes[Offset]) |
             (static_cast<uint32_t>(Bytes[Offset + 1]) << 8) |
             (static_cast<uint32_t>(Bytes[Offset + 2]) << 16) |
             (static_cast<uint32_t>(Bytes[Offset + 3]) << 24);
    };
    auto writeWord = [](std::vector<uint8_t> &Bytes, size_t Offset,
                        uint16_t Value) {
      Bytes[Offset] = static_cast<uint8_t>(Value);
      Bytes[Offset + 1] = static_cast<uint8_t>(Value >> 8);
    };
    auto writeDword = [](std::vector<uint8_t> &Bytes, size_t Offset,
                         uint32_t Value) {
      for (unsigned Byte = 0; Byte < 4; ++Byte)
        Bytes[Offset + Byte] = static_cast<uint8_t>(Value >> (Byte * 8));
    };
    auto mxcsrRounding = [](uint32_t Control) {
      switch ((Control >> 13) & 3U) {
      case 1:
        return llvm::APFloat::rmTowardNegative;
      case 2:
        return llvm::APFloat::rmTowardPositive;
      case 3:
        return llvm::APFloat::rmTowardZero;
      default:
        return llvm::APFloat::rmNearestTiesToEven;
      }
    };
    auto immediateRounding = [](uint8_t Immediate) {
      switch (Immediate & 3U) {
      case 1:
        return llvm::APFloat::rmTowardNegative;
      case 2:
        return llvm::APFloat::rmTowardPositive;
      case 3:
        return llvm::APFloat::rmTowardZero;
      default:
        return llvm::APFloat::rmNearestTiesToEven;
      }
    };
    auto statusFlags = [](llvm::APFloat::opStatus Status) {
      const unsigned Raw = static_cast<unsigned>(Status);
      uint32_t Flags = 0;
      if (Raw & llvm::APFloat::opInvalidOp)
        Flags |= 1U << 0;
      if (Raw & llvm::APFloat::opDivByZero)
        Flags |= 1U << 2;
      if (Raw & llvm::APFloat::opOverflow)
        Flags |= 1U << 3;
      if (Raw & llvm::APFloat::opUnderflow)
        Flags |= 1U << 4;
      if (Raw & llvm::APFloat::opInexact)
        Flags |= 1U << 5;
      return Flags;
    };

    const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[2]);
    std::vector<uint8_t> Result(Op.Output.Size, 0);
    uint32_t Raised = 0;
    if (HalfToSingle) {
      const size_t Elements = Op.Output.Size / 4;
      for (size_t Element = 0; Element < Elements; ++Element) {
        const uint16_t Bits = readWord(Source, Element * 2);
        const uint16_t Fraction = Bits & 0x03ffU;
        uint32_t Converted = 0;
        if ((Bits & 0x7c00U) == 0x7c00U && Fraction != 0) {
          if ((Fraction & 0x0200U) == 0)
            Raised |= 1U << 0;
          Converted = (static_cast<uint32_t>(Bits & 0x8000U) << 16) |
                      0x7f800000U | (static_cast<uint32_t>(Fraction) << 13) |
                      0x00400000U;
        } else {
          llvm::APFloat Value(llvm::APFloat::IEEEhalf(), llvm::APInt(16, Bits));
          bool LosesInfo = false;
          const llvm::APFloat::opStatus Status =
              Value.convert(llvm::APFloat::IEEEsingle(),
                            llvm::APFloat::rmNearestTiesToEven, &LosesInfo);
          Raised |= statusFlags(Status);
          Converted =
              static_cast<uint32_t>(Value.bitcastToAPInt().getZExtValue());
        }
        writeDword(Result, Element * 4, Converted);
      }
    } else {
      const uint8_t Immediate = static_cast<uint8_t>(readOperand(Op.Inputs[3]));
      const llvm::APFloat::roundingMode Rounding =
          (Immediate & 4U) ? mxcsrRounding(MXCSR)
                           : immediateRounding(Immediate);
      const bool Daz = (MXCSR & (1U << 6)) != 0;
      const size_t Elements = Op.Inputs[2].Size / 4;
      for (size_t Element = 0; Element < Elements; ++Element) {
        uint32_t Bits = readDword(Source, Element * 4);
        const uint32_t Exponent = Bits & 0x7f800000U;
        const uint32_t Fraction = Bits & 0x007fffffU;
        const bool Denormal = Exponent == 0 && Fraction != 0;
        if (Denormal && Daz) {
          Bits &= 0x80000000U;
        } else if (Denormal) {
          Raised |= 1U << 1;
        }

        uint16_t Converted = 0;
        const uint32_t EffectiveExponent = Bits & 0x7f800000U;
        const uint32_t EffectiveFraction = Bits & 0x007fffffU;
        if (EffectiveExponent == 0x7f800000U && EffectiveFraction != 0) {
          if ((EffectiveFraction & 0x00400000U) == 0)
            Raised |= 1U << 0;
          Converted = static_cast<uint16_t>(
              ((Bits >> 16) & 0x8000U) | 0x7c00U |
              ((EffectiveFraction >> 13) & 0x03ffU) | 0x0200U);
        } else {
          llvm::APFloat Value(llvm::APFloat::IEEEsingle(),
                              llvm::APInt(32, Bits));
          bool LosesInfo = false;
          const llvm::APFloat::opStatus Status =
              Value.convert(llvm::APFloat::IEEEhalf(), Rounding, &LosesInfo);
          Raised |= statusFlags(Status);
          Converted =
              static_cast<uint16_t>(Value.bitcastToAPInt().getZExtValue());
          const bool NonzeroFinite =
              (Bits & 0x7fffffffU) != 0 && EffectiveExponent != 0x7f800000U;
          if (NonzeroFinite && (Converted & 0x7c00U) == 0)
            Raised |= 1U << 4;
        }
        writeWord(Result, Element * 2, Converted);
      }
    }

    if (!SuppressExceptions) {
      MXCSR |= Raised;
      const uint32_t Unmasked = Raised & ~(MXCSR >> 7) & 0x3fU;
      if (Unmasked != 0)
        return false;
    }
    writeOutputBytes(Op.Output, Result);
    return true;
  }

  if (Id == Intrinsic::EVEXCompressStore || Id == Intrinsic::EVEXExpandLoad) {
    const bool Expand = Id == Intrinsic::EVEXExpandLoad;
    if (Op.NumInputs != 5 || !Op.Inputs[4].isConst() ||
        Op.Inputs[1].Size != 8 || Op.Inputs[2].Size == 0 ||
        Op.Inputs[2].Size > 8 ||
        (Expand ? Op.Inputs[3].Size != Op.Output.Size : Op.Output.Size != 0))
      return false;
    const uint8_t Control = static_cast<uint8_t>(Op.Inputs[4].Offset);
    const unsigned ElementBytes = Control & 0x0f;
    if ((ElementBytes != 1 && ElementBytes != 2 && ElementBytes != 4 &&
         ElementBytes != 8) ||
        (!Expand && (Control & 0x80) != 0))
      return false;
    const uint16_t VectorBytes = Expand ? Op.Output.Size : Op.Inputs[3].Size;
    if ((VectorBytes != 16 && VectorBytes != 32 && VectorBytes != 64) ||
        VectorBytes % ElementBytes != 0)
      return false;
    const auto Base = resolveMemoryAddress(Op, readOperand(Op.Inputs[1]));
    if (!Base)
      return false;
    const uint64_t Mask = readOperand(Op.Inputs[2]);
    const unsigned LaneCount = VectorBytes / ElementBytes;
    uint64_t Cursor = *Base;

    if (Expand) {
      std::vector<uint8_t> Result = readOperandBytes(Op.Inputs[3]);
      if (Result.size() != VectorBytes)
        return false;
      if ((Control & 0x80) != 0)
        std::fill(Result.begin(), Result.end(), 0);
      std::vector<LoadRecord> Records;
      for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
        if (((Mask >> Lane) & 1U) == 0)
          continue;
        auto Value = loadMemory(Cursor, ElementBytes);
        if (!Value)
          return false;
        std::memcpy(Result.data() + Lane * ElementBytes, &*Value, ElementBytes);
        Records.push_back({Cursor, static_cast<uint16_t>(ElementBytes)});
        Cursor += ElementBytes;
      }
      if (CollectLoads)
        for (const LoadRecord &Record : Records)
          if (static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
            LoadLog.push_back(Record);
      writeOutputBytes(Op.Output, Result);
      return true;
    }

    const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[3]);
    if (Source.size() != VectorBytes)
      return false;
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      if (((Mask >> Lane) & 1U) == 0)
        continue;
      uint64_t Value = 0;
      std::memcpy(&Value, Source.data() + Lane * ElementBytes, ElementBytes);
      if (!storeMemory(Cursor, ElementBytes, Value))
        return false;
      Cursor += ElementBytes;
    }
    return true;
  }

  if (Id == Intrinsic::X86FPClass) {
    if (Op.NumInputs != 5 || !Op.Inputs[1].isConst() ||
        Op.Inputs[1].Size != 1 || !Op.Inputs[4].isConst() ||
        Op.Inputs[4].Size != 1 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Op.Inputs[2].Size != 16 && Op.Inputs[2].Size != 32 &&
         Op.Inputs[2].Size != 64) ||
        Op.Inputs[3].Size == 0 || Op.Inputs[3].Size > sizeof(uint64_t))
      return false;

    const uint8_t Control = static_cast<uint8_t>(readOperand(Op.Inputs[1]));
    if ((Control & ~UINT8_C(0x03)) != 0)
      return false;
    const bool F64 = (Control & 1) != 0;
    const bool Scalar = (Control & 2) != 0;
    const size_t ElementSize = F64 ? 8 : 4;
    if (Scalar && Op.Inputs[2].Size != 16)
      return false;
    const size_t LaneCount = Scalar ? 1 : Op.Inputs[2].Size / ElementSize;
    const size_t ResultSize = std::max<size_t>(1, (LaneCount + 7) / 8);
    if (Op.Output.Size != ResultSize || LaneCount > Op.Inputs[3].Size * 8)
      return false;

    const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[2]);
    const std::vector<uint8_t> Mask = readOperandBytes(Op.Inputs[3]);
    if (Source.size() != Op.Inputs[2].Size || Mask.size() != Op.Inputs[3].Size)
      return false;
    const uint8_t Immediate = static_cast<uint8_t>(readOperand(Op.Inputs[4]));
    const bool Daz = (MXCSR & (1U << 6)) != 0;
    std::vector<uint8_t> Result(ResultSize, 0);

    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      if (((Mask[Lane / 8] >> (Lane % 8)) & 1U) == 0)
        continue;

      uint64_t Bits = 0;
      for (size_t Byte = 0; Byte < ElementSize; ++Byte)
        Bits |= static_cast<uint64_t>(Source[Lane * ElementSize + Byte])
                << (Byte * 8);
      const unsigned FractionBits = F64 ? 52 : 23;
      const uint64_t FractionMask =
          F64 ? UINT64_C(0x000fffffffffffff) : UINT64_C(0x007fffff);
      const uint64_t ExponentMask =
          F64 ? UINT64_C(0x7ff0000000000000) : UINT64_C(0x7f800000);
      const uint64_t SignMask =
          F64 ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000);
      const bool Sign = (Bits & SignMask) != 0;
      const uint64_t Exponent = Bits & ExponentMask;
      uint64_t Fraction = Bits & FractionMask;
      if (Daz && Exponent == 0 && Fraction != 0)
        Fraction = 0;

      const bool ExponentAllOnes = Exponent == ExponentMask;
      const bool Zero = Exponent == 0 && Fraction == 0;
      const bool Denormal = Exponent == 0 && Fraction != 0;
      const bool Infinity = ExponentAllOnes && Fraction == 0;
      const bool NaN = ExponentAllOnes && Fraction != 0;
      const bool QuietNaN =
          NaN && (Fraction & (UINT64_C(1) << (FractionBits - 1))) != 0;
      const bool SignalingNaN = NaN && !QuietNaN;
      const bool NegativeFinite = Sign && !ExponentAllOnes && !Zero;
      const bool Selected = (QuietNaN && (Immediate & 0x01)) ||
                            (!Sign && Zero && (Immediate & 0x02)) ||
                            (Sign && Zero && (Immediate & 0x04)) ||
                            (!Sign && Infinity && (Immediate & 0x08)) ||
                            (Sign && Infinity && (Immediate & 0x10)) ||
                            (Denormal && (Immediate & 0x20)) ||
                            (NegativeFinite && (Immediate & 0x40)) ||
                            (SignalingNaN && (Immediate & 0x80));
      if (Selected)
        Result[Lane / 8] |= static_cast<uint8_t>(1U << (Lane % 8));
    }

    writeOutputBytes(Op.Output, Result);
    return true;
  }

  if (Id == Intrinsic::X86ApproxFloat) {
    if (Op.NumInputs != 6 || !Op.Inputs[1].isConst() ||
        Op.Inputs[1].Size != 1 ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        (Op.Output.Size != 16 && Op.Output.Size != 32 &&
         Op.Output.Size != 64) ||
        Op.Inputs[2].Size != Op.Output.Size ||
        Op.Inputs[3].Size != Op.Output.Size ||
        Op.Inputs[4].Size != Op.Output.Size || Op.Inputs[5].Size == 0 ||
        Op.Inputs[5].Size > sizeof(uint64_t))
      return false;

    const uint8_t Control = static_cast<uint8_t>(readOperand(Op.Inputs[1]));
    if ((Control & UINT8_C(0x80)) != 0)
      return false;
    const uint64_t RawKind = Control & UINT8_C(0x0f);
    if (RawKind > static_cast<uint64_t>(X86ApproxFloatKind::Exp2F64))
      return false;
    const auto Kind = static_cast<X86ApproxFloatKind>(RawKind);
    const bool IsF64 = Kind == X86ApproxFloatKind::Rcp14F64 ||
                       Kind == X86ApproxFloatKind::Rsqrt14F64 ||
                       Kind == X86ApproxFloatKind::Rcp28F64 ||
                       Kind == X86ApproxFloatKind::Rsqrt28F64 ||
                       Kind == X86ApproxFloatKind::Exp2F64;
    const bool IsReciprocal = Kind == X86ApproxFloatKind::Rcp14F32 ||
                              Kind == X86ApproxFloatKind::Rcp14F64 ||
                              Kind == X86ApproxFloatKind::Rcp28F32 ||
                              Kind == X86ApproxFloatKind::Rcp28F64;
    const bool IsReciprocalSqrt = Kind == X86ApproxFloatKind::Rsqrt14F32 ||
                                  Kind == X86ApproxFloatKind::Rsqrt14F64 ||
                                  Kind == X86ApproxFloatKind::Rsqrt28F32 ||
                                  Kind == X86ApproxFloatKind::Rsqrt28F64;
    const bool IsExp2 = Kind == X86ApproxFloatKind::Exp2F32 ||
                        Kind == X86ApproxFloatKind::Exp2F64;
    const bool Is14 = Kind == X86ApproxFloatKind::Rcp14F32 ||
                      Kind == X86ApproxFloatKind::Rcp14F64 ||
                      Kind == X86ApproxFloatKind::Rsqrt14F32 ||
                      Kind == X86ApproxFloatKind::Rsqrt14F64;
    if (!IsReciprocal && !IsReciprocalSqrt && !IsExp2)
      return false;

    const bool ZeroInactive = (Control & 0x10) != 0;
    const bool Scalar = (Control & 0x20) != 0;
    const bool SuppressExceptions = (Control & 0x40) != 0;
    if ((Scalar && Op.Output.Size != 16) || (Is14 && SuppressExceptions) ||
        (IsExp2 && Scalar))
      return false;

    const size_t ElementSize = IsF64 ? 8 : 4;
    const size_t LaneCount = Scalar ? 1 : Op.Output.Size / ElementSize;
    if (LaneCount > Op.Inputs[5].Size * 8)
      return false;
    const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[2]);
    const std::vector<uint8_t> OldDestination = readOperandBytes(Op.Inputs[3]);
    const std::vector<uint8_t> PassThrough = readOperandBytes(Op.Inputs[4]);
    const std::vector<uint8_t> Mask = readOperandBytes(Op.Inputs[5]);
    std::vector<uint8_t> Result = Scalar ? PassThrough : OldDestination;
    uint32_t Raised = 0;

    auto readElement = [](llvm::ArrayRef<uint8_t> Bytes, size_t Offset,
                          size_t Size) {
      uint64_t Value = 0;
      for (size_t Byte = 0; Byte < Size; ++Byte)
        Value |= static_cast<uint64_t>(Bytes[Offset + Byte]) << (Byte * 8);
      return Value;
    };
    auto writeElement = [](std::vector<uint8_t> &Bytes, size_t Offset,
                           size_t Size, uint64_t Value) {
      for (size_t Byte = 0; Byte < Size; ++Byte)
        Bytes[Offset + Byte] = static_cast<uint8_t>(Value >> (Byte * 8));
    };

    for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
      const size_t Offset = Lane * ElementSize;
      const bool Active = ((Mask[Lane / 8] >> (Lane % 8)) & 1U) != 0;
      if (!Active) {
        if (ZeroInactive) {
          std::fill_n(Result.begin() + Offset, ElementSize, 0);
        } else if (Scalar) {
          std::copy_n(OldDestination.begin() + Offset, ElementSize,
                      Result.begin() + Offset);
        }
        continue;
      }

      const uint64_t Input = readElement(Source, Offset, ElementSize);
      uint64_t Output = 0;
      uint32_t LaneFlags = 0;
      if (Is14 && IsF64) {
        Output = IsReciprocal ? neverd_x86_rcp14_f64(MXCSR, Input)
                              : neverd_x86_rsqrt14_f64(MXCSR, Input);
      } else if (Is14) {
        Output =
            IsReciprocal
                ? neverd_x86_rcp14_f32(MXCSR, static_cast<uint32_t>(Input))
                : neverd_x86_rsqrt14_f32(MXCSR, static_cast<uint32_t>(Input));
      } else if (IsF64 && IsReciprocal) {
        Output = neverd_x86_rcp28_f64(Input, &LaneFlags);
      } else if (IsF64 && IsReciprocalSqrt) {
        Output = neverd_x86_rsqrt28_f64(Input, &LaneFlags);
      } else if (IsF64) {
        Output = neverd_x86_exp2_f64(Input, &LaneFlags);
      } else if (IsReciprocal) {
        Output = neverd_x86_rcp28_f32(static_cast<uint32_t>(Input), &LaneFlags);
      } else if (IsReciprocalSqrt) {
        Output =
            neverd_x86_rsqrt28_f32(static_cast<uint32_t>(Input), &LaneFlags);
      } else {
        Output = neverd_x86_exp2_f32(static_cast<uint32_t>(Input), &LaneFlags);
      }
      Raised |= LaneFlags;
      writeElement(Result, Offset, ElementSize, Output);
    }

    if (!SuppressExceptions) {
      MXCSR |= Raised;
      const uint32_t Unmasked = Raised & ~(MXCSR >> 7) & 0x3fU;
      if (Unmasked != 0)
        return false;
    }
    writeOutputBytes(Op.Output, Result);
    return true;
  }

  const bool IsAMXMemory =
      Id == Intrinsic::AMXLoadConfig || Id == Intrinsic::AMXStoreConfig ||
      Id == Intrinsic::AMXTileLoad || Id == Intrinsic::AMXTileStore;
  const bool IsAMX = IsAMXMemory || Id == Intrinsic::AMXTileZero ||
                     Id == Intrinsic::AMXClearStartRow ||
                     Id == Intrinsic::AMXTileCompute ||
                     Id == Intrinsic::AMXTileRow;
  if (IsAMX) {
    if (IsAMXMemory && !intrinsicMemoryAddressSpaceShapeIsValid(
                           Id, Op.NumInputs, Op.Output.Size,
                           Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
                           Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
                           Op.NumInputs > 3 ? Op.Inputs[3].Size : 0))
      return false;

    auto validateConfig = [](std::vector<uint8_t> &Config) {
      if (Config.size() != x86reg::TileConfigSize)
        return false;
      if (Config[0] == 0) {
        std::fill(Config.begin(), Config.end(), 0);
        return true;
      }
      if (Config[0] != 1)
        return false;
      auto AllZero = [&](size_t Begin, size_t End) {
        return std::all_of(Config.begin() + Begin, Config.begin() + End,
                           [](uint8_t Byte) { return Byte == 0; });
      };
      if (!AllZero(2, 16) || !AllZero(32, 48) || !AllZero(56, 64))
        return false;
      for (unsigned Tile = 0; Tile < x86reg::TileRegCount; ++Tile) {
        const uint16_t ColumnBytes =
            static_cast<uint16_t>(Config[16 + Tile * 2]) |
            (static_cast<uint16_t>(Config[17 + Tile * 2]) << 8);
        const uint8_t Rows = Config[48 + Tile];
        if (ColumnBytes > 64 || Rows > 16 ||
            ((ColumnBytes == 0) != (Rows == 0)))
          return false;
      }
      return true;
    };

    auto tileIndex = [](const NdVar &Tile) -> std::optional<unsigned> {
      if (!Tile.isReg() || Tile.Size != x86reg::TileRegStride ||
          Tile.Offset < x86reg::TileBase ||
          Tile.Offset >= x86reg::tileReg(x86reg::TileRegCount) ||
          (Tile.Offset - x86reg::TileBase) % x86reg::TileRegStride != 0)
        return std::nullopt;
      return static_cast<unsigned>((Tile.Offset - x86reg::TileBase) /
                                   x86reg::TileRegStride);
    };

    auto addressSize = [&](unsigned Input) -> std::optional<unsigned> {
      if (Input >= Op.NumInputs || !Op.Inputs[Input].isConst() ||
          Op.Inputs[Input].Size != 1)
        return std::nullopt;
      const uint64_t Size = readOperand(Op.Inputs[Input]);
      if (Size != 4 && Size != 8)
        return std::nullopt;
      return static_cast<unsigned>(Size);
    };

    auto memoryAddress = [&](uint64_t Base, uint64_t Delta,
                             unsigned AddressSize) -> std::optional<uint64_t> {
      uint64_t Offset = Base + Delta;
      if (AddressSize == 4)
        Offset = static_cast<uint32_t>(Offset);
      return resolveMemoryAddress(Op, Offset);
    };

    auto loadBytes = [&](uint64_t Base, uint64_t Delta, unsigned AddressSize,
                         size_t Size, std::vector<uint8_t> &Bytes) -> bool {
      Bytes.assign(Size, 0);
      for (size_t I = 0; I < Size; ++I) {
        const auto Address = memoryAddress(Base, Delta + I, AddressSize);
        if (!Address)
          return false;
        const auto Byte = loadMemory(*Address, 1);
        if (!Byte)
          return false;
        Bytes[I] = static_cast<uint8_t>(*Byte);
        if (CollectLoads &&
            static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
          LoadLog.push_back({*Address, 1});
      }
      return true;
    };

    auto storeBytesAtomically = [&](uint64_t Base, uint64_t Delta,
                                    unsigned AddressSize,
                                    llvm::ArrayRef<uint8_t> Bytes) -> bool {
      std::map<uint64_t, uint64_t> UpdatedStarts = MemStore;
      std::map<uint64_t, uint8_t> UpdatedBytes = MemStoreBytes;
      std::optional<uint64_t> StoreStart;
      for (size_t I = 0; I < Bytes.size(); ++I) {
        const auto Address = memoryAddress(Base, Delta + I, AddressSize);
        if (!Address)
          return false;
        if (!StoreStart)
          StoreStart = *Address;
        const Segment *Mapped = Img.getSegmentFor(*Address);
        if (!Mapped || !Mapped->isWritable() || *Address < Mapped->VA ||
            *Address - Mapped->VA >= Mapped->Size)
          return false;
        UpdatedBytes[*Address] = Bytes[I];
      }
      if (!StoreStart)
        return true;
      UpdatedStarts[*StoreStart] = 0;
      if (static_cast<int>(UpdatedStarts.size()) >
          limits::kMaxEmulatorStoreEntries) {
        ++Skips.DroppedStores;
        return false;
      }
      MemStore = std::move(UpdatedStarts);
      MemStoreBytes = std::move(UpdatedBytes);
      return true;
    };

    auto tileShape = [&](const std::vector<uint8_t> &Config, unsigned Tile,
                         bool IsMemoryTransfer, uint16_t &ColumnBytes,
                         uint8_t &Rows) {
      if (Config.size() != x86reg::TileConfigSize || Config[0] != 1 ||
          Tile >= x86reg::TileRegCount)
        return false;
      ColumnBytes = static_cast<uint16_t>(Config[16 + Tile * 2]) |
                    (static_cast<uint16_t>(Config[17 + Tile * 2]) << 8);
      Rows = Config[48 + Tile];
      if (ColumnBytes == 0 || Rows == 0)
        return false;
      if (IsMemoryTransfer && ((ColumnBytes % 4) != 0 || Config[1] >= Rows))
        return false;
      return true;
    };

    if (Id == Intrinsic::AMXLoadConfig) {
      const auto AddressBytes = addressSize(2);
      if (!AddressBytes)
        return false;
      std::vector<uint8_t> Config;
      if (!loadBytes(readOperand(Op.Inputs[1]), 0, *AddressBytes,
                     x86reg::TileConfigSize, Config) ||
          !validateConfig(Config))
        return false;
      writeOutputBytes(Op.Output, Config);
      return true;
    }

    if (Id == Intrinsic::AMXStoreConfig) {
      const auto AddressBytes = addressSize(3);
      if (!AddressBytes)
        return false;
      std::vector<uint8_t> Config = readOperandBytes(Op.Inputs[2]);
      if (!validateConfig(Config))
        return false;
      return storeBytesAtomically(readOperand(Op.Inputs[1]), 0, *AddressBytes,
                                  Config);
    }

    if (Id == Intrinsic::AMXClearStartRow) {
      if (Op.NumInputs != 2 || Op.Output.Size != x86reg::TileConfigSize ||
          Op.Inputs[1].Size != x86reg::TileConfigSize ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      std::vector<uint8_t> Config = readOperandBytes(Op.Inputs[1]);
      if (!validateConfig(Config) || Config[0] != 1)
        return false;
      Config[1] = 0;
      writeOutputBytes(Op.Output, Config);
      return true;
    }

    if (Id == Intrinsic::AMXTileZero) {
      if (Op.NumInputs != 2 || Op.Inputs[1].Size != x86reg::TileConfigSize ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      const auto Tile = tileIndex(Op.Output);
      if (!Tile)
        return false;
      std::vector<uint8_t> Config = readOperandBytes(Op.Inputs[1]);
      uint16_t ColumnBytes = 0;
      uint8_t Rows = 0;
      if (!validateConfig(Config) ||
          !tileShape(Config, *Tile, false, ColumnBytes, Rows))
        return false;
      writeOutputBytes(Op.Output,
                       std::vector<uint8_t>(x86reg::TileRegStride, 0));
      return true;
    }

    if (Id == Intrinsic::AMXTileRow) {
      if (Op.NumInputs != 5 || !Op.Output.isReg() ||
          Op.Output.Size != x86reg::VectorRegStride ||
          Op.Output.Offset < x86reg::VectorBase ||
          Op.Output.Offset >= x86reg::vectorReg(x86reg::VectorRegCount) ||
          (Op.Output.Offset - x86reg::VectorBase) % x86reg::VectorRegStride !=
              0 ||
          !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 1 ||
          Op.Inputs[2].Size != x86reg::TileConfigSize ||
          Op.Inputs[3].Size != x86reg::TileRegStride ||
          Op.Inputs[4].Size != 4 ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      const uint64_t RawKind = readOperand(Op.Inputs[1]);
      if (RawKind > static_cast<uint64_t>(AMXTileRowKind::FP32ToFP16Low))
        return false;
      const auto SourceIndex = tileIndex(Op.Inputs[3]);
      if (!SourceIndex)
        return false;
      std::vector<uint8_t> Config = readOperandBytes(Op.Inputs[2]);
      uint16_t ColumnBytes = 0;
      uint8_t Rows = 0;
      if (!validateConfig(Config) ||
          !tileShape(Config, *SourceIndex, false, ColumnBytes, Rows) ||
          ColumnBytes % 4 != 0)
        return false;

      const uint32_t Selector =
          static_cast<uint32_t>(readOperand(Op.Inputs[4]));
      const unsigned Row = Selector & 0xffff;
      const unsigned Chunk = Selector >> 16;
      const size_t RowOffset = static_cast<size_t>(Chunk) * 64;
      if (Row >= Rows || RowOffset >= ColumnBytes)
        return false;

      const auto Kind = static_cast<AMXTileRowKind>(RawKind);
      const std::vector<uint8_t> Source = readOperandBytes(Op.Inputs[3]);
      std::vector<uint8_t> Result(x86reg::VectorRegStride, 0);
      const size_t SourceRow = static_cast<size_t>(Row) * 64 + RowOffset;
      if (Kind == AMXTileRowKind::Move) {
        const size_t Bytes =
            std::min<size_t>(x86reg::VectorRegStride,
                             static_cast<size_t>(ColumnBytes) - RowOffset);
        std::copy_n(Source.begin() + SourceRow, Bytes, Result.begin());
        writeOutputBytes(Op.Output, Result);
        return true;
      }

      constexpr llvm::APFloat::roundingMode Rounding =
          llvm::APFloat::rmNearestTiesToEven;
      auto readDword = [](llvm::ArrayRef<uint8_t> Bytes, size_t Offset) {
        return static_cast<uint32_t>(Bytes[Offset]) |
               (static_cast<uint32_t>(Bytes[Offset + 1]) << 8) |
               (static_cast<uint32_t>(Bytes[Offset + 2]) << 16) |
               (static_cast<uint32_t>(Bytes[Offset + 3]) << 24);
      };
      auto writeDword = [](std::vector<uint8_t> &Bytes, size_t Offset,
                           uint32_t Value) {
        for (unsigned Byte = 0; Byte < 4; ++Byte)
          Bytes[Offset + Byte] = static_cast<uint8_t>(Value >> (Byte * 8));
      };
      auto float32ToBF16 = [](uint32_t Value) -> uint16_t {
        const uint32_t Exponent = Value & 0x7f800000U;
        const uint32_t Fraction = Value & 0x007fffffU;
        if (Exponent == 0)
          return static_cast<uint16_t>((Value >> 16) & 0x8000U);
        if (Exponent == 0x7f800000U) {
          uint16_t Result = static_cast<uint16_t>(Value >> 16);
          if (Fraction != 0)
            Result |= 0x0040U;
          return Result;
        }
        Value += 0x00007fffU + ((Value >> 16) & 1U);
        return static_cast<uint16_t>(Value >> 16);
      };
      auto float32ToFP16 = [&](uint32_t Bits) -> uint16_t {
        if ((Bits & 0x7f800000U) == 0)
          return static_cast<uint16_t>((Bits >> 16) & 0x8000U);
        llvm::APFloat Value(llvm::APFloat::IEEEsingle(), llvm::APInt(32, Bits));
        bool LosesInfo = false;
        Value.convert(llvm::APFloat::IEEEhalf(), Rounding, &LosesInfo);
        return static_cast<uint16_t>(Value.bitcastToAPInt().getZExtValue());
      };

      for (unsigned Element = 0; Element < 16; ++Element) {
        const size_t Byte = RowOffset + Element * 4;
        if (Byte >= ColumnBytes)
          continue;
        const uint32_t Value =
            readDword(Source, static_cast<size_t>(Row) * 64 + Byte);
        uint32_t Converted = 0;
        if (Kind == AMXTileRowKind::Int32ToFP32) {
          llvm::APFloat Float(llvm::APFloat::IEEEsingle());
          Float.convertFromAPInt(llvm::APInt(32, Value), true, Rounding);
          Converted =
              static_cast<uint32_t>(Float.bitcastToAPInt().getZExtValue());
        } else {
          const bool IsBF16 = Kind == AMXTileRowKind::FP32ToBF16High ||
                              Kind == AMXTileRowKind::FP32ToBF16Low;
          const bool High = Kind == AMXTileRowKind::FP32ToBF16High ||
                            Kind == AMXTileRowKind::FP32ToFP16High;
          const uint16_t Narrow =
              IsBF16 ? float32ToBF16(Value) : float32ToFP16(Value);
          Converted = High ? static_cast<uint32_t>(Narrow) << 16 : Narrow;
        }
        writeDword(Result, Element * 4, Converted);
      }
      writeOutputBytes(Op.Output, Result);
      return true;
    }

    if (Id == Intrinsic::AMXTileCompute) {
      if (Op.NumInputs != 6 || Op.Output.Size != x86reg::TileRegStride ||
          !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 1 ||
          Op.Inputs[2].Size != x86reg::TileConfigSize ||
          Op.Inputs[3].Size != x86reg::TileRegStride ||
          Op.Inputs[4].Size != x86reg::TileRegStride ||
          Op.Inputs[5].Size != x86reg::TileRegStride ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return false;
      const auto Destination = tileIndex(Op.Output);
      const auto DestinationInput = tileIndex(Op.Inputs[3]);
      const auto Source1 = tileIndex(Op.Inputs[4]);
      const auto Source2 = tileIndex(Op.Inputs[5]);
      if (!Destination || !DestinationInput || !Source1 || !Source2 ||
          *Destination != *DestinationInput || *Destination == *Source1 ||
          *Destination == *Source2 || *Source1 == *Source2)
        return false;

      const uint64_t RawKind = readOperand(Op.Inputs[1]);
      if (RawKind > static_cast<uint64_t>(AMXTileComputeKind::TF32))
        return false;
      const auto Kind = static_cast<AMXTileComputeKind>(RawKind);
      std::vector<uint8_t> Config = readOperandBytes(Op.Inputs[2]);
      uint16_t DestinationColumns = 0;
      uint16_t Source1Columns = 0;
      uint16_t Source2Columns = 0;
      uint8_t DestinationRows = 0;
      uint8_t Source1Rows = 0;
      uint8_t Source2Rows = 0;
      if (!validateConfig(Config) ||
          !tileShape(Config, *Destination, false, DestinationColumns,
                     DestinationRows) ||
          !tileShape(Config, *Source1, false, Source1Columns, Source1Rows) ||
          !tileShape(Config, *Source2, false, Source2Columns, Source2Rows) ||
          (DestinationColumns % 4) != 0 || (Source1Columns % 4) != 0 ||
          (Source2Columns % 4) != 0 || DestinationColumns != Source2Columns ||
          DestinationRows != Source1Rows || Source1Columns / 4 != Source2Rows)
        return false;

      const std::vector<uint8_t> OldDestination =
          readOperandBytes(Op.Inputs[3]);
      const std::vector<uint8_t> Left = readOperandBytes(Op.Inputs[4]);
      const std::vector<uint8_t> Right = readOperandBytes(Op.Inputs[5]);
      if (OldDestination.size() != x86reg::TileRegStride ||
          Left.size() != x86reg::TileRegStride ||
          Right.size() != x86reg::TileRegStride)
        return false;

      auto readDword = [](llvm::ArrayRef<uint8_t> Bytes,
                          size_t Offset) -> uint32_t {
        return static_cast<uint32_t>(Bytes[Offset]) |
               (static_cast<uint32_t>(Bytes[Offset + 1]) << 8) |
               (static_cast<uint32_t>(Bytes[Offset + 2]) << 16) |
               (static_cast<uint32_t>(Bytes[Offset + 3]) << 24);
      };
      auto writeDword = [](std::vector<uint8_t> &Bytes, size_t Offset,
                           uint32_t Value) {
        for (unsigned Byte = 0; Byte < 4; ++Byte)
          Bytes[Offset + Byte] = static_cast<uint8_t>(Value >> (Byte * 8));
      };
      const unsigned KElements = Source1Columns / 4;
      const unsigned NElements = DestinationColumns / 4;
      std::vector<uint8_t> Result(x86reg::TileRegStride, 0);
      if (RawKind <=
          static_cast<uint64_t>(AMXTileComputeKind::Int8UnsignedUnsigned)) {
        const bool LeftSigned = Kind == AMXTileComputeKind::Int8SignedSigned ||
                                Kind == AMXTileComputeKind::Int8SignedUnsigned;
        const bool RightSigned = Kind == AMXTileComputeKind::Int8SignedSigned ||
                                 Kind == AMXTileComputeKind::Int8UnsignedSigned;
        auto signedByte = [](uint8_t Byte) -> int32_t {
          return Byte < 0x80 ? Byte : static_cast<int32_t>(Byte) - 0x100;
        };
        for (unsigned Row = 0; Row < DestinationRows; ++Row) {
          for (unsigned Column = 0; Column < NElements; ++Column) {
            const size_t DestinationOffset =
                static_cast<size_t>(Row) * 64 + Column * 4;
            uint32_t Accumulator = readDword(OldDestination, DestinationOffset);
            for (unsigned K = 0; K < KElements; ++K) {
              const size_t LeftOffset = static_cast<size_t>(Row) * 64 + K * 4;
              const size_t RightOffset =
                  static_cast<size_t>(K) * 64 + Column * 4;
              for (unsigned Element = 0; Element < 4; ++Element) {
                const int32_t LeftValue =
                    LeftSigned ? signedByte(Left[LeftOffset + Element])
                               : Left[LeftOffset + Element];
                const int32_t RightValue =
                    RightSigned ? signedByte(Right[RightOffset + Element])
                                : Right[RightOffset + Element];
                Accumulator += static_cast<uint32_t>(LeftValue * RightValue);
              }
            }
            writeDword(Result, DestinationOffset, Accumulator);
          }
        }
        writeOutputBytes(Op.Output, Result);
        return true;
      }

      constexpr llvm::APFloat::roundingMode Rounding =
          llvm::APFloat::rmNearestTiesToEven;
      auto singleFromBits = [](uint32_t Bits) {
        return llvm::APFloat(llvm::APFloat::IEEEsingle(),
                             llvm::APInt(32, Bits));
      };
      auto singleBits = [](const llvm::APFloat &Value) {
        return static_cast<uint32_t>(Value.bitcastToAPInt().getZExtValue());
      };
      auto flushDenormal = [](llvm::APFloat Value) {
        if (Value.isDenormal())
          return llvm::APFloat::getZero(llvm::APFloat::IEEEsingle(),
                                        Value.isNegative());
        return Value;
      };
      auto fmaSingle = [&](llvm::APFloat LeftValue, llvm::APFloat RightValue,
                           llvm::APFloat Accumulator, bool Daz) {
        if (Daz) {
          LeftValue = flushDenormal(std::move(LeftValue));
          RightValue = flushDenormal(std::move(RightValue));
          Accumulator = flushDenormal(std::move(Accumulator));
        }
        LeftValue.fusedMultiplyAdd(RightValue, Accumulator, Rounding);
        return flushDenormal(std::move(LeftValue));
      };
      auto addSingle = [&](llvm::APFloat LeftValue, llvm::APFloat RightValue,
                           bool Daz) {
        if (Daz) {
          LeftValue = flushDenormal(std::move(LeftValue));
          RightValue = flushDenormal(std::move(RightValue));
        }
        LeftValue.add(RightValue, Rounding);
        return flushDenormal(std::move(LeftValue));
      };
      auto readWord = [](llvm::ArrayRef<uint8_t> Bytes, size_t Offset) {
        return static_cast<uint16_t>(Bytes[Offset]) |
               (static_cast<uint16_t>(Bytes[Offset + 1]) << 8);
      };
      auto halfToSingle = [&](uint16_t Bits) {
        llvm::APFloat Value(llvm::APFloat::IEEEhalf(), llvm::APInt(16, Bits));
        bool LosesInfo = false;
        Value.convert(llvm::APFloat::IEEEsingle(), Rounding, &LosesInfo);
        return Value;
      };
      auto bfloatToSingle = [&](uint16_t Bits) {
        return singleFromBits(static_cast<uint32_t>(Bits) << 16);
      };

      const bool IsPairFloat =
          Kind == AMXTileComputeKind::BF16 ||
          Kind == AMXTileComputeKind::FP16 ||
          Kind == AMXTileComputeKind::ComplexFP16Imaginary ||
          Kind == AMXTileComputeKind::ComplexFP16Real;
      if (IsPairFloat) {
        const bool IsBFloat = Kind == AMXTileComputeKind::BF16;
        const bool IsComplexImaginary =
            Kind == AMXTileComputeKind::ComplexFP16Imaginary;
        const bool IsComplexReal = Kind == AMXTileComputeKind::ComplexFP16Real;
        for (unsigned Row = 0; Row < DestinationRows; ++Row) {
          for (unsigned Column = 0; Column < NElements; ++Column) {
            llvm::APFloat Even =
                llvm::APFloat::getZero(llvm::APFloat::IEEEsingle());
            llvm::APFloat Odd =
                llvm::APFloat::getZero(llvm::APFloat::IEEEsingle());
            for (unsigned K = 0; K < KElements; ++K) {
              const size_t LeftOffset = static_cast<size_t>(Row) * 64 + K * 4;
              const size_t RightOffset =
                  static_cast<size_t>(K) * 64 + Column * 4;
              uint16_t Left0Bits = readWord(Left, LeftOffset);
              uint16_t Left1Bits = readWord(Left, LeftOffset + 2);
              const uint16_t Right0Bits = readWord(Right, RightOffset);
              const uint16_t Right1Bits = readWord(Right, RightOffset + 2);
              if (IsComplexReal)
                Left1Bits ^= 0x8000;
              llvm::APFloat Left0 = IsBFloat ? bfloatToSingle(Left0Bits)
                                             : halfToSingle(Left0Bits);
              llvm::APFloat Left1 = IsBFloat ? bfloatToSingle(Left1Bits)
                                             : halfToSingle(Left1Bits);
              llvm::APFloat Right0 = IsBFloat ? bfloatToSingle(Right0Bits)
                                              : halfToSingle(Right0Bits);
              llvm::APFloat Right1 = IsBFloat ? bfloatToSingle(Right1Bits)
                                              : halfToSingle(Right1Bits);
              if (IsComplexImaginary) {
                Even = fmaSingle(std::move(Left1), std::move(Right0),
                                 std::move(Even), true);
                Odd = fmaSingle(std::move(Left0), std::move(Right1),
                                std::move(Odd), true);
              } else {
                Even = fmaSingle(std::move(Left0), std::move(Right0),
                                 std::move(Even), true);
                Odd = fmaSingle(std::move(Left1), std::move(Right1),
                                std::move(Odd), true);
              }
            }
            llvm::APFloat Dot =
                addSingle(std::move(Even), std::move(Odd), true);
            const size_t DestinationOffset =
                static_cast<size_t>(Row) * 64 + Column * 4;
            llvm::APFloat Accumulator =
                singleFromBits(readDword(OldDestination, DestinationOffset));
            llvm::APFloat Value =
                addSingle(std::move(Accumulator), std::move(Dot), true);
            writeDword(Result, DestinationOffset, singleBits(Value));
          }
        }
        writeOutputBytes(Op.Output, Result);
        return true;
      }

      if (Kind == AMXTileComputeKind::TF32) {
        auto tf32Value = [&](uint32_t Bits) {
          const uint32_t Exponent = Bits & 0x7f800000U;
          const uint32_t Fraction = Bits & 0x007fffffU;
          if (Exponent == 0x7f800000U && Fraction != 0 &&
              (Fraction & 0x00400000U) == 0)
            Bits |= 0x00400000U;
          return singleFromBits(Bits & 0xffffe000U);
        };
        auto tf32Fma = [&](llvm::APFloat LeftValue, llvm::APFloat RightValue,
                           llvm::APFloat Accumulator) {
          const bool HadNaN =
              LeftValue.isNaN() || RightValue.isNaN() || Accumulator.isNaN();
          llvm::APFloat Value =
              fmaSingle(std::move(LeftValue), std::move(RightValue),
                        std::move(Accumulator), false);
          if (Value.isNaN() && !HadNaN)
            return singleFromBits(0xffc00000U);
          return Value;
        };
        auto tf32Add = [&](llvm::APFloat LeftValue, llvm::APFloat RightValue) {
          const bool HadNaN = LeftValue.isNaN() || RightValue.isNaN();
          llvm::APFloat Value =
              addSingle(std::move(LeftValue), std::move(RightValue), false);
          if (Value.isNaN() && !HadNaN)
            return singleFromBits(0xffc00000U);
          return Value;
        };
        for (unsigned Row = 0; Row < DestinationRows; ++Row) {
          for (unsigned Column = 0; Column < NElements; ++Column) {
            llvm::APFloat Dot =
                llvm::APFloat::getZero(llvm::APFloat::IEEEsingle());
            for (unsigned K = 0; K < KElements; ++K) {
              const size_t LeftOffset = static_cast<size_t>(Row) * 64 + K * 4;
              const size_t RightOffset =
                  static_cast<size_t>(K) * 64 + Column * 4;
              Dot = tf32Fma(tf32Value(readDword(Left, LeftOffset)),
                            tf32Value(readDword(Right, RightOffset)),
                            std::move(Dot));
            }
            const size_t DestinationOffset =
                static_cast<size_t>(Row) * 64 + Column * 4;
            llvm::APFloat Accumulator =
                singleFromBits(readDword(OldDestination, DestinationOffset));
            llvm::APFloat Value =
                tf32Add(std::move(Accumulator), std::move(Dot));
            writeDword(Result, DestinationOffset, singleBits(Value));
          }
        }
        writeOutputBytes(Op.Output, Result);
        return true;
      }

      const bool LeftHF8 = Kind == AMXTileComputeKind::HF8BF8 ||
                           Kind == AMXTileComputeKind::HF8HF8;
      const bool RightHF8 = Kind == AMXTileComputeKind::BF8HF8 ||
                            Kind == AMXTileComputeKind::HF8HF8;
      auto fp8IsNaN = [](uint8_t Value, bool HF8) {
        if (HF8)
          return (Value & 0x7f) == 0x7f;
        return (Value & 0x7c) == 0x7c && (Value & 0x03) != 0;
      };
      auto fp8IsInfinity = [](uint8_t Value, bool HF8) {
        return !HF8 && (Value & 0x7f) == 0x7c;
      };
      auto fp8IsZero = [](uint8_t Value) { return (Value & 0x7f) == 0; };
      auto fp8ToFixed = [](uint8_t Value, bool HF8) -> int64_t {
        const bool Negative = (Value & 0x80) != 0;
        const unsigned Exponent = HF8 ? (Value >> 3) & 15 : (Value >> 2) & 31;
        const unsigned Fraction = HF8 ? Value & 7 : Value & 3;
        const uint64_t Mantissa =
            Exponent ? Fraction | (HF8 ? 8 : 4) : Fraction;
        const uint64_t Magnitude = Mantissa << (Exponent ? Exponent - 1 : 0);
        return Negative ? -static_cast<int64_t>(Magnitude)
                        : static_cast<int64_t>(Magnitude);
      };
      auto fixedToSingleBits = [](const llvm::APInt &Value, bool Source1HF8,
                                  bool Source2HF8) -> uint32_t {
        if (Value.isZero())
          return 0;
        const bool Negative = Value.isNegative();
        const unsigned Factor =
            Source1HF8 ? (Source2HF8 ? 18 : 25) : (Source2HF8 ? 25 : 32);
        const llvm::APInt Magnitude = Value.abs();
        unsigned Highest = Magnitude.logBase2();
        uint64_t Significand = 0;
        if (Highest <= 23) {
          Significand = Magnitude.shl(23 - Highest).getZExtValue();
        } else {
          const unsigned Shift = Highest - 23;
          const llvm::APInt Truncated = Magnitude.lshr(Shift);
          const llvm::APInt Remainder =
              Magnitude & llvm::APInt::getLowBitsSet(128, Shift);
          const llvm::APInt Halfway = llvm::APInt::getOneBitSet(128, Shift - 1);
          Significand = Truncated.getZExtValue();
          if (Remainder.ugt(Halfway) ||
              (Remainder == Halfway && (Significand & 1)))
            ++Significand;
          if (Significand == (UINT64_C(1) << 24)) {
            Significand >>= 1;
            ++Highest;
          }
        }
        const unsigned Exponent = 127 + Highest - Factor;
        return (Negative ? 0x80000000U : 0) | (Exponent << 23) |
               (static_cast<uint32_t>(Significand) & 0x007fffffU);
      };
      auto singleIsNaN = [](uint32_t Bits) {
        return (Bits & 0x7f800000U) == 0x7f800000U && (Bits & 0x007fffffU) != 0;
      };
      auto singleIsInfinity = [](uint32_t Bits) {
        return (Bits & 0x7fffffffU) == 0x7f800000U;
      };
      for (unsigned Row = 0; Row < DestinationRows; ++Row) {
        for (unsigned Column = 0; Column < NElements; ++Column) {
          const size_t DestinationOffset =
              static_cast<size_t>(Row) * 64 + Column * 4;
          const uint32_t AccumulatorBits =
              readDword(OldDestination, DestinationOffset);
          llvm::APInt Fixed(128, 0);
          int InfinitySign = 0;
          bool Indefinite = singleIsNaN(AccumulatorBits);
          for (unsigned K = 0; K < KElements && !Indefinite; ++K) {
            const size_t LeftOffset = static_cast<size_t>(Row) * 64 + K * 4;
            const size_t RightOffset = static_cast<size_t>(K) * 64 + Column * 4;
            for (unsigned Element = 0; Element < 4; ++Element) {
              const uint8_t LeftValue = Left[LeftOffset + Element];
              const uint8_t RightValue = Right[RightOffset + Element];
              const bool LeftInfinity = fp8IsInfinity(LeftValue, LeftHF8);
              const bool RightInfinity = fp8IsInfinity(RightValue, RightHF8);
              if (fp8IsNaN(LeftValue, LeftHF8) ||
                  fp8IsNaN(RightValue, RightHF8) ||
                  (LeftInfinity && fp8IsZero(RightValue)) ||
                  (RightInfinity && fp8IsZero(LeftValue))) {
                Indefinite = true;
                break;
              }
              if (LeftInfinity || RightInfinity) {
                const int ProductSign =
                    ((LeftValue ^ RightValue) & 0x80) != 0 ? -1 : 1;
                if (InfinitySign != 0 && InfinitySign != ProductSign) {
                  Indefinite = true;
                  break;
                }
                InfinitySign = ProductSign;
              } else {
                const int64_t LeftFixed = fp8ToFixed(LeftValue, LeftHF8);
                const int64_t RightFixed = fp8ToFixed(RightValue, RightHF8);
                Fixed +=
                    llvm::APInt(128, static_cast<uint64_t>(LeftFixed), true) *
                    llvm::APInt(128, static_cast<uint64_t>(RightFixed), true);
              }
            }
          }
          if (!Indefinite && InfinitySign != 0 &&
              singleIsInfinity(AccumulatorBits) &&
              (((AccumulatorBits >> 31) != 0) != (InfinitySign < 0)))
            Indefinite = true;

          uint32_t ValueBits = 0;
          if (Indefinite) {
            ValueBits = 0xffc00000U;
          } else if (InfinitySign != 0) {
            ValueBits = InfinitySign < 0 ? 0xff800000U : 0x7f800000U;
          } else if (singleIsInfinity(AccumulatorBits)) {
            ValueBits = AccumulatorBits;
          } else {
            const uint32_t DotBits =
                fixedToSingleBits(Fixed, LeftHF8, RightHF8);
            llvm::APFloat Value = addSingle(singleFromBits(AccumulatorBits),
                                            singleFromBits(DotBits), false);
            ValueBits = singleBits(Value);
          }
          writeDword(Result, DestinationOffset, ValueBits);
        }
      }
      writeOutputBytes(Op.Output, Result);
      return true;
    }

    if (Id == Intrinsic::AMXTileLoad) {
      const auto AddressBytes = addressSize(5);
      const auto Tile = tileIndex(Op.Output);
      if (!AddressBytes || !Tile ||
          Op.Inputs[3].Size != x86reg::TileConfigSize ||
          Op.Inputs[4].Size != x86reg::TileRegStride)
        return false;
      std::vector<uint8_t> Config = readOperandBytes(Op.Inputs[3]);
      uint16_t ColumnBytes = 0;
      uint8_t Rows = 0;
      if (!validateConfig(Config) ||
          !tileShape(Config, *Tile, true, ColumnBytes, Rows))
        return false;
      std::vector<uint8_t> TileData = readOperandBytes(Op.Inputs[4]);
      const uint8_t Start = Config[1];
      std::fill(TileData.begin() + static_cast<size_t>(Start) * 64,
                TileData.end(), 0);
      const uint64_t Base = readOperand(Op.Inputs[1]);
      const uint64_t Stride = readOperand(Op.Inputs[2]);
      for (uint8_t Row = Start; Row < Rows; ++Row) {
        std::vector<uint8_t> RowData;
        if (!loadBytes(Base, static_cast<uint64_t>(Row) * Stride, *AddressBytes,
                       ColumnBytes, RowData)) {
          Config[1] = Row;
          writeOutputBytes(Op.Output, TileData);
          writeOutputBytes(
              NdVar::reg(x86reg::TileConfig, x86reg::TileConfigSize), Config);
          return false;
        }
        std::copy(RowData.begin(), RowData.end(),
                  TileData.begin() + static_cast<size_t>(Row) * 64);
        std::fill(TileData.begin() + static_cast<size_t>(Row) * 64 +
                      ColumnBytes,
                  TileData.begin() + static_cast<size_t>(Row + 1) * 64, 0);
      }
      writeOutputBytes(Op.Output, TileData);
      return true;
    }

    if (Id == Intrinsic::AMXTileStore) {
      const auto AddressBytes = addressSize(5);
      const auto Tile = tileIndex(Op.Inputs[4]);
      if (!AddressBytes || !Tile ||
          Op.Inputs[3].Size != x86reg::TileConfigSize ||
          Op.Output.Size != x86reg::TileConfigSize)
        return false;
      std::vector<uint8_t> Config = readOperandBytes(Op.Inputs[3]);
      uint16_t ColumnBytes = 0;
      uint8_t Rows = 0;
      if (!validateConfig(Config) ||
          !tileShape(Config, *Tile, true, ColumnBytes, Rows))
        return false;
      const std::vector<uint8_t> TileData = readOperandBytes(Op.Inputs[4]);
      const uint8_t Start = Config[1];
      const uint64_t Base = readOperand(Op.Inputs[1]);
      const uint64_t Stride = readOperand(Op.Inputs[2]);
      for (uint8_t Row = Start; Row < Rows; ++Row) {
        const llvm::ArrayRef<uint8_t> RowData(
            TileData.data() + static_cast<size_t>(Row) * 64, ColumnBytes);
        if (!storeBytesAtomically(Base, static_cast<uint64_t>(Row) * Stride,
                                  *AddressBytes, RowData)) {
          Config[1] = Row;
          writeOutputBytes(Op.Output, Config);
          return false;
        }
      }
      Config[1] = 0;
      writeOutputBytes(Op.Output, Config);
      return true;
    }

    return false;
  }

  const bool IsCacheFlush = Id == Intrinsic::Clflush ||
                            Id == Intrinsic::Clflushopt ||
                            Id == Intrinsic::Clwb;
  const bool IsPrefetch =
      Id == Intrinsic::Prefetch || Id == Intrinsic::PrefetchT0 ||
      Id == Intrinsic::PrefetchT1 || Id == Intrinsic::PrefetchT2 ||
      Id == Intrinsic::PrefetchNta || Id == Intrinsic::PrefetchW ||
      Id == Intrinsic::PrefetchWT1;
  const bool IsMXCSR = Id == Intrinsic::Ldmxcsr || Id == Intrinsic::Stmxcsr;
  const bool IsLoad =
      Id == Intrinsic::MaskedLoadB || Id == Intrinsic::MaskedLoadW ||
      Id == Intrinsic::MaskedLoadD || Id == Intrinsic::MaskedLoadQ;
  const bool IsStore =
      Id == Intrinsic::MaskedStoreW || Id == Intrinsic::MaskedStoreD ||
      Id == Intrinsic::MaskedStoreQ || Id == Intrinsic::MaskedStoreB;
  if (!IsLoad && !IsStore && !IsCacheFlush && !IsPrefetch && !IsMXCSR)
    return false;
  if (!intrinsicMemoryAddressSpaceShapeIsValid(
          Id, Op.NumInputs, Op.Output.Size,
          Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
          Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
          Op.NumInputs > 3 ? Op.Inputs[3].Size : 0))
    return false;

  const auto Base = resolveMemoryAddress(Op, readOperand(Op.Inputs[1]));
  if (!Base)
    return false;

  if (IsPrefetch)
    return true; // architecturally a non-faulting hint with no data result
  if (IsCacheFlush) {
    if (!loadMemory(*Base, 1))
      return false;
    return true;
  }
  if (Id == Intrinsic::Ldmxcsr) {
    auto Value = loadMemory(*Base, 4);
    if (!Value)
      return false;
    // Bits 31:16 are reserved on every implementation.  The CPU-specific
    // low-bit mask is unavailable here, but this universal fault must occur
    // before the architectural control state changes.
    if ((*Value & UINT64_C(0xffff0000)) != 0)
      return false;
    MXCSR = static_cast<uint32_t>(*Value);
    return true;
  }
  if (Id == Intrinsic::Stmxcsr)
    return storeMemory(*Base, 4, MXCSR) || !strictMode();

  const unsigned ElementBytes =
      (Id == Intrinsic::MaskedLoadB || Id == Intrinsic::MaskedStoreB)   ? 1
      : (Id == Intrinsic::MaskedLoadW || Id == Intrinsic::MaskedStoreW) ? 2
      : (Id == Intrinsic::MaskedLoadQ || Id == Intrinsic::MaskedStoreQ) ? 8
                                                                        : 4;
  const uint16_t VectorBytes = IsLoad ? Op.Output.Size : Op.Inputs[3].Size;
  const std::vector<uint8_t> Mask = readOperandBytes(Op.Inputs[2]);
  if (Mask.size() < VectorBytes || VectorBytes % ElementBytes != 0)
    return false;
  auto elementAddress = [&](uint16_t Offset) {
    uint64_t Address = *Base + Offset;
    if (Img.Arch == Arch::X86)
      Address &= UINT64_C(0xffffffff);
    return Address;
  };

  if (IsLoad) {
    std::vector<uint8_t> Result(VectorBytes, 0);
    std::vector<LoadRecord> Records;
    for (uint16_t Offset = 0; Offset < VectorBytes; Offset += ElementBytes) {
      if ((Mask[Offset + ElementBytes - 1] & 0x80) == 0)
        continue;
      const uint64_t Address = elementAddress(Offset);
      auto Value = loadMemory(Address, ElementBytes);
      if (!Value)
        return false;
      std::memcpy(Result.data() + Offset, &*Value, ElementBytes);
      Records.push_back({Address, static_cast<uint16_t>(ElementBytes)});
    }
    if (CollectLoads)
      for (const LoadRecord &Record : Records)
        if (static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
          LoadLog.push_back(Record);
    writeOutputBytes(Op.Output, Result);
    return true;
  }

  const std::vector<uint8_t> Data = readOperandBytes(Op.Inputs[3]);
  if (Data.size() < VectorBytes)
    return false;
  // Ordinary masked moves are one architectural store instruction, unlike
  // restartable scatter.  Validate every active element and stage the entire
  // write-back map before committing any entry.  A VSIB scatter emits one
  // intrinsic per lane, so it retains its required per-lane progress while a
  // later fault cannot leave a partial ordinary vector store behind.
  std::map<uint64_t, uint64_t> UpdatedStore = MemStore;
  std::map<uint64_t, uint8_t> UpdatedBytes = MemStoreBytes;
  for (uint16_t Offset = 0; Offset < VectorBytes; Offset += ElementBytes) {
    if ((Mask[Offset + ElementBytes - 1] & 0x80) == 0)
      continue;
    const uint64_t Address = elementAddress(Offset);
    if (ElementBytes - 1 > UINT64_MAX - Address)
      return false;
    const Segment *Mapped = Img.getSegmentFor(Address);
    if (!Mapped || !Mapped->isWritable() || Address < Mapped->VA ||
        ElementBytes > Mapped->Size - (Address - Mapped->VA))
      return false;
    uint64_t Value = 0;
    std::memcpy(&Value, Data.data() + Offset, ElementBytes);
    const uint64_t ValueMask = ElementBytes < sizeof(uint64_t)
                                   ? (UINT64_C(1) << (ElementBytes * 8)) - 1
                                   : UINT64_MAX;
    UpdatedStore[Address] = Value & ValueMask;
    for (unsigned I = 0; I < ElementBytes; ++I)
      UpdatedBytes[Address + I] = static_cast<uint8_t>(Value >> (I * 8));
  }
  if (static_cast<int>(UpdatedStore.size()) >
      limits::kMaxEmulatorStoreEntries) {
    ++Skips.DroppedStores;
    return !strictMode();
  }
  MemStore = std::move(UpdatedStore);
  MemStoreBytes = std::move(UpdatedBytes);
  return true;
}

bool NdOpEmulator::executeCopy(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return false;

  if (Op.Output.Size > 8 || Op.Inputs[0].Size > 8 ||
      (Op.Opcode == NdOp::CONCAT && Op.NumInputs >= 2 &&
       Op.Inputs[1].Size > 8)) {
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT: {
      std::vector<uint8_t> Bytes = readOperandBytes(Op.Inputs[0]);
      Bytes.resize(Op.Output.Size, 0);
      writeOutputBytes(Op.Output, Bytes);
      return true;
    }
    case NdOp::INT_SEXT: {
      std::vector<uint8_t> Bytes = readOperandBytes(Op.Inputs[0]);
      const uint8_t Fill = !Bytes.empty() && (Bytes.back() & 0x80) ? 0xff : 0;
      Bytes.resize(Op.Output.Size, Fill);
      writeOutputBytes(Op.Output, Bytes);
      return true;
    }
    case NdOp::SUBBYTES: {
      if (Op.NumInputs < 2)
        return false;
      const uint64_t Off = readOperand(Op.Inputs[1]);
      const std::vector<uint8_t> Input = readOperandBytes(Op.Inputs[0]);
      if (Off > Input.size() || Op.Output.Size > Input.size() - Off)
        return false;
      writeOutputBytes(
          Op.Output, llvm::ArrayRef<uint8_t>(Input).slice(Off, Op.Output.Size));
      return true;
    }
    case NdOp::CONCAT: {
      if (Op.NumInputs < 2)
        return false;
      const std::vector<uint8_t> Hi = readOperandBytes(Op.Inputs[0]);
      const std::vector<uint8_t> Lo = readOperandBytes(Op.Inputs[1]);
      std::vector<uint8_t> Bytes;
      Bytes.reserve(Lo.size() + Hi.size());
      Bytes.insert(Bytes.end(), Lo.begin(), Lo.end());
      Bytes.insert(Bytes.end(), Hi.begin(), Hi.end());
      Bytes.resize(Op.Output.Size, 0);
      writeOutputBytes(Op.Output, Bytes);
      return true;
    }
    default:
      return false;
    }
  }
  uint64_t Val = readOperand(Op.Inputs[0]);

  switch (Op.Opcode) {
  case NdOp::COPY:
    writeOutput(Op.Output, Val);
    return true;
  case NdOp::INT_ZEXT:
    writeOutput(Op.Output, Val);
    return true;
  case NdOp::INT_SEXT: {
    if (Op.Inputs[0].Size > 0 && Op.Inputs[0].Size < 8) {
      int Bits = Op.Inputs[0].Size * 8;
      int64_t Signed = static_cast<int64_t>(Val << (64 - Bits)) >> (64 - Bits);
      writeOutput(Op.Output, static_cast<uint64_t>(Signed));
    } else {
      writeOutput(Op.Output, Val);
    }
    return true;
  }
  case NdOp::SUBBYTES: {
    uint64_t Off = Op.NumInputs >= 2 ? readOperand(Op.Inputs[1]) : 0;
    if (Off >= 8)
      return false;
    writeOutput(Op.Output, Val >> (Off * 8));
    return true;
  }
  case NdOp::INT_NEGATE:
    writeOutput(Op.Output, ~Val);
    return true;
  case NdOp::INT_NEG2:
    writeOutput(Op.Output, ~Val + 1);
    return true;
  case NdOp::CONCAT: {
    if (Op.NumInputs < 2)
      return false;
    uint64_t Hi = Val;
    uint64_t Lo = readOperand(Op.Inputs[1]);
    uint16_t LoSize = Op.Inputs[1].Size > 0 ? Op.Inputs[1].Size : 4;
    if (LoSize >= 8)
      writeOutput(Op.Output, Lo);
    else {
      uint64_t LoMask = (1ULL << (LoSize * 8)) - 1;
      writeOutput(Op.Output, (Hi << (LoSize * 8)) | (Lo & LoMask));
    }
    return true;
  }
  default:
    return false;
  }
}

bool NdOpEmulator::executeCompare(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return false;
  uint64_t A = readOperand(Op.Inputs[0]);
  uint64_t B = readOperand(Op.Inputs[1]);
  uint64_t Result = 0;
  auto signedAtWidth = [](uint64_t Value, uint16_t Size) {
    if (Size == 0 || Size >= 8)
      return static_cast<int64_t>(Value);
    const unsigned Bits = Size * 8;
    const uint64_t Mask = (uint64_t{1} << Bits) - 1;
    Value &= Mask;
    if ((Value & (uint64_t{1} << (Bits - 1))) != 0)
      Value |= ~Mask;
    return static_cast<int64_t>(Value);
  };

  switch (Op.Opcode) {
  case NdOp::INT_EQUAL:
    Result = (A == B) ? 1 : 0;
    break;
  case NdOp::INT_NOTEQUAL:
    Result = (A != B) ? 1 : 0;
    break;
  case NdOp::INT_LESS:
    Result = (A < B) ? 1 : 0;
    break;
  case NdOp::INT_SLESS:
    Result = (signedAtWidth(A, Op.Inputs[0].Size) <
              signedAtWidth(B, Op.Inputs[1].Size))
                 ? 1
                 : 0;
    break;
  case NdOp::INT_LESSEQUAL:
    Result = (A <= B) ? 1 : 0;
    break;
  case NdOp::INT_SLESSEQUAL:
    Result = (signedAtWidth(A, Op.Inputs[0].Size) <=
              signedAtWidth(B, Op.Inputs[1].Size))
                 ? 1
                 : 0;
    break;
  case NdOp::INT_CARRY: {
    const unsigned Bits = Op.Inputs[0].Size * 8;
    const uint64_t Mask = Bits >= 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
    A &= Mask;
    B &= Mask;
    Result = Bits >= 64 ? A + B < A : A + B > Mask;
    break;
  }
  case NdOp::INT_SOVF: {
    const unsigned Bits = Op.Inputs[0].Size * 8;
    const uint64_t Mask = Bits >= 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
    const uint64_t Sign = UINT64_C(1) << (Bits - 1);
    A &= Mask;
    B &= Mask;
    const uint64_t Sum = (A + B) & Mask;
    Result = ((~(A ^ B) & (A ^ Sum) & Sign) != 0) ? 1 : 0;
    break;
  }
  case NdOp::INT_SBOR: {
    const unsigned Bits = Op.Inputs[0].Size * 8;
    const uint64_t Mask = Bits >= 64 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
    const uint64_t Sign = UINT64_C(1) << (Bits - 1);
    A &= Mask;
    B &= Mask;
    const uint64_t Difference = (A - B) & Mask;
    Result = (((A ^ B) & (A ^ Difference) & Sign) != 0) ? 1 : 0;
    break;
  }
  default:
    return false;
  }

  writeOutput(Op.Output, Result);
  return true;
}

bool NdOpEmulator::executeBool(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return false;
  uint64_t A = readOperand(Op.Inputs[0]);
  uint64_t Result = 0;

  switch (Op.Opcode) {
  case NdOp::BOOL_NOT:
    Result = (A == 0) ? 1 : 0;
    break;
  case NdOp::BOOL_AND:
    if (Op.NumInputs < 2)
      return false;
    Result = ((A != 0) && (readOperand(Op.Inputs[1]) != 0)) ? 1 : 0;
    break;
  case NdOp::BOOL_OR:
    if (Op.NumInputs < 2)
      return false;
    Result = ((A != 0) || (readOperand(Op.Inputs[1]) != 0)) ? 1 : 0;
    break;
  case NdOp::BOOL_XOR:
    if (Op.NumInputs < 2)
      return false;
    Result = ((A != 0) != (readOperand(Op.Inputs[1]) != 0)) ? 1 : 0;
    break;
  default:
    return false;
  }

  writeOutput(Op.Output, Result);
  return true;
}

bool NdOpEmulator::executeMisc(const LowOp &Op) {
  switch (Op.Opcode) {
  case NdOp::SELECT: {
    if (Op.NumInputs < 3)
      return false;
    uint64_t Cond = readOperand(Op.Inputs[0]);
    uint64_t TrueVal = readOperand(Op.Inputs[1]);
    uint64_t FalseVal = readOperand(Op.Inputs[2]);
    writeOutput(Op.Output, Cond ? TrueVal : FalseVal);
    return true;
  }
  case NdOp::INT_NOT: {
    if (Op.NumInputs < 1)
      return false;
    if (Op.Output.Size > sizeof(uint64_t) ||
        Op.Inputs[0].Size > sizeof(uint64_t)) {
      if (Op.Output.Size == 0 || Op.Inputs[0].Size != Op.Output.Size)
        return false;
      std::vector<uint8_t> Value = readOperandBytes(Op.Inputs[0]);
      for (uint8_t &Byte : Value)
        Byte = static_cast<uint8_t>(~Byte);
      writeOutputBytes(Op.Output, Value);
      return true;
    }
    writeOutput(Op.Output, ~readOperand(Op.Inputs[0]));
    return true;
  }
  case NdOp::POPCOUNT: {
    if (Op.NumInputs < 1)
      return false;
    uint64_t Val = readOperand(Op.Inputs[0]);
    // Counted at the width the operand declares, as LZCOUNT below already is.
    // A register slot holds whatever the widest write to it left behind, so
    // counting all sixty-four bits answers for bits this operand does not
    // name — and disagrees with the symbolic model of the same opcode, which
    // is a divergence neither engine can notice from its own side.
    const uint16_t ByteWidth = Op.Inputs[0].Size;
    if (ByteWidth > 0 && ByteWidth < 8)
      Val &= (1ULL << (ByteWidth * 8)) - 1;
    writeOutput(Op.Output, std::popcount(Val));
    return true;
  }
  case NdOp::LZCOUNT: {
    if (Op.NumInputs < 1)
      return false;
    uint64_t Val = readOperand(Op.Inputs[0]);
    uint16_t ByteWidth = Op.Inputs[0].Size;
    if (ByteWidth == 0 || ByteWidth > 8)
      return false;
    uint16_t BitWidth = ByteWidth * 8;
    if (BitWidth < 64)
      Val &= (1ULL << BitWidth) - 1;
    if (Val == 0)
      writeOutput(Op.Output, BitWidth);
    else if (BitWidth == 64)
      writeOutput(Op.Output, std::countl_zero(Val));
    else
      writeOutput(Op.Output, std::countl_zero(Val) - (64 - BitWidth));
    return true;
  }
  case NdOp::INSERT: {
    if (Op.NumInputs < 4)
      return false;
    uint64_t Base = readOperand(Op.Inputs[0]);
    uint64_t Val = readOperand(Op.Inputs[1]);
    uint64_t Pos = readOperand(Op.Inputs[2]);
    uint64_t Len = readOperand(Op.Inputs[3]);
    if (Pos >= 64 || Len > 64 - Pos)
      return false;
    uint64_t FieldMask = Len == 64 ? ~0ULL : ((1ULL << Len) - 1);
    uint64_t Mask = FieldMask << Pos;
    writeOutput(Op.Output, (Base & ~Mask) | ((Val << Pos) & Mask));
    return true;
  }
  case NdOp::EXTRACT: {
    if (Op.NumInputs < 3)
      return false;
    uint64_t Base = readOperand(Op.Inputs[0]);
    uint64_t Pos = readOperand(Op.Inputs[1]);
    uint64_t Len = readOperand(Op.Inputs[2]);
    if (Pos >= 64 || Len > 64 - Pos)
      return false;
    uint64_t Mask = Len == 64 ? ~0ULL : ((1ULL << Len) - 1);
    writeOutput(Op.Output, (Base >> Pos) & Mask);
    return true;
  }
  default:
    return false;
  }
}

} // namespace neverd
