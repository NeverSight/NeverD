//===- SBFSourceLimits.h - Portable source backend limits ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_EMIT_SBFSOURCELIMITS_H
#define NEVERD_SBF_EMIT_SBFSOURCELIMITS_H

#include <cstddef>

namespace neverd::sbf {

#define SBF_SOURCE_LIMIT(NAME, VALUE) inline constexpr size_t k##NAME = VALUE;
#include "neverd/sbf/emit/SBFSourceLimits.def"

} // namespace neverd::sbf

#endif // NEVERD_SBF_EMIT_SBFSOURCELIMITS_H
