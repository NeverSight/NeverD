//===- HeaderPrototype.h - Qualified source declarations ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared admission of portable source spellings after call-layout checks.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_HEADERPROTOTYPE_H
#define NEVERD_BACKEND_C_HEADERPROTOTYPE_H

#include "LibraryPrototype.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"
#include "neverd/libc/LibCRecordArguments.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <optional>
#include <set>

namespace neverd::c_library {
/// The outer convention of a qualified platform header declaration.
inline llvm::StringRef headerConvention(const FunctionSym &Function, Arch A) {
  return A == Arch::X86 && Function.CallConv == DebugCallConv::Stdcall
             ? "__attribute__((stdcall)) "
             : "";
}

/// A named value record occupies consecutive integer positions or a single
/// pointer to its caller-provided copy. Both emitters use this target layout.
inline std::optional<libc::RecordCallArguments>
standaloneIntegerRecord(const FunctionSym &Function,
                        const CEmitterOptions &Options) {
  if ((Options.TheArch != Arch::X86 && Options.TheArch != Arch::X64 &&
       Options.TheArch != Arch::AArch64) ||
      Options.Format != BinaryFormat::COFF || !Options.EmitIncludes ||
      !Options.RecordDefinitions || !Function.PrototypeFromHeader ||
      !Function.ParamEntryLocations.empty() || !Function.ReturnType ||
      !libc::hasRecordCallParameters(Function, Options.libcVariant()))
    return std::nullopt;
  const TypeRef &Result = Function.ReturnType;
  if (Result->Kind != NdTypeKind::Void && Result->Kind != NdTypeKind::Ptr &&
      Result->Kind != NdTypeKind::Int && !Result->IsEnum)
    return std::nullopt;
  if (Result->Size > getTargetRegInfo(Options.TheArch).PointerSize)
    return std::nullopt;
  auto Layout = libc::recordCallArguments(
      Function, Options.TheArch, Options.Format, Options.libcVariant());
  if (!Layout || Layout->ResultPointer || Layout->FloatingResult)
    return std::nullopt;
  unsigned Position = 0;
  for (size_t I = 0; I < Layout->Parameters.size(); ++I) {
    const auto &Type = Function.Params[I].second;
    if (libc::isRecordCallParameter(Function, I, Options.libcVariant()) &&
        Type->SourceName.empty())
      return std::nullopt;
    const auto &P = Layout->Parameters[I];
    if (P.Pieces.empty())
      return std::nullopt;
    // Microsoft x64 passes a non-register-sized C record by pointing to a
    // copy. The carrier covers the pointer, while the declaration owns the
    // size of the object read through it. C++ copy semantics need more proof.
    if (P.Indirect &&
        (Options.TheArch != Arch::X64 ||
         !libc::isRecordCallParameter(Function, I, Options.libcVariant()) ||
         Type->Passing != NdRecordPassing::ByValue || P.Pieces.size() != 1))
      return std::nullopt;
    unsigned Offset = 0;
    for (const auto &Piece : P.Pieces) {
      if (Piece.Floating || Piece.BitOffset || Piece.CarrierBytes ||
          !Piece.Bytes || Piece.Argument != Position || Piece.Offset != Offset)
        return std::nullopt;
      Offset += Piece.Bytes;
      ++Position;
    }
    const unsigned ExpectedBytes =
        P.Indirect ? getTargetRegInfo(Options.TheArch).PointerSize : Type->Size;
    if (Offset != ExpectedBytes)
      return std::nullopt;
  }
  return Position == Layout->Positions ? Layout : std::nullopt;
}

template <typename WidthOf>
bool matchesIntegerRecordOperands(const IntegerCallArguments &Layout,
                                  size_t Count, WidthOf Width) {
  if (Count != Layout.Positions)
    return false;
  for (const auto &Parameter : Layout.Parameters)
    for (const auto &Piece : Parameter.Pieces)
      if (Width(Piece.Argument) < Piece.Bytes)
        return false;
  return true;
}

/// Source qualifiers belong to the selected header or local declaration,
/// not to the machine carrier type. Reuse that spelling after checking the
/// call layout, and never replace an aggregate's lowered ABI with its source
/// signature. The outer calling convention is supplied separately.
inline llvm::StringRef portableScalarDeclaration(const FunctionSym &Function,
                                                 llvm::StringRef Name) {
  if ((!Function.PrototypeFromHeader && !Function.PrototypeFromCLibrary &&
       Function.PrototypeLibrary != UserPrototypeLibrary) ||
      !Function.PlatformConvention || !Function.ParamEntryLocations.empty() ||
      (Function.CallConv != DebugCallConv::Unknown &&
       Function.CallConv != DebugCallConv::Cdecl &&
       Function.CallConv != DebugCallConv::Stdcall) ||
      Function.Name != Name)
    return {};
  const auto Scalar = [](const TypeRef &Type) {
    return Type &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Float ||
            Type->Kind == NdTypeKind::Ptr ||
            (Type->Kind == NdTypeKind::Struct && Type->IsEnum));
  };
  if (!Function.ReturnType || (Function.ReturnType->Kind != NdTypeKind::Void &&
                               !Scalar(Function.ReturnType)))
    return {};
  std::set<std::string, std::less<>> Enumerations;
  const auto EnumName = [&](const TypeRef &Type) {
    if (Type && Type->Kind == NdTypeKind::Struct && Type->IsEnum) {
      if (!Type->SourceName.empty())
        Enumerations.insert(Type->SourceName);
      if (!Type->TypedefName.empty())
        Enumerations.insert(Type->TypedefName);
    }
  };
  EnumName(Function.ReturnType);
  for (const auto &[Parameter, Type] : Function.Params) {
    if (!Scalar(Type))
      return {};
    EnumName(Type);
  }
  return selfContainedDeclaration(Function.PortablePrototypeDeclaration, Name,
                                  Enumerations)
             ? llvm::StringRef(Function.PortablePrototypeDeclaration)
             : llvm::StringRef();
}

/// A call conversion uses the same source spelling as its admitted prototype.
/// The caller has already checked the operand's scalar placement and width.
inline llvm::StringRef portableScalarParameterType(const FunctionSym &Function,
                                                   std::size_t Index) {
  if (Function.PortableParameterTypes.size() != Function.Params.size() ||
      Index >= Function.Params.size() ||
      portableScalarDeclaration(Function, Function.Name).empty())
    return {};
  return Function.PortableParameterTypes[Index];
}

} // namespace neverd::c_library

#endif // NEVERD_BACKEND_C_HEADERPROTOTYPE_H
