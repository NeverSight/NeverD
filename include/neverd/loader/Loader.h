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

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace neverd {

// ===--------------------------------------------------------------------===//
// Loader — abstract base for format-specific binary parsers
// ===--------------------------------------------------------------------===//

class Loader {
public:
  virtual ~Loader() = default;
  virtual llvm::Expected<BinaryImage>
  load(const std::filesystem::path &Path) = 0;

  /// Limit PE unwind/language materialization to these entries on the next
  /// load.  Empty means decode every runtime-function record.  Exclusive-end
  /// ranges from the rest of `.pdata` are still recorded.
  void restrictFunctions(std::set<va_t> Entries) {
    RestrictFunctionEntries = std::move(Entries);
  }

  /// Auto-detect the binary format from file content and return the
  /// appropriate loader.  Follows LLVM's factory pattern
  /// (cf. llvm::object::ObjectFile::createObjectFile).
  static std::unique_ptr<Loader> create(const std::filesystem::path &Path);

  /// Create a loader for a known format.
  static std::unique_ptr<Loader> create(BinaryFormat Format);

protected:
  std::set<va_t> RestrictFunctionEntries;

  /// Read a file into a MemoryBuffer.  Copy raw bytes into \p Img.Raw unless
  /// \p CopyRaw is false.  `--func` PE loads already keep section bytes in
  /// Segment.Data; a second image-wide copy adds load time and memory use.
  /// Returns the buffer or an error.  Shared by all format loaders.
  static llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
  readFileInto(const std::filesystem::path &Path, BinaryImage &Img,
               BinaryFormat Fmt, bool CopyRaw = true) {
    auto BufOrErr = llvm::MemoryBuffer::getFile(Path.string());
    if (!BufOrErr)
      return llvm::make_error<llvm::StringError>(
          std::string(getFormatTag(Fmt)) + ": cannot open " + Path.string(),
          llvm::inconvertibleErrorCode());
    auto &Buf = *BufOrErr;
    Img.Format = Fmt;
    if (CopyRaw)
      Img.Raw.assign(reinterpret_cast<const uint8_t *>(Buf->getBufferStart()),
                     reinterpret_cast<const uint8_t *>(Buf->getBufferEnd()));
    return std::move(Buf);
  }

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
    default:
      return "loader";
    }
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_LOADER_H
