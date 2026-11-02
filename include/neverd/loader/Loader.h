//===- Loader.h - Format loader interface and factory ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The abstract base every format-specific binary parser implements, and the
/// auto-detection factory that picks one for a file.  Follows LLVM's factory
/// pattern (cf. llvm::object::ObjectFile::createObjectFile).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LOADER_H
#define NEVERD_LOADER_LOADER_H

#include "neverd/Common.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/InputDigest.h"
#include "neverd/loader/LoadCandidate.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/LoadProgress.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

// ===--------------------------------------------------------------------===//
// Loader — abstract base for format-specific binary parsers
// ===--------------------------------------------------------------------===//

class Loader {
public:
  virtual ~Loader() = default;
  virtual llvm::Expected<BinaryImage>
  load(const std::filesystem::path &Path) = 0;
  /// Borrow immutable bytes for this call only. The resulting image owns its
  /// data. Formats without a buffer reader reject this explicitly.
  virtual llvm::Expected<BinaryImage> loadBuffer(llvm::MemoryBufferRef Buffer);

  /// Synchronous controlled entry points. Callbacks are borrowed for this
  /// call only and cleared before return, including failure/cancellation.
  /// An observed stop returns operation_canceled, never a partial image.
  llvm::Expected<BinaryImage>
  loadWithProgress(const std::filesystem::path &Path,
                   const LoadProgress &Progress);
  llvm::Expected<BinaryImage>
  loadBufferWithProgress(llvm::MemoryBufferRef Buffer,
                         const LoadProgress &Progress);

  /// Limit PE unwind/language materialization to these entries on the next
  /// load.  Empty means decode every runtime-function record.  Exclusive-end
  /// ranges from the rest of `.pdata` are still recorded.
  void restrictFunctions(std::set<va_t> Entries) {
    RestrictFunctionEntries = std::move(Entries);
  }

  void setARMFunctionModes(std::map<va_t, InstructionMode> Modes) {
    ARMFunctionModes = std::move(Modes);
  }

  /// Whether the next load reads the image's static symbol table, or reads
  /// the image as if stripped (BinaryImage::ReadsStaticSymbols).
  void setReadsStaticSymbols(bool Reads) { ReadsStaticSymbols = Reads; }

  /// Auto-detect the binary format from file content and return the
  /// appropriate loader.  Follows LLVM's factory pattern
  /// (cf. llvm::object::ObjectFile::createObjectFile).
  static std::unique_ptr<Loader> create(const std::filesystem::path &Path);

  /// Why create(Path) found no loader for \p Path: no format it reads, or
  /// only the file's name ties it to one, which the user has to choose.
  static std::string describeRefusal(const std::filesystem::path &Path);

  /// Create a loader for a known format.
  static std::unique_ptr<Loader> create(BinaryFormat Format);

protected:
  std::set<va_t> RestrictFunctionEntries;
  std::map<va_t, InstructionMode> ARMFunctionModes;
  bool ReadsStaticSymbols = true;
  LoadProgress Progress;

  /// File and buffer entry points share the same format parser. The optional
  /// control preserves the existing protected helpers for external subclasses.
  static llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
  readFileBuffer(const std::filesystem::path &Path, BinaryFormat Fmt,
                 const LoadProgress &Progress = {});

  /// Restricted PE loads already retain section data and may omit Img.Raw.
  static void initializeImage(llvm::MemoryBufferRef Buffer, BinaryImage &Img,
                              BinaryFormat Fmt, bool CopyRaw = true,
                              const LoadProgress &Progress = {});

  /// Copy a byte span with checkpoints, without a second full-size temporary.
  static void copyImageBytes(llvm::ArrayRef<uint8_t> Bytes,
                             std::vector<uint8_t> &Destination,
                             const LoadProgress &Progress = {});

  static llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
  readFileInto(const std::filesystem::path &Path, BinaryImage &Img,
               BinaryFormat Fmt, bool CopyRaw = true,
               const LoadProgress &Progress = {});

private:
  static const char *getFormatTag(BinaryFormat Fmt) {
    switch (Fmt) {
    case BinaryFormat::ELF:
      return "elf";
    case BinaryFormat::COFF:
      return "coff";
    case BinaryFormat::MachO:
      return "macho";
    case BinaryFormat::EVM:
      return kEVMArchName.data();
    case BinaryFormat::Raw:
      return "binary";
    default:
      return "loader";
    }
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_LOADER_H
