//===- SourceParameterPlacementMIPS.cpp - MIPS scalar carriers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// MIPS scalar locations and separate complete n32/n64 record transport.
/// ELF source admission authenticates the FP profile and callable contract.
//===----------------------------------------------------------------------===//

#include "../SourceParameterPlacementDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/MIPS/SourceRecordABI.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <algorithm>
#include <utility>

using namespace neverd;
using namespace neverd::source_placement;

namespace {
struct ParameterCursor {
  const TargetRegInfo &TRI;
  IntegerArgumentLayout Layout;
  bool MIPS16;
  unsigned Next = 0;
  unsigned FloatingPrefix = 0;
  bool LeadingFloating;

  bool appendScalar(const TypeRef &Type, bool Named,
                    std::vector<SourceParameterPiece> &Pieces) {
    const unsigned Slot = Layout.SlotBytes;
    const bool Little = archLittleEndian(TRI.TheArch);
    const bool Floating = isFloatingScalar(Type);
    if (Floating && Type->Size != 4 && Type->Size != 8)
      return false;
    if ((!isIntegerScalar(Type) && !Floating) ||
        (Type->Size != 1 && Type->Size != 2 && Type->Size != 4 &&
         Type->Size != 8))
      return false;
    // o32 long long uses an even/odd register pair, or an 8-byte aligned
    // stack pair; it never straddles a3 and the first stack argument.
    if (Slot == 4 && Type->Size == 8)
      Next = (Next + 1) & ~1u;
    const unsigned Count = (Type->Size + Slot - 1) / Slot;
    // n32 and n64 share eight argument positions between a0-a7 and f12-f19.
    // o32 uses f12/f14 only for the first two leading FP arguments; their
    // word positions still consume the integer argument area.
    const bool InFPR =
        Floating && !MIPS16 &&
        (Slot == 8 ? Named && Next < 8 : LeadingFloating && FloatingPrefix < 2);
    LeadingFloating &= Floating;
    if (InFPR) {
      const unsigned Register =
          Slot == 8 ? 12 + Next : 12 + 2 * FloatingPrefix++;
      Pieces.push_back(registerPiece(SourceABICarrierKind::FloatingRegister,
                                     mipsreg::w(Register), 0, Type->Size));
      Next += Count;
      return true;
    }
    for (unsigned I = 0; I < Count; ++I, ++Next) {
      const auto Bytes =
          static_cast<uint16_t>(std::min<unsigned>(Type->Size, Slot));
      // Integer pieces use significance offsets, not host byte addresses.
      const auto Offset =
          static_cast<uint16_t>((Little ? I : Count - I - 1) * Slot);
      if (Next < Layout.Registers.size()) {
        Pieces.push_back(registerPiece(SourceABICarrierKind::IntegerRegister,
                                       Layout.Registers[Next], Offset, Bytes));
      } else {
        const int64_t Stack =
            Layout.EntryStackBase + (Next - Layout.Registers.size()) * Slot;
        // Big-endian integers are right-justified; floating values occupy
        // the lowest addressed bytes of the stack slot.
        Pieces.push_back(stackPiece(
            Stack + (Little || Floating ? 0 : Slot - Bytes), Offset, Bytes));
      }
    }
    return true;
  }
};

/// Layout has already validated the finite, acyclic member tree. Passing is
/// a separate language contract: a natural shape alone does not make a C++
/// class or a compiler register tuple a C by-value record.
bool hasCRecordMembers(const TypeRef &Type) {
  if (Type->Kind == NdTypeKind::Array)
    return hasCRecordMembers(Type->ElemType);
  if (Type->Kind != NdTypeKind::Struct)
    return !sourceAArch64VectorBytes(Type);
  return !Type->IsEnum && !Type->RegisterReturnRecord &&
         Type->Passing == NdRecordPassing::ByValue &&
         Type->FieldDisplayBits.empty() &&
         std::all_of(Type->Fields.begin(), Type->Fields.end(),
                     hasCRecordMembers);
}

bool completeRecord(const TypeRef &Type, const TargetRegInfo &TRI) {
  if (!isRecord(Type))
    return false;
  const auto Natural = NdType::makeStruct(Type->Fields, TRI.PointerSize);
  return Natural && Type->Size == Natural->Size &&
         Type->Alignment == Natural->Alignment &&
         Type->FieldOffsets == Natural->FieldOffsets && hasCRecordMembers(Type);
}

void setCarrierWidths(std::vector<SourceParameterPiece> &Pieces,
                      const TargetRegInfo &TRI) {
  for (auto &Piece : Pieces)
    Piece.CarrierBytes = Piece.Location.Kind == SourceABICarrierKind::Stack
                             ? Piece.Location.ValueBytes
                             : TRI.FullRegWidth;
}
} // namespace

