//===- LibCTablePrototype.h - Declarations from prototype tables -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The declaration NeverD's C library prototype table gives a routine, where
/// no header the image joins declares it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCTABLEPROTOTYPE_H
#define NEVERD_LIBC_LIBCTABLEPROTOTYPE_H

#include "neverd/Common.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/libc/LibCNames.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace neverd::libc {

/// The type the prototype table's spelling \p Spelling names for a \p Format
/// image of \p A: a pointer for any spelling with a `*`, the C integer types
/// and their fixed-width names at their widths (`long` 4 bytes on Windows
/// and ILP32, 8 elsewhere), the UCRT's pointer and enumeration typedefs, and
/// `float`, `double` and `void`.  Null for a spelling of no certain type.
inline TypeRef tableType(std::string_view Spelling, BinaryFormat Format,
                         Arch A) {
  const uint16_t Pointer = getTargetRegInfo(A).PointerSize;
  const uint16_t Long = Format == BinaryFormat::COFF || Pointer == 4 ? 4 : 8;
  const auto Integer = [](uint16_t Bytes, bool Signed) {
    return NdType::makeInt(Bytes, Signed);
  };
  const auto PointerType = [&] {
    TypeRef Type = NdType::makePtr();
    Type->Size = Pointer;
    return Type;
  };
  if (Spelling.find('*') != std::string_view::npos || Spelling == "_locale_t" ||
      Spelling == "_invalid_parameter_handler" ||
      Spelling == "_tls_callback_type")
    return PointerType();
  if (Spelling == "void")
    return NdType::makeVoid();
  if (Spelling == "int" || Spelling == "int32_t" ||
      Spelling.starts_with("enum ") || Spelling == "EXCEPTION_DISPOSITION")
    return Integer(4, true);
  if (Spelling == "unsigned int" || Spelling == "uint32_t")
    return Integer(4, false);
  if (Spelling == "long")
    return Integer(Long, true);
  if (Spelling == "unsigned long")
    return Integer(Long, false);
  if (Spelling == "long long" || Spelling == "int64_t" ||
      Spelling == "intmax_t")
    return Integer(8, true);
  if (Spelling == "unsigned long long" || Spelling == "uint64_t" ||
      Spelling == "uintmax_t")
    return Integer(8, false);
  if (Spelling == "size_t" || Spelling == "uintptr_t")
    return Integer(Pointer, false);
  if (Spelling == "ssize_t" || Spelling == "ptrdiff_t" ||
      Spelling == "intptr_t")
    return Integer(Pointer, true);
  if (Spelling == "float")
    return NdType::makeFloat(4);
  if (Spelling == "double")
    return NdType::makeFloat(8);
  return nullptr;
}

/// The declaration of the routine \p Name that the C library prototype
/// table gives a \p Format image of \p A (libcPrototype), for an image whose
/// headers declare it nowhere NeverD reads: an ARM32 Windows image joins no
/// Windows header library, and its UCRT imports had only their parameter
/// count, which matched `__stdio_common_vfprintf`'s 64-bit options to one
/// register.  A variadic routine, or one with a spelling of no certain type
/// (tableType), has none.
inline std::optional<FunctionSym> tablePrototype(std::string_view Name,
                                                 BinaryFormat Format, Arch A) {
  const LibCPrototype *Prototype = libcPrototype(Name, Format);
  if (!Prototype || Prototype->Variadic)
    return std::nullopt;
  FunctionSym Function;
  Function.Name = std::string(Name);
  Function.ReturnType = tableType(Prototype->Return, Format, A);
  if (!Function.ReturnType)
    return std::nullopt;
  for (uint8_t I = 0; I < Prototype->ParamCount; ++I) {
    TypeRef Type = tableType(Prototype->Params[I], Format, A);
    if (!Type || Type->Kind == NdTypeKind::Void)
      return std::nullopt;
    Function.Params.emplace_back(std::string(), std::move(Type));
  }
  Function.CallConv = Prototype->Winapi && A == Arch::X86
                          ? DebugCallConv::Stdcall
                          : DebugCallConv::Cdecl;
  Function.PrototypeFromCLibrary = true;
  return Function;
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCTABLEPROTOTYPE_H
