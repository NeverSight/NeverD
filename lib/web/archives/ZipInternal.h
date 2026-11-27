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

#include "../PathPolicy.h"

#include "neverd/web/PackageArchive.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::web {
class BlobStore;
void readZipArchive(PackageArchive &Archive, uint64_t ExpandedBudget,
                    std::string (*PathKey)(std::string_view) = archivePathKey,
                    bool Jar = false, llvm::function_ref<bool()> Stop = {});
/// Append exactly one validated member or throw; the caller seals only after
/// every member has succeeded. Steps is shared across the entire archive.
void decodeZipMember(const Blob &Stored, uint16_t Method, uint64_t Size,
                     uint32_t CRC, BlobStore &Store, uint64_t &Steps,
                     llvm::function_ref<bool()> Stop);
} // namespace neverd::web