std::optional<SourceParameterPlacement> source_placement::placeMIPS(
    const TargetRegInfo &TRI, BinaryFormat F, const TypeRef &ReturnType,
    llvm::ArrayRef<TypeRef> ParamTypes, std::optional<unsigned> FixedCount,
    InstructionMode EntryMode) {
  const bool ComplexResult =
      !FixedCount && sourceComplexComponentBytes(ReturnType);
  if (F != BinaryFormat::ELF || !ReturnType ||
      (!ComplexResult && ReturnType->Kind != NdTypeKind::Void &&
       ((!isIntegerScalar(ReturnType) && !isFloatingScalar(ReturnType)) ||
        ReturnType->Size >
            (ReturnType->Kind == NdTypeKind::Int ? 2 * TRI.FullRegWidth : 8))))
    return std::nullopt;
  // The MIPS16 body receives the leading floating arguments in GPRs.
  // Its classic entry stub is a separate function with ordinary FP carriers.
  const bool MIPS16 = EntryMode == InstructionMode::MIPS16;
  if (MIPS16 && isMIPS64(TRI.TheArch))
    return std::nullopt;
  ParameterCursor Cursor{TRI,        TRI.integerArgumentLayout(F), MIPS16, 0, 0,
                         !FixedCount};
  SourceParameterPlacement Result;
  for (const TypeRef &Type : ParamTypes) {
    std::vector<SourceParameterPiece> Pieces;
    if (!Cursor.appendScalar(
            Type, !FixedCount || Result.Parameters.size() < *FixedCount,
            Pieces))
      break;
    Result.Parameters.push_back(std::move(Pieces));
  }
  return Result;
}

