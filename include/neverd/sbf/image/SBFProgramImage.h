//===- SBFProgramImage.h - Canonical Solana SBF VM image --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the immutable post-load, post-relocation SBF program image. All
/// decoders, interpreters, and emitters consume this representation so text and
/// data bytes cannot diverge after relocation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_IMAGE_SBFPROGRAMIMAGE_H
#define NEVERD_SBF_IMAGE_SBFPROGRAMIMAGE_H

#include "neverd/Common.h"
#include "neverd/sbf/SBFMetadata.h"
#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;

namespace sbf {

class ResolvedRuntimeEnvironment;

enum class ProgramRegionKind : uint8_t {
  Bytecode,
  ReadOnly,
  LegacyReadOnly,
};

struct ProgramSectionSpan {
  std::string Name;
  size_t Offset = 0;
  size_t Size = 0;
  bool Executable = false;
};

struct ProgramRegion {
  va_t Address = 0;
  std::vector<uint8_t> Bytes;
  ProgramRegionKind Kind = ProgramRegionKind::ReadOnly;
  bool DataVisible = true;
  std::string Name;
  std::vector<ProgramSectionSpan> Sections;

  bool contains(va_t Start, size_t Size = 1) const;
};

/// One entry in the legacy function registry constructed by the SBF loader.
/// The key is the value encoded in CALL_IMM; the target is an instruction slot
/// relative to the executable text section.
struct ProgramFunctionEntry {
  uint32_t Key = 0;
  size_t TargetSlot = 0;
  std::string Name;
};

class ProgramImage {
public:
  static constexpr size_t NoRegion = std::numeric_limits<size_t>::max();

  llvm::ArrayRef<ProgramRegion> regions() const { return Regions; }
  /// Legacy function entries, sorted by Key and immutable after construction.
  llvm::ArrayRef<ProgramFunctionEntry> functions() const { return Functions; }
  const ProgramFunctionEntry *findFunction(uint32_t Key) const;
  llvm::ArrayRef<uint8_t> text() const;
  va_t textAddress() const { return TextAddress; }
  /// Logical ELF .text extent. This can exceed text().size() for a legacy
  /// SHT_NOBITS section; the loader records that range for entry/function
  /// validation while exposing no invented instruction bytes.
  size_t textVirtualSize() const { return TextVirtualSize; }
  Version version() const { return TheVersion; }
  size_t entrySlot() const { return EntrySlot; }
  bool empty() const { return Regions.empty(); }

  const ProgramRegion *findRegion(va_t Address, size_t Size = 1,
                                  bool DataAccess = false) const;
  llvm::Expected<llvm::ArrayRef<uint8_t>> slice(va_t Address, size_t Size,
                                                bool DataAccess = false) const;

private:
  friend llvm::Expected<ProgramImage>
  buildProgramImage(const BinaryImage &, const Metadata &, const SBFVMConfig &,
                    llvm::ArrayRef<uint32_t>);
  friend llvm::Expected<ProgramImage>
  createProgramImage(llvm::ArrayRef<uint8_t>, va_t, llvm::ArrayRef<uint8_t>,
                     va_t, bool, Version, size_t,
                     llvm::ArrayRef<ProgramFunctionEntry>);

  llvm::Error finalize(va_t Address, size_t Size);
  llvm::Error
  relocateLegacyRelativeCalls(llvm::MutableArrayRef<uint8_t> Text,
                              llvm::ArrayRef<uint32_t> RegisteredSyscalls);
  llvm::Error registerFunction(uint32_t Key, size_t TargetSlot,
                               llvm::StringRef Name);
  llvm::Error
  registerLoaderFunction(uint32_t Key, size_t TargetSlot, llvm::StringRef Name,
                         llvm::ArrayRef<uint32_t> RegisteredSyscalls);
  llvm::Error registerEntrypoint(llvm::ArrayRef<uint32_t> RegisteredSyscalls);
  void finalizeFunctions();

  std::vector<ProgramRegion> Regions;
  std::vector<ProgramFunctionEntry> Functions;
  llvm::DenseMap<uint32_t, ProgramFunctionEntry> PendingFunctions;
  size_t TextRegion = NoRegion;
  size_t TextOffset = 0;
  size_t TextSize = 0;
  size_t TextVirtualSize = 0;
  va_t TextAddress = 0;
  Version TheVersion = Version::Reserved;
  size_t EntrySlot = 0;
};

/// Build the canonical runtime image from a loaded SBF ELF and apply every
/// legacy relocation exactly once.
llvm::Expected<ProgramImage>
buildProgramImage(const BinaryImage &Image, const Metadata &Metadata,
                  const SBFVMConfig &Config = {},
                  llvm::ArrayRef<uint32_t> RegisteredSyscallHashes = {});

/// Build through a fully resolved runtime authority.  This is the executable-
/// image boundary shared by analysis and loader conformance: it checks the
/// runtime's enabled version range before applying that same environment's VM
/// configuration and function registry.
llvm::Expected<ProgramImage>
buildProgramImage(const BinaryImage &Image, const Metadata &Metadata,
                  const ResolvedRuntimeEnvironment &Environment);

/// Construct a checked synthetic image for unit tests and backend fixtures.
llvm::Expected<ProgramImage>
createProgramImage(llvm::ArrayRef<uint8_t> Text, va_t TextAddress,
                   llvm::ArrayRef<uint8_t> Rodata = {}, va_t RodataAddress = 0,
                   bool LegacyTextIsDataVisible = false,
                   Version TheVersion = Version::Reserved, size_t EntrySlot = 0,
                   llvm::ArrayRef<ProgramFunctionEntry> Functions = {});

} // namespace sbf
} // namespace neverd

#endif // NEVERD_SBF_IMAGE_SBFPROGRAMIMAGE_H
