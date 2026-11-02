//===- LibCVariadicArguments.h - Header varargs -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The fixed integer prefix of a selected Windows variadic declaration.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCVARIADICARGUMENTS_H
#define NEVERD_LIBC_LIBCVARIADICARGUMENTS_H

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/TargetRegInfo.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::libc {

/// Widths of the fixed scalar parameters, one per machine argument position.
/// The tail has no declared type or count. This adapter does not authorize
/// fixed FP parameters, records, split integers, or another platform's varargs
/// convention. All consumers use the same admission before typing a call.
inline std::optional<std::vector<uint16_t>>
variadicCallArguments(const FunctionSym &Function, Arch A, BinaryFormat F) {
  if (!Function.IsVariadic ||
      (!Function.PrototypeFromHeader && !Function.PrototypeFromCLibrary) ||
      !Function.PlatformConvention || !Function.ParamEntryLocations.empty() ||
      (Function.CallConv != DebugCallConv::Unknown &&
       Function.CallConv != DebugCallConv::Cdecl) ||
      F != BinaryFormat::COFF ||
      (A != Arch::X86 && A != Arch::X64 && A != Arch::AArch64) ||
      Function.Params.empty() || Function.Params.size() > limits::kMaxCallArgs)
    return std::nullopt;
  const uint16_t Word = getTargetRegInfo(A).PointerSize;
  const auto Scalar = [Word](const TypeRef &Type) {
    return Type && Type->Size && Type->Size <= Word &&
           (Type->Kind != NdTypeKind::Ptr || Type->Size == Word) &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Ptr ||
            (Type->Kind == NdTypeKind::Struct && Type->IsEnum));
  };
  if (!Function.ReturnType || (Function.ReturnType->Kind != NdTypeKind::Void &&
                               !Scalar(Function.ReturnType)))
    return std::nullopt;
  std::vector<TypeRef> Types;
  std::vector<uint16_t> Widths;
  for (const auto &[Name, Type] : Function.Params) {
    if (!Scalar(Type))
      return std::nullopt;
    Types.push_back(Type);
    Widths.push_back(Type->Size);
  }
  const auto Layout = integerCallArguments(A, F, Function.ReturnType, Types);
  if (!Layout || Layout->Positions != Types.size())
    return std::nullopt;
  for (size_t I = 0; I < Types.size(); ++I) {
    const auto &Parameter = Layout->Parameters[I];
    if (Parameter.Indirect || Parameter.Pieces.size() != 1 ||
        Parameter.Pieces[0].Argument != I)
      return std::nullopt;
  }
  return Widths;
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCVARIADICARGUMENTS_H
