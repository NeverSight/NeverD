//===- ComparisonInitializers.h - Initial reference hints ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captures relocation-marked function initializers without granting object,
/// runtime pointer, or function-identity authority to their numeric values.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_COMPARISONINITIALIZERS_H
#define NEVERD_SDK_CAPI_COMPARISONINITIALIZERS_H

#include "neverd/Common.h"
#include "neverd/diff/Program.h"

#include <optional>
#include <set>
#include <vector>

namespace neverd {
struct BinaryImage;
namespace sdk {

std::optional<diff::ComparisonState> readComparisonInitializers(
    const BinaryImage &Image, const std::set<va_t> &Functions,
    uint64_t AvailableBytes, const diff::ComparisonControl &Control,
    std::vector<diff::CodePointerInitializer> &Result);

} // namespace sdk
} // namespace neverd

#endif // NEVERD_SDK_CAPI_COMPARISONINITIALIZERS_H
