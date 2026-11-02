//===- COFFRegistrationScalarLeaf.h - Scalar helper proof ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bound retained scalar helper paths and report their image dependencies.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_COFF_REGISTRATIONSCALARLEAF_H
#define NEVERD_LOADER_COFF_REGISTRATIONSCALARLEAF_H

#include "neverd/Common.h"
#include "neverd/loader/ExceptionCommon.h"

#include <map>
#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;

namespace coff_loader {
/// Prove a bounded cdecl scalar leaf cannot observe its caller's frame.
/// The original code and dynamic image reads remain in place. This grants
/// neither permission to copy the helper nor a constant result for its reads.
struct X86RegistrationScalarLeaf {
  std::vector<ExceptionAddressRange> CodeRanges;
  std::vector<ExceptionAddressRange> ImageReads;
  std::map<va_t, va_t> AbsoluteOperands;
};
std::optional<X86RegistrationScalarLeaf>
getCheckedX86RegistrationScalarLeaf(const BinaryImage &Image, va_t Entry);
} // namespace coff_loader
} // namespace neverd
#endif // NEVERD_LOADER_COFF_REGISTRATIONSCALARLEAF_H
