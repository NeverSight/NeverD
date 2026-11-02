//===- DyldCacheLoad.h - Shared-cache selection contracts -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared loader, CLI and SDK spellings for selecting a cached image.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_DYLDCACHELOAD_H
#define NEVERD_LOADER_MACHO_DYLDCACHELOAD_H

namespace neverd::macho_loader::cache_load {
#define NEVERD_CACHE_LOAD_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/loader/MachO/DyldCacheLoad.def"
} // namespace neverd::macho_loader::cache_load

#endif // NEVERD_LOADER_MACHO_DYLDCACHELOAD_H
