//===- DarwinFiles.cpp - Workload-owned Darwin file services --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original model of the BSD ABI described in the pinned XNU sources linked
// from docs/darwin-emulation.md. No host descriptors or filesystem calls.
#include "DarwinFiles.h"

#include "DarwinDirectory.h"
#include "DarwinUserMemory.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
namespace limits = darwin_file_limits;
bool canonicalPath(llvm::StringRef Path, bool AllowRoot = false) {
  if (AllowRoot && Path == "/")
    return true;
  if (!Path.consume_front("/") || Path.empty() || Path.contains('\0'))
    return false;
  llvm::SmallVector<llvm::StringRef> Parts;
  Path.split(Parts, '/');
  return llvm::all_of(Parts, [](llvm::StringRef Part) {
    return !Part.empty() && Part != "." && Part != "..";
  });
}
enum class PathKind { Missing, File, Directory, SymbolicLink };
std::string parentPath(const std::string &Path) {
  return Path.substr(0, std::max<size_t>(1, Path.rfind('/')));
}
bool containsPath(llvm::StringRef Directory, llvm::StringRef Path) {
  return Directory == "/" || Path == Directory ||
         (Path.consume_front(Directory) && Path.starts_with('/'));
}
bool declaredNonMountSubtree(const DarwinFileOptions &Options,
                             llvm::StringRef Path) {
  for (const auto *Roots :
       {&Options.MovableDirectories, &Options.ExchangeableDirectories})
    if (llvm::any_of(*Roots, [&](const auto &Root) {
          return Path == Root || Path.starts_with(Root + '/');
        }))
      return true;
  return false;
}
std::string initialMountDomain(const DarwinFileOptions &Options,
                               std::string Directory) {
  while (Directory != "/" && declaredNonMountSubtree(Options, Directory))
    Directory = parentPath(Directory);
  return Directory;
}
PathKind pathKind(const DarwinFileOptions &Options, const std::string &Path) {
  if (Options.Files.contains(Path))
    return PathKind::File;
  if (Options.SymbolicLinks.contains(Path))
    return PathKind::SymbolicLink;
  if (Path == "/" || Options.Directories.contains(Path))
    return PathKind::Directory;
  const auto Prefix = Path + '/';
  auto File = Options.Files.lower_bound(Prefix);
  auto Directory = Options.Directories.lower_bound(Prefix);
  auto Link = Options.SymbolicLinks.lower_bound(Prefix);
  if ((File != Options.Files.end() &&
       llvm::StringRef(File->first).starts_with(Prefix)) ||
      (Directory != Options.Directories.end() &&
       llvm::StringRef(*Directory).starts_with(Prefix)) ||
      (Link != Options.SymbolicLinks.end() &&
       llvm::StringRef(Link->first).starts_with(Prefix)))
    return PathKind::Directory;
  return PathKind::Missing;
}
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
struct FileFootprint {
  uint64_t Bytes;
  uint32_t Entries;
};
bool validTime(const DarwinFileTime &Time) {
  return Time.Nanoseconds >= 0 && Time.Nanoseconds < 1000000000;
}
bool validMutationPolicy(const DarwinFileMutationPolicy &Policy) {
  const auto Unit = Policy.AllocationUnit;
  return Unit >= 512 && Unit <= limits::Bytes && !(Unit & (Unit - 1)) &&
         validTime(Policy.Time);
}
llvm::Expected<FileFootprint>
fileOptionsFootprint(const DarwinFileOptions &Options) {
  if (Options.InitialUmask && *Options.InitialUmask > 07777)
    return failure(diagnostic::FileUmaskOption);
  uint64_t Entries = Options.Files.size() + Options.Directories.size() +
                     Options.SymbolicLinks.size();
  if (Options.DescriptorLimit < 3 ||
      Options.DescriptorLimit > limits::Descriptors || Entries > limits::Files)
    return failure(diagnostic::FileOptionsLimit);
  // Every admitted deletion/rekey subtree needs a mutable parent. Excluding
  // fixed link names from those parents also protects them from subtree moves
  // and inherited creation grants. Target bytes may name mutable objects.
  for (const auto &Directory : Options.MutableDirectories)
    if (llvm::any_of(Options.SymbolicLinks, [&](const auto &Link) {
          return containsPath(Directory, Link.first);
        }))
      return failure(diagnostic::SymbolicLinkNamespace);
  uint64_t Total = Options.StandardInput ? Options.StandardInput->size() : 0;
  if (Total > limits::Bytes)
    return failure(diagnostic::FileOptionsLimit);
  auto PathInput = [&](const std::string &Path, bool Directory,
                       uint64_t Bytes = 0) -> llvm::Error {
    if (Path.size() >= limits::Path || !canonicalPath(Path, Directory))
      return failure(diagnostic::FileOptionPath);
    llvm::SmallVector<llvm::StringRef> Parts;
    llvm::StringRef(Path).split(Parts, '/');
    if (llvm::any_of(Parts,
                     [](llvm::StringRef P) { return P.size() > limits::Name; }))
      return failure(diagnostic::FileOptionPath);
    if (Directory &&
        (Options.Files.contains(Path) || Options.SymbolicLinks.contains(Path)))
      return failure(diagnostic::FileOptionPath);
    for (size_t I = Path.find('/', 1); I != std::string::npos;
         I = Path.find('/', I + 1))
      if (Options.Files.contains(Path.substr(0, I)) ||
          Options.SymbolicLinks.contains(Path.substr(0, I)))
        return failure(diagnostic::FileOptionPath);
    const uint64_t Cost = Path.size() + 1;
    if (Cost > limits::Bytes - Total || Bytes > limits::Bytes - Total - Cost)
      return failure(diagnostic::FileOptionsLimit);
    Total += Cost + Bytes;
    return llvm::Error::success();
  };
  for (const auto &[Path, Bytes] : Options.Files) {
    if (Options.SymbolicLinks.contains(Path))
      return failure(diagnostic::FileOptionPath);
    if (auto E = PathInput(Path, false, Bytes.size()))
      return E;
  }
  for (const auto &[Path, Target] : Options.SymbolicLinks) {
    if (Options.Files.contains(Path) || Options.Directories.contains(Path))
      return failure(diagnostic::FileOptionPath);
    if (Target.empty() || Target.size() >= limits::Path ||
        llvm::is_contained(Target, uint8_t(0)))
      return failure(diagnostic::SymbolicLinkTarget);
    if (auto E = PathInput(Path, false, Target.size()))
      return E;
  }
  for (const auto &Path : Options.Directories)
    if (auto E = PathInput(Path, true))
      return E;
  for (const auto &[Path, M] : Options.Metadata) {
    const auto Kind = pathKind(Options, Path);
    if (Kind == PathKind::Missing)
      return failure(diagnostic::FileMetadataPath);
    if (Kind == PathKind::Directory && !Options.Directories.contains(Path)) {
      if (++Entries > limits::Files)
        return failure(diagnostic::FileOptionsLimit);
      if (auto E = PathInput(Path, true))
        return E;
    }
    const auto Mode = Kind == PathKind::File           ? FileRegularMode
                      : Kind == PathKind::SymbolicLink ? FileSymbolicLinkMode
                                                       : FileDirectoryMode;
    if ((M.Mode & ~FilePermissionMask) != Mode || M.Size > INT64_MAX ||
        (Kind == PathKind::File && M.Size != Options.Files.at(Path).size()) ||
        (Kind == PathKind::SymbolicLink &&
         M.Size != Options.SymbolicLinks.at(Path).size()) ||
        M.Blocks > INT64_MAX || M.BlockSize > INT32_MAX)
      return failure(diagnostic::FileMetadataOption);
    for (auto T : {M.AccessTime, M.ModificationTime, M.ChangeTime, M.BirthTime})
      if (!validTime(T))
        return failure(diagnostic::FileMetadataOption);
  }
  uint64_t DirectoryRecords = 0;
  std::map<std::string, uint64_t> Inodes;
  for (const auto &[Path, Metadata] : Options.Metadata)
    Inodes.emplace(Path, Metadata.Inode);
  for (const auto &[Path, Contents] : Options.DirectoryContents) {
    if (pathKind(Options, Path) != PathKind::Directory)
      return failure(diagnostic::DirectoryContentsOption);
    if (!Options.Directories.contains(Path) &&
        !Options.Metadata.contains(Path)) {
      if (++Entries > limits::Files)
        return failure(diagnostic::FileOptionsLimit);
      if (auto E = PathInput(Path, true))
        return E;
    }
    if (Contents.MinimumBufferSize == 0 ||
        Contents.MinimumBufferSize > DirectoryPayloadLimit)
      return failure(diagnostic::DirectoryContentsOption);
    if (Contents.Entries.size() > limits::DirectoryEntries - DirectoryRecords)
      return failure(diagnostic::FileOptionsLimit);
    DirectoryRecords += Contents.Entries.size();
    const std::string Prefix = Path == "/" ? "/" : Path + '/';
    std::set<std::string> Children = {".", ".."};
    auto Child = [&](llvm::StringRef Other) {
      if (Other.consume_front(Prefix) && !Other.empty())
        Children.insert(Other.take_front(Other.find('/')).str());
    };
    for (const auto &[File, Bytes] : Options.Files)
      Child(File);
    for (const auto &Directory : Options.Directories)
      Child(Directory);
    for (const auto &[Link, Target] : Options.SymbolicLinks)
      Child(Link);
    std::set<uint64_t> Cookies;
    for (const auto &Entry : Contents.Entries) {
      if (Entry.Name.empty() || Entry.Name.size() > limits::Name ||
          llvm::StringRef(Entry.Name).contains('/') ||
          llvm::StringRef(Entry.Name).contains('\0') ||
          !Children.erase(Entry.Name) || !Entry.Inode || !Entry.NextOffset ||
          Entry.NextOffset > INT64_MAX ||
          !Cookies.insert(Entry.NextOffset).second ||
          Entry.MinimumBufferSize > DirectoryPayloadLimit)
        return failure(diagnostic::DirectoryContentsOption);
      std::string Target = Entry.Name == "." ? Path : Prefix + Entry.Name;
      if (Entry.Name == "..")
        Target = Path.substr(0, std::max<size_t>(1, Path.rfind('/')));
      const auto Kind = pathKind(Options, Target);
      const uint8_t Type = Kind == PathKind::File           ? 8
                           : Kind == PathKind::SymbolicLink ? 10
                                                            : 4;
      if (Kind == PathKind::Missing || (Entry.Type && Entry.Type != Type) ||
          (Kind == PathKind::SymbolicLink && Entry.Type != 10))
        return failure(diagnostic::DirectoryContentsOption);
      auto [Known, Inserted] = Inodes.emplace(Target, Entry.Inode);
      if (!Inserted && Known->second != Entry.Inode)
        return failure(diagnostic::DirectoryContentsOption);
      const uint64_t Cost = directoryRecordSize(Entry.Name.size());
      if (Cost > limits::Bytes - Total)
        return failure(diagnostic::FileOptionsLimit);
      Total += Cost;
    }
    if (!Children.empty())
      return failure(diagnostic::DirectoryContentsOption);
  }
  if (Options.WorkingDirectory) {
    const auto &Path = *Options.WorkingDirectory;
    if (pathKind(Options, Path) != PathKind::Directory)
      return failure(diagnostic::FileWorkingDirectoryOption);
    if (auto E = PathInput(Path, true))
      return E;
  }
  auto HasAlias = [&](const std::string &Path) {
    auto Inode = Inodes.find(Path);
    if (Inode == Inodes.end())
      return false;
    for (const auto &[Other, Number] : Inodes) {
      if (Other == Path || Number != Inode->second)
        continue;
      auto A = Options.Metadata.find(Path), B = Options.Metadata.find(Other);
      if (A == Options.Metadata.end() || B == Options.Metadata.end() ||
          A->second.Device == B->second.Device)
        return true;
    }
    return false;
  };
  for (const auto &Path : Options.MutableDirectories) {
    if (pathKind(Options, Path) != PathKind::Directory)
      return failure(diagnostic::DirectoryMutableOption);
    if (!Options.Directories.contains(Path) &&
        !Options.Metadata.contains(Path) &&
        !Options.DirectoryContents.contains(Path) && ++Entries > limits::Files)
      return failure(diagnostic::FileOptionsLimit);
    if (auto E = PathInput(Path, true))
      return E;
    const auto M = Options.Metadata.find(Path);
    if (M != Options.Metadata.end() &&
        (M->second.Flags || (M->second.Mode & 07000)))
      return failure(diagnostic::NamespaceFlags);
    if (HasAlias(Path))
      return failure(diagnostic::NamespaceAlias);
  }
  for (const auto &[Path, Bytes] : Options.Files) {
    if (!Options.MutableDirectories.contains(parentPath(Path)))
      continue;
    const auto M = Options.Metadata.find(Path);
    if (M != Options.Metadata.end() && M->second.Flags)
      return failure(diagnostic::NamespaceFlags);
    if (HasAlias(Path) ||
        (M != Options.Metadata.end() && M->second.LinkCount != 1))
      return failure(diagnostic::NamespaceAlias);
  }
  for (const auto &Path : Options.SwapRenameDirectories) {
    if (!Options.Directories.contains(Path) ||
        !Options.MutableDirectories.contains(Path))
      return failure(diagnostic::DirectorySwapOption);
    if (auto E = PathInput(Path, true))
      return E;
  }
  for (const auto &Path : Options.RemovableDirectories) {
    const auto Parent = parentPath(Path);
    if (Path == "/" || !Options.Directories.contains(Path) ||
        !Options.MutableDirectories.contains(Parent))
      return failure(diagnostic::DirectoryRemovableOption);
    if (auto E = PathInput(Path, true))
      return E;
    const auto M = Options.Metadata.find(Path);
    if (M != Options.Metadata.end() &&
        (M->second.Flags || (M->second.Mode & 07000)))
      return failure(diagnostic::NamespaceFlags);
    if (HasAlias(Path))
      return failure(diagnostic::NamespaceAlias);
    const auto P = Options.Metadata.find(Parent);
    if (M != Options.Metadata.end() && P != Options.Metadata.end() &&
        M->second.Device != P->second.Device)
      return failure(diagnostic::DirectoryRemovalDevice);
  }
  std::set<std::string> MoveDomains;
  for (const auto *Roots :
       {&Options.MovableDirectories, &Options.ExchangeableDirectories}) {
    for (const auto &Path : *Roots) {
      if (Path == "/" || !Options.Directories.contains(Path) ||
          !Options.MutableDirectories.contains(parentPath(Path)))
        return failure(Roots == &Options.MovableDirectories
                           ? diagnostic::DirectoryMovableOption
                           : diagnostic::DirectoryExchangeableOption);
      if (auto E = PathInput(Path, true))
        return E;
      MoveDomains.insert(initialMountDomain(Options, Path));
      const auto Prefix = Path + '/';
      for (const auto &[Child, M] : Options.Metadata) {
        if (Child != Path && !llvm::StringRef(Child).starts_with(Prefix))
          continue;
        const bool Directory =
            (M.Mode & ~FilePermissionMask) == FileDirectoryMode;
        if (M.Flags || (Directory && (M.Mode & 07000)))
          return failure(diagnostic::NamespaceFlags);
        if (!Directory && M.LinkCount != 1)
          return failure(diagnostic::NamespaceAlias);
      }
      for (const auto &[Child, Inode] : Inodes)
        if ((Child == Path || llvm::StringRef(Child).starts_with(Prefix)) &&
            HasAlias(Child))
          return failure(diagnostic::NamespaceAlias);
    }
  }
  // Declarations can join sibling subtrees through an unobserved ancestor.
  // Validate the entire joined component, including files at its domain root,
  // rather than assuming that pairwise root/parent checks prove one device.
  std::map<std::string, int32_t> MoveDevices;
  for (const auto &[Path, M] : Options.Metadata) {
    const auto Directory =
        Options.Files.contains(Path) ? parentPath(Path) : Path;
    const auto Domain = initialMountDomain(Options, Directory);
    if (!MoveDomains.contains(Domain))
      continue;
    const auto [Known, Added] = MoveDevices.emplace(Domain, M.Device);
    if (!Added && Known->second != M.Device)
      return failure(diagnostic::DirectoryMoveDevice);
  }
  for (const auto &Path : Options.WritableFiles) {
    if (!Options.Files.contains(Path))
      return failure(diagnostic::FileWritableOption);
    if (auto E = PathInput(Path, false))
      return E;
    auto Metadata = Options.Metadata.find(Path);
    if (Metadata != Options.Metadata.end() &&
        (Metadata->second.Flags & FileRestrictedFlags))
      return failure(diagnostic::FileWritableFlags);
    if (HasAlias(Path))
      return failure(diagnostic::FileWritableAlias);
  }
  for (const auto &[Path, Policy] : Options.MutationPolicies) {
    const auto Metadata = Options.Metadata.find(Path);
    const uint64_t Unit = Policy.AllocationUnit;
    if (!Options.WritableFiles.contains(Path) ||
        Metadata == Options.Metadata.end() || !validMutationPolicy(Policy))
      return failure(diagnostic::FileMutationPolicy);
    const auto &M = Metadata->second;
    if ((M.Mode & ~0777) != FileRegularMode || M.Flags || M.LinkCount != 1 ||
        M.Blocks != ((M.Size + Unit - 1) / Unit) * (Unit / 512))
      return failure(diagnostic::FileMutationPolicy);
    if (auto E = PathInput(Path, false))
      return E;
  }
  if (Options.CreationPolicy) {
    const auto &Policy = *Options.CreationPolicy;
    if (Options.MutableDirectories.empty() || !Policy.FirstInode ||
        !Policy.BlockSize || Policy.BlockSize > INT32_MAX ||
        !Options.InitialUmask || !validTime(Policy.Time) ||
        !validMutationPolicy(Policy.Mutation) ||
        llvm::any_of(Inodes, [&](const auto &I) {
          return I.second >= Policy.FirstInode;
        }))
      return failure(diagnostic::FileCreationPolicy);
    for (const auto &Path : Options.MutableDirectories)
      if (!Options.Metadata.contains(Path))
        return failure(diagnostic::FileCreationParent);
  }
  return FileFootprint{Total, uint32_t(Entries)};
}
} // namespace

