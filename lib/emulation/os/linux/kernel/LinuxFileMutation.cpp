//===- LinuxFileMutation.cpp - Guest file names and object lifetime
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit rename and unlink policies share stable regular-file identities.
///
//===----------------------------------------------------------------------===//

#include "LinuxFiles.h"
#include "LinuxTime.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <string>
#include <utility>

namespace neverd::emulation::linux_model {
const std::string *LinuxFiles::fileIdentity(const std::string &Name) const {
  if (FileNames) {
    auto I = FileNames->find(Name);
    return I == FileNames->end() ? nullptr : &I->second;
  }
  auto I = Options->Files.find(Name);
  return I == Options->Files.end() ? nullptr : &I->first;
}

std::optional<std::string>
LinuxFiles::fileName(const std::string &Identity) const {
  if (!FileNames)
    return Identity;
  for (const auto &[Name, Object] : *FileNames)
    if (Object == Identity)
      return Name;
  return std::nullopt;
}

bool LinuxFiles::hasFileDescendant(const std::string &Prefix) const {
  const auto Has = [&](const auto &Files) {
    auto Next = Files.lower_bound(Prefix);
    return Next != Files.end() &&
           llvm::StringRef(Next->first).starts_with(Prefix);
  };
  return FileNames ? Has(*FileNames) : Has(Options->Files);
}

void LinuxFiles::forEachFilePath(
    const std::function<void(const std::string &)> &Visit) const {
  if (FileNames) {
    for (const auto &[Name, Object] : *FileNames)
      Visit(Name);
  } else {
    for (const auto &[Name, Bytes] : Options->Files)
      Visit(Name);
  }
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::unlinkPath(uint64_t Address, uint32_t Flags,
                       ProcessResult &Result) {
  if (Flags)
    return unsupported(Result, "Linux unlinkat flags are unmodeled");
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  auto Path = parsePath(std::get<std::string>(*Imported));
  if (!Path)
    return unsupported(Result, FilePathForm);
  Path->Name = procPath(Path->Name);
  // Resolve the live namespace, including directories created by the guest.
  // An initial catalogue miss alone never licenses a fabricated ENOENT.
  switch (lookupPath(Path->Name, Path->RequiresDirectory)) {
  case PathKind::Missing:
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::File:
    break;
  default:
    return unsupported(
        Result,
        "Linux unlink of an existing or unresolved object is unmodeled");
  }
  if (!Options->FileUnlinks)
    return unsupported(Result,
                       "Linux file unlink requires an explicit provider");
  const size_t Slash = Path->Name.rfind('/');
  const std::string Parent = Slash ? Path->Name.substr(0, Slash) : "/";
  if (!Options->FileUnlinks->Parents.contains(Parent))
    return unsupported(Result, "Linux file unlink parent is not granted");
  const std::string Key = *fileIdentity(Path->Name);
  const auto *File = fileMetadata(Key);
  const auto *Directory = directoryMetadata(Parent);
  if (!tracksFileMetadata(Key) || !File || !Directory || File->LinkCount != 1 ||
      File->Device != Directory->Device ||
      File->BlockSize != Directory->BlockSize ||
      Directory->Size < 3 * TmpfsDirectoryEntryBytes)
    return unsupported(Result, "Linux file unlink requires coherent tracked "
                               "single-link file and directory metadata");
  if (!canSearchPath(Path->Name) ||
      (metadataPermissions(*Directory) & 3) != 3 ||
      ((Directory->Mode & 01000) && Directory->UID != UserID &&
       File->UID != UserID))
    return unsupported(Result,
                       "Linux file unlink permission case is unmodeled");
  for (const auto &[FD, Description] : Descriptors) {
    if (const auto *Open = std::get_if<OpenFile>(Description.get());
        Open && Open->Path == Key)
      return unsupported(Result,
                         "Linux file unlink with an open file is unmodeled");
    if (const auto *Open = std::get_if<OpenDirectory>(Description.get());
        Open && Open->Path == Parent)
      return unsupported(Result,
                         "Linux file unlink with an open parent is unmodeled");
  }
  if (Options->DynamicProc)
    for (const auto &Mapping : Options->DynamicProc->Mappings)
      if (Mapping.Path == Path->Name || Mapping.Path == Key)
        return unsupported(
            Result, "Linux file unlink of a captured mapping is unmodeled");
  if (FileUnlinkCount >= Options->FileUnlinks->Limit)
    return unsupported(Result, "Linux file unlink reservation exhausted");
  if (!Clock)
    return unsupported(Result, "Linux file unlink requires a realtime clock");
  auto Now = Clock->read(ClockRealtime, Result);
  if (!Now)
    return std::optional<uint64_t>();

  // All refusal paths precede publication. Retire the name, retaining the
  // initial object key and its reservation. Persistent parent directories
  // survive removal of their last child, including their ancestor paths.
  std::map<std::string, std::string> Names;
  if (FileNames)
    Names = *FileNames;
  else
    for (const auto &[Name, Bytes] : Options->Files)
      Names.emplace(Name, Name);
  Names.erase(Path->Name);
  auto UpdatedFile = *File;
  UpdatedFile.LinkCount = 0;
  UpdatedFile.ChangeTime = *Now;
  auto UpdatedDirectory = *Directory;
  UpdatedDirectory.Size -= TmpfsDirectoryEntryBytes;
  UpdatedDirectory.ModificationTime = UpdatedDirectory.ChangeTime = *Now;
  mutableFile(Key).Metadata = UpdatedFile;
  DirectoryMetadata.insert_or_assign(Parent, UpdatedDirectory);
  FileNames = std::move(Names);
  ++FileUnlinkCount;
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::renameFile(uint64_t OldAddress, uint64_t NewAddress,
                       ProcessResult &Result) {
  if (!Options->FileRenames)
    return unsupported(Result,
                       "Linux file rename requires an explicit provider");
  auto OldImported = readPath(OldAddress);
  if (!OldImported)
    return OldImported.takeError();
  auto NewImported = readPath(NewAddress);
  if (!NewImported)
    return NewImported.takeError();
  // This provider admits a positive move only. Unmodeled pathname error
  // precedence must not be replaced by a convenient guest errno.
  const auto *OldText = std::get_if<std::string>(&*OldImported);
  const auto *NewText = std::get_if<std::string>(&*NewImported);
  auto Old = OldText ? parsePath(*OldText) : std::nullopt;
  auto New = NewText ? parsePath(*NewText) : std::nullopt;
  if (!Old || !New || Old->RequiresDirectory || New->RequiresDirectory)
    return unsupported(Result, "Linux file rename pathname case is unmodeled");
  const auto ParentOf = [](const std::string &Name) {
    const auto Slash = Name.rfind('/');
    return Slash ? Name.substr(0, Slash) : std::string("/");
  };
  const std::string Parent = ParentOf(Old->Name);
  if (Parent != ParentOf(New->Name) || Old->Name == New->Name ||
      !Options->FileRenames->Parents.contains(Parent) ||
      lookupPath(Old->Name) != PathKind::File ||
      lookupPath(New->Name) != PathKind::Missing)
    return unsupported(Result,
                       "Linux file rename requires one regular source and an "
                       "absent name in the same granted directory");
  const auto *Identity = fileIdentity(Old->Name);
  const std::string Key = *Identity;
  const auto *File = fileMetadata(Key);
  const auto *Directory = directoryMetadata(Parent);
  if (!tracksFileMetadata(Key) || !File || !Directory || File->LinkCount != 1 ||
      File->Device != Directory->Device || File->BlockSize != 4096)
    return unsupported(Result, "Linux file rename requires coherent tracked "
                               "file and directory metadata");
  if (!canSearchPath(Old->Name) || (metadataPermissions(*Directory) & 3) != 3 ||
      ((Directory->Mode & 01000) && Directory->UID != UserID &&
       File->UID != UserID))
    return unsupported(Result,
                       "Linux file rename permission case is unmodeled");
  for (const auto &[FD, Description] : Descriptors)
    if (const auto *D = std::get_if<OpenDirectory>(Description.get());
        D && D->Path == Parent)
      return unsupported(
          Result,
          "Linux file rename with an open parent directory is unmodeled");
  if (Options->DynamicProc)
    for (const auto &M : Options->DynamicProc->Mappings)
      if (M.Path == Old->Name || M.Path == Key || M.Path == New->Name)
        return unsupported(
            Result,
            "Linux file rename of captured mapping identities is unmodeled");
  if (FileRenameCount >= Options->FileRenames->Limit)
    return unsupported(Result, "Linux file rename reservation exhausted");
  if (!Clock)
    return unsupported(Result, "Linux file rename requires a realtime clock");
  auto Now = Clock->read(ClockRealtime, Result);
  if (!Now)
    return std::optional<uint64_t>();

  // Stage the complete binding map before publishing a move. File contents,
  // open cursors and write reservations remain attached to the original key.
  std::map<std::string, std::string> Names;
  if (FileNames)
    Names = *FileNames;
  else
    for (const auto &[Name, Bytes] : Options->Files)
      Names.emplace(Name, Name);
  Names.erase(Old->Name);
  Names.emplace(New->Name, Key);
  auto UpdatedFile = *File;
  UpdatedFile.ChangeTime = *Now;
  auto UpdatedDirectory = *Directory;
  UpdatedDirectory.ModificationTime = UpdatedDirectory.ChangeTime = *Now;
  auto &Object = mutableFile(Key);
  Object.Metadata = UpdatedFile;
  DirectoryMetadata.insert_or_assign(Parent, UpdatedDirectory);
  FileNames = std::move(Names);
  ++FileRenameCount;
  return std::optional<uint64_t>(0);
}

} // namespace neverd::emulation::linux_model
