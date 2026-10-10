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

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

#include <filesystem>
#include <map>
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
  /// Borrow immutable bytes for this call only. The resulting image owns its
  /// data. Formats without a buffer reader reject this explicitly.
  virtual llvm::Expected<BinaryImage> loadBuffer(llvm::MemoryBufferRef Buffer);

  /// Limit PE unwind/language materialization to these entries on the next
  /// load.  Empty means decode every runtime-function record.  Exclusive-end
  /// ranges from the rest of `.pdata` are still recorded.
  void restrictFunctions(std::set<va_t> Entries) {
    RestrictFunctionEntries = std::move(Entries);
  }

  void setARMFunctionModes(std::map<va_t, InstructionMode> Modes) {
    ARMFunctionModes = std::move(Modes);
  }

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

  /// File and buffer entry points share the same format parser.
  static llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
  readFileBuffer(const std::filesystem::path &Path, BinaryFormat Fmt) {
    auto BufOrErr = llvm::MemoryBuffer::getFile(pathToUTF8(Path));
    if (!BufOrErr)
      return llvm::make_error<llvm::StringError>(
          std::string(getFormatTag(Fmt)) + ": cannot open " + pathToUTF8(Path),
          llvm::inconvertibleErrorCode());
    return std::move(*BufOrErr);
  }

  /// Restricted PE loads already retain section data and may omit Img.Raw.
  static void initializeImage(llvm::MemoryBufferRef Buffer, BinaryImage &Img,
                              BinaryFormat Fmt, bool CopyRaw = true) {
    Img.Format = Fmt;
    Img.InputFileSHA256 = sha256(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(Buffer.getBufferStart()),
        Buffer.getBufferSize()));
    if (CopyRaw)
      Img.Raw.assign(reinterpret_cast<const uint8_t *>(Buffer.getBufferStart()),
                     reinterpret_cast<const uint8_t *>(Buffer.getBufferEnd()));
  }

  /// Retain the existing protected helper for out-of-tree loader subclasses.
  static llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
  readFileInto(const std::filesystem::path &Path, BinaryImage &Img,
               BinaryFormat Fmt, bool CopyRaw = true) {
    auto Buffer = readFileBuffer(Path, Fmt);
    if (!Buffer)
      return Buffer.takeError();
    initializeImage((*Buffer)->getMemBufferRef(), Img, Fmt, CopyRaw);
    return std::move(*Buffer);
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
    case BinaryFormat::Raw:
      return "binary";
    default:
      return "loader";
    }
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_LOADER_H
