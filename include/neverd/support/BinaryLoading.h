//===- BinaryLoading.h - Binary format auto-detection ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Auto-detects binary format (ELF, PE, Mach-O) by magic bytes and loads
/// via the appropriate Loader.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_BINARYLOADING_H
#define NEVERD_SUPPORT_BINARYLOADING_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Raw/RawLoader.h"

#include "llvm/Support/Error.h"

#include <filesystem>
#include <map>
#include <set>

namespace neverd {

/// The loader the user chose for a file, as the load dialog's rows name them.
struct LoaderChoice {
  /// Unknown reads the file as its header or contents say, and refuses a
  /// file only its name ties to a format; EVM reads it as EVM bytecode; Raw
  /// as a binary file of a processor at an address, as Raw places it.
  BinaryFormat Format = BinaryFormat::Unknown;
  RawLoadOptions Raw;
};

/// Load-time work-set for a single-function CLI/C-API request.  Empty keeps
/// the historical full-image decode.
struct BinaryLoadOptions {
  std::set<va_t> OnlyFunctionEntries;
  LoaderChoice Choice;
  /// Caller-asserted instruction state at exact AArch32 function entries.
  /// Use only when the binary lacks mode evidence; conflicting evidence fails.
  std::map<va_t, InstructionMode> ARMFunctionModes;
};

/// Normalize every externally sourced text field in an already loaded image so
/// downstream diagnostics and JSON writers receive valid UTF-8.
void normalizeBinaryMetadata(BinaryImage &Img);

/// Auto-detect binary format from magic bytes and load via the appropriate
/// loader. Returns an error if the format is unrecognized.
llvm::Expected<BinaryImage> loadBinary(const std::filesystem::path &Path,
                                       const BinaryLoadOptions &Opts = {});
/// Load a selected immutable native byte image, without host file access or
/// extension-based guessing. Universal Mach-O requires explicit slice selection
/// and is refused here. File-specific loader choices are also refused. The
/// returned image owns its data.
llvm::Expected<BinaryImage>
loadBinaryBuffer(llvm::MemoryBufferRef Buffer,
                 const BinaryLoadOptions &Opts = {});

} // namespace neverd

#endif // NEVERD_SUPPORT_BINARYLOADING_H
