//===- CodeViewCallingConvention.h - PDB function conventions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Translate CodeView conventions for local types and debug callback types.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_CODEVIEWCALLINGCONVENTION_H
#define NEVERD_TYPES_CODEVIEWCALLINGCONVENTION_H

#include "neverd/types/CallingConvention.h"

#include "llvm/DebugInfo/CodeView/CodeView.h"

#include <optional>

namespace neverd::types {

inline std::optional<CallingConvention>
codeViewCallingConvention(llvm::codeview::CallingConvention Convention,
                          unsigned PointerBytes) {
  using CC = llvm::codeview::CallingConvention;
  CallingConvention Result;
  switch (Convention) {
  case CC::NearC:
  case CC::FarC:
    Result = CallingConvention::Cdecl;
    break;
  case CC::NearStdCall:
  case CC::FarStdCall:
    Result = CallingConvention::Stdcall;
    break;
  case CC::NearFast:
  case CC::FarFast:
    Result = CallingConvention::Fastcall;
    break;
  case CC::ThisCall:
    Result = CallingConvention::Thiscall;
    break;
  case CC::NearVector:
    Result = CallingConvention::Vectorcall;
    break;
  default:
    return std::nullopt;
  }
  if (PointerBytes == 8)
    switch (Result) {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide, Pops)              \
  case CallingConvention::Name:                                                \
    return Wide ? Result : CallingConvention::Unspecified;
#include "neverd/types/CallingConventions.def"
    }
  return Result;
}

} // namespace neverd::types

#endif // NEVERD_TYPES_CODEVIEWCALLINGCONVENTION_H