llvm::Error validateFileOptions(const DarwinFileOptions &Options) {
  auto Size = fileOptionsFootprint(Options);
  return Size ? llvm::Error::success() : Size.takeError();
}

DarwinFiles::DarwinFiles(GuestMemory &Memory,
                         const std::optional<DarwinFileOptions> &Options,
                         uint64_t OutputLimit, uint32_t EffectiveUID)
    : Memory(Memory), Options(Options), OutputLimit(OutputLimit),
      EffectiveUID(EffectiveUID) {
  const llvm::ArrayRef<uint8_t> Input =
      Options && Options->StandardInput
          ? llvm::ArrayRef<uint8_t>(*Options->StandardInput)
          : llvm::ArrayRef<uint8_t>();
  Descriptors.emplace(0, Descriptor{std::make_shared<Description>(
                             Description{Kind::Input, Input})});
  Descriptors.emplace(1, Descriptor{std::make_shared<Description>(
                             Description{Kind::Output, {}})});
  Descriptors.emplace(2, Descriptor{std::make_shared<Description>(
                             Description{Kind::Error, {}})});
  Descriptors.at(1).Open->Flags = Descriptors.at(2).Open->Flags = OpenWriteOnly;
}
uint32_t DarwinFiles::limit() const {
  return Options ? Options->DescriptorLimit : limits::DefaultDescriptors;
}
uint32_t DarwinFiles::freeDescriptor(uint32_t Minimum) const {
  while (Minimum < limit() && Descriptors.contains(Minimum))
    ++Minimum;
  return Minimum;
}
void DarwinFiles::initializeNamespace() {
  if (NamespaceReady)
    return;
  if (Options->CreationPolicy) {
    NextCreatedInode = Options->CreationPolicy->FirstInode;
  }
  if (Options->InitialUmask)
    CurrentUmask = *Options->InitialUmask;
  initialDirectoryNode("/");
  for (const auto &Path : Options->Directories)
    initialDirectoryNode(Path);
  for (const auto &[Path, Bytes] : Options->Files) {
    auto Node = std::make_shared<Contents>();
    Node->Initial = Bytes;
    Node->Path = Path;
    Node->Parent = initialDirectoryNode(parentPath(Path));
    Node->Writable = Options->WritableFiles.contains(Path);
    const auto Metadata = Options->Metadata.find(Path);
    if (Metadata != Options->Metadata.end())
      Node->InitialMetadata = &Metadata->second;
    const auto Policy = Options->MutationPolicies.find(Path);
    if (Policy != Options->MutationPolicies.end())
      Node->Policy = &Policy->second;
    Nodes.emplace(Path, std::move(Node));
  }
  for (const auto &[Path, Target] : Options->SymbolicLinks) {
    auto Node = std::make_shared<LinkNode>();
    Node->Initial = Target;
    Node->Path = Path;
    Node->Parent = initialDirectoryNode(parentPath(Path));
    Node->Protected = true;
    const auto Metadata = Options->Metadata.find(Path);
    if (Metadata != Options->Metadata.end())
      Node->Metadata = &Metadata->second;
    Links.emplace(Path, std::move(Node));
  }
  NamespaceReady = true;
  if (Options->WorkingDirectory)
    CurrentDirectory = Directories.at(*Options->WorkingDirectory);
}

