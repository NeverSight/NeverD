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

#include "neverd/Limits.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include <bit>

namespace neverd {

bool NdOpEmulator::executeArith(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return false;
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
    if (B == 0)
      return false;
    Result = A / B;
    break;
  case NdOp::INT_SDIV:
    if (B == 0)
      return false;
    if (A == (1ULL << 63) && B == ~0ULL)
      return false;
    Result = static_cast<uint64_t>(static_cast<int64_t>(A) /
                                   static_cast<int64_t>(B));
    break;
  case NdOp::INT_REM:
    if (B == 0)
      return false;
    Result = A % B;
    break;
  case NdOp::INT_SREM:
    if (B == 0)
      return false;
    if (A == (1ULL << 63) && B == ~0ULL)
      return false;
    Result = static_cast<uint64_t>(static_cast<int64_t>(A) %
                                   static_cast<int64_t>(B));
    break;
  default:
    return false;
  }

  writeOutput(Op.Output, Result);
  return true;
}

bool NdOpEmulator::executeLoad(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return false;

  uint64_t Addr;
  if (Op.NumInputs >= 2)
    Addr = readOperand(Op.Inputs[1]);
  else
    Addr = readOperand(Op.Inputs[0]);

  if (CollectLoads &&
      static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
    LoadLog.push_back({Addr, Op.Output.Size});

  auto Val = loadMemory(Addr, Op.Output.Size);
  if (!Val)
    return false;
  writeOutput(Op.Output, *Val);
  return true;
}

bool NdOpEmulator::executeStore(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return false;

  uint64_t Addr;
  uint64_t Val;
  if (Op.NumInputs >= 3) {
    Addr = readOperand(Op.Inputs[1]);
    Val = readOperand(Op.Inputs[2]);
  } else {
    Addr = readOperand(Op.Inputs[0]);
    Val = readOperand(Op.Inputs[1]);
  }

  uint16_t Size = Op.NumInputs >= 3 ? Op.Inputs[2].Size : Op.Inputs[1].Size;
  if (Size == 0)
    Size = 8;
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return false;
  storeMemory(Addr, Size, Val);
  return true;
}

bool NdOpEmulator::executeCopy(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return false;
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
    Result = (static_cast<int64_t>(A) < static_cast<int64_t>(B)) ? 1 : 0;
    break;
  case NdOp::INT_LESSEQUAL:
    Result = (A <= B) ? 1 : 0;
    break;
  case NdOp::INT_SLESSEQUAL:
    Result = (static_cast<int64_t>(A) <= static_cast<int64_t>(B)) ? 1 : 0;
    break;
  case NdOp::INT_CARRY:
    Result = (A + B < A) ? 1 : 0;
    break;
  case NdOp::INT_SOVF: {
    int64_t SA = static_cast<int64_t>(A);
    int64_t SB = static_cast<int64_t>(B);
    int64_t Sum = static_cast<int64_t>(A + B);
    Result = ((SA > 0 && SB > 0 && Sum < 0) || (SA < 0 && SB < 0 && Sum >= 0))
                 ? 1
                 : 0;
    break;
  }
  case NdOp::INT_SBOR: {
    int64_t SA = static_cast<int64_t>(A);
    int64_t SB = static_cast<int64_t>(B);
    int64_t Diff = static_cast<int64_t>(A - B);
    Result =
        ((SA >= 0 && SB < 0 && Diff < 0) || (SA < 0 && SB >= 0 && Diff >= 0))
            ? 1
            : 0;
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
    writeOutput(Op.Output, ~readOperand(Op.Inputs[0]));
    return true;
  }
  case NdOp::POPCOUNT: {
    if (Op.NumInputs < 1)
      return false;
    uint64_t Val = readOperand(Op.Inputs[0]);
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
