//===- LinuxFilePaths.cpp - Canonical guest pathname operations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxUserMemory.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::getWorkingDirectory(uint64_t Address, uint64_t Size,
                                ProcessResult &Result) {
  if (!Options->WorkingDirectory)
    return unsupported(Result, "Linux working directory is not supplied");
  const auto &Path = *Options->WorkingDirectory;
  // Linux returns the terminating NUL in the count; capacity is checked
  // before the user copy. The declared cwd is linked and reachable.
  if (Size < Path.size() + 1)
    return std::optional<uint64_t>(uint64_t(0) - RangeError);
  std::vector<uint8_t> Bytes(Path.begin(), Path.end());
  Bytes.push_back(0);
  auto Stored = writeUserMemory(CPU, Layout, Address, Bytes);
  if (!Stored)
    return Stored.takeError();
  switch (*Stored) {
  case UserWriteResult::Stored:
    return std::optional<uint64_t>(Bytes.size());
  case UserWriteResult::BadAddress:
    return std::optional<uint64_t>(uint64_t(0) - BadAddress);
  case UserWriteResult::MixedAccess:
    return unsupported(Result,
                       "partial Linux getcwd output fault is unmodeled");
  }
  llvm_unreachable("unknown Linux user-copy outcome");
}

bool isCanonicalFilePath(llvm::StringRef Path) {
  if (!Path.consume_front("/") || Path.empty() || Path.contains('\0'))
    return false;
  llvm::SmallVector<llvm::StringRef> Components;
  Path.split(Components, '/');
  return llvm::all_of(Components, [](llvm::StringRef Part) {
    return !Part.empty() && Part != "." && Part != ".." &&
           Part.size() <= FileNameLimit;
  });
}

llvm::Expected<LinuxFiles::Pathname> LinuxFiles::readPath(uint64_t Address,
                                                          bool AllowEmpty) {
  // Import only through the first NUL, including at a readable page's last
  // byte. No host pathname conversion or filesystem lookup occurs.
  std::string Path;
  for (uint64_t I = 0; I < FilePathLimit; ++I) {
    if (Address >= Layout.UserLimit || I >= Layout.UserLimit - Address)
      return uint32_t(BadAddress);
    auto Access = CPU.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return uint32_t(BadAddress);
    auto Byte = CPU.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      break;
    Path.push_back(*Byte);
  }
  if (Path.size() == FilePathLimit)
    return uint32_t(NameTooLong);
  if (Path.empty() && !AllowEmpty)
    return uint32_t(NoEntry);
  return Path;
}

std::optional<LinuxFiles::ParsedPath>
LinuxFiles::parsePath(llvm::StringRef Path) {
  if (!Path.starts_with('/'))
    return std::nullopt;
  const bool RequiresDirectory = Path.ends_with('/');
  // Interior current-directory components retain the same parent walk.
  // Preserve the terminal directory requirement; final dot, parent walks and
  // link traversal keep their explicit boundaries. In particular, dropping a
  // final dot would incorrectly permit creation of its preceding component.
  llvm::SmallVector<llvm::StringRef> Components;
  Path.split(Components, '/', -1, false);
  if (!Components.empty() && Components.back() == ".")
    return std::nullopt;
  std::string Name;
  Name.reserve(Path.size());
  for (llvm::StringRef Part : Components) {
    if (Part == ".")
      continue;
    Name.push_back('/');
    Name.append(Part.data(), Part.size());
  }
  if (Name.empty())
    return ParsedPath{"/", true};
  if (!isCanonicalFilePath(Name))
    return std::nullopt;
  return ParsedPath{std::move(Name), RequiresDirectory};
}

