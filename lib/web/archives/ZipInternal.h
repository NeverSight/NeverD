//===- ZipInternal.h - ZIP record and payload boundary ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private ZIP32 admission and bounded payload decoding interfaces.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PackageArchive.h"

namespace neverd::web {
class BlobStore;
void readZipArchive(PackageArchive &Archive, uint64_t ExpandedBudget);
/// Append exactly one validated member or throw; the caller seals only after
/// every member has succeeded. Steps is shared across the entire archive.
void decodeZipMember(const Blob &Stored, uint16_t Method, uint64_t Size,
                     uint32_t CRC, BlobStore &Store, uint64_t &Steps);
} // namespace neverd::web
