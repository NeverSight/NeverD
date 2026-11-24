//===- LibCArityPrototype.h - Declarations from arity tables ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The declaration of a C library routine of only floating arguments, as
/// its arity table counts them, where no header declares it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCARITYPROTOTYPE_H
#define NEVERD_LIBC_LIBCARITYPROTOTYPE_H

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCVariant.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace neverd::libc {

/// The declaration of the C library routine \p Name of \p Variant that takes
/// only floating arguments, as its arity table counts them (libcArity), for
/// an image whose platform headers declare it nowhere NeverD reads: an MSVC
/// image's UCRT math names `log` and `exp` by its import table alone.  The
/// tables list such a routine by its floating return (floatReturnBytes), of
/// its arguments' width.  A routine they give no row, or whose arguments
/// include an integer or a pointer, whose type no count tells, has none.
inline std::optional<FunctionSym> arityFloatPrototype(std::string_view Name,
                                                      LibCVariant Variant) {
  const std::optional<LibCArity> Arity = libcArityForSymbol(Name, Variant);
  if (!Arity || Arity->IntArgs != 0 || Arity->FpArgs <= 0)
    return std::nullopt;
  const uint16_t Bytes = floatReturnBytes(*Arity);
  if (!Bytes)
    return std::nullopt;
  FunctionSym Function;
  Function.Name = std::string(Name);
  Function.ReturnType = NdType::makeFloat(Bytes);
  Function.CallConv = DebugCallConv::Cdecl;
  Function.PrototypeFromCLibrary = true;
  for (int I = 0; I < Arity->FpArgs; ++I)
    Function.Params.emplace_back(std::string(), NdType::makeFloat(Bytes));
  return Function;
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCARITYPROTOTYPE_H
