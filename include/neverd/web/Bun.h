//===- Bun.h - Qualified Bun graph extraction --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Qualified Bun graph extraction.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"

#include <limits>

namespace neverd::web {
inline constexpr std::string_view BunProfile = "bun-1.4.2-linux-x64-elf-v1";
inline constexpr std::string_view BunPrelinkedProfile =
    "bun-71d0d439-prelinked-linux-x64-elf-v1";
inline constexpr uint32_t MaxBunModules = 4096;
inline constexpr uint32_t MaxBunBuiltins = 4096;
inline constexpr uint64_t MaxBunNameBytes = 1024 * 1024;
inline constexpr uint64_t MaxBunDecodedSourceBytes = 64ULL * 1024 * 1024;
inline constexpr uint32_t NoBunIndex = std::numeric_limits<uint32_t>::max();

/// Every range is relative to the original container, never a host pathname.
/// Cache/map bodies retain raw evidence and are never loaded as code.
struct BunRegion {
  std::string ID, Kind, BlobHash;
  uint64_t Offset = 0;
  uint32_t Module = NoBunIndex;
  Blob Content;
};

struct BunModule {
  std::string ID, SourceArtifactID;
  uint32_t Contents = NoBunIndex, Name = NoBunIndex;
  uint32_t SourceMap = NoBunIndex, Bytecode = NoBunIndex;
  uint32_t ModuleInfo = NoBunIndex, BytecodeOrigin = NoBunIndex;
  uint8_t Encoding = 0, Loader = 0, Format = 0, Side = 0;
};

struct BunExtraction {
  std::string ID, ArtifactID;
  std::string Profile;
  std::string ContainerFormat, Platform, Architecture;
  uint64_t GraphOffset = 0, GraphSize = 0;
  uint32_t EntryPoint = 0, StartupCount = 0, Flags = 0;
  std::vector<BunModule> Modules;
  std::vector<BunRegion> Regions;
};

/// Explicit versioned layout selection, not producer/version authentication.
/// Failure publishes no partial extraction. The input remains inert.
BunExtraction extractBun(const Artifact &Input);
/// Exact layout contracts advertised by all transports, not producer claims.
std::vector<std::string> bunProfiles();
/// Decode only an already admitted module's stored text. UTF-16 lone
/// surrogates refuse UTF-8 projection; the original bytes remain available.
std::string bunSourceBytes(const BunExtraction &Extraction,
                           const BunModule &Module, uint64_t MaxBytes);
struct BunSourceRange {
  std::string Text;
  uint64_t Offset = 0, Size = 0;
};
/// Uses the same decoder as bunSourceBytes. Offset/Length select decoded UTF-8
/// scalar/CRLF boundaries; the result range uses original container bytes.
BunSourceRange bunSourceRange(const BunExtraction &Extraction,
                              const BunModule &Module, uint64_t Offset,
                              uint64_t Length, uint64_t MaxBytes);
} // namespace neverd::web
