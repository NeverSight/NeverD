//===- LinuxFileStatus.cpp - Explicit guest file status observations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxUserMemory.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::statusDescriptor(uint32_t FD, uint64_t Address,
                             ProcessResult &Result) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  if (const auto *D = std::get_if<OpenDirectory>(I->second.get())) {
    const auto *M = directoryMetadata(D->Path);
    return M ? status(*M, Address, Result)
             : llvm::Expected<std::optional<uint64_t>>(
                   unsupported(Result, FileStatusDirectory));
  }
  const auto *File = std::get_if<OpenFile>(I->second.get());
  if (!File || !File->Metadata)
    return unsupported(Result, FileStatusMissing);
  const auto *Metadata = fileMetadata(File->Path);
  if (!Metadata)
    return unsupported(Result,
                       "file mutation invalidated captured stat metadata");
  return status(*Metadata, Address, Result);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::statusAt(uint32_t Directory, uint64_t PathAddress, uint64_t Address,
                     uint32_t Flags, ProcessResult &Result) {
  constexpr uint32_t CommonFlags =
      AtSymlinkNoFollow | AtNoAutomount | AtEmptyPath;
  // Validate flags before importing a pathname or touching the output. Sync
  // bits and NULL empty paths differ across Linux releases; this catalogue
  // does not choose a kernel version or invent remote synchronization.
  if (Flags & ~(CommonFlags | AtStatSyncMask))
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  if (Flags & AtStatSyncMask)
    return unsupported(Result, FileStatusFlags);
  if (!PathAddress && (Flags & AtEmptyPath))
    return unsupported(Result, FileStatusNullPath);
  auto Imported = readPath(PathAddress, Flags & AtEmptyPath);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  const auto &Name = std::get<std::string>(*Imported);
  if (Name.empty()) {
    if (Directory == AtCurrentDirectory)
      return unsupported(Result, FileCurrentDirectory);
    return statusDescriptor(Directory, Address, Result);
  }
  auto Path = parsePath(Name);
  if (!Path)
    return unsupported(Result, FilePathForm);
  // Absolute names ignore dirfd. Link targets do not establish traversal or
  // link metadata; directory prefixes carry no metadata observation.
  switch (lookupPath(Path->Name, Path->RequiresDirectory)) {
  case PathKind::SymbolicLink:
  case PathKind::LinkTraversal:
    return unsupported(Result, FileLinkTraversal);
  case PathKind::UnmodeledProc:
    return unsupported(Result, "proc entry has no modeled semantics");
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::Directory:
    if (auto *M = directoryMetadata(Path->Name))
      return status(*M, Address, Result);
    return unsupported(Result, FileStatusDirectory);
  case PathKind::Device:
    return unsupported(Result, FileStatusMissing);
  case PathKind::ProcFile:
  case PathKind::File:
    break;
  }
  const auto *Object = fileIdentity(Path->Name);
  const auto &Identity = Object ? *Object : Path->Name;
  auto Metadata = Options->Metadata.find(Identity);
  if (Metadata == Options->Metadata.end() &&
      !CreatedFileLimits.contains(Identity))
    return unsupported(Result, FileStatusMissing);
  const auto *Current = fileMetadata(Identity);
  if (!Current)
    return unsupported(Result,
                       "file mutation invalidated captured stat metadata");
  return status(*Current, Address, Result);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::status(const LinuxFileMetadata &Metadata, uint64_t Address,
                   ProcessResult &Result) {
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  llvm::SmallVector<uint8_t, FileStatusSizeX64> Bytes(
      X64 ? FileStatusSizeX64 : FileStatusSizeARM64, 0);
  auto Put = [&](unsigned Offset, unsigned Width, uint64_t Value) {
    if (Width == 4)
      llvm::support::endian::write32le(Bytes.data() + Offset, Value);
    else
      llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
#define NEVERD_LINUX_FILE_STATUS(Member, ARMOffset, ARMWidth, X64Offset,       \
                                 X64Width)                                     \
  Put(X64 ? X64Offset : ARMOffset, X64 ? X64Width : ARMWidth,                  \
      static_cast<uint64_t>(Metadata.Member));
#include "LinuxFileStatus.def"
#undef NEVERD_LINUX_FILE_STATUS
  auto Stored = writeUserMemory(CPU, Layout, Address, Bytes);
  if (!Stored)
    return Stored.takeError();
  switch (*Stored) {
  case UserWriteResult::Stored:
    return std::optional<uint64_t>(0);
  case UserWriteResult::BadAddress:
    return std::optional<uint64_t>(uint64_t(0) - BadAddress);
  case UserWriteResult::MixedAccess:
    return unsupported(Result, FileStatusPartialOutput);
  }
  llvm_unreachable("unknown Linux user-copy outcome");
}

} // namespace neverd::emulation::linux_model
