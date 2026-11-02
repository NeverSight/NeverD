//===- DyldSharedCache.h - Owned shared-cache address space ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Immutable cache files, validated VM mappings, image identities and slide
/// recipes. Cache-relative targets are resolved before per-image analysis.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_DYLDSHAREDCACHE_H
#define NEVERD_LOADER_MACHO_DYLDSHAREDCACHE_H

#include "neverd/Common.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/support/LoadProgress.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::macho_loader {

struct DyldCacheMapping {
  va_t Address = 0;
  uint64_t Size = 0;
  uint64_t FileOffset = 0;
  uint64_t SlideOffset = 0;
  uint64_t SlideSize = 0;
  uint64_t Flags = 0;
  uint32_t MaxProtection = 0;
  uint32_t InitialProtection = 0;
  size_t FileIndex = 0;
};

struct DyldCacheImage {
  va_t Address = 0;
  std::string Path;
};

struct DyldCacheImageSegment {
  std::string Name;
  va_t Address = 0;
  uint64_t Size = 0;
  uint64_t FileOffset = 0;
  uint64_t FileSize = 0;
  uint32_t MaxProtection = 0;
  uint32_t InitialProtection = 0;
};

struct DyldCacheImageLayout {
  /// Borrowed from the owning cache. Unknown commands remain available to
  /// their semantic reader; parsing this layout does not interpret them.
  llvm::ArrayRef<uint8_t> HeaderBytes;
  std::vector<DyldCacheImageSegment> Segments;
  std::vector<Section> Sections;
};

struct DyldCacheFixup {
  va_t Slot = 0;
  va_t Target = 0;
  uint64_t EncodedValue = 0;
  uint32_t SlideVersion = 0;
  std::optional<MachOAuthenticatedPointer> Authentication;
};

enum class DyldCacheSelectorEncoding { RecordRelative, BaseRelative };

struct DyldCacheSelectorLayout {
  DyldCacheSelectorEncoding Encoding =
      DyldCacheSelectorEncoding::RecordRelative;
  va_t Base = 0;
};

class DyldSharedCache {
public:
  static bool hasMagic(llvm::StringRef Bytes);
  /// Main file followed by its table's subcaches in table order. A failed
  /// parse publishes no mappings. Returned byte views live with this object.
  static llvm::Expected<DyldSharedCache>
  parse(std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files,
        const LoadProgress &Progress = {});
  static llvm::Expected<DyldSharedCache>
  open(const std::filesystem::path &Path, const LoadProgress &Progress = {});

  llvm::ArrayRef<DyldCacheMapping> mappings() const { return Mappings; }
  llvm::ArrayRef<DyldCacheImage> images() const { return Images; }
  va_t baseAddress() const { return Base; }
  /// Hash each exact owned input in main-file/subcache order. The prepared
  /// Mach-O view is not the input's identity.
  std::vector<std::array<uint8_t, 32>>
  fileDigests(const LoadProgress &Progress = {}) const;
  const DyldCacheMapping *mappingFor(va_t Address) const;
  std::optional<llvm::ArrayRef<uint8_t>> readBytes(va_t Address,
                                                   uint64_t Size) const;
  llvm::Expected<DyldCacheImageLayout> readImageLayout(size_t ImageIndex) const;
  /// A bounded contiguous view for ordinary Mach-O semantic readers. Keeps
  /// original VAs but packs file ranges and referenced symbol strings. This
  /// is an analysis projection, not a standalone or correctly signed dylib.
  llvm::Expected<std::vector<uint8_t>>
  prepareImage(size_t ImageIndex, const LoadProgress &Progress = {}) const;
  /// Addressing used by cached compact Objective-C method selectors. Missing
  /// optimization metadata is distinct from a malformed or unknown layout.
  llvm::Expected<std::optional<DyldCacheSelectorLayout>>
  objcSelectorLayout() const;
  /// Objective-C header-info indices have their own ordering; they are not
  /// indices into the cache image catalogue.
  llvm::Expected<va_t> objcImageAddress(uint32_t Index) const;
  /// Translate an embedded Mach-O file offset through that image's segment
  /// commands and the cache mappings. It is not an offset from its header,
  /// nor necessarily an offset in the file containing that header.
  llvm::Expected<llvm::ArrayRef<uint8_t>>
  readImageFileBytes(size_t ImageIndex, uint64_t Offset, uint64_t Size) const;
  /// Decode a complete mapping transactionally. Preserve original bytes and
  /// signing recipes; no offline reader can manufacture a process's PAC.
  llvm::Expected<std::vector<DyldCacheFixup>>
  readSlideFixups(size_t MappingIndex, const LoadProgress &Progress = {}) const;
  /// Decode the pages intersecting a VM range, retaining every fixup whose
  /// storage overlaps it. Validation still covers each complete selected page.
  llvm::Expected<std::vector<DyldCacheFixup>>
  readSlideFixupsInRange(va_t Address, uint64_t Size,
                         const LoadProgress &Progress = {}) const;

private:
  struct ObjCOptimization {
    va_t Address = 0;
    uint32_t Version = 0;
    bool Legacy = false;
    llvm::ArrayRef<uint8_t> Data;
  };
  llvm::Expected<std::optional<ObjCOptimization>> readObjCOptimization() const;
  llvm::Expected<std::vector<DyldCacheFixup>>
  readMappingSlides(size_t MappingIndex, va_t First, va_t End,
                    const LoadProgress &Progress) const;
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Files;
  std::vector<DyldCacheMapping> Mappings;
  std::vector<DyldCacheImage> Images;
  va_t Base = 0;
};

} // namespace neverd::macho_loader

#endif // NEVERD_LOADER_MACHO_DYLDSHAREDCACHE_H