void DarwinFiles::reclaimUnlinked() {
  for (auto I = Unlinked.begin(); I != Unlinked.end();) {
    if (I->use_count() == 1 && (*I)->Lease.use_count() == 1) {
      *StorageUsed -= (*I)->bytes().size() + (*I)->PathCharge;
      I = Unlinked.erase(I);
    } else {
      ++I;
    }
  }
  // A removed child retains its original parent. Releasing a leaf can make
  // earlier parents unreachable, so reclaim the bounded chain to a fixed point.
  bool Reclaimed;
  do {
    Reclaimed = false;
    for (auto I = UnlinkedDirectories.begin();
         I != UnlinkedDirectories.end();) {
      if (I->use_count() == 1) {
        *StorageUsed -= (*I)->PathCharge;
        I = UnlinkedDirectories.erase(I);
        Reclaimed = true;
      } else {
        ++I;
      }
    }
  } while (Reclaimed);
}

std::shared_ptr<DarwinFiles::DirectoryNode>
DarwinFiles::directoryNode(const std::string &Path) {
  initializeNamespace();
  const auto I = Directories.find(Path);
  return I == Directories.end() ? nullptr : I->second;
}

std::shared_ptr<DarwinFiles::DirectoryNode>
DarwinFiles::initialDirectoryNode(const std::string &Path) {
  if (auto I = Directories.find(Path); I != Directories.end())
    return I->second;
  if (pathKind(*Options, Path) != PathKind::Directory)
    return nullptr;
  auto Node = std::make_shared<DirectoryNode>();
  Node->Path = Path;
  if (Path != "/")
    Node->Parent = initialDirectoryNode(parentPath(Path));
  const auto Metadata = Options->Metadata.find(Path);
  if (Metadata != Options->Metadata.end()) {
    Node->Metadata = &Metadata->second;
    Node->Identity =
        DirectoryIdentity{Metadata->second.Device, Metadata->second.GID};
  }
  const auto Snapshot = Options->DirectoryContents.find(Path);
  if (Snapshot != Options->DirectoryContents.end())
    Node->Snapshot = &Snapshot->second;
  Node->Mutable = Options->MutableDirectories.contains(Path);
  Node->Removable = Options->RemovableDirectories.contains(Path);
  Node->Movable = Options->MovableDirectories.contains(Path);
  Node->Exchangeable = Options->ExchangeableDirectories.contains(Path);
  Node->NonMount = declaredNonMountSubtree(*Options, Path);
  Node->SwapSupport = Options->SwapRenameDirectories.contains(Path);
  Directories.emplace(Path, Node);
  return Node;
}

uint32_t DarwinFiles::dynamicEntries() const {
  // FixedEntries already includes every initial link and directory. Only
  // created links consume additional entries; files retain their orphan charge.
  return Nodes.size() + Unlinked.size() +
         llvm::count_if(
             Links,
             [](const auto &Entry) { return !Entry.second->Protected; }) +
         llvm::count_if(
             Directories,
             [](const auto &Entry) { return Entry.second->Created; }) +
         llvm::count_if(UnlinkedDirectories,
                        [](const auto &Node) { return Node->Created; });
}

bool DarwinFiles::mutableDirectory(const std::string &Path) const {
  const auto I = Directories.find(Path);
  return I != Directories.end() && I->second->Mutable;
}

std::optional<DarwinFiles::DirectoryIdentity>
DarwinFiles::directoryIdentity(const std::string &Path) {
  const auto Node = directoryNode(Path);
  return Node ? Node->Identity : std::nullopt;
}

llvm::Error DarwinFiles::prepareMutation() {
  if (!StorageUsed) {
    auto Footprint = fileOptionsFootprint(*Options);
    if (!Footprint)
      return Footprint.takeError();
    StorageUsed = Footprint->Bytes;
    FixedEntries = Footprint->Entries - Options->Files.size();
  }
  reclaimUnlinked();
  return llvm::Error::success();
}

llvm::Expected<DarwinFiles::Pathname> DarwinFiles::readPath(uint64_t Address) {
  std::string Path;
  for (uint64_t I = 0; I < limits::Path; ++I) {
    if (Address >= UserLimit || I >= UserLimit - Address)
      return uint32_t(BadAddress);
    auto Access = Memory.canAccess(Address + I, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return uint32_t(BadAddress);
    auto Byte = Memory.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte)
      return Path;
    Path.push_back(*Byte);
  }
  return uint32_t(NameTooLong);
}

DarwinFiles::DirectoryLookup
DarwinFiles::directoryDescriptor(uint32_t FD) const {
  const auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint32_t(BadDescriptor);
  if (I->second.Open->Type == Kind::File)
    return uint32_t(NotDirectory);
  if (I->second.Open->Type != Kind::Directory)
    return diagnostic::FileDirectoryKind;
  return I->second.Open->Directory;
}

