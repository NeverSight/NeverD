//===- PEImage.h - PE32+ input images ---------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_FORMAT_PE_PEIMAGE_H
#define NEVERD_UNPACK_FORMAT_PE_PEIMAGE_H

#include "../../core/Capture.h"
#include "../../core/Image.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Casting.h"

#include <memory>

namespace neverd::unpack::pe {
namespace value {
#define NEVERD_UNPACK_PE_VALUE(Name, Value)                                    \
  inline constexpr uint64_t Name = Value;
#define NEVERD_UNPACK_PE_BYTES(Name, ...)                                      \
  inline constexpr uint8_t Name[] = {__VA_ARGS__};
#include "PE.def"
#undef NEVERD_UNPACK_PE_BYTES
#undef NEVERD_UNPACK_PE_VALUE
} // namespace value
namespace text {
#define NEVERD_UNPACK_PE_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "PE.def"
#undef NEVERD_UNPACK_PE_TEXT
} // namespace text

/// Section facts beyond the mapped extent every container has.
struct Section {
  uint32_t VirtualSize, Characteristics;
};

/// Header facts of one PE32+ file. Offsets are file offsets.
struct Headers {
  uint32_t SizeOfHeaders, SectionAlignment, FileAlignment;
  uint32_t FileHeaderOffset, OptionalHeaderOffset, SectionTableOffset;
  /// One entry per region of the image, in the same order.
  std::vector<Section> Sections;
  std::vector<llvm::object::data_directory> Directories;
};

class Image final : public InputImage {
public:
  /// Decode and validate the headers that unpacking relies on. Sections must
  /// be ordered, disjoint, page aligned and inside the image; anything else
  /// is an error rather than a guessed layout.
  static llvm::Expected<std::unique_ptr<Image>>
  read(llvm::ArrayRef<uint8_t> File);
  static bool classof(const InputImage *I) {
    return I->format() == FormatKind::PE64;
  }
  const Headers &headers() const { return H; }
  /// The header's directory \p Index; empty when the header ends before it.
  llvm::object::data_directory directory(unsigned Index) const;

private:
  explicit Image(llvm::ArrayRef<uint8_t> File)
      : InputImage(FormatKind::PE64, File) {}
  Headers H{};
};

/// Write the observed memory as a PE32+ file with the recovered entry point
/// and a new import directory over the cells the program already uses. The
/// result is
/// fixed at the observed base: relocation metadata for generated content is
/// unknown and is therefore removed, not assumed.
llvm::Expected<RebuiltImage>
rebuild(const Image &Input, const Capture &Observed, const RebuildPlan &Plan);

/// Append a separately linked fixed-address runtime. This owner alone rebases
/// its relative metadata and combines imports, unwind records and TLS startup.
llvm::Expected<RebuiltImage> appendRuntime(RebuiltImage Output,
                                           llvm::ArrayRef<uint8_t> Runtime);

/// Dispatch process attach to the recovered entry, retaining the original
/// DLL entry for other loader notifications. Returns the actual header RVA.
llvm::Expected<uint64_t> rebuildEntry(const Image &Input,
                                      const Capture &Observed,
                                      uint64_t MetadataRVA,
                                      std::vector<uint8_t> &Metadata);

struct DelayImportRange {
  uint64_t Begin, End;
};
struct DelayImportState {
  std::vector<DelayImportRange> Metadata;
  struct Binding {
    uint64_t RVA;
    const ExportBinding *Target;
  };
  std::vector<Binding> Resolved;
};
/// Clear process-local delay handles and bound caches. Validated descriptors
/// own exact cells: resolved exports need fresh bindings, while internal
/// thunks retain their lazy behavior. Other delay metadata cannot become IAT.
llvm::Expected<DelayImportState>
restoreDelayImports(const Image &Input, const Capture &Observed,
                    llvm::MutableArrayRef<uint8_t> Memory);

/// Recover a replaced TLS directory from allocation identity and observed
/// callback entries. Ambiguous records are errors, not arbitrary choices.
llvm::Expected<std::optional<uint64_t>>
recoverTLSDirectory(const Image &Input, const Capture &Observed);

struct TLSRebuild {
  uint64_t DirectoryRVA = 0, MaterializedCallbacks = 0;
  /// Restoration can need executable code even without a completed callback.
  bool HasCode = false;
};
/// Validate and recover TLS metadata, then restore captured main-thread bytes
/// and adapt initializers whose process-attach effects are already captured.
/// Other TLS reasons retain the original callback and loader template.
llvm::Expected<TLSRebuild> rebuildTLS(const Image &Input,
                                      const Capture &Observed,
                                      uint64_t MetadataRVA,
                                      std::vector<uint8_t> &Metadata);
} // namespace neverd::unpack::pe
#endif
