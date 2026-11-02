//===- LinuxDirectoryCreation.cpp - Guest child inode lifecycle ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded noatime tmpfs children share namespace, stat and clock owners.
#include "LinuxFiles.h"
#include "LinuxTime.h"

namespace neverd::emulation::linux_model {
const LinuxFileMetadata *
LinuxFiles::directoryMetadata(const std::string &Path) const {
  if (auto I = DirectoryMetadata.find(Path); I != DirectoryMetadata.end())
    return &I->second;
  if (Options->DirectoryCreation) {
    const auto &Parents = Options->DirectoryCreation->Parents;
    if (auto I = Parents.find(Path); I != Parents.end())
      return &I->second;
  }
  return nullptr;
}

uint32_t LinuxFiles::metadataPermissions(const LinuxFileMetadata &M) {
  return (M.Mode >> (M.UID == UserID ? 6 : M.GID == GroupID ? 3 : 0)) & 7;
}

bool LinuxFiles::canSearchPath(const std::string &Path) const {
  // Supplied parents admit unknown ancestors; observed search bits prevail.
  for (size_t Slash = 0; Slash != std::string::npos;
       Slash = Path.find('/', Slash + 1)) {
    const auto Prefix = Slash ? Path.substr(0, Slash) : "/";
    if (const auto *M = directoryMetadata(Prefix);
        M && !(metadataPermissions(*M) & 1))
      return false;
  }
  return true;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::createFile(const std::string &Path, uint32_t Flags, uint32_t Mode,
                       uint32_t FD, bool SearchAncestors,
                       ProcessResult &Result) {
  if (!Options->FileCreations || !Options->FileCreations->Files.contains(Path))
    return unsupported(
        Result, "memory-file creation requires an explicit object reservation");
  const auto &Reservation = Options->FileCreations->Files.at(Path);
  if (auto I = FileCreationCounts.find(Path);
      I != FileCreationCounts.end() && I->second >= Reservation.Count)
    return unsupported(Result, "file creation reservation already consumed");
  if (Mode & ~uint32_t(FileRWXModeBits))
    return unsupported(Result,
                       "file creation special permission bits are unmodeled");
  const auto Slash = Path.rfind('/');
  const auto Parent = Slash ? Path.substr(0, Slash) : std::string("/");
  const auto *Previous = directoryMetadata(Parent);
  if (!Options->DirectoryCreation || !Previous ||
      lookupPath(Parent) != PathKind::Directory)
    return unsupported(Result,
                       "file creation requires explicit parent metadata");
  // The open component walk has already checked each visited directory.
  // Rechecking a canonical path would revisit ancestors above a directory FD.
  if ((SearchAncestors && !canSearchPath(Path)) ||
      (metadataPermissions(*Previous) & 3) != 3)
    return std::optional<uint64_t>(uint64_t(0) - AccessDenied);
  for (const auto &[Number, Description] : Descriptors)
    if (const auto *D = std::get_if<OpenDirectory>(Description.get());
        D && D->Path == Parent)
      return unsupported(Result,
                         "file creation with an open parent is unmodeled");
  if (Options->DynamicProc)
    for (const auto &Mapping : Options->DynamicProc->Mappings)
      if (Mapping.Path == Path)
        return unsupported(
            Result,
            "file creation at a captured mapping identity is unmodeled");
  if (Previous->Size > MaxSignedIOSize - TmpfsDirectoryEntryBytes)
    return unsupported(Result, "directory metadata growth exceeds model range");
  if (!Clock)
    return unsupported(Result, "file creation requires a realtime clock");
  auto Now = Clock->read(ClockRealtime, Result);
  if (!Now)
    return std::optional<uint64_t>();

  std::map<std::string, std::string> Names;
  if (FileNames)
    Names = *FileNames;
  else
    for (const auto &[Name, Bytes] : Options->Files)
      Names.emplace(Name, Name);
  // No initial canonical pathname contains NUL. An internal key preserves
  // the old object even when its former pathname is created again.
  const std::string Key =
      std::string(1, '\0') + std::to_string(CreatedFileLimits.size());
  auto Number = inode(Key);
  if (!Number)
    return Number.takeError();
  LinuxFileMetadata Metadata;
  Metadata.Device = Previous->Device;
  Metadata.Inode = *Number;
  Metadata.Mode = FileRegularMode | (Mode & ~Options->DirectoryCreation->Umask);
  Metadata.UID = UserID;
  Metadata.GID = (Previous->Mode & 02000) ? Previous->GID : GroupID;
  Metadata.LinkCount = 1;
  Metadata.BlockSize = Previous->BlockSize;
  Metadata.AccessTime = Metadata.ModificationTime = Metadata.ChangeTime = *Now;
  auto Updated = *Previous;
  Updated.Size += TmpfsDirectoryEntryBytes;
  Updated.ModificationTime = Updated.ChangeTime = *Now;
  MutableFile Object;
  Object.Metadata = Metadata;
  Names.emplace(Path, Key);
  MutableFiles.emplace(Key, std::move(Object));
  CreatedFileLimits.emplace(Key, Reservation.WriteLimit);
  ++FileCreationCounts[Path];
  DirectoryMetadata.insert_or_assign(Parent, Updated);
  FileNames = std::move(Names);
  Descriptors.emplace(FD, std::make_shared<Descriptor>(
                              OpenFile{{}, fileMetadata(Key), 0, Key, Flags}));
  opened(FD, Flags);
  return std::optional<uint64_t>(FD);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::makeDirectory(uint64_t Address, uint32_t Mode,
                          ProcessResult &Result) {
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  auto Parsed = parsePath(std::get<std::string>(*Imported));
  if (!Parsed)
    return unsupported(Result, FilePathForm);
  const auto &Path = Parsed->Name;
  if (Path == "/")
    return std::optional<uint64_t>(uint64_t(0) - AlreadyExists);

  // Supplied parents grant traversal through unknown ancestors. Once an
  // ancestor has modeled metadata, its search bit remains authoritative.
  if (!canSearchPath(Path))
    return std::optional<uint64_t>(uint64_t(0) - AccessDenied);
  // Linux resolves the parent and final component before applying mode,
  // umask or creation policy. Only these catalogue-derived failures are known.
  const size_t Slash = Path.rfind('/');
  const std::string Parent = Slash ? Path.substr(0, Slash) : "/";
  switch (lookupPath(Parent)) {
  case PathKind::SymbolicLink:
  case PathKind::LinkTraversal:
    return unsupported(Result, FileLinkTraversal);
  case PathKind::UnmodeledProc:
    return unsupported(Result, "proc entry has no modeled semantics");
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::ProcFile:
  case PathKind::File:
  case PathKind::Device:
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::Directory:
    break;
  }
  // Creation checks the final name itself: even a regular file followed by
  // slashes already exists. The directory requirement applies to its parent.
  if (lookupPath(Path) != PathKind::Missing)
    return std::optional<uint64_t>(uint64_t(0) - AlreadyExists);
  const auto *Previous = directoryMetadata(Parent);
  if (!Options->DirectoryCreation || !Previous)
    return unsupported(Result, FileDirectoryCreation);
  // The explicit provider excludes ACLs and security hooks. Use the same
  // credentials as getuid/getgid; an observed inode's owner is not the caller.
  if ((metadataPermissions(*Previous) & 3) != 3)
    return std::optional<uint64_t>(uint64_t(0) - AccessDenied);
  if (CreatedDirectories.size() >= Options->DirectoryCreation->Limit)
    return unsupported(Result, "directory creation reservation exhausted");
  if (Previous->LinkCount == UINT32_MAX ||
      Previous->Size > MaxSignedIOSize - TmpfsDirectoryEntryBytes)
    return unsupported(Result, "directory metadata growth exceeds model range");
  if (!Clock)
    return unsupported(Result, "directory creation requires a realtime clock");
  auto Now = Clock->read(ClockRealtime, Result);
  if (!Now)
    return std::optional<uint64_t>();
  auto Number = inode(Path);
  if (!Number)
    return Number.takeError();
  // All admission checks precede namespace or metadata changes. Linux tmpfs
  // uses two 20-byte logical entries initially, no data blocks, and inherits
  // the parent's setgid group/bit after stripping requested set-ID bits.
  LinuxFileMetadata Child;
  Child.Device = Previous->Device;
  Child.Inode = *Number;
  Child.Mode = 0040000 | ((Mode & 01777) & ~Options->DirectoryCreation->Umask);
  Child.UID = UserID;
  Child.GID = (Previous->Mode & 02000) ? Previous->GID : GroupID;
  Child.Mode |= Previous->Mode & 02000;
  Child.LinkCount = 2;
  Child.Size = 2 * TmpfsDirectoryEntryBytes;
  Child.BlockSize = 4096;
  Child.AccessTime = Child.ModificationTime = Child.ChangeTime = *Now;
  auto Updated = *Previous;
  ++Updated.LinkCount;
  Updated.Size += TmpfsDirectoryEntryBytes;
  Updated.ModificationTime = Updated.ChangeTime = *Now;
  DirectoryMetadata.insert_or_assign(Parent, Updated);
  DirectoryMetadata.emplace(Path, Child);
  CreatedDirectories.push_back(Path);
  return std::optional<uint64_t>(0);
}

} // namespace neverd::emulation::linux_model
