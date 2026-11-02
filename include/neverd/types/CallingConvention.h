//===- CallingConvention.h - Source function conventions -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The source convention shared by local declarations and IR callback types.
/// This carries declaration evidence, not a recovered machine calling ABI.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_CALLINGCONVENTION_H
#define NEVERD_TYPES_CALLINGCONVENTION_H

#include <cstdint>

namespace neverd::types {

enum class CallingConvention : uint8_t {
#define NEVERD_CALLING_CONVENTION(Name, C, Rust, IDA, Wide, Pops) Name,
#include "neverd/types/CallingConventions.def"
};

} // namespace neverd::types

#endif // NEVERD_TYPES_CALLINGCONVENTION_H
