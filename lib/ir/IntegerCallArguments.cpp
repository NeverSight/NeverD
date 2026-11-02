//===- IntegerCallArguments.cpp - Integer source-call carriers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Match source parameter pieces to the flat integer operands of a call.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/IntegerCallArguments.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <set>

using namespace neverd;

std::optional<FixedIntegerCallArguments>
neverd::fixedIntegerCallArguments(Arch A, BinaryFormat F,
                                  const TypeRef &ReturnType,
                                  llvm::ArrayRef<TypeRef> Parameters) {
  if ((A != Arch::X64 && A != Arch::AArch64) || !ReturnType ||
      Parameters.size() > static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
    return std::nullopt;
  const auto Scalar = [&](const TypeRef &Type) {
    return Type &&
           (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
            Type->Size == 8) &&
           (Type->Kind == NdTypeKind::Int ||
            (Type->Kind == NdTypeKind::Ptr &&
             Type->Size == getTargetRegInfo(A).PointerSize) ||
            (Type->Kind == NdTypeKind::Struct && Type->IsEnum));
  };
  if ((ReturnType->Kind != NdTypeKind::Void && !Scalar(ReturnType)) ||
      !llvm::all_of(Parameters, Scalar))
    return std::nullopt;
  const auto Placement = placeSourceParameters(A, F, ReturnType, Parameters);
  if (!Placement || Placement->ResultPointer ||
      Placement->Parameters.size() != Parameters.size())
    return std::nullopt;
  FixedIntegerCallArguments Result;
  for (size_t I = 0; I < Parameters.size(); ++I) {
    const auto &Pieces = Placement->Parameters[I];
    if (Pieces.size() != 1 || Pieces[0].Indirect || Pieces[0].Offset ||
        Pieces[0].Location.ValueBytes != Parameters[I]->Size)
      return std::nullopt;
    const auto &Location = Pieces[0].Location;
    if (Location.Kind != SourceABICarrierKind::IntegerRegister &&
        Location.Kind != SourceABICarrierKind::Stack)
      return std::nullopt;
    Result.Parameters.push_back({Location.Kind, Location.RegisterOffset,
                                 Location.EntryStackOffset,
                                 Location.ValueBytes});
  }
  return Result;
}

std::optional<IntegerCallArguments>
neverd::integerCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                             llvm::ArrayRef<TypeRef> Parameters) {
  if (!ReturnType)
    return std::nullopt;
  const auto Placement = placeSourceParameters(A, F, ReturnType, Parameters);
  if (!Placement || Placement->Parameters.size() != Parameters.size())
    return std::nullopt;
  const auto Layout = getTargetRegInfo(A).integerArgumentLayout(F);
  const llvm::ArrayRef<uint64_t> IntegerRegisters =
      A == Arch::X86 ? llvm::ArrayRef<uint64_t>() : Layout.Registers;
  if (!Layout.SlotBytes)
    return std::nullopt;
  IntegerCallArguments Result;
  std::set<unsigned> Registers;
  if (Placement->ResultPointer) {
    // Only these conventions both place the hidden pointer first and return
    // it in the ordinary integer result register. Other result transports
    // need their own adapter; an address parameter alone proves none.
    if (F != BinaryFormat::COFF || (A != Arch::X64 && A != Arch::X86))
      return std::nullopt;
    const auto &Pointer = *Placement->ResultPointer;
    if (Pointer.ValueBytes != getTargetRegInfo(A).PointerSize)
      return std::nullopt;
    if (A == Arch::X64) {
      if (IntegerRegisters.empty() ||
          Pointer.Kind != SourceABICarrierKind::IntegerRegister ||
          Pointer.RegisterOffset != IntegerRegisters.front())
        return std::nullopt;
      Registers.insert(0);
      Result.Registers = 1;
    } else if (Pointer.Kind != SourceABICarrierKind::Stack ||
               Pointer.EntryStackOffset != Layout.EntryStackBase) {
      return std::nullopt;
    }
    Result.ResultPointer = 0;
    Result.Positions = 1;
  }
  for (size_t I = 0; I < Parameters.size(); ++I) {
    const auto &Type = Parameters[I];
    const auto &Pieces = Placement->Parameters[I];
    if (!Type || !Type->Size || Type->Kind == NdTypeKind::Float ||
        Pieces.empty())
      return std::nullopt;
    IntegerCallParameter Parameter;
    for (const auto &Piece : Pieces) {
      const auto &Location = Piece.Location;
      if (!Location.ValueBytes ||
          (Piece.Indirect &&
           (Pieces.size() != 1 || Piece.Offset != 0 ||
            Location.ValueBytes != getTargetRegInfo(A).PointerSize)))
        return std::nullopt;
      Parameter.Indirect = Piece.Indirect;
      unsigned Argument = 0;
      if (Location.Kind == SourceABICarrierKind::IntegerRegister) {
        const auto It = llvm::find(IntegerRegisters, Location.RegisterOffset);
        if (It == IntegerRegisters.end() ||
            Location.ValueBytes > Layout.SlotBytes)
          return std::nullopt;
        Argument = static_cast<unsigned>(It - IntegerRegisters.begin());
        Registers.insert(Argument);
        Result.Registers = std::max(Result.Registers, Argument + 1);
      } else if (Location.Kind == SourceABICarrierKind::Stack) {
        if (!Piece.Indirect && Type->Alignment > Layout.SlotBytes &&
            !(A == Arch::X86 && F == BinaryFormat::COFF))
          return std::nullopt;
        const int64_t Offset =
            Location.EntryStackOffset - Layout.EntryStackBase;
        if (Offset < 0 || Offset % Layout.SlotBytes)
          return std::nullopt;
        const uint64_t Slot = static_cast<uint64_t>(Offset) / Layout.SlotBytes;
        if (Slot >= static_cast<uint64_t>(limits::kMaxCallArgs))
          return std::nullopt;
        Argument = static_cast<unsigned>(IntegerRegisters.size() + Slot);
      } else {
        return std::nullopt;
      }
      for (unsigned Offset = 0; Offset < Location.ValueBytes;
           Offset += Layout.SlotBytes, ++Argument) {
        const unsigned Bytes =
            std::min<unsigned>(Layout.SlotBytes, Location.ValueBytes - Offset);
        if (Argument >= static_cast<unsigned>(limits::kMaxCallArgs) ||
            (!Piece.Indirect && Piece.Offset + Offset + Bytes > Type->Size))
          return std::nullopt;
        Parameter.Pieces.push_back(
            {Argument, static_cast<uint16_t>(Piece.Offset + Offset),
             static_cast<uint16_t>(Bytes)});
        Result.Positions = std::max(Result.Positions, Argument + 1);
      }
    }
    if (!Parameter.Indirect) {
      unsigned Covered = 0;
      for (const auto &Piece : Parameter.Pieces) {
        if (Piece.Offset != Covered)
          return std::nullopt;
        Covered += Piece.Bytes;
      }
      if (Covered != Type->Size)
        return std::nullopt;
    }
    Result.Parameters.push_back(std::move(Parameter));
  }
  // These counts describe a dense register prefix. Never let an ABI's
  // alignment gap become an invented argument-register read.
  if (Registers.size() != Result.Registers)
    return std::nullopt;
  return Result;
}