llvm::Expected<std::optional<DarwinFiles::Lookup>>
DarwinFiles::directoryPrefix(uint64_t Address, uint32_t DirectoryFD) {
  // nameiat/open1at copy exactly one byte before looking up a relative dirfd.
  // Keep this separate from the full import: open validates flags/capacity
  // between these phases, while other *at services enter nameiat directly.
  if (DirectoryFD != AtCurrentDirectory) {
    if (Address >= UserLimit)
      return std::optional<Lookup>(uint32_t(BadAddress));
    auto Access = Memory.canAccess(Address, 1, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return std::optional<Lookup>(uint32_t(BadAddress));
    auto Byte = Memory.readInteger(Address, 1);
    if (!Byte)
      return Byte.takeError();
    if (*Byte != '/') {
      auto Directory = directoryDescriptor(DirectoryFD);
      if (auto *Error = std::get_if<uint32_t>(&Directory))
        return std::optional<Lookup>(*Error);
      if (auto *Reason = std::get_if<const char *>(&Directory))
        return std::optional<Lookup>(*Reason);
    }
  }
  return std::optional<Lookup>();
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::open(uint64_t Address, uint32_t Flags, uint32_t DirectoryFD,
                  uint32_t Mode, ProcessResult &Result) {
  if (!Options)
    return unsupported(Result, diagnostic::FileInputs);
  auto Prefix = directoryPrefix(Address, DirectoryFD);
  if (!Prefix)
    return Prefix.takeError();
  if (*Prefix) {
    if (auto *Error = std::get_if<uint32_t>(&**Prefix))
      return returned(*Error, true);
    return unsupported(Result, std::get<const char *>(**Prefix));
  }
  if ((Flags & OpenAccessMask) == OpenAccessMask)
    return returned(InvalidArgument, true);
  if (Flags & ~uint32_t(OpenCloseOnExec | OpenDirectory | OpenAccessMask |
                        OpenAppend | OpenTruncate | OpenCreate | OpenExclusive |
                        OpenNoFollow | OpenNoFollowAny))
    return unsupported(Result, diagnostic::FileOpenFlags);
  // XNU reserves the descriptor before resolving the pathname. A failed
  // open does not retain that reservation.
  const uint32_t FD = freeDescriptor();
  if (FD == limit())
    return returned(TooManyFiles, true);
  if ((Flags & OpenNoFollow) && (Flags & OpenNoFollowAny))
    return returned(InvalidArgument, true);
  if ((Flags & OpenCreate) && (Flags & OpenDirectory))
    return returned(InvalidArgument, true);
  const bool ExclusiveCreate = (Flags & OpenCreate) && (Flags & OpenExclusive);
  auto Resolved = resolvePath(Address, DirectoryFD,
                              Flags & OpenCreate ? LookupMode::CreateFile
                                                 : LookupMode::Existing,
                              {!(Flags & OpenNoFollow) && !ExclusiveCreate,
                               bool(Flags & OpenNoFollowAny)},
                              false);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  auto &File = std::get<Description>(*Resolved);
  const bool Created = File.Type == Kind::Missing;
  if (!Created && (Flags & OpenCreate) && (Flags & OpenExclusive))
    return returned(FileExists, true);
  if ((Flags & OpenDirectory) && File.Type == Kind::SymbolicLink)
    return returned(NotDirectory, true);
  if (File.Type == Kind::SymbolicLink)
    return returned(TooManyLinks, true);
  if (Created) {
    auto Made = create(File, Mode, Result);
    if (!Made || !*Made || (**Made).Error)
      return Made;
  }
  if ((Flags & OpenDirectory) && File.Type != Kind::Directory)
    return returned(NotDirectory, true);
  const bool Mutating = (Flags & OpenAccessMask) || (Flags & OpenTruncate);
  if (Mutating && File.Type == Kind::Directory)
    return returned(IsDirectory, true);
  if (Mutating && !File.File->Writable)
    return unsupported(Result, diagnostic::FileNotWritable);
  File.Flags = Flags & (OpenAccessMask | OpenAppend);
  if ((Flags & OpenTruncate) && !Created) {
    auto Truncated = resize(File, 0, Result);
    if (!Truncated || !*Truncated || (**Truncated).Error)
      return Truncated;
    File.Flags |= FileWasWritten;
  }
  Descriptors.emplace(FD, Descriptor{std::make_shared<Description>(File),
                                     bool(Flags & OpenCloseOnExec)});
  return returned(FD);
}

llvm::Expected<DarwinFiles::Lookup>
DarwinFiles::resolvePath(uint64_t Address, uint32_t DirectoryFD,
                         LookupMode Mode, LinkPolicy Links,
                         bool CheckDirectoryPrefix) {
  if (!Options)
    return diagnostic::FileInputs;
  initializeNamespace();
  if (CheckDirectoryPrefix) {
    auto Prefix = directoryPrefix(Address, DirectoryFD);
    if (!Prefix)
      return Prefix.takeError();
    if (*Prefix)
      return std::move(**Prefix);
  }
  auto Imported = readPath(Address);
  if (!Imported)
    return Imported.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Imported))
    return *Error;
  std::string Path = std::move(std::get<std::string>(*Imported));
  const bool Absolute = llvm::StringRef(Path).starts_with('/');
  auto Directory = directoryNode("/");
  if (!Absolute) {
    if (DirectoryFD != AtCurrentDirectory) {
      auto Found = directoryDescriptor(DirectoryFD);
      if (auto *Error = std::get_if<uint32_t>(&Found))
        return *Error;
      if (auto *Reason = std::get_if<const char *>(&Found))
        return *Reason;
      Directory = std::get<std::shared_ptr<DirectoryNode>>(std::move(Found));
    } else if (!Path.empty()) {
      if (!CurrentDirectory)
        return diagnostic::FileWorkingDirectory;
      Directory = CurrentDirectory;
    }
  }
  if (Path.empty())
    return uint32_t(NoEntry);
  unsigned Expansions = 0;
  while (true) {
    const bool RestartAbsolute = llvm::StringRef(Path).starts_with('/');
    const auto Trimmed = llvm::StringRef(Path).rtrim('/');
    const auto Slash = Trimmed.rfind('/');
    const auto Leaf =
        Slash == llvm::StringRef::npos ? Trimmed : Trimmed.substr(Slash + 1);
    const Terminal FinalComponent = Leaf == "."    ? Terminal::Dot
                                    : Leaf == ".." ? Terminal::DotDot
                                                   : Terminal::Ordinary;
    llvm::SmallVector<llvm::StringRef> Parts;
    llvm::StringRef(Path).split(Parts, '/');
    std::string Prefix = Directory->Path;
    auto Type = PathKind::Directory;
    bool FinalParentUnlinked = false;
    bool Restart = false;
    // Walk objects before reducing dots. An unlinked directory keeps its own
    // identity and parent; its last path must never resolve into a reused name.
    for (size_t Index = RestartAbsolute; Index < Parts.size(); ++Index) {
      const auto Part = Parts[Index];
      if (Type != PathKind::Directory)
        return uint32_t(NotDirectory);
      if (Part.size() > limits::Name)
        return uint32_t(NameTooLong);
      if (Part.empty())
        continue;
      // RENAME rejects its terminal dot before looking up that component.
      // Earlier missing/file/removed ancestors still take precedence.
      if ((Mode == LookupMode::RenameTarget ||
           Mode == LookupMode::RenameDirectoryTarget) &&
          (Part == "." || Part == "..") &&
          llvm::all_of(
              llvm::ArrayRef<llvm::StringRef>(Parts).drop_front(Index + 1),
              [](llvm::StringRef Part) { return Part.empty(); }))
        return uint32_t(InvalidArgument);
      if (Part == ".")
        continue;
      if (Part == "..") {
        FinalParentUnlinked = !Directory->Linked;
        if (Directory->Parent)
          Directory = Directory->Parent;
        // LOOKUP can follow the retained parent vnode. Namespace operations
        // cannot traverse a removed parent, even to a later dot or ancestor.
        if (Mode != LookupMode::Existing && !Directory->Linked)
          return uint32_t(NoEntry);
        Prefix = Directory->Path;
        continue;
      }
      if (!Directory->Linked)
        return uint32_t(NoEntry);
      Prefix = Directory->Path;
      if (Prefix != "/")
        Prefix += '/';
      Prefix.append(Part.data(), Part.size());
      auto ChildDirectory = directoryNode(Prefix);
      const auto Link = this->Links.find(Prefix);
      Type = Nodes.contains(Prefix)      ? PathKind::File
             : ChildDirectory            ? PathKind::Directory
             : Link != this->Links.end() ? PathKind::SymbolicLink
                                         : PathKind::Missing;
      if (Type == PathKind::SymbolicLink &&
          (Links.FollowFinal || Index + 1 < Parts.size())) {
        if (Links.NoExpansion || Expansions++ >= MaxSymbolicLinks)
          return uint32_t(TooManyLinks);
        // Lookup consumes terminal slashes before expanding a link. The new
        // target may supply its own slashes, but the consumed suffix is only
        // NUL.
        auto Suffix = llvm::StringRef(Path).substr(
            size_t(Part.data() + Part.size() - Path.data()));
        // namei consumes repeated separators at this component before
        // charging ni_pathlen. Later suffix components remain unprocessed.
        while (Suffix.size() > 1 && Suffix[0] == '/' && Suffix[1] == '/')
          Suffix = Suffix.drop_front();
        if (Suffix == "/")
          Suffix = {};
        const auto Bytes = Link->second->bytes();
        // XNU rejects an empty link at the expansion boundary, before adding
        // any suffix. Retaining the terminal object still permits readlink.
        if (Bytes.empty())
          return uint32_t(NoEntry);
        const llvm::StringRef Target(
            reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
        if (Target.size() + Suffix.size() + 1 > limits::Path)
          return uint32_t(NameTooLong);
        Path = (Target + Suffix).str();
        if (llvm::StringRef(Path).starts_with('/'))
          Directory = directoryNode("/");
        Restart = true;
        break;
      }
      if (Type == PathKind::Missing) {
        const bool CreatesFile =
            Mode == LookupMode::CreateFile || Mode == LookupMode::RenameTarget;
        const bool CreatesDirectory = Mode == LookupMode::CreateDirectory ||
                                      Mode == LookupMode::RenameDirectoryTarget;
        const bool Final = Index + 1 == Parts.size();
        const bool DirectoryTail =
            CreatesDirectory &&
            llvm::all_of(
                llvm::ArrayRef<llvm::StringRef>(Parts).drop_front(Index + 1),
                [](llvm::StringRef Part) { return Part.empty(); });
        if (((CreatesFile || CreatesDirectory) && Final) || DirectoryTail)
          return Description{Kind::Missing, {}, 0, nullptr, std::move(Prefix)};
        return uint32_t(NoEntry);
      }
      if (ChildDirectory)
        Directory = std::move(ChildDirectory);
      FinalParentUnlinked = false;
    }
    if (Restart)
      continue;
    Description File{Type == PathKind::File           ? Kind::File
                     : Type == PathKind::SymbolicLink ? Kind::SymbolicLink
                                                      : Kind::Directory,
                     {},
                     0,
                     Directory->Metadata,
                     std::move(Prefix)};
    if (Type == PathKind::File) {
      File.File = Nodes.at(File.Path);
      File.Metadata = nullptr;
    } else if (Type == PathKind::SymbolicLink) {
      File.Link = this->Links.at(File.Path);
      File.Metadata = File.Link->Metadata;
    } else {
      File.Directory = std::move(Directory);
    }
    File.FinalComponent = FinalComponent;
    File.FinalParentUnlinked =
        FinalComponent == Terminal::DotDot && FinalParentUnlinked;
    return File;
  }
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::access(uint64_t Path, uint32_t DirectoryFD, uint32_t Mode,
                    ProcessResult &Result, LinkPolicy Links) {
  auto Resolved = resolvePath(Path, DirectoryFD, LookupMode::Existing, Links);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  // Native access1 requests authorization only for R/W/X or extended access
  // bits. Other low-carrier bits are ignored, not an EINVAL or a grant. The
  // catalogue proves name existence; stat observations do not prove ACL/MAC
  // authorization, including when mutation grants permit model operations.
  if (Mode & AccessPermissionMask)
    return unsupported(Result, diagnostic::FileAccessPermissions);
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::copyout(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
                     const char *PartialDiagnostic, ProcessResult &Result) {
  return copyUserMemory(Memory, Address, Bytes, PartialDiagnostic, Result);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::status(const Description &File, uint64_t Address,
                    ProcessResult &Result) {
  if (File.Type == Kind::Directory && File.Directory->Changed)
    return unsupported(Result, diagnostic::DirectoryMutated);
  if (File.File && File.File->MetadataInvalidated)
    return unsupported(Result, diagnostic::FileMutatedMetadata);
  const auto *Metadata = File.File ? File.File->metadata() : File.Metadata;
  if (!Metadata)
    return unsupported(Result, diagnostic::FileMetadata);
  std::array<uint8_t, FileStatusSize> Bytes{};
  auto Put = [&](unsigned Offset, unsigned Width, uint64_t Value) {
    if (Width == 2)
      llvm::support::endian::write16le(Bytes.data() + Offset, Value);
    else if (Width == 4)
      llvm::support::endian::write32le(Bytes.data() + Offset, Value);
    else
      llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
#define NEVERD_DARWIN_FILE_STATUS(Member, Native, Offset, Width)               \
  Put(Offset, Width, static_cast<uint64_t>(Metadata->Member));
#include "DarwinFileStatus.def"
#undef NEVERD_DARWIN_FILE_STATUS
  return copyout(Address, Bytes, diagnostic::FilePartialStatus, Result);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::statusPath(uint64_t Path, uint64_t Address, uint32_t DirectoryFD,
                        ProcessResult &Result, LinkPolicy Links) {
  auto Resolved = resolvePath(Path, DirectoryFD, LookupMode::Existing, Links);
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  return status(std::get<Description>(*Resolved), Address, Result);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::readLink(uint64_t Path, uint64_t Address, uint64_t Size,
                      uint32_t DirectoryFD, ProcessResult &Result) {
  // readlink passes a signed low32 count; readlinkat preserves size_t. Both
  // enter the same internal INT32_MAX check before importing path or dirfd.
  if (Size > INT32_MAX)
    return returned(InvalidArgument, true);
  auto Resolved =
      resolvePath(Path, DirectoryFD, LookupMode::Existing, {false, false});
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  const auto &Link = std::get<Description>(*Resolved);
  if (Link.Type != Kind::SymbolicLink)
    return returned(InvalidArgument, true);
  // Empty targets perform no copy, even with a nonzero count and a bad
  // output pointer. Count/path/kind checks still precede this native boundary.
  if (!Size || Link.bytes().empty())
    return returned(0);
  const auto Bytes =
      Link.bytes().take_front(std::min<uint64_t>(Size, Link.bytes().size()));
  auto Copied =
      copyout(Address, Bytes, diagnostic::SymbolicLinkPartialOutput, Result);
  if (Copied && *Copied && !(**Copied).Error)
    (**Copied).Value = Bytes.size();
  return Copied;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::makeSymbolicLink(uint64_t Target, uint64_t Path,
                              uint32_t DirectoryFD, ProcessResult &Result) {
  // copyinstr(target) precedes destination nameiat, including its dirfd and
  // existence checks. Targets are opaque bytes, not catalogue path inputs.
  auto Imported = readPath(Target);
  if (!Imported)
    return Imported.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Imported))
    return returned(*Error, true);
  auto Resolved =
      resolvePath(Path, DirectoryFD, LookupMode::CreateFile, {false, false});
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  const auto &File = std::get<Description>(*Resolved);
  if (File.Type != Kind::Missing)
    return returned(FileExists, true);
  auto Parent = directoryNode(parentPath(File.Path));
  if (!Parent->Mutable)
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  const auto &Bytes = std::get<std::string>(*Imported);
  const uint64_t Charge = File.Path.size() + 1 + Bytes.size();
  if (File.Path.size() >= limits::Path ||
      FixedEntries + dynamicEntries() >= limits::Files ||
      Charge > limits::Bytes - *StorageUsed)
    return unsupported(Result, diagnostic::SymbolicLinkCreationLimit);
  auto Node = std::make_shared<LinkNode>();
  Node->CreatedTarget.emplace(Bytes.begin(), Bytes.end());
  Node->Path = File.Path;
  Node->Parent = Parent;
  // The regular-file CreationPolicy does not establish link metadata or an
  // inode. Reused names never inherit an old Options.Metadata observation.
  Links.emplace(File.Path, std::move(Node));
  *StorageUsed += Charge;
  Parent->Changed = true;
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::makeDirectory(uint64_t Path, uint32_t DirectoryFD,
                           ProcessResult &Result) {
  auto Resolved = resolvePath(Path, DirectoryFD, LookupMode::CreateDirectory,
                              {false, false});
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  const auto &File = std::get<Description>(*Resolved);
  if (File.Type != Kind::Missing)
    return returned(FileExists, true);
  const auto Parent = parentPath(File.Path);
  if (!mutableDirectory(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Charge = File.Path.size() + 1;
  if (File.Path.size() >= limits::Path ||
      FixedEntries + dynamicEntries() >= limits::Files ||
      Charge > limits::Bytes - *StorageUsed)
    return unsupported(Result, diagnostic::DirectoryCreationLimit);
  auto Node = std::make_shared<DirectoryNode>();
  Node->Path = File.Path;
  Node->Parent = directoryNode(Parent);
  Node->Identity = directoryIdentity(Parent);
  Node->Created = Node->Mutable = Node->NonMount = true;
  Node->PathCharge = Charge;
  Node->SwapSupport = Node->Parent->SwapSupport;
  Directories.emplace(File.Path, std::move(Node));
  *StorageUsed += Charge;
  directoryNode(Parent)->Changed = true;
  return returned(0);
}

std::optional<uint32_t> DarwinFiles::rootRemovalError(const Description &File) {
  if (File.Path != "/")
    return std::nullopt;
  // DELETE lookup rejects a slash-only root before the vnode removal checks.
  // A terminal dot or dotdot resolves the root vnode and reaches VROOT/EBUSY.
  return File.FinalComponent == Terminal::Ordinary ? IsDirectory : ResourceBusy;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::removeDirectory(uint64_t Path, uint32_t DirectoryFD,
                             ProcessResult &Result, bool NoExpansion) {
  auto Resolved = resolvePath(Path, DirectoryFD, LookupMode::DeleteDirectory,
                              {false, NoExpansion});
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  const auto &File = std::get<Description>(*Resolved);
  if (File.Type != Kind::Directory)
    return returned(NotDirectory, true);
  if (auto Error = rootRemovalError(File))
    return returned(*Error, true);
  if (!File.Directory->Created && !File.Directory->Removable)
    return unsupported(Result, diagnostic::DirectoryRemovalInitial);
  if (File.FinalComponent == Terminal::Dot)
    return returned(InvalidArgument, true);
  if (File.FinalComponent == Terminal::DotDot)
    return returned(File.FinalParentUnlinked ? NoEntry : DirectoryNotEmpty,
                    true);
  const auto Prefix = File.Path + '/';
  const auto ChildFile = Nodes.lower_bound(Prefix);
  const auto ChildDirectory = Directories.lower_bound(Prefix);
  const auto ChildLink = Links.lower_bound(Prefix);
  if ((ChildFile != Nodes.end() &&
       llvm::StringRef(ChildFile->first).starts_with(Prefix)) ||
      (ChildDirectory != Directories.end() &&
       llvm::StringRef(ChildDirectory->first).starts_with(Prefix)) ||
      (ChildLink != Links.end() &&
       llvm::StringRef(ChildLink->first).starts_with(Prefix)))
    return returned(DirectoryNotEmpty, true);
  if (!mutableDirectory(File.Directory->Parent->Path))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  UnlinkedDirectories.reserve(UnlinkedDirectories.size() + 1);
  File.Directory->Linked = false;
  File.Directory->Changed = true;
  UnlinkedDirectories.push_back(File.Directory);
  Directories.erase(File.Path);
  File.Directory->Parent->Changed = true;
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::unlink(uint64_t Path, uint32_t DirectoryFD, ProcessResult &Result,
                    bool NoExpansion) {
  auto Resolved = resolvePath(Path, DirectoryFD, LookupMode::DeleteFile,
                              {false, NoExpansion});
  if (!Resolved)
    return Resolved.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*Resolved))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*Resolved))
    return unsupported(Result, *Reason);
  auto &File = std::get<Description>(*Resolved);
  if (auto Error = rootRemovalError(File))
    return returned(*Error, true);
  if (File.Type == Kind::Directory)
    return returned(OperationNotPermitted, true);
  if (File.Type == Kind::SymbolicLink) {
    const auto &Link = File.Link;
    if (Link->Protected)
      return unsupported(Result, diagnostic::SymbolicLinkMutation);
    if (!Link->Parent->Mutable)
      return unsupported(Result, diagnostic::DirectoryNotMutable);
    if (auto E = prepareMutation())
      return std::move(E);
    // No descriptor or mapping owns the link itself. Its opaque target and
    // the current path charge are released with this namespace entry.
    const uint64_t Charge = Link->Path.size() + 1 + Link->bytes().size();
    Links.erase(Link->Path);
    *StorageUsed -= Charge;
    Link->Parent->Changed = true;
    return returned(0);
  }
  const auto Parent = parentPath(File.Path);
  if (!mutableDirectory(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  updateNamespaceMetadata(*File.File, true);
  directoryNode(Parent)->Changed = true;
  Unlinked.push_back(File.File);
  Nodes.erase(File.Path);
  return returned(0);
}

void DarwinFiles::updateNamespaceMetadata(Contents &Node, bool Removed) {
  if (Node.Policy && !Node.MetadataInvalidated) {
    if (!Node.CurrentMetadata)
      Node.CurrentMetadata = *Node.InitialMetadata;
    if (Removed)
      Node.CurrentMetadata->LinkCount = 0;
    Node.CurrentMetadata->ChangeTime = Node.Policy->Time;
  } else {
    Node.MetadataInvalidated = true;
  }
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::rename(uint64_t SourcePath, uint32_t SourceDirectory,
                    uint64_t TargetPath, uint32_t TargetDirectory,
                    RenameMode Mode, ProcessResult &Result, bool NoExpansion) {
  auto From = resolvePath(SourcePath, SourceDirectory, LookupMode::DeleteFile,
                          {false, NoExpansion});
  if (!From)
    return From.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*From))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*From))
    return unsupported(Result, *Reason);
  auto &Source = std::get<Description>(*From);
  if (Source.Type == Kind::SymbolicLink)
    return unsupported(Result, diagnostic::SymbolicLinkMutation);
  if (Source.Type == Kind::Directory &&
      (!Source.Directory->Linked ||
       (!Source.Directory->Created &&
        !(Mode == RenameMode::Swap ? Source.Directory->Exchangeable
                                   : Source.Directory->Movable))))
    return unsupported(Result, diagnostic::RenameKind);
  // Directory sources set WILLBEDIR for target lookup, permitting a missing
  // final directory with trailing slashes while retaining ancestor errors.
  auto To = resolvePath(TargetPath, TargetDirectory,
                        Source.Type == Kind::Directory
                            ? LookupMode::RenameDirectoryTarget
                            : LookupMode::RenameTarget,
                        {false, NoExpansion});
  if (!To)
    return To.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*To))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*To))
    return unsupported(Result, *Reason);
  auto &Target = std::get<Description>(*To);
  if (Target.Type == Kind::SymbolicLink)
    return unsupported(Result, diagnostic::SymbolicLinkMutation);
  if (Mode == RenameMode::Exclusive && (Target.File || Target.Directory)) {
    // Same-object exclusive rename depends on filesystem case sensitivity.
    // Exact catalogue keys do not supply that missing filesystem property.
    if ((Source.File && Target.File == Source.File) ||
        (Source.Directory && Target.Directory == Source.Directory))
      return unsupported(Result, diagnostic::RenameCaseSensitivity);
    return returned(FileExists, true);
  }
  if (Mode == RenameMode::Swap && Target.Type == Kind::Missing)
    return returned(NoEntry, true);
  // Source dots reach the rename checks after target lookup, EXCL and SWAP's
  // missing-target check. These failures precede filesystem dot rejection.
  if (Source.Type == Kind::Directory &&
      Source.FinalComponent != Terminal::Ordinary) {
    // XNU can finish a same-vnode rename on a case-sensitive filesystem
    // before its filesystem rejects the source dot. No such property is
    // supplied by exact catalogue keys, so do not generalize APFS's EINVAL.
    if (Source.Directory == Target.Directory)
      return unsupported(Result, diagnostic::RenameDotCaseSensitivity);
    return returned(InvalidArgument, true);
  }
  if (Mode == RenameMode::Swap) {
    if ((Target.Type != Kind::File && Target.Type != Kind::Directory) ||
        (Target.Directory &&
         (!Target.Directory->Linked ||
          (!Target.Directory->Created && !Target.Directory->Exchangeable))))
      return unsupported(Result, diagnostic::RenameSwapKind);
  }
  const auto Parent =
      Source.File ? Source.File->Parent : Source.Directory->Parent;
  const auto TargetParent = directoryNode(parentPath(Target.Path));
  // Device equality alone does not establish one mount. Only mkdir and
  // declared initial subtrees supply non-mount parent relationships.
  if (Parent->mountAncestor() != TargetParent->mountAncestor())
    return unsupported(Result, diagnostic::RenameMount);
  // Even a same-name native rename performs authorization. The namespace
  // grant excludes known restricted flags, aliases and special parent modes.
  if (!mutableDirectory(Parent->Path) || !mutableDirectory(TargetParent->Path))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  std::optional<int32_t> Device;
  auto SameDevice = [&](int32_t D) {
    if (Device && *Device != D)
      return false;
    Device = D;
    return true;
  };
  for (const auto &Directory : {Parent, TargetParent})
    if (const auto Identity = directoryIdentity(Directory->Path);
        Identity && !SameDevice(Identity->Device))
      return unsupported(Result, diagnostic::RenameMount);
  for (const auto &Directory : {Source.Directory, Target.Directory})
    if (Directory && Directory->Identity &&
        !SameDevice(Directory->Identity->Device))
      return unsupported(Result, diagnostic::RenameMount);
  for (const auto *Metadata :
       {Source.File ? Source.File->metadata() : Source.Metadata,
        Target.File ? Target.File->metadata() : Target.Metadata})
    if (Metadata && !SameDevice(Metadata->Device))
      return unsupported(Result, diagnostic::RenameMount);
  if (Source.Path == Target.Path)
    return returned(0);
  if (Mode == RenameMode::Swap &&
      (!Parent->SwapSupport || !TargetParent->SwapSupport))
    return unsupported(Result, diagnostic::RenameSwapSupport);
  if (Source.Type == Kind::Directory ||
      (Mode == RenameMode::Swap && Target.Type == Kind::Directory))
    return renameSubtrees(Source, Target, Parent, TargetParent,
                          Mode == RenameMode::Swap, Result);
  if (Target.Type == Kind::Directory)
    return returned(IsDirectory, true);
  if (auto E = prepareMutation())
    return std::move(E);
  if (Mode == RenameMode::Swap) {
    // Both objects stay linked. Neither their bytes nor their mapping leases
    // provide replacement credit; only their previous dynamic path charges do.
    const uint64_t Other =
        *StorageUsed - Source.File->PathCharge - Target.File->PathCharge;
    const uint64_t SourceCharge = Target.Path.size() + 1;
    const uint64_t TargetCharge = Source.Path.size() + 1;
    if (Target.Path.size() >= limits::Path ||
        Source.Path.size() >= limits::Path ||
        SourceCharge + TargetCharge > limits::Bytes - Other)
      return unsupported(Result, diagnostic::RenameLimit);
    // Allocate every potentially throwing string before extracting either
    // name. Node insertion and string swaps publish the bounded transaction.
    std::string SourceKey = Target.Path, TargetKey = Source.Path;
    std::string SourceIdentity = Target.Path, TargetIdentity = Source.Path;
    auto SourceNode = Nodes.extract(Source.Path);
    auto TargetNode = Nodes.extract(Target.Path);
    SourceNode.key().swap(SourceKey);
    TargetNode.key().swap(TargetKey);
    Nodes.insert(std::move(SourceNode));
    Nodes.insert(std::move(TargetNode));
    Source.File->Path.swap(SourceIdentity);
    Target.File->Path.swap(TargetIdentity);
    Source.File->PathCharge = SourceCharge;
    Target.File->PathCharge = TargetCharge;
    Source.File->Parent = TargetParent;
    Target.File->Parent = Parent;
    *StorageUsed = Other + SourceCharge + TargetCharge;
    updateNamespaceMetadata(*Source.File, false);
    updateNamespaceMetadata(*Target.File, false);
    Parent->Changed = TargetParent->Changed = true;
    return returned(0);
  }
  // Only Nodes and this lookup may own an immediately reclaimable target.
  // A mapping retains a separate lease even after all descriptors close.
  const uint64_t Credit = Target.File && Target.File.use_count() == 2 &&
                                  Target.File->Lease.use_count() == 1
                              ? Target.bytes().size() + Target.File->PathCharge
                              : 0;
  const uint64_t Other = *StorageUsed - Source.File->PathCharge - Credit;
  const uint64_t Charge = Target.Path.size() + 1;
  if (Target.Path.size() >= limits::Path || Charge > limits::Bytes - Other)
    return unsupported(Result, diagnostic::RenameLimit);
  std::string NewKey = Target.Path, NewIdentity = Target.Path;
  if (Target.File)
    Unlinked.reserve(Unlinked.size() + 1);
  auto Moved = Nodes.extract(Source.Path);
  if (Target.File) {
    updateNamespaceMetadata(*Target.File, true);
    Unlinked.push_back(Target.File);
    Nodes.erase(Target.Path);
  }
  Moved.key().swap(NewKey);
  Nodes.insert(std::move(Moved));
  updateNamespaceMetadata(*Source.File, false);
  Source.File->Path.swap(NewIdentity);
  // Reclaim below deducts Credit exactly once, after the lookup releases it.
  *StorageUsed = *StorageUsed - Source.File->PathCharge + Charge;
  Source.File->PathCharge = Charge;
  Source.File->Parent = TargetParent;
  Parent->Changed = TargetParent->Changed = true;
  Target.File.reset();
  reclaimUnlinked();
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::renameSubtrees(Description &Source, Description &Target,
                            const std::shared_ptr<DirectoryNode> &Parent,
                            const std::shared_ptr<DirectoryNode> &TargetParent,
                            bool Swap, ProcessResult &Result) {
  if (!Swap && Target.Type == Kind::File)
    return returned(NotDirectory, true);
  if (Target.Directory && !Target.Directory->Created &&
      !(Swap ? Target.Directory->Exchangeable : Target.Directory->Removable))
    return unsupported(Result, diagnostic::RenameKind);
  auto Descendant = [](const std::shared_ptr<DirectoryNode> &Directory,
                       const std::shared_ptr<DirectoryNode> &Root) {
    for (const auto *Node = Directory.get(); Node; Node = Node->Parent.get())
      if (Node == Root.get())
        return true;
    return false;
  };
  if (Descendant(TargetParent, Source.Directory) ||
      (Swap && Descendant(Parent, Target.Directory)))
    return returned(InvalidArgument, true);
  if (!Swap && Target.Directory) {
    const auto Prefix = Target.Path + '/';
    const auto File = Nodes.lower_bound(Prefix);
    const auto Directory = Directories.lower_bound(Prefix);
    const auto Link = Links.lower_bound(Prefix);
    if ((File != Nodes.end() &&
         llvm::StringRef(File->first).starts_with(Prefix)) ||
        (Directory != Directories.end() &&
         llvm::StringRef(Directory->first).starts_with(Prefix)) ||
        (Link != Links.end() &&
         llvm::StringRef(Link->first).starts_with(Prefix)))
      return returned(DirectoryNotEmpty, true);
  }
  // This bounded transaction currently plans file/directory rekeys only.
  // A link on either moving side must be refused before reclamation or effects.
  if (llvm::any_of(Links, [&](const auto &Entry) {
        return Descendant(Entry.second->Parent, Source.Directory) ||
               (Swap && Descendant(Entry.second->Parent, Target.Directory));
      }))
    return unsupported(Result, diagnostic::SymbolicLinkDirectoryMove);
  // Reclaim before transaction references are collected. A held orphan file
  // or directory retains its original parent, including an empty target.
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Credit =
      !Swap && Target.Directory && Target.Directory.use_count() == 2
          ? Target.Directory->PathCharge
          : 0;
  struct DirectoryMove {
    std::shared_ptr<DirectoryNode> Node;
    std::string Path, Key;
    bool Linked;
    decltype(Directories)::node_type Entry;
  };
  struct FileMove {
    std::shared_ptr<Contents> Node;
    std::string Path, Key;
    bool Linked;
    decltype(Nodes)::node_type Entry;
  };
  std::vector<DirectoryMove> DirectoryMoves;
  std::vector<FileMove> Files;
  uint64_t OldCharge = 0, NewCharge = 0;
  auto NewPath = [&](const std::string &Path,
                     bool Forward) -> std::optional<std::string> {
    const auto &From = Forward ? Source.Path : Target.Path;
    const auto &To = Forward ? Target.Path : Source.Path;
    if (Path != From && !llvm::StringRef(Path).starts_with(From + '/'))
      return std::nullopt;
    return To + Path.substr(From.size());
  };
  auto MovesDirectory = [&](const std::shared_ptr<DirectoryNode> &Node) {
    return Descendant(Node, Source.Directory) ||
           (Swap && Descendant(Node, Target.Directory));
  };
  auto MovesFile = [&](const std::shared_ptr<Contents> &Node) {
    return Node == Source.File || Descendant(Node->Parent, Source.Directory) ||
           (Swap && (Node == Target.File ||
                     Descendant(Node->Parent, Target.Directory)));
  };
  auto Directory = [&](const std::shared_ptr<DirectoryNode> &Node,
                       bool Linked) -> const char * {
    if (!MovesDirectory(Node))
      return nullptr;
    auto Path = NewPath(Node->Path, Descendant(Node, Source.Directory));
    if (!Path)
      return diagnostic::NamespaceAlias;
    if (Path->size() >= limits::Path)
      return diagnostic::RenameLimit;
    OldCharge += Node->PathCharge;
    NewCharge += Path->size() + 1;
    auto Key = Linked ? *Path : std::string();
    DirectoryMoves.push_back(
        {Node, std::move(*Path), std::move(Key), Linked, {}});
    return nullptr;
  };
  auto File = [&](const std::shared_ptr<Contents> &Node,
                  bool Linked) -> const char * {
    if (!MovesFile(Node))
      return nullptr;
    auto Path =
        NewPath(Node->Path, Node == Source.File ||
                                Descendant(Node->Parent, Source.Directory));
    if (!Path)
      return diagnostic::NamespaceAlias;
    if (Path->size() >= limits::Path)
      return diagnostic::RenameLimit;
    OldCharge += Node->PathCharge;
    NewCharge += Path->size() + 1;
    auto Key = Linked ? *Path : std::string();
    Files.push_back({Node, std::move(*Path), std::move(Key), Linked, {}});
    return nullptr;
  };
  for (const auto &[Path, Node] : Directories)
    if (auto Reason = Directory(Node, true))
      return unsupported(Result, Reason);
  for (const auto &Node : UnlinkedDirectories)
    if (auto Reason = Directory(Node, false))
      return unsupported(Result, Reason);
  for (const auto &[Path, Node] : Nodes)
    if (auto Reason = File(Node, true))
      return unsupported(Result, Reason);
  for (const auto &Node : Unlinked)
    if (auto Reason = File(Node, false))
      return unsupported(Result, Reason);
  const uint64_t Other = *StorageUsed - OldCharge - Credit;
  if (NewCharge > limits::Bytes - Other)
    return unsupported(Result, diagnostic::RenameLimit);
  auto AvailableKey = [&](const std::string &Key) {
    const auto Directory = Directories.find(Key);
    const auto File = Nodes.find(Key);
    return (Directory == Directories.end() ||
            MovesDirectory(Directory->second) ||
            (!Swap && Directory->second == Target.Directory)) &&
           (File == Nodes.end() || MovesFile(File->second));
  };
  for (const auto &Move : DirectoryMoves)
    if (Move.Linked && !AvailableKey(Move.Key))
      return unsupported(Result, diagnostic::NamespaceAlias);
  for (const auto &Move : Files)
    if (Move.Linked && !AvailableKey(Move.Key))
      return unsupported(Result, diagnostic::NamespaceAlias);
  if (!Swap && Target.Directory)
    UnlinkedDirectories.reserve(UnlinkedDirectories.size() + 1);
  // Allocate every path, key and staged node handle before effects. A SWAP
  // keeps both roots linked and supplies no replacement/content/lease credit.
  if (!Swap && Target.Directory) {
    Target.Directory->Linked = false;
    Target.Directory->Changed = true;
    UnlinkedDirectories.push_back(Target.Directory);
    Directories.erase(Target.Path);
  }
  // Opposing subtrees may have identical child suffixes, including mixed
  // root types. Extract both maps completely before inserting any new key.
  for (auto &Move : DirectoryMoves)
    if (Move.Linked)
      Move.Entry = Directories.extract(Move.Node->Path);
  for (auto &Move : Files)
    if (Move.Linked)
      Move.Entry = Nodes.extract(Move.Node->Path);
  for (auto &Move : DirectoryMoves) {
    if (Move.Linked) {
      Move.Entry.key().swap(Move.Key);
      Directories.insert(std::move(Move.Entry));
    }
    Move.Node->Path.swap(Move.Path);
    Move.Node->PathCharge = Move.Node->Path.size() + 1;
  }
  for (auto &Move : Files) {
    if (Move.Linked) {
      Move.Entry.key().swap(Move.Key);
      Nodes.insert(std::move(Move.Entry));
    }
    Move.Node->Path.swap(Move.Path);
    Move.Node->PathCharge = Move.Node->Path.size() + 1;
  }
  if (Source.Directory) {
    Source.Directory->Parent = TargetParent;
    Source.Directory->Changed = true;
  } else {
    Source.File->Parent = TargetParent;
    updateNamespaceMetadata(*Source.File, false);
  }
  if (Swap) {
    if (Target.Directory) {
      Target.Directory->Parent = Parent;
      Target.Directory->Changed = true;
    } else {
      Target.File->Parent = Parent;
      updateNamespaceMetadata(*Target.File, false);
    }
  }
  Parent->Changed = TargetParent->Changed = true;
  // The replaced target remains charged until its real final reference is
  // released. Reclaim deducts Credit once, after lookup/plan references end.
  *StorageUsed = *StorageUsed - OldCharge + NewCharge;
  DirectoryMoves.clear();
  Files.clear();
  if (!Swap) {
    Target.Directory.reset();
    reclaimUnlinked();
  }
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::create(Description &File, uint32_t Mode, ProcessResult &Result) {
  const auto Parent = parentPath(File.Path);
  if (!mutableDirectory(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Charge = File.Path.size() + 1;
  if (File.Path.size() >= limits::Path ||
      FixedEntries + dynamicEntries() >= limits::Files ||
      Charge > limits::Bytes - *StorageUsed)
    return unsupported(Result, diagnostic::FileCreationLimit);
  if (Options->CreationPolicy && !NextCreatedInode)
    return unsupported(Result, diagnostic::FileCreationInode);
  auto Node = std::make_shared<Contents>();
  Node->Writable = true;
  Node->Path = File.Path;
  Node->Parent = directoryNode(Parent);
  Node->PathCharge = Charge;
  if (Options->CreationPolicy) {
    const auto &Policy = *Options->CreationPolicy;
    // Namespace changes invalidate the parent's complete stat observation,
    // but cannot change these supplied device/group fields.
    const auto ParentIdentity = directoryIdentity(Parent);
    if (!ParentIdentity)
      return unsupported(Result, diagnostic::FileCreationParent);
    auto &M = Node->CurrentMetadata.emplace();
    M.Device = ParentIdentity->Device;
    M.GID = ParentIdentity->GID;
    M.UID = EffectiveUID;
    M.Inode = NextCreatedInode;
    M.Mode = FileRegularMode | (Mode & 0777 & ~CurrentUmask);
    M.LinkCount = 1;
    M.BlockSize = Policy.BlockSize;
    M.Generation = Policy.Generation;
    M.AccessTime = M.ModificationTime = M.ChangeTime = M.BirthTime =
        Policy.Time;
    Node->Policy = &Policy.Mutation;
  }
  // A new object never inherits an old observation/policy at the same name.
  File.Type = Kind::File;
  File.File = Node;
  *StorageUsed += Charge;
  Nodes.emplace(File.Path, std::move(Node));
  directoryNode(Parent)->Changed = true;
  if (Options->CreationPolicy)
    NextCreatedInode =
        NextCreatedInode == UINT64_MAX ? 0 : NextCreatedInode + 1;
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::admitMutation(const Description &File, uint64_t Size,
                           ProcessResult &Result) {
  if (!File.File->Writable)
    return unsupported(Result, diagnostic::FileNotWritable);
  if (File.File->Lease.use_count() != 1)
    return unsupported(Result, diagnostic::FileMutationMapping);
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Other = *StorageUsed - File.bytes().size();
  if (Size > limits::Bytes - Other)
    return unsupported(Result, diagnostic::FileMutationLimit);
  return returned(0);
}

void DarwinFiles::publish(
    Description &File, std::vector<uint8_t> Bytes,
    std::optional<std::pair<uint64_t, uint64_t>> Written) {
  *StorageUsed = *StorageUsed - File.bytes().size() + Bytes.size();
  auto &Node = *File.File;
  Node.Modified = std::move(Bytes);
  if (!Node.Policy || Node.MetadataInvalidated) {
    Node.MetadataInvalidated = true;
    return;
  }
  // This allocation rule is opted into explicitly, never inferred from the
  // observed st_blksize or from which input bytes happen to be zero.
  const uint64_t Unit = Node.Policy->AllocationUnit;
  auto Units = [Unit](uint64_t Size) { return (Size + Unit - 1) / Unit; };
  if (!Node.Allocated) {
    Node.Allocated.emplace(Units(Node.Initial.size()), true);
  }
  if (!Node.CurrentMetadata)
    Node.CurrentMetadata = *Node.InitialMetadata;
  Node.Allocated->resize(Units(Node.bytes().size()), false);
  if (Written)
    Node.Allocated->set(Written->first / Unit,
                        Units(Written->first + Written->second));
  auto &M = *Node.CurrentMetadata;
  M.Size = Node.bytes().size();
  M.Blocks = Node.Allocated->count() * (Unit / 512);
  M.ModificationTime = M.ChangeTime = Node.Policy->Time;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::resize(Description &File, uint64_t Size, ProcessResult &Result) {
  auto Admitted = admitMutation(File, Size, Result);
  if (!Admitted || !*Admitted)
    return Admitted;
  // Replace storage even when shrinking, so reclaimed logical capacity cannot
  // accumulate in retained vector allocations across successive files.
  std::vector<uint8_t> Bytes(Size, 0);
  const auto Previous = File.bytes();
  std::copy_n(Previous.begin(), std::min<uint64_t>(Size, Previous.size()),
              Bytes.begin());
  publish(File, std::move(Bytes));
  return returned(0);
}

ServiceResult DarwinFiles::seek(Description &File, uint64_t Offset,
                                uint32_t Whence) {
  if (File.Type != Kind::File && File.Type != Kind::Directory)
    return {IllegalSeek, true};
  if (Whence == SeekHole || Whence == SeekData) {
    if (Offset > INT64_MAX)
      return {InvalidArgument, true};
    const uint64_t Size = File.bytes().size();
    if (Offset >= Size)
      return {NoSuchAddress, true};
    const auto &Node = *File.File;
    const bool Data = Whence == SeekData;
    uint64_t Position = Data ? Offset : Size;
    // Before the first mutation the admitted policy asserts dense allocation.
    // Afterward, consult the same ledger used for st_blocks; zero bytes alone
    // do not describe a hole.
    if (Node.Allocated) {
      const uint64_t Unit = Node.Policy->AllocationUnit;
      const unsigned Index = Offset / Unit;
      if (Node.Allocated->test(Index) == Data) {
        Position = Offset;
      } else {
        const int Next = Data ? Node.Allocated->find_next(Index)
                              : Node.Allocated->find_next_unset(Index);
        if (Next < 0 && Data)
          return {NoSuchAddress, true};
        Position = Next < 0 ? Size : std::min(Size, uint64_t(Next) * Unit);
      }
    }
    File.Offset = Position;
    return {Position, false};
  }
  uint64_t Base;
  switch (Whence) {
  case 0:
    Base = 0;
    break;
  case 1:
    Base = File.Offset;
    break;
  case 2:
    Base = File.Type == Kind::Directory ? File.Metadata->Size
                                        : File.bytes().size();
    break;
  default:
    return {InvalidArgument, true};
  }
  if (Offset <= INT64_MAX) {
    if (Offset > uint64_t(INT64_MAX) - Base)
      return {Overflow, true};
    Base += Offset;
  } else {
    const uint64_t Magnitude = uint64_t(0) - Offset;
    if (Magnitude > Base)
      return {InvalidArgument, true};
    Base -= Magnitude;
  }
  File.Offset = Base;
  return {Base, false};
}

DarwinFiles::MappingSource DarwinFiles::mappingSource(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint32_t(BadDescriptor);
  if (I->second.Open->Type == Kind::Directory)
    return uint32_t(InvalidArgument);
  if (I->second.Open->Type != Kind::File)
    return diagnostic::MemoryFileKind;
  const auto &File = *I->second.Open->File;
  return Mapping{File.bytes(), File.Lease,
                 (I->second.Open->Flags & OpenAccessMask) != OpenWriteOnly};
}

ServiceResult DarwinFiles::duplicate(const Descriptor &Source, uint32_t Minimum,
                                     bool CloseOnExec) {
  const uint32_t FD = freeDescriptor(Minimum);
  if (FD >= limit())
    return {TooManyFiles, true};
  Descriptors.emplace(FD, Descriptor{Source.Open, CloseOnExec});
  return {FD, false};
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::handle(ServiceKind Service, const ProcessServiceEvent &Event,
                    ProcessResult &Result) {
  const auto &A = Event.Arguments;
  if (Service == ServiceKind::Umask) {
    if (!Options || !Options->InitialUmask)
      return unsupported(Result, diagnostic::FileUmask);
    initializeNamespace();
    const auto Previous = CurrentUmask;
    CurrentUmask = A[0] & 07777;
    return returned(Previous);
  }
  if (Service == ServiceKind::Symlink)
    return makeSymbolicLink(A[0], A[1], AtCurrentDirectory, Result);
  if (Service == ServiceKind::SymlinkAt)
    return makeSymbolicLink(A[0], A[2], uint32_t(A[1]), Result);
  if (Service == ServiceKind::ReadLink)
    return readLink(A[0], A[1], uint32_t(A[2]), AtCurrentDirectory, Result);
  if (Service == ServiceKind::ReadLinkAt)
    return readLink(A[1], A[2], A[3], uint32_t(A[0]), Result);
  if (Service == ServiceKind::Open)
    return open(A[0], A[1], AtCurrentDirectory, A[2], Result);
  if (Service == ServiceKind::OpenAt)
    return open(A[1], A[2], A[0], A[3], Result);
  if (Service == ServiceKind::Mkdir)
    return makeDirectory(A[0], AtCurrentDirectory, Result);
  if (Service == ServiceKind::MkdirAt)
    return makeDirectory(A[1], A[0], Result);
  if (Service == ServiceKind::Rmdir)
    return removeDirectory(A[0], AtCurrentDirectory, Result);
  if (Service == ServiceKind::Access)
    return access(A[0], AtCurrentDirectory, A[1], Result);
  if (Service == ServiceKind::FaccessAt) {
    const uint32_t Flags = A[3];
    if (Flags & ~uint32_t(AtEffectiveAccess | AtNoFollow | AtNoFollowAny))
      return returned(InvalidArgument, true);
    return access(
        A[1], A[0], A[2], Result,
        {!(Flags & (AtNoFollow | AtNoFollowAny)), bool(Flags & AtNoFollowAny)});
  }
  if (Service == ServiceKind::Rename)
    return rename(A[0], AtCurrentDirectory, A[1], AtCurrentDirectory,
                  RenameMode::Replace, Result);
  if (Service == ServiceKind::RenameAt || Service == ServiceKind::RenameAtX) {
    auto Mode = RenameMode::Replace;
    if (Service == ServiceKind::RenameAtX) {
      const uint32_t Flags = A[4];
      if ((Flags & ~(RenameSeclude | RenameSwap | RenameExclusive |
                     RenameNoFollowAny)) ||
          (Flags & (RenameSwap | RenameExclusive)) ==
              (RenameSwap | RenameExclusive))
        return returned(InvalidArgument, true);
      if (Flags & RenameSeclude)
        return unsupported(Result, diagnostic::RenameFlags);
      Mode = Flags & RenameSwap        ? RenameMode::Swap
             : Flags & RenameExclusive ? RenameMode::Exclusive
                                       : RenameMode::Replace;
    }
    return rename(A[1], A[0], A[3], A[2], Mode, Result,
                  Service == ServiceKind::RenameAtX &&
                      bool(uint32_t(A[4]) & RenameNoFollowAny));
  }
  if (Service == ServiceKind::Unlink)
    return unlink(A[0], AtCurrentDirectory, Result);
  if (Service == ServiceKind::UnlinkAt) {
    const uint32_t Flags = A[2];
    if (Flags & ~uint32_t(AtRemoveDirectory | AtRemoveDatalessDirectory |
                          AtNoFollowAny | AtSystemDiscarded))
      return returned(InvalidArgument, true);
    if (Flags & (AtRemoveDatalessDirectory | AtSystemDiscarded))
      return unsupported(Result, diagnostic::UnlinkFlags);
    if (Flags & AtRemoveDirectory)
      return removeDirectory(A[1], A[0], Result, bool(Flags & AtNoFollowAny));
    return unlink(A[1], A[0], Result, bool(Flags & AtNoFollowAny));
  }
  if (Service == ServiceKind::Truncate || Service == ServiceKind::Ftruncate) {
    if (A[1] > INT64_MAX)
      return returned(InvalidArgument, true);
    if (Service == ServiceKind::Truncate) {
      auto Resolved = resolvePath(A[0], AtCurrentDirectory);
      if (!Resolved)
        return Resolved.takeError();
      if (auto *Error = std::get_if<uint32_t>(&*Resolved))
        return returned(*Error, true);
      if (auto *Reason = std::get_if<const char *>(&*Resolved))
        return unsupported(Result, *Reason);
      auto &File = std::get<Description>(*Resolved);
      if (File.Type == Kind::Directory)
        return returned(IsDirectory, true);
      return resize(File, A[1], Result);
    }
  }
  if (Service == ServiceKind::Stat64 || Service == ServiceKind::Lstat64)
    return statusPath(A[0], A[1], AtCurrentDirectory, Result,
                      {Service == ServiceKind::Stat64, false});
  if (Service == ServiceKind::FstatAt64) {
    const uint32_t Flags = A[3];
    if (Flags & ~uint32_t(AtNoFollow | AtNoFollowAny | AtFDOnly | AtRealDevice))
      return returned(InvalidArgument, true);
    if (Flags & AtRealDevice)
      return unsupported(Result, diagnostic::FileStatFlags);
    if (!(Flags & AtFDOnly))
      return statusPath(A[1], A[2], A[0], Result,
                        {!(Flags & (AtNoFollow | AtNoFollowAny)),
                         bool(Flags & AtNoFollowAny)});
    // AT_FDONLY ignores the pathname completely, including invalid pointers.
  }
  if (Service == ServiceKind::Chdir) {
    auto Resolved = resolvePath(A[0], AtCurrentDirectory);
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    const auto &Directory = std::get<Description>(*Resolved);
    if (Directory.Type != Kind::Directory)
      return returned(NotDirectory, true);
    CurrentDirectory = Directory.Directory;
    reclaimUnlinked();
    return returned(0);
  }
  const bool Vectored =
      Service == ServiceKind::Readv || Service == ServiceKind::Preadv ||
      Service == ServiceKind::Writev || Service == ServiceKind::Pwritev;
  const bool Writing =
      Service == ServiceKind::Write || Service == ServiceKind::Pwrite ||
      Service == ServiceKind::Writev || Service == ServiceKind::Pwritev;
  const bool Positioned =
      Service == ServiceKind::Pread || Service == ServiceKind::Pwrite ||
      Service == ServiceKind::Preadv || Service == ServiceKind::Pwritev;
  if (Service == ServiceKind::Pwritev && A[3] > INT64_MAX)
    return returned(InvalidArgument, true);
  std::vector<Buffer> Vectors;
  if (Vectored) {
    auto Input = readVectors(A[1], uint32_t(A[2]));
    if (!Input)
      return Input.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Input))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Input))
      return unsupported(Result, *Reason);
    Vectors = std::move(std::get<std::vector<Buffer>>(*Input));
  }
  if (Service == ServiceKind::Pwrite && A[3] == UINT64_MAX)
    return returned(InvalidArgument, true);
  if ((Service == ServiceKind::Read || Service == ServiceKind::Pread ||
       Service == ServiceKind::Write || Service == ServiceKind::Pwrite) &&
      A[2] > MaxWriteBytes)
    return returned(InvalidArgument, true);
  auto I = Descriptors.find(uint32_t(A[0]));
  if (I == Descriptors.end())
    return returned(BadDescriptor, true);
  auto &FD = I->second;
  auto &File = *FD.Open;
  if (Vectored || Writing || Service == ServiceKind::Read ||
      Service == ServiceKind::Pread) {
    const auto Access = File.Flags & OpenAccessMask;
    if ((Writing && !Access) || (!Writing && Access == OpenWriteOnly))
      return returned(BadDescriptor, true);
    const bool Vnode = File.Type == Kind::File || File.Type == Kind::Directory;
    if (Positioned && !Vnode)
      return returned(IllegalSeek, true);
    const Buffer Scalar{A[1], A[2]};
    const llvm::ArrayRef<Buffer> Buffers =
        Vectored ? llvm::ArrayRef<Buffer>(Vectors) : llvm::ArrayRef(Scalar);
    uint64_t Count = 0;
    // Metadata is copied before FD lookup, but uio lengths are admitted only
    // after access and positioned-stream checks. Vnodes have a smaller limit.
    for (const auto &B : Buffers) {
      if (B.Size > uint64_t(INT64_MAX) - Count)
        return returned(InvalidArgument, true);
      Count += B.Size;
    }
    if (Vnode && Count > MaxWriteBytes)
      return returned(InvalidArgument, true);
    const uint64_t Offset = Positioned ? A[3] : File.Offset;
    if (!Writing)
      return read(File, Buffers, Count, Offset, Positioned, Result);
    if (File.Type == Kind::Output || File.Type == Kind::Error)
      return capture(File, Buffers, Count, Vectored, Result);
    return write(File, Buffers, Count, Offset, Positioned, Result);
  }
  switch (Service) {
  case ServiceKind::Ftruncate: {
    if (File.Type != Kind::File || !(File.Flags & OpenAccessMask))
      return returned(InvalidArgument, true);
    auto Truncated = resize(File, A[1], Result);
    if (Truncated && *Truncated && !(**Truncated).Error)
      File.Flags |= FileWasWritten;
    return Truncated;
  }
  case ServiceKind::Fstat64:
    return status(File, A[1], Result);
  case ServiceKind::GetDirEntries64:
    return directory(File, A[1], A[2], A[3], Result);
  case ServiceKind::FstatAt64:
    return status(File, A[2], Result);
  case ServiceKind::Fchdir:
    if (File.Type == Kind::File)
      return returned(NotDirectory, true);
    if (File.Type != Kind::Directory)
      return unsupported(Result, diagnostic::FileDirectoryKind);
    CurrentDirectory = File.Directory;
    reclaimUnlinked();
    return returned(0);
  case ServiceKind::Close:
    Descriptors.erase(I);
    reclaimUnlinked();
    return returned(0);
  case ServiceKind::Lseek:
    if (File.Type == Kind::File || File.Type == Kind::Directory) {
      if (uint32_t(A[2]) == SeekHole || uint32_t(A[2]) == SeekData)
        if (File.Type != Kind::File || !File.File->Policy ||
            File.File->MetadataInvalidated)
          return unsupported(Result, diagnostic::FileSeek);
      if (File.Type == Kind::Directory && uint32_t(A[2]) == 2 && !File.Metadata)
        return unsupported(Result, diagnostic::FileMetadata);
      if (File.Type == Kind::Directory && uint32_t(A[2]) == 2 &&
          File.Directory->Changed)
        return unsupported(Result, diagnostic::DirectoryMutated);
    }
    return std::optional<ServiceResult>(seek(File, A[1], A[2]));
  case ServiceKind::Dup:
    return std::optional<ServiceResult>(duplicate(FD, 0, false));
  case ServiceKind::Dup2: {
    const uint32_t Target = A[1];
    if (Target >= limit())
      return returned(BadDescriptor, true);
    if (Target != I->first)
      Descriptors.insert_or_assign(Target, Descriptor{FD.Open, false});
    reclaimUnlinked();
    return returned(Target);
  }
  case ServiceKind::Fcntl:
    switch (uint32_t(A[1])) {
    case DuplicateFD:
    case DuplicateCloseOnExec:
      if (uint32_t(A[2]) >= limit())
        return returned(InvalidArgument, true);
      return std::optional<ServiceResult>(
          duplicate(FD, A[2], uint32_t(A[1]) == DuplicateCloseOnExec));
    case GetDescriptorFlags:
      return returned(FD.CloseOnExec ? 1 : 0);
    case SetDescriptorFlags:
      FD.CloseOnExec = A[2] & 1;
      return returned(0);
    case GetPath: {
      const auto &Path = File.File        ? File.File->Path
                         : File.Directory ? File.Directory->Path
                                          : File.Path;
      if (Path.empty())
        return unsupported(Result, diagnostic::FilePathIdentity);
      return copyout(
          A[2],
          llvm::ArrayRef<uint8_t>(
              reinterpret_cast<const uint8_t *>(Path.c_str()), Path.size() + 1),
          diagnostic::FilePartialPath, Result);
    }
    case GetFileFlags:
      return returned(File.Flags);
    case SetFileFlags:
      if (uint32_t(A[2]) &
          ~uint32_t(OpenAccessMask | OpenAppend | FileWasWritten))
        return unsupported(Result, diagnostic::FileControl);
      File.Flags = (File.Flags & ~OpenAppend) | (A[2] & OpenAppend);
      return returned(0);
    default:
      return unsupported(Result, diagnostic::FileControl);
    }
  default:
    llvm_unreachable("non-file Darwin service");
  }
}
} // namespace neverd::emulation::darwin_model
