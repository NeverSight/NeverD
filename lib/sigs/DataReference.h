//===- DataReference.h - Validated signature data operands ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Locates canonical external-data operands in mapped, readable storage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_DATAREFERENCE_H
#define NEVERD_SIGS_DATAREFERENCE_H

#include <cstdint>
#include <optional>

namespace neverd {
struct BinaryImage;
namespace sigs {
struct LocalDataAnchor;
std::optional<uint64_t> dataReferenceTarget(const BinaryImage &Img,
                                            uint64_t Start, uint32_t Offset,
                                            uint32_t Length);
/// Invert a parsed source-layout relation inside the same mapped data region.
/// The consumer and frozen anchor operands separately validate their extents.
std::optional<uint64_t> localDataReferenceBase(const BinaryImage &Img,
                                               uint64_t Target,
                                               const LocalDataAnchor &Anchor);
} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_DATAREFERENCE_H
