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
#include <set>

namespace neverd::c_library {
/// A standalone Windows ARM64 prototype may spell a by-value record when
/// each source parameter occupies one integer register in declaration order.
/// The target classifier distinguishes these records from HFAs, register
/// pairs, indirect copies and hidden results.
inline bool standaloneRegisterRecord(const FunctionSym &Function,
                                     const CEmitterOptions &Options) {
  if (Options.TheArch != Arch::AArch64 ||
      Options.Format != BinaryFormat::COFF || !Options.EmitIncludes ||
      !Options.RecordDefinitions || !Function.PrototypeFromHeader ||
      !Function.ParamEntryLocations.empty() || !Function.ReturnType ||
      !libc::hasRecordCallParameters(Function, Options.libcVariant()))
    return false;
  const TypeRef &Result = Function.ReturnType;
  if (Result->Kind != NdTypeKind::Void && Result->Kind != NdTypeKind::Ptr &&
      Result->Kind != NdTypeKind::Int && !Result->IsEnum)
    return false;
  if (Result->Size > getTargetRegInfo(Options.TheArch).PointerSize)
    return false;
  const auto Layout = libc::recordCallArguments(
      Function, Options.TheArch, Options.Format, Options.libcVariant());
  if (!Layout || Layout->ResultPointer || Layout->FloatingResult ||
      Layout->Registers != Function.Params.size() ||
      Layout->Positions != Function.Params.size())
    return false;
  for (size_t I = 0; I < Layout->Parameters.size(); ++I) {
    const auto &Type = Function.Params[I].second;
    if (libc::isRecordCallParameter(Function, I, Options.libcVariant()) &&
        Type->SourceName.empty())
      return false;
    const auto &P = Layout->Parameters[I];
    if (P.Indirect || P.Pieces.size() != 1 || P.Pieces[0].Floating ||
        P.Pieces[0].Argument != I || P.Pieces[0].Offset ||
        P.Pieces[0].Bytes != Type->Size)
      return false;
  }
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