std::optional<mips::RecordCallPlacement>
mips::placeRecordCall(Arch Architecture, BinaryFormat Format,
                      const TypeRef &ReturnType,
                      llvm::ArrayRef<TypeRef> Parameters, bool Variadic) {
  if (!isMIPS64(Architecture) || Format != BinaryFormat::ELF || Variadic ||
      !ReturnType ||
      (ReturnType->Kind == NdTypeKind::Void && ReturnType->Size != 0))
    return std::nullopt;
  const auto &TRI = getTargetRegInfo(Architecture);
  const bool RecordResult = isRecord(ReturnType);
  if (RecordResult && !completeRecord(ReturnType, TRI))
    return std::nullopt;
  if (!RecordResult && ReturnType->Kind != NdTypeKind::Void &&
      ((!isIntegerScalar(ReturnType) && !isFloatingScalar(ReturnType)) ||
       (ReturnType->Size != 1 && ReturnType->Size != 2 &&
        ReturnType->Size != 4 && ReturnType->Size != 8) ||
       (ReturnType->Kind == NdTypeKind::Float && ReturnType->Size < 4) ||
       (ReturnType->Kind == NdTypeKind::Ptr &&
        ReturnType->Size != TRI.PointerSize)))
    return std::nullopt;
  RecordCallPlacement Result;
  ParameterCursor Cursor{TRI, TRI.integerArgumentLayout(Format), false, 0, 0,
                         true};
  if (RecordResult && ReturnType->Size > 2 * TRI.FullRegWidth) {
    Result.Arguments.ResultPointer =
        SourceABIValueLocation{SourceABICarrierKind::IntegerRegister,
                               Cursor.Layout.Registers[0], 0, TRI.PointerSize};
    ++Cursor.Next;
  } else if (RecordResult) {
    const bool Floating =
        ReturnType->Fields.size() <= 2 &&
        std::all_of(ReturnType->Fields.begin(), ReturnType->Fields.end(),
                    [](const TypeRef &Field) {
                      return Field->Kind == NdTypeKind::Float;
                    });
    if (Floating) {
      for (size_t I = 0; I < ReturnType->Fields.size(); ++I)
        Result.Result.push_back(registerPiece(
            SourceABICarrierKind::FloatingRegister, TRI.FPReturnRegs[I],
            ReturnType->FieldOffsets[I], ReturnType->Fields[I]->Size));
    } else {
      for (unsigned Offset = 0; Offset < ReturnType->Size;
           Offset += TRI.FullRegWidth) {
        const auto Bytes = static_cast<uint16_t>(
            std::min<unsigned>(TRI.FullRegWidth, ReturnType->Size - Offset));
        auto Piece = registerPiece(
            SourceABICarrierKind::IntegerRegister,
            Offset ? TRI.IntReturnReg2 : TRI.IntReturnReg, Offset, Bytes);
        if (!archLittleEndian(Architecture))
          Piece.RegisterBitOffset = (TRI.FullRegWidth - Bytes) * 8;
        Result.Result.push_back(Piece);
      }
    }
  } else if (ReturnType->Kind != NdTypeKind::Void) {
    const bool Floating = ReturnType->Kind == NdTypeKind::Float;
    Result.Result.push_back(registerPiece(
        Floating ? SourceABICarrierKind::FloatingRegister
                 : SourceABICarrierKind::IntegerRegister,
        Floating ? TRI.FPReturnReg : TRI.IntReturnReg, 0, ReturnType->Size));
  }
  setCarrierWidths(Result.Result, TRI);
  bool HasRecord = RecordResult;
  for (const TypeRef &Type : Parameters) {
    auto &Pieces = Result.Arguments.Parameters.emplace_back();
    if (!isRecord(Type)) {
      if (!Cursor.appendScalar(Type, true, Pieces) ||
          (Type->Kind == NdTypeKind::Ptr && Type->Size != TRI.PointerSize))
        return std::nullopt;
      setCarrierWidths(Pieces, TRI);
      continue;
    }
    if (!completeRecord(Type, TRI))
      return std::nullopt;
    HasRecord = true;
    size_t Field = 0;
    for (unsigned Offset = 0; Offset < Type->Size;
         Offset += Cursor.Layout.SlotBytes, ++Cursor.Next) {
      const auto Bytes = static_cast<uint16_t>(
          std::min<unsigned>(Cursor.Layout.SlotBytes, Type->Size - Offset));
      if (Cursor.Next >= Cursor.Layout.Registers.size()) {
        // Record tails keep their memory byte address, including a partial
        // big-endian slot. They are not right-justified narrow integers.
        Pieces.push_back(
            stackPiece(Cursor.Layout.EntryStackBase +
                           (Cursor.Next - Cursor.Layout.Registers.size()) *
                               Cursor.Layout.SlotBytes,
                       Offset, Bytes));
        continue;
      }
      while (Field < Type->Fields.size() && Type->FieldOffsets[Field] < Offset)
        ++Field;
      const bool Floating =
          Field < Type->Fields.size() && Type->FieldOffsets[Field] == Offset &&
          Type->Fields[Field]->Kind == NdTypeKind::Float &&
          Type->Fields[Field]->Size == Cursor.Layout.SlotBytes;
      auto Piece =
          registerPiece(Floating ? SourceABICarrierKind::FloatingRegister
                                 : SourceABICarrierKind::IntegerRegister,
                        Floating ? mipsreg::w(12 + Cursor.Next)
                                 : Cursor.Layout.Registers[Cursor.Next],
                        Offset, Bytes);
      if (!Floating && !archLittleEndian(Architecture))
        Piece.RegisterBitOffset = (TRI.FullRegWidth - Bytes) * 8;
      Pieces.push_back(Piece);
    }
    setCarrierWidths(Pieces, TRI);
  }
  return HasRecord ? std::optional(std::move(Result)) : std::nullopt;
}
