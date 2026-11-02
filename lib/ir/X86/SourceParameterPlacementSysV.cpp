//===- SourceParameterPlacementSysV.cpp - System V x86-64 -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The System V AMD64 ABI's parameter passing: integer-class values in RDI,
/// RSI, RDX, RCX, R8 and R9, SSE-class values in XMM0-XMM7, each eightbyte of
/// a record of at most 16 bytes by its class, and MEMORY-class values (larger
/// records, values the registers no longer hold) on the stack in 8-byte
/// slots.  A scalar wider than a register has no certain place: 16 integer
/// bytes are an __int128 or a _BitInt, which compilers have passed apart,
/// and 16 floating bytes an x87 long double on the stack or a __float128 in
/// a register.
///
//===----------------------------------------------------------------------===//

#include "../SourceParameterPlacementDetail.h"

#include "llvm/Support/MathExtras.h"

using namespace neverd;
using namespace neverd::source_placement;

namespace {
/// Records larger than this are passed and returned in memory.
constexpr uint16_t kMaxRegisterRecordBytes = 16;
constexpr uint16_t kEightbyte = 8;
constexpr uint16_t kSlotBytes = 8;

enum class EightbyteClass : uint8_t { Integer, Sse };

/// Where the convention passes a record or complex number.
struct RecordClass {
  enum class Kind : uint8_t {
    /// In registers, an eightbyte at a time by Eightbytes.
    Registers,
    /// In memory: larger than 16 bytes, or with a scalar off its alignment.
    Memory,
    /// Its layout does not say.
    Unknown,
  } TheKind = Kind::Unknown;
  std::vector<EightbyteClass> Eightbytes;
};

RecordClass classifyRecord(const TypeRef &Record) {
  RecordClass Class;
  if (Record->Size > kMaxRegisterRecordBytes) {
    Class.TheKind = RecordClass::Kind::Memory;
    return Class;
  }
  const auto Leaves = scalarLeaves(Record);
  if (!Leaves || Leaves->empty())
    return Class;
  const unsigned Count = (Record->Size + kEightbyte - 1) / kEightbyte;
  for (unsigned I = 0; I < Count; ++I) {
    bool Any = false, Integer = false;
    for (const NdScalarLeaf &L : *Leaves) {
      const unsigned First = L.Offset / kEightbyte;
      const unsigned Last = (L.Offset + L.Size - 1) / kEightbyte;
      // A scalar across two eightbytes is a packed member, MEMORY class, or
      // a bit field, which is not; a wide floating scalar is x87 (MEMORY)
      // or __float128 (SSE).  Neither says which.
      if (First != Last || (L.Floating && L.Size > kEightbyte))
        return RecordClass();
      if (First != I)
        continue;
      Any = true;
      Integer |= !L.Floating;
    }
    // An eightbyte of padding alone has no class here.
    if (!Any)
      return RecordClass();
    Class.Eightbytes.push_back(Integer ? EightbyteClass::Integer
                                       : EightbyteClass::Sse);
  }
  Class.TheKind = RecordClass::Kind::Registers;
  return Class;
}
} // namespace