LinuxFiles::PathKind LinuxFiles::lookupPath(const std::string &Path,
                                            bool RequiresDirectory) const {
  if (Path == "/" || DirectoryMetadata.contains(Path) ||
      llvm::is_contained(Options->Directories, Path))
    return PathKind::Directory;
  if (auto Kind = procKind(Path, RequiresDirectory))
    return *Kind;
  if (Options->DynamicProcFDs) {
    if (Path == "/proc" || Path == "/proc/self/fd")
      return PathKind::Directory;
    if (Path == "/proc/self")
      return RequiresDirectory ? PathKind::LinkTraversal
                               : PathKind::SymbolicLink;
    if (llvm::StringRef(Path).starts_with("/proc/self/fd/")) {
      auto FD = procDescriptor(Path);
      if (FD && Descriptors.contains(*FD))
        return RequiresDirectory ? PathKind::LinkTraversal
                                 : PathKind::SymbolicLink;
      return PathKind::Missing;
    }
  }
  if (fileIdentity(Path))
    return RequiresDirectory ? PathKind::NotDirectory : PathKind::File;
  if (Options->Devices.contains(Path))
    return RequiresDirectory ? PathKind::NotDirectory : PathKind::Device;
  if (Options->SymbolicLinks.contains(Path))
    return RequiresDirectory ? PathKind::LinkTraversal : PathKind::SymbolicLink;
  // Catalogue prefixes denote directories; a link component requires an
  // unmodeled traversal, while a regular file component is ENOTDIR.
  const std::string Prefix = Path + '/';
  if (hasFileDescendant(Prefix))
    return PathKind::Directory;
  auto Link = Options->SymbolicLinks.lower_bound(Prefix);
  if (Link != Options->SymbolicLinks.end() &&
      llvm::StringRef(Link->first).starts_with(Prefix))
    return PathKind::Directory;
  for (const auto &Directory : Options->Directories)
    if (llvm::StringRef(Directory).starts_with(Prefix))
      return PathKind::Directory;
  auto Device = Options->Devices.lower_bound(Prefix);
  if (Device != Options->Devices.end() &&
      llvm::StringRef(Device->first).starts_with(Prefix))
    return PathKind::Directory;
  for (size_t I = Path.find('/', 1); I != std::string::npos;
       I = Path.find('/', I + 1)) {
    const auto Parent = Path.substr(0, I);
    if (fileIdentity(Parent) || Options->Devices.contains(Parent))
      return PathKind::NotDirectory;
    if (Options->SymbolicLinks.contains(Parent))
      return PathKind::LinkTraversal;
  }
  return PathKind::Missing;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::access(uint64_t Address, uint32_t Mode, ProcessResult &Result) {
  // Linux validates the int mode before importing the pathname. Credentials,
  // ACLs and mount policy are not implied by a file's metadata observation.
  if (Mode & ~uint32_t(FileAccessMask))
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  auto Path = parsePath(std::get<std::string>(*Imported));
  if (!Path)
    return unsupported(Result, FilePathForm);
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
  case PathKind::ProcFile:
  case PathKind::File:
  case PathKind::Device:
  case PathKind::Directory:
    if (Mode)
      return unsupported(Result, FileAccessMode);
    return std::optional<uint64_t>(0);
  }
  llvm_unreachable("unknown Linux catalogue entry kind");
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::open(uint64_t Address, uint32_t Flags, uint32_t Mode,
                 ProcessResult &Result, uint32_t DirectoryFD) {
  const bool ARM64 = CPU.architecture() == GuestArchitecture::AArch64;
  const uint32_t LargeFile = ARM64 ? OpenLargeFileARM64 : OpenLargeFileX64;
  const uint32_t Directory = ARM64 ? OpenDirectoryARM64 : OpenDirectoryX64;
  const uint32_t Direct = ARM64 ? OpenDirectARM64 : OpenDirectX64;
  if ((Flags & Directory) && (Flags & OpenCreate))
    return unsupported(Result,
                       "O_CREAT with O_DIRECTORY is kernel-version dependent");
  const uint32_t AccessMask =
      3 | OpenCreate | OpenExclusive | OpenTruncate | OpenAppend;
  if ((Flags & ~(OpenCloseOnExec | OpenNonBlock | LargeFile | Directory |
                 Direct | AccessMask)) ||
      (Flags & 3) == 3)
    return unsupported(Result, FileOpenFlags);
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (const auto *Error = std::get_if<uint32_t>(&*Imported))
    return std::optional<uint64_t>(uint64_t(0) - *Error);
  const auto &Original = std::get<std::string>(*Imported);
  std::string Start = "/";
  const bool FromDirectory = !llvm::StringRef(Original).starts_with('/') &&
                             DirectoryFD != AtCurrentDirectory;
  if (FromDirectory) {
    auto I = Descriptors.find(DirectoryFD);
    if (I == Descriptors.end())
      return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
    const auto *D = std::get_if<OpenDirectory>(I->second.get());
    if (!D)
      return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
    Start = D->Path;
  }
  auto Path = FromDirectory ? std::nullopt : parsePath(Original);
  const bool Walked = !Path;
  if (Walked) {
    if (!llvm::StringRef(Original).starts_with('/') &&
        DirectoryFD == AtCurrentDirectory)
      return unsupported(Result, FilePathForm);
    llvm::SmallVector<llvm::StringRef> Components;
    llvm::StringRef(Original).split(Components, '/', -1, false);
    if (llvm::any_of(Components, [](llvm::StringRef Part) {
          return Part.size() > FileNameLimit;
        }))
      return unsupported(Result, FilePathForm);
    // Resolve each visited directory before interpreting its next component.
    // Lexical cancellation would hide missing files, ENOTDIR and denied search.
    // A directory FD starts at that object, without revisiting its ancestors.
    std::string Current = Start;
    for (llvm::StringRef Part : Components) {
      Current = procPath(Current);
      switch (lookupPath(Current, true)) {
      case PathKind::Directory:
        break;
      case PathKind::Missing:
        return std::optional<uint64_t>(uint64_t(0) - NoEntry);
      case PathKind::File:
      case PathKind::Device:
      case PathKind::ProcFile:
      case PathKind::NotDirectory:
        return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
      case PathKind::SymbolicLink:
      case PathKind::LinkTraversal:
        return unsupported(Result, FileLinkTraversal);
      case PathKind::UnmodeledProc:
        return unsupported(Result, "proc entry has no modeled semantics");
      }
      if (const auto *M = directoryMetadata(Current);
          M && !(metadataPermissions(*M) & 1))
        return std::optional<uint64_t>(uint64_t(0) - AccessDenied);
      if (Part == ".")
        continue;
      if (Part == "..") {
        const size_t Slash = Current.rfind('/');
        Current.resize(Slash ? Slash : 1);
        continue;
      }
      if (Current != "/")
        Current.push_back('/');
      Current.append(Part.data(), Part.size());
    }
    Path = ParsedPath{std::move(Current),
                      llvm::StringRef(Original).ends_with('/') ||
                          (!Components.empty() && (Components.back() == "." ||
                                                   Components.back() == ".."))};
  }

  Path->Name = procPath(Path->Name);
  if ((Flags & OpenNonBlock) && (!Options->Devices.contains(Path->Name) ||
                                 !Options->Devices.at(Path->Name).Binder))
    return unsupported(Result, FileOpenFlags);
  uint32_t FD = nextDescriptor();
  if (FD == Options->DescriptorLimit)
    return std::optional<uint64_t>(uint64_t(0) - TooManyFiles);
  const auto Kind =
      lookupPath(Path->Name, Path->RequiresDirectory || (Flags & Directory));
  if ((Flags & (OpenCreate | OpenExclusive)) == (OpenCreate | OpenExclusive) &&
      Kind != PathKind::Missing && Kind != PathKind::NotDirectory &&
      Kind != PathKind::LinkTraversal && Kind != PathKind::UnmodeledProc)
    return std::optional<uint64_t>(uint64_t(0) - AlreadyExists);
  switch (Kind) {
  case PathKind::SymbolicLink:
  case PathKind::LinkTraversal:
    return unsupported(Result, FileLinkTraversal);
  case PathKind::UnmodeledProc:
    return unsupported(Result, "proc entry has no modeled semantics");
  case PathKind::Missing:
    if (Flags & OpenCreate) {
      if (Path->RequiresDirectory)
        return unsupported(Result,
                           "file creation with a trailing slash is unmodeled");
      if (Flags & Direct)
        return unsupported(Result, FileDirectIO);
      return createFile(Path->Name, Flags, Mode, FD, !Walked, Result);
    }
    return std::optional<uint64_t>(uint64_t(0) - NoEntry);
  case PathKind::NotDirectory:
    return std::optional<uint64_t>(uint64_t(0) - NotDirectory);
  case PathKind::Directory:
    if ((Flags & 3) || (Flags & (OpenCreate | OpenTruncate)))
      return std::optional<uint64_t>(uint64_t(0) - IsDirectory);
    if (Flags & Direct)
      return unsupported(Result, FileDirectIO);
    Descriptors.emplace(
        FD, std::make_shared<Descriptor>(OpenDirectory{Path->Name, 0, Flags}));
    opened(FD, Flags);
    return std::optional<uint64_t>(FD);
  case PathKind::Device: {
    if (Flags & (OpenCreate | OpenExclusive | OpenTruncate | OpenAppend))
      return unsupported(Result, DeviceOpenMode);
    if (Flags & Direct)
      return unsupported(Result, FileDirectIO);
    const auto &Device = Options->Devices.at(Path->Name);
    if ((Flags & 3) != Device.AccessMode)
      return unsupported(Result, DeviceOpenMode);
    if (Device.OpenErrno)
      return std::optional<uint64_t>(uint64_t(0) - Device.OpenErrno);
    std::erase_if(DeviceDescriptions,
                  [](const auto &D) { return D.expired(); });
    auto Description =
        std::make_shared<LinuxDeviceState>(LinuxDeviceState{&Device});
    Description->OpenFlags = Flags;
    if (Device.KGSL &&
        (Device.KGSL->SynchronousA3xx || Device.KGSL->MemoryObjects)) {
      for (const auto &Weak : DeviceDescriptions)
        if (auto Open = Weak.lock())
          if (Open->Options == &Device)
            Description->GPU = Open->GPU;
      if (!Description->GPU)
        Description->GPU = std::make_shared<KGSLProcess>();
    }
    DeviceDescriptions.push_back(Description);
    Descriptors.emplace(FD,
                        std::make_shared<Descriptor>(std::move(Description)));
    opened(FD, Flags);
    return std::optional<uint64_t>(FD);
  }
  case PathKind::ProcFile:
    if ((Flags & 3) || (Flags & (OpenTruncate | OpenAppend | Direct)))
      return unsupported(Result, "proc write and direct IO are unmodeled");
    Descriptors.emplace(FD, std::make_shared<Descriptor>(
                                OpenProcFile{Path->Name, 0, 0, Flags}));
    opened(FD, Flags);
    return std::optional<uint64_t>(FD);
  case PathKind::File:
    break;
  }
  // The kernel resolves the pathname before checking an opened file's direct
  // I/O support. A missing path or non-directory component is already known;
  // this catalogue does not infer a backing filesystem's direct-I/O policy.
  if (Flags & Direct)
    return unsupported(Result, FileDirectIO);
  const auto *Identity = fileIdentity(Path->Name);
  assert(Identity && "resolved regular file lost its identity");
  if (CreatedFileLimits.contains(*Identity)) {
    const uint32_t Required = ((Flags & 3) == 1 ? 0 : 4) |
                              ((Flags & 3) || (Flags & OpenTruncate) ? 2 : 0);
    if ((metadataPermissions(*fileMetadata(*Identity)) & Required) != Required)
      return std::optional<uint64_t>(uint64_t(0) - AccessDenied);
  }
  if ((Flags & 3) || (Flags & OpenTruncate)) {
    if (!fileWriteLimit(*Identity))
      return unsupported(Result,
                         "memory-file write requires an explicit reservation");
    auto &Object = mutableFile(*Identity);
    if (Flags & OpenTruncate) {
      if (!touchWrite(Object, Result))
        return std::optional<uint64_t>();
      Object.Bytes.clear();
      Object.Changed = true;
      Object.AllocatedPages.clear();
      if (Object.Metadata) {
        Object.Metadata->Size = 0;
        Object.Metadata->Blocks = 0;
      }
    }
  }
  llvm::ArrayRef<uint8_t> Bytes;
  const LinuxFileMetadata *Metadata = nullptr;
  if (CreatedFileLimits.contains(*Identity))
    Metadata = fileMetadata(*Identity);
  else {
    Bytes = Options->Files.at(*Identity);
    if (auto I = Options->Metadata.find(*Identity);
        I != Options->Metadata.end())
      Metadata = &I->second;
  }
  Descriptors.emplace(FD, std::make_shared<Descriptor>(
                              OpenFile{Bytes, Metadata, 0, *Identity, Flags}));
  opened(FD, Flags);
  return std::optional<uint64_t>(FD);
}

} // namespace neverd::emulation::linux_model
