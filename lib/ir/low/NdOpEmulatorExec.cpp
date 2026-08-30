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
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include <bit>
#include <cstring>

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
  const LowMemoryOperandView Memory = lowMemoryOperands(Op);
  if (!Memory.Complete)
    return false;
  const auto Addr = resolveMemoryAddress(Op, readOperand(*Memory.Address));
  if (!Addr)
    return false;

  if (CollectLoads &&
      static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
    LoadLog.push_back({*Addr, Op.Output.Size});

  auto Val = loadMemory(*Addr, Op.Output.Size);
  if (!Val)
    return false;
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
  const uint64_t Val = readOperand(*Memory.StoredValue);
  const uint16_t Size = Memory.AccessSize;
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return false;
  if (!storeMemory(*Addr, Size, Val))
    return !strictMode();
  return true;
}

bool NdOpEmulator::executeIntrinsic(const LowOp &Op) {
  if (Op.NumInputs < 1 || !Op.Inputs[0].isConst())
    return false;
  const Intrinsic Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  const bool IsCacheFlush = Id == Intrinsic::Clflush ||
                            Id == Intrinsic::Clflushopt ||
                            Id == Intrinsic::Clwb;
  const bool IsPrefetch =
      Id == Intrinsic::Prefetch || Id == Intrinsic::PrefetchT0 ||
      Id == Intrinsic::PrefetchT1 || Id == Intrinsic::PrefetchT2 ||
      Id == Intrinsic::PrefetchNta || Id == Intrinsic::PrefetchW ||
      Id == Intrinsic::PrefetchWT1;
  const bool IsMXCSR =
      Id == Intrinsic::Ldmxcsr || Id == Intrinsic::Stmxcsr;
  const bool IsLoad = Id == Intrinsic::MaskedLoadD ||
                      Id == Intrinsic::MaskedLoadQ;
  const bool IsStore = Id == Intrinsic::MaskedStoreD ||
                       Id == Intrinsic::MaskedStoreQ ||
                       Id == Intrinsic::MaskedStoreB;
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
      Id == Intrinsic::MaskedStoreB ? 1
      : (Id == Intrinsic::MaskedLoadQ || Id == Intrinsic::MaskedStoreQ) ? 8
                                                                          : 4;
  const uint16_t VectorBytes =
      IsLoad ? Op.Output.Size : Op.Inputs[3].Size;
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
    for (uint16_t Offset = 0; Offset < VectorBytes;
         Offset += ElementBytes) {
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
  for (uint16_t Offset = 0; Offset < VectorBytes; Offset += ElementBytes) {
    if ((Mask[Offset + ElementBytes - 1] & 0x80) == 0)
      continue;
    uint64_t Value = 0;
    std::memcpy(&Value, Data.data() + Offset, ElementBytes);
    if (!storeMemory(elementAddress(Offset), ElementBytes, Value))
      return !strictMode();
  }
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
      writeOutputBytes(Op.Output,
                       llvm::ArrayRef<uint8_t>(Input).slice(Off,
                                                             Op.Output.Size));
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