std::optional<SourceParameterPlacement>
source_placement::placeSysV(const TargetRegInfo &TRI, BinaryFormat F,
                            const TypeRef &ReturnType,
                            llvm::ArrayRef<TypeRef> ParamTypes) {
  const auto Layout = TRI.integerArgumentLayout(F);
  const llvm::ArrayRef<uint64_t> Integer = Layout.Registers;
  const llvm::ArrayRef<uint64_t> Vector = TRI.FPParamRegs;
  if (Integer.empty() || Vector.empty())
    return std::nullopt;
  SourceParameterPlacement Placement;
  size_t NextInteger = 0, NextVector = 0;
  int64_t NextStack = Layout.EntryStackBase;

  // A record returned in memory takes its storage's address in RDI.  A
  // complex number comes back in registers, x87 ones for a long double's
  // parts.  A result whose passing or layout is unknown leaves every
  // register in doubt.
  if (isRecord(ReturnType) && ReturnType->Passing != NdRecordPassing::Complex) {
    bool InMemory = ReturnType->Passing == NdRecordPassing::ByReference;
    if (ReturnType->Passing == NdRecordPassing::ByValue) {
      const RecordClass Class = classifyRecord(ReturnType);
      if (Class.TheKind == RecordClass::Kind::Unknown)
        return Placement;
      InMemory = Class.TheKind == RecordClass::Kind::Memory;
    } else if (ReturnType->Passing != NdRecordPassing::ByReference) {
      return Placement;
    }
    if (InMemory) {
      SourceABIValueLocation Result;
      Result.Kind = SourceABICarrierKind::IntegerRegister;
      Result.RegisterOffset = Integer[NextInteger++];
      Result.ValueBytes = TRI.PointerSize;
      Placement.ResultPointer = Result;
    }
  }

  auto OnStack = [&](uint16_t Bytes, uint16_t Align, bool Indirect = false) {
    NextStack = static_cast<int64_t>(
        llvm::alignTo(static_cast<uint64_t>(NextStack),
                      std::max<uint16_t>(Align, kSlotBytes)));
    std::vector<SourceParameterPiece> Pieces = {
        stackPiece(NextStack, 0, Bytes, Indirect)};
    NextStack += static_cast<int64_t>(llvm::alignTo(Bytes, kSlotBytes));
    return Pieces;
  };

  for (const TypeRef &Type : ParamTypes) {
    std::vector<SourceParameterPiece> Pieces;
    if (isIntegerScalar(Type) && Type->Size <= kEightbyte) {
      Pieces = NextInteger < Integer.size()
                   ? std::vector{registerPiece(
                         SourceABICarrierKind::IntegerRegister,
                         Integer[NextInteger++], 0, Type->Size)}
                   : OnStack(Type->Size, kSlotBytes);
    } else if (isFloatingScalar(Type) && Type->Size <= kEightbyte) {
      Pieces = NextVector < Vector.size()
                   ? std::vector{registerPiece(
                         SourceABICarrierKind::FloatingRegister,
                         Vector[NextVector++], 0, Type->Size)}
                   : OnStack(Type->Size, kSlotBytes);
    } else if (isRecord(Type) &&
               Type->Passing == NdRecordPassing::ByReference) {
      // The address of a copy, as an integer value.
      Pieces = NextInteger < Integer.size()
                   ? std::vector{registerPiece(
                         SourceABICarrierKind::IntegerRegister,
                         Integer[NextInteger++], 0, TRI.PointerSize,
                         /*Indirect=*/true)}
                   : OnStack(TRI.PointerSize, kSlotBytes, /*Indirect=*/true);
    } else if (isRecord(Type) && Type->Size &&
               (Type->Passing == NdRecordPassing::ByValue ||
                Type->Passing == NdRecordPassing::Complex)) {
      const RecordClass Class = classifyRecord(Type);
      if (Class.TheKind == RecordClass::Kind::Unknown)
        break;
      size_t Integers = 0, Vectors = 0;
      for (EightbyteClass Eightbyte : Class.Eightbytes)
        ++(Eightbyte == EightbyteClass::Integer ? Integers : Vectors);
      if (Class.TheKind == RecordClass::Kind::Memory ||
          NextInteger + Integers > Integer.size() ||
          NextVector + Vectors > Vector.size()) {
        // In memory, or the whole record there when its registers ran out;
        // its slot follows its alignment.
        if (!Type->Alignment)
          break;
        Pieces = OnStack(Type->Size, Type->Alignment);
      } else {
        for (size_t I = 0; I < Class.Eightbytes.size(); ++I) {
          const uint16_t Offset = static_cast<uint16_t>(I * kEightbyte);
          const uint16_t Bytes =
              std::min<uint16_t>(kEightbyte, Type->Size - Offset);
          Pieces.push_back(
              Class.Eightbytes[I] == EightbyteClass::Integer
                  ? registerPiece(SourceABICarrierKind::IntegerRegister,
                                  Integer[NextInteger++], Offset, Bytes)
                  : registerPiece(SourceABICarrierKind::FloatingRegister,
                                  Vector[NextVector++], Offset, Bytes));
        }
      }
    } else {
      break;
    }
    Placement.Parameters.push_back(std::move(Pieces));
  }
  return Placement;
}
