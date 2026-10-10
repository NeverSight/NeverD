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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
namespace limits = darwin_file_limits;
constexpr uint32_t StatAttributeMask =
    AttributeDevice | AttributeBirthTime | AttributeModificationTime |
    AttributeChangeTime | AttributeAccessTime | AttributeUID | AttributeGID |
    AttributeMode | AttributeFlags | AttributeInode;
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
  uint32_t Attributes;
  std::set<std::string> Aliases;
};
bool validTime(const DarwinFileTime &Time) {
  return Time.Nanoseconds >= 0 && Time.Nanoseconds < 1000000000;
}
bool validAllocationUnit(uint32_t Unit) {
  return Unit >= 512 && Unit <= limits::Bytes && !(Unit & (Unit - 1));
}
bool validMutationPolicy(const DarwinFileMutationPolicy &Policy) {
  return validAllocationUnit(Policy.AllocationUnit) && validTime(Policy.Time);
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
  // A mutable ancestor cannot confer mutation authority on an initial link.
  // Each admitted link retains its independent, object-owned grant.
  for (const auto &Directory : Options.MutableDirectories)
    if (llvm::any_of(Options.SymbolicLinks, [&](const auto &Link) {
          return containsPath(Directory, Link.first) &&
                 !Options.MutableSymbolicLinks.contains(Link.first);
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
  for (const auto &[Path, Policy] : Options.DirectoryEnumerationPolicies) {
    const auto M = Options.Metadata.find(Path);
    if (pathKind(Options, Path) != PathKind::Directory ||
        Options.DirectoryContents.contains(Path) ||
        M == Options.Metadata.end() || !M->second.Inode ||
        !Policy.MinimumBufferSize ||
        Policy.MinimumBufferSize > DirectoryPayloadLimit ||
        Policy.InitialMinimumBufferSize > DirectoryPayloadLimit)
      return failure(diagnostic::DirectoryEnumerationOption);
    // Metadata already retains this directory's entry/path charge. The
    // policy map keeps one additional fixed path reference, not a new entry.
    if (auto E = PathInput(Path, true))
      return E;
  }
  uint64_t AttributeCount = 0;
  for (const auto &[Path, Attributes] : Options.ExtendedAttributes) {
    const auto Kind = pathKind(Options, Path);
    if (Kind == PathKind::Missing)
      return failure(diagnostic::ExtendedAttributeOption);
    if (Kind == PathKind::Directory && !Options.Directories.contains(Path) &&
        !Options.Metadata.contains(Path) &&
        !Options.DirectoryContents.contains(Path)) {
      if (++Entries > limits::Files)
        return failure(diagnostic::FileOptionsLimit);
      if (auto E = PathInput(Path, true))
        return E;
    }
    if (Attributes.size() > limits::ExtendedAttributes - AttributeCount)
      return failure(diagnostic::FileOptionsLimit);
    AttributeCount += Attributes.size();
    std::set<llvm::StringRef> Names;
    for (const auto &Attribute : Attributes) {
      if (!validExtendedAttributeName(Attribute.Name) ||
          !ordinaryExtendedAttributeName(Attribute.Name) ||
          !Names.insert(Attribute.Name).second)
        return failure(diagnostic::ExtendedAttributeOption);
      const uint64_t Cost = Attribute.Name.size() + 1;
      if (Cost > limits::Bytes - Total ||
          Attribute.Bytes.size() > limits::Bytes - Total - Cost)
        return failure(diagnostic::FileOptionsLimit);
      Total += Cost + Attribute.Bytes.size();
    }
  }
  for (const auto &[Path, Policy] : Options.DirectoryMutationPolicies) {
    const auto M = Options.Metadata.find(Path);
    if (pathKind(Options, Path) != PathKind::Directory ||
        M == Options.Metadata.end() || !M->second.Inode ||
        !Policy.DirectoryEntrySize ||
        Policy.DirectoryEntrySize > limits::Bytes || !validTime(Policy.Time))
      return failure(diagnostic::DirectoryMetadataMutationOption);
    if (auto E = PathInput(Path, true))
      return E;
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
  for (const auto &Path : Options.MutableExtendedAttributes) {
    const auto Kind = pathKind(Options, Path);
    const auto M = Options.Metadata.find(Path);
    if (Kind == PathKind::Missing ||
        !Options.ExtendedAttributes.contains(Path) || HasAlias(Path) ||
        (M != Options.Metadata.end() &&
         (M->second.Flags ||
          (Kind != PathKind::Directory && M->second.LinkCount != 1))))
      return failure(diagnostic::ExtendedAttributeMutationOption);
    // A grant retains one additional fixed path reference, never a new entry.
    if (auto E = PathInput(Path, Kind == PathKind::Directory))
      return E;
  }
  for (const auto &Path : Options.MutableDirectories) {
    if (pathKind(Options, Path) != PathKind::Directory)
      return failure(diagnostic::DirectoryMutableOption);
    if (!Options.Directories.contains(Path) &&
        !Options.Metadata.contains(Path) &&
        !Options.DirectoryContents.contains(Path) &&
        !Options.ExtendedAttributes.contains(Path) && ++Entries > limits::Files)
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
  for (const auto &Path : Options.MutableSymbolicLinks) {
    const auto Parent = parentPath(Path);
    if (!Options.SymbolicLinks.contains(Path) ||
        !Options.MutableDirectories.contains(Parent))
      return failure(diagnostic::SymbolicLinkMutableOption);
    if (auto E = PathInput(Path, false))
      return E;
    const auto M = Options.Metadata.find(Path);
    if (M != Options.Metadata.end() &&
        (M->second.Flags || (M->second.Mode & 07000)))
      return failure(diagnostic::NamespaceFlags);
    if (HasAlias(Path) ||
        (M != Options.Metadata.end() && M->second.LinkCount != 1))
      return failure(diagnostic::NamespaceAlias);
    const auto P = Options.Metadata.find(Parent);
    if (M != Options.Metadata.end() && P != Options.Metadata.end() &&
        M->second.Device != P->second.Device)
      return failure(diagnostic::SymbolicLinkMutableDevice);
  }
  for (const auto &[Path, Policy] : Options.SymbolicLinkMutationPolicies) {
    const auto M = Options.Metadata.find(Path);
    if (!Options.MutableSymbolicLinks.contains(Path) ||
        M == Options.Metadata.end() || !M->second.Inode ||
        !validTime(Policy.Time))
      return failure(diagnostic::SymbolicLinkMetadataMutationOption);
    if (auto E = PathInput(Path, false))
      return E;
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
        Options.Files.contains(Path) || Options.SymbolicLinks.contains(Path)
            ? parentPath(Path)
            : Path;
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
    if (Policy.Namespace &&
        (!validAllocationUnit(Policy.Namespace->SymbolicLinkAllocationUnit) ||
         !Policy.Namespace->DirectoryEntrySize ||
         Policy.Namespace->DirectoryEntrySize > limits::Bytes ||
         Policy.Namespace->DirectoryBlocks > INT64_MAX))
      return failure(diagnostic::FileCreationPolicy);
    for (const auto &Path : Options.MutableDirectories)
      if (!Options.Metadata.contains(Path))
        return failure(diagnostic::FileCreationParent);
  }
  std::set<std::string> Aliases;
  for (const auto &[Path, Inode] : Inodes)
    if (HasAlias(Path))
      Aliases.insert(Path);
  return FileFootprint{Total, uint32_t(Entries), uint32_t(AttributeCount),
                       std::move(Aliases)};
}
} // namespace

bool validExtendedAttributeName(llvm::StringRef Name) {
  if (Name.empty() || Name.size() > limits::ExtendedAttributeName ||
      Name.contains('\0'))
    return false;
  const auto *Start = reinterpret_cast<const llvm::UTF8 *>(Name.data());
  return llvm::isLegalUTF8String(&Start, Start + Name.size());
}
bool ordinaryExtendedAttributeName(llvm::StringRef Name) {
  return !Name.starts_with("com.apple.system.") &&
         Name != "com.apple.ResourceFork" && Name != "com.apple.FinderInfo" &&
         Name != "com.apple.decmpfs";
}

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
    Node->InitialPath = &Path;
    Node->Name->ChargedEntry = true;
    Node->Name->Path = Path;
    Node->Name->Parent = initialDirectoryNode(parentPath(Path));
    Node->Writable = Options->WritableFiles.contains(Path);
    const auto Attributes = Options->ExtendedAttributes.find(Path);
    if (Attributes != Options->ExtendedAttributes.end())
      Node->ExtendedAttributes.attach(
          Attributes->second,
          Options->MutableExtendedAttributes.contains(Path));
    const auto Metadata = Options->Metadata.find(Path);
    if (Metadata != Options->Metadata.end())
      Node->InitialMetadata = &Metadata->second;
    const auto Policy = Options->MutationPolicies.find(Path);
    if (Policy != Options->MutationPolicies.end())
      Node->Policy = &Policy->second;
    Nodes.emplace(Path, std::make_shared<FileEntry>(std::move(Node)));
  }
  for (const auto &[Path, Target] : Options->SymbolicLinks) {
    auto Node = std::make_shared<LinkNode>();
    Node->Initial = Target;
    Node->InitialPath = &Path;
    Node->Name->Path = Path;
    Node->Name->Parent = initialDirectoryNode(parentPath(Path));
    Node->Name->Protected = !Options->MutableSymbolicLinks.contains(Path);
    const auto Attributes = Options->ExtendedAttributes.find(Path);
    if (Attributes != Options->ExtendedAttributes.end())
      Node->ExtendedAttributes.attach(
          Attributes->second,
          Options->MutableExtendedAttributes.contains(Path));
    const auto Metadata = Options->Metadata.find(Path);
    if (Metadata != Options->Metadata.end())
      Node->Metadata = &Metadata->second;
    const auto Policy = Options->SymbolicLinkMutationPolicies.find(Path);
    if (Policy != Options->SymbolicLinkMutationPolicies.end())
      Node->MutationTime = &Policy->second.Time;
    Links.emplace(Path, std::make_shared<LinkEntry>(std::move(Node)));
  }
  NamespaceReady = true;
  if (Options->WorkingDirectory)
    CurrentDirectory = Directories.at(*Options->WorkingDirectory);
}

void DarwinFiles::reclaimUnlinked() {
  for (auto I = Unlinked.begin(); I != Unlinked.end();) {
    if (I->use_count() == 1 && (*I)->Lease.use_count() == 1) {
      *StorageUsed -=
          (*I)->bytes().size() + (*I)->ExtendedAttributes.DynamicCharge;
      AttributeSlots -= (*I)->ExtendedAttributes.ExtraCount;
      I = Unlinked.erase(I);
    } else {
      ++I;
    }
  }
  for (auto I = UnlinkedLinks.begin(); I != UnlinkedLinks.end();) {
    if (I->use_count() == 1) {
      const auto &Link = **I;
      *StorageUsed -= (Link.CreatedTarget ? Link.CreatedTarget->size() : 0) +
                      Link.ExtendedAttributes.DynamicCharge;
      AttributeSlots -= Link.ExtendedAttributes.ExtraCount;
      I = UnlinkedLinks.erase(I);
    } else {
      ++I;
    }
  }
  // Objects release their final identity first. A description can also retain
  // a detached identity while the shared object still has linked names.
  for (auto I = DetachedNames.begin(); I != DetachedNames.end();) {
    if (I->use_count() == 1) {
      *StorageUsed -= (*I)->PathCharge;
      I = DetachedNames.erase(I);
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
        *StorageUsed -=
            (*I)->PathCharge + (*I)->ExtendedAttributes.DynamicCharge;
        AttributeSlots -= (*I)->ExtendedAttributes.ExtraCount;
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
  const auto Attributes = Options->ExtendedAttributes.find(Path);
  if (Attributes != Options->ExtendedAttributes.end())
    Node->ExtendedAttributes.attach(
        Attributes->second, Options->MutableExtendedAttributes.contains(Path));
  const auto Metadata = Options->Metadata.find(Path);
  if (Metadata != Options->Metadata.end()) {
    Node->Metadata = &Metadata->second;
    Node->Identity =
        DirectoryIdentity{Metadata->second.Device, Metadata->second.GID};
  }
  const auto Snapshot = Options->DirectoryContents.find(Path);
  if (Snapshot != Options->DirectoryContents.end())
    Node->Snapshot = &Snapshot->second;
  const auto Enumeration = Options->DirectoryEnumerationPolicies.find(Path);
  if (Enumeration != Options->DirectoryEnumerationPolicies.end()) {
    Node->EnumerationPolicy = &Enumeration->second;
    Node->BulkAttributes = Enumeration->second.BulkAttributes;
  }
  const auto Mutation = Options->DirectoryMutationPolicies.find(Path);
  if (Mutation != Options->DirectoryMutationPolicies.end()) {
    Node->DirectoryEntrySize = Mutation->second.DirectoryEntrySize;
    Node->MutationTime = &Mutation->second.Time;
  }
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
  // FixedEntries includes initial symbolic/directory entries. Detached names
  // own their entry costs; zero-name objects add no second orphan entry slot.
  return Nodes.size() +
         llvm::count_if(Links,
                        [](const auto &Entry) {
                          return Entry.second->Name->ChargedEntry;
                        }) +
         llvm::count_if(DetachedNames,
                        [](const auto &Name) { return Name->ChargedEntry; }) +
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
    AttributeSlots = Footprint->Attributes;
    InitialAliases = std::move(Footprint->Aliases);
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
  if (I->second.Open->Type == Kind::File ||
      I->second.Open->Type == Kind::SymbolicLink)
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
  if (Flags &
      ~uint32_t(OpenCloseOnExec | OpenDirectory | OpenSymbolic |
                OpenAccessMask | OpenNonBlocking | OpenAppend | OpenTruncate |
                OpenCreate | OpenExclusive | OpenNoFollow | OpenNoFollowAny))
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
  const bool RetainSymbolic = (Flags & OpenSymbolic) && !(Flags & OpenCreate);
  auto Resolved = resolvePath(
      Address, DirectoryFD,
      Flags & OpenCreate ? LookupMode::CreateFile : LookupMode::Existing,
      {!(Flags & OpenNoFollow) && !ExclusiveCreate && !RetainSymbolic,
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
  if (File.Type == Kind::SymbolicLink &&
      (!RetainSymbolic || (Flags & OpenNoFollow)))
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
  if (Mutating && File.Type == Kind::File && !File.File->Writable)
    return unsupported(Result, diagnostic::FileNotWritable);
  File.Flags = Flags & (OpenAccessMask | OpenNonBlocking | OpenAppend);
  if ((Flags & OpenTruncate) && !Created) {
    if (File.Type == Kind::File) {
      auto Truncated = resize(File, 0, Result);
      if (!Truncated || !*Truncated || (**Truncated).Error)
        return Truncated;
    }
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
        const auto Bytes = Link->second->Object->bytes();
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
      const auto &Entry = Nodes.at(File.Path);
      File.File = Entry->Object;
      File.Name = Entry->Name;
      File.Metadata = nullptr;
    } else if (Type == PathKind::SymbolicLink) {
      const auto &Entry = this->Links.at(File.Path);
      File.Link = Entry->Object;
      File.Name = Entry->Name;
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

const DarwinFileMetadata *DarwinFiles::metadata(const Description &File) const {
  if (File.File)
    return File.File->metadata();
  if (File.Directory && File.Directory->CurrentMetadata)
    return &*File.Directory->CurrentMetadata;
  if (File.Link) {
    if (File.Link->MetadataInvalidated)
      return nullptr;
    if (File.Link->CurrentMetadata)
      return &*File.Link->CurrentMetadata;
  }
  return File.Metadata;
}

DarwinFiles::StatusMetadata
DarwinFiles::statusMetadata(const Description &File) const {
  if (File.Type == Kind::Directory &&
      (File.Directory->MetadataInvalidated ||
       (File.Directory->Changed && !File.Directory->CurrentMetadata)))
    return diagnostic::DirectoryMutated;
  if (File.File && File.File->MetadataInvalidated)
    return diagnostic::FileMutatedMetadata;
  if (File.Link && File.Link->MetadataInvalidated)
    return diagnostic::SymbolicLinkMutatedMetadata;
  const auto *Metadata = metadata(File);
  if (!Metadata)
    return diagnostic::FileMetadata;
  return Metadata;
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::status(const Description &File, uint64_t Address,
                    ProcessResult &Result) {
  auto Selected = statusMetadata(File);
  if (auto *Reason = std::get_if<const char *>(&Selected))
    return unsupported(Result, *Reason);
  const auto *Metadata = std::get<const DarwinFileMetadata *>(Selected);
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

llvm::Expected<DarwinFiles::AttributeInput>
DarwinFiles::readAttributes(uint64_t Address) {
  auto Prefix = userMemoryPrefix(Memory, Address, AttributeRequestSize, Read);
  if (!Prefix)
    return Prefix.takeError();
  if (!*Prefix)
    return AttributeInput(uint32_t(BadAddress));
  if (*Prefix != AttributeRequestSize)
    return AttributeInput(diagnostic::FileAttributePartialInput);
  std::array<uint8_t, AttributeRequestSize> Bytes{};
  if (auto E = Memory.read(Address, Bytes))
    return std::move(E);
  AttributeRequest Request{llvm::support::endian::read16le(Bytes.data()), {}};
  // Native ignores the reserved16 word; masks retain all 32 bits.
  for (unsigned I = 0; I != Request.Masks.size(); ++I)
    Request.Masks[I] =
        llvm::support::endian::read32le(Bytes.data() + 4 + I * 4);
  return AttributeInput(Request);
}

bool DarwinFiles::supportedAttributeMask(uint32_t Mask) {
  return !(Mask & ~(StatAttributeMask | AttributeName | AttributeObjectType |
                    AttributeReturned));
}

DarwinFiles::AttributeRecord
DarwinFiles::attributeRecord(const Description &File, uint32_t Mask,
                             std::optional<llvm::StringRef> EntryName) const {
  if (!supportedAttributeMask(Mask))
    return diagnostic::FileAttributeSelection;
  const uint32_t Type = File.Type == Kind::File           ? 1
                        : File.Type == Kind::Directory    ? 2
                        : File.Type == Kind::SymbolicLink ? 5
                                                          : 0;
  if (!Type)
    return diagnostic::FileAttributeKind;
  std::optional<llvm::StringRef> Name;
  if (Mask & AttributeName) {
    if (!EntryName && File.ambiguousName())
      return diagnostic::HardLinkName;
    const auto Path = File.path();
    // The root's label is a mount observation, not its synthetic guest path.
    // Select the declared object's spelling, never the caller's path alias.
    if (!EntryName && (Path.empty() || Path == "/"))
      return diagnostic::FileAttributeName;
    Name = EntryName ? *EntryName : Path.substr(Path.rfind('/') + 1);
    const auto *Start = reinterpret_cast<const llvm::UTF8 *>(Name->data());
    if (Name->empty() || Name->size() > limits::Name ||
        !llvm::isLegalUTF8String(&Start, Start + Name->size()))
      return diagnostic::FileAttributeName;
  }
  const DarwinFileMetadata *Metadata = nullptr;
  if (Mask & StatAttributeMask) {
    auto Selected = statusMetadata(File);
    if (auto *Reason = std::get_if<const char *>(&Selected))
      return *Reason;
    Metadata = std::get<const DarwinFileMetadata *>(Selected);
  }
  std::vector<uint8_t> Bytes(4);
  auto Put = [&](uint64_t Value, unsigned Width) {
    const size_t Offset = Bytes.size();
    Bytes.resize(Offset + Width);
    if (Width == 4)
      llvm::support::endian::write32le(Bytes.data() + Offset, Value);
    else
      llvm::support::endian::write64le(Bytes.data() + Offset, Value);
  };
  if (Mask & AttributeReturned) {
    Put(Mask, 4);
    for (unsigned I = 0; I != 4; ++I)
      Put(0, 4);
  }
  const size_t NameReference = Bytes.size();
  if (Name) {
    Put(0, 4);
    Put(Name->size() + 1, 4);
  }
  if (Mask & AttributeDevice)
    Put(uint32_t(Metadata->Device), 4);
  if (Mask & AttributeObjectType)
    Put(Type, 4);
  for (auto [Bit, Time] :
       {std::pair{AttributeBirthTime, &DarwinFileMetadata::BirthTime},
        std::pair{AttributeModificationTime,
                  &DarwinFileMetadata::ModificationTime},
        std::pair{AttributeChangeTime, &DarwinFileMetadata::ChangeTime},
        std::pair{AttributeAccessTime, &DarwinFileMetadata::AccessTime}})
    if (Mask & Bit) {
      const auto &Value = Metadata->*Time;
      Put(uint64_t(Value.Seconds), 8);
      Put(uint64_t(Value.Nanoseconds), 8);
    }
  if (Mask & AttributeUID)
    Put(Metadata->UID, 4);
  if (Mask & AttributeGID)
    Put(Metadata->GID, 4);
  if (Mask & AttributeMode)
    Put(Metadata->Mode, 4);
  if (Mask & AttributeFlags)
    Put(Metadata->Flags, 4);
  if (Mask & AttributeInode)
    Put(Metadata->Inode, 8);
  if (Name) {
    const size_t Start = Bytes.size();
    llvm::support::endian::write32le(Bytes.data() + NameReference,
                                     Start - NameReference);
    Bytes.insert(Bytes.end(), Name->begin(), Name->end());
    Bytes.resize((Bytes.size() + 1 + 3) & ~size_t(3), 0);
  }
  // Admitted native queries report their complete required size,
  // including short buffers, independently of FULLSIZE/RETURNED_ATTRS.
  llvm::support::endian::write32le(Bytes.data(), Bytes.size());
  return Bytes;
}

llvm::Expected<std::optional<ServiceResult>> DarwinFiles::attributeList(
    const Description &File, const AttributeRequest &Request, uint64_t Address,
    uint64_t Size, uint64_t Options, ProcessResult &Result) {
  if (Size < 4)
    return returned(ResultTooLarge, true);
  if (Request.BitmapCount != AttributeBitmapCount)
    return returned(InvalidArgument, true);
  if (llvm::any_of(llvm::ArrayRef(Request.Masks).drop_front(),
                   [](uint32_t Mask) { return Mask != 0; }))
    return unsupported(Result, diagnostic::FileAttributeSelection);
  const uint32_t Mask = Request.Masks[0];
  if ((Options & AttributePackInvalid) && !(Mask & AttributeReturned))
    return returned(InvalidArgument, true);
  if (Options & ~uint64_t(AttributeNoFollow | AttributeFullSize |
                          AttributePackInvalid | AttributeNoFollowAny))
    return unsupported(Result, diagnostic::FileAttributeOptions);
  auto Built = attributeRecord(File, Mask);
  if (auto *Reason = std::get_if<const char *>(&Built))
    return unsupported(Result, *Reason);
  // Signed residual admission stays after selected object observations.
  if (Size > INT64_MAX)
    return returned(InvalidArgument, true);
  const auto &Bytes = std::get<std::vector<uint8_t>>(Built);
  return copyout(
      Address,
      llvm::ArrayRef(Bytes).take_front(std::min<uint64_t>(Size, Bytes.size())),
      diagnostic::FileAttributePartialOutput, Result);
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
  const auto *Policy =
      Options->CreationPolicy && Options->CreationPolicy->Namespace
          ? &*Options->CreationPolicy
          : nullptr;
  if (Policy && !NextCreatedInode)
    return unsupported(Result, diagnostic::FileCreationInode);
  if (Policy && !Parent->Identity)
    return unsupported(Result, diagnostic::FileCreationParent);
  auto Node = std::make_shared<LinkNode>();
  Node->Created = true;
  Node->Name->ChargedEntry = true;
  Node->CreatedTarget.emplace(Bytes.begin(), Bytes.end());
  Node->Name->Path = File.Path;
  Node->Name->PathCharge = File.Path.size() + 1;
  Node->Name->Parent = Parent;
  if (Policy) {
    const uint64_t Unit = Policy->Namespace->SymbolicLinkAllocationUnit;
    Node->CurrentMetadata = createdMetadata(
        *Parent->Identity, FileSymbolicLinkMode | (0777 & ~CurrentUmask), 1,
        Bytes.size(), ((Bytes.size() + Unit - 1) / Unit) * (Unit / 512));
    Node->MutationTime = &Policy->Mutation.Time;
  }
  // Reused names never inherit an old Options.Metadata observation. The
  // namespace extension is the only authority for link/directory metadata.
  Links.emplace(File.Path, std::make_shared<LinkEntry>(std::move(Node)));
  *StorageUsed += Charge;
  updateDirectoryMetadata(*Parent, true);
  if (Policy)
    consumeCreatedInode();
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::makeDirectory(uint64_t Path, uint32_t DirectoryFD, uint32_t Mode,
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
  const auto *Policy =
      Options->CreationPolicy && Options->CreationPolicy->Namespace
          ? &*Options->CreationPolicy
          : nullptr;
  if (Policy && !NextCreatedInode)
    return unsupported(Result, diagnostic::FileCreationInode);
  const auto ParentIdentity = directoryIdentity(Parent);
  if (Policy && !ParentIdentity)
    return unsupported(Result, diagnostic::FileCreationParent);
  auto Node = std::make_shared<DirectoryNode>();
  Node->Path = File.Path;
  Node->Parent = directoryNode(Parent);
  Node->Identity = ParentIdentity;
  Node->Created = Node->Mutable = Node->NonMount = true;
  Node->PathCharge = Charge;
  Node->SwapSupport = Node->Parent->SwapSupport;
  Node->EnumerationPolicy = Node->Parent->EnumerationPolicy;
  if (Policy) {
    Node->DirectoryEntrySize = Policy->Namespace->DirectoryEntrySize;
    Node->MutationTime = &Policy->Mutation.Time;
    Node->CurrentMetadata = createdMetadata(
        *ParentIdentity, FileDirectoryMode | (Mode & 0777 & ~CurrentUmask), 2,
        uint64_t(2) * Node->DirectoryEntrySize,
        Policy->Namespace->DirectoryBlocks);
  }
  Directories.emplace(File.Path, std::move(Node));
  *StorageUsed += Charge;
  updateDirectoryMetadata(*directoryNode(Parent), true);
  if (Policy)
    consumeCreatedInode();
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
  updateDirectoryMetadata(*File.Directory, false);
  UnlinkedDirectories.push_back(File.Directory);
  Directories.erase(File.Path);
  updateDirectoryMetadata(*File.Directory->Parent, true);
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::hardLink(uint64_t SourcePath, uint32_t SourceDirectory,
                      uint64_t TargetPath, uint32_t TargetDirectory,
                      bool FollowFinal, ProcessResult &Result) {
  auto From = resolvePath(SourcePath, SourceDirectory, LookupMode::Existing,
                          {FollowFinal, false});
  if (!From)
    return From.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*From))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*From))
    return unsupported(Result, *Reason);
  auto &Source = std::get<Description>(*From);
  // Source lookup, including the directory rejection, precedes destination
  // import. Neither a faulting destination nor an existing name overrides it.
  if (Source.Directory)
    return returned(OperationNotPermitted, true);
  auto To = resolvePath(TargetPath, TargetDirectory, LookupMode::CreateFile,
                        {false, false});
  if (!To)
    return To.takeError();
  if (auto *Error = std::get_if<uint32_t>(&*To))
    return returned(*Error, true);
  if (auto *Reason = std::get_if<const char *>(&*To))
    return unsupported(Result, *Reason);
  const auto &Target = std::get<Description>(*To);
  if (Target.Type != Kind::Missing)
    return returned(FileExists, true);
  const auto Parent = directoryNode(parentPath(Target.Path));
  if (!Parent->Mutable)
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (Parent->mountAncestor() != Source.Name->Parent->mountAncestor())
    return unsupported(Result, diagnostic::RenameMount);
  const auto *Initial =
      Source.File ? Source.File->InitialMetadata : Source.Link->Metadata;
  const NamedObject &Object =
      Source.File ? static_cast<const NamedObject &>(*Source.File)
                  : static_cast<const NamedObject &>(*Source.Link);
  const auto *Identity = Source.File ? Source.File->metadata()
                         : Source.Link->CurrentMetadata
                             ? &*Source.Link->CurrentMetadata
                             : Source.Link->Metadata;
  if (Identity && (Identity->Flags || (Identity->Mode & 07000)))
    return unsupported(Result, diagnostic::NamespaceFlags);
  if (Identity && Parent->Identity &&
      Identity->Device != Parent->Identity->Device)
    return unsupported(Result, diagnostic::RenameMount);
  if (auto E = prepareMutation())
    return std::move(E);
  if (Object.InitialPath && (InitialAliases.contains(*Object.InitialPath) ||
                             (Initial && Initial->LinkCount != 1)))
    return unsupported(Result, diagnostic::HardLinkSource);
  const uint64_t Charge = Target.Path.size() + 1;
  if (Target.Path.size() >= limits::Path ||
      FixedEntries + dynamicEntries() >= limits::Files ||
      Charge > limits::Bytes - *StorageUsed)
    return unsupported(Result, diagnostic::HardLinkLimit);
  auto Name = std::make_shared<NameIdentity>();
  Name->Path = Target.Path;
  Name->Parent = Parent;
  Name->PathCharge = Charge;
  Name->ChargedEntry = true;
  // Construct the complete entry and insert its map node before publishing
  // counts, history, metadata or charges. Hard links consume no new inode.
  if (Source.File) {
    Nodes.emplace(Target.Path, std::make_shared<FileEntry>(Source.File, Name));
    ++Source.File->LinkedNames;
    Source.File->HadMultipleNames = true;
    updateNamespaceMetadata(*Source.File);
  } else {
    Links.emplace(Target.Path, std::make_shared<LinkEntry>(Source.Link, Name));
    ++Source.Link->LinkedNames;
    Source.Link->HadMultipleNames = true;
    updateNamespaceMetadata(*Source.Link);
  }
  *StorageUsed += Charge;
  updateDirectoryMetadata(*Parent, true);
  return returned(0);
}

void DarwinFiles::reserveDetachedName(const Description &File) {
  DetachedNames.reserve(DetachedNames.size() + 1);
  if (File.File && File.File->LinkedNames == 1)
    Unlinked.reserve(Unlinked.size() + 1);
  if (File.Link && File.Link->LinkedNames == 1)
    UnlinkedLinks.reserve(UnlinkedLinks.size() + 1);
}

void DarwinFiles::detachName(Description &File) {
  DetachedNames.push_back(File.Name);
  auto Detach = [&](auto &Table, auto &Object) {
    Table.erase(File.Name->Path);
    --Object->LinkedNames;
    if (!Object->LinkedNames) {
      Object->Name = File.Name;
    } else if (Object->Name == File.Name) {
      // Select a surviving identity only for virtual lifetime accounting.
      // Multiple-name vnode observations remain permanently unsupported.
      for (const auto &[Path, Entry] : Table)
        if (Entry->Object == Object) {
          Object->Name = Entry->Name;
          break;
        }
    }
  };
  if (File.File) {
    Detach(Nodes, File.File);
    updateNamespaceMetadata(*File.File);
    if (!File.File->LinkedNames)
      Unlinked.push_back(File.File);
  } else {
    Detach(Links, File.Link);
    updateNamespaceMetadata(*File.Link);
    if (!File.Link->LinkedNames)
      UnlinkedLinks.push_back(File.Link);
  }
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
    if (File.Name->Protected)
      return unsupported(Result, diagnostic::SymbolicLinkMutation);
    if (!File.Name->Parent->Mutable)
      return unsupported(Result, diagnostic::DirectoryNotMutable);
    if (auto E = prepareMutation())
      return std::move(E);
    reserveDetachedName(File);
    detachName(File);
    updateDirectoryMetadata(*File.Name->Parent, true);
    // Release only this operation's temporary observers. Held descriptions
    // continue to own their object and separately charged selected name.
    File.Link.reset();
    File.Name.reset();
    reclaimUnlinked();
    return returned(0);
  }
  const auto Parent = parentPath(File.Path);
  if (!mutableDirectory(Parent))
    return unsupported(Result, diagnostic::DirectoryNotMutable);
  if (auto E = prepareMutation())
    return std::move(E);
  reserveDetachedName(File);
  detachName(File);
  updateDirectoryMetadata(*directoryNode(Parent), true);
  return returned(0);
}

void DarwinFiles::updateNamespaceMetadata(Contents &Node) {
  if (Node.Policy && !Node.MetadataInvalidated) {
    if (!Node.CurrentMetadata)
      Node.CurrentMetadata = *Node.InitialMetadata;
    Node.CurrentMetadata->LinkCount = Node.LinkedNames;
    Node.CurrentMetadata->ChangeTime = Node.Policy->Time;
  } else {
    Node.MetadataInvalidated = true;
  }
}

void DarwinFiles::updateNamespaceMetadata(LinkNode &Node) {
  if (Node.MutationTime) {
    if (!Node.CurrentMetadata)
      Node.CurrentMetadata = *Node.Metadata;
    Node.CurrentMetadata->LinkCount = Node.LinkedNames;
    Node.CurrentMetadata->ChangeTime = *Node.MutationTime;
  } else if (Node.Metadata) {
    Node.MetadataInvalidated = true;
  }
}

void DarwinFiles::updateDirectoryMetadata(DirectoryNode &Node,
                                          bool ContentsChanged) {
  Node.Changed = true;
  // A direct move can change dotdot; child-name changes can change ordinal
  // positions. Saturation prevents old cursors becoming valid after wrap and
  // allocates nothing after the namespace transaction has been published.
  if (Node.EnumerationPolicy && Node.EnumerationVersion != UINT64_MAX)
    ++Node.EnumerationVersion;
  // Only the explicit initial-directory policy authorizes keeping the full
  // observed record. Copying its scalar fields allocates nothing after commit.
  if (Node.MetadataInvalidated)
    return;
  if (!Node.CurrentMetadata && Node.DirectoryEntrySize)
    Node.CurrentMetadata = *Node.Metadata;
  if (!Node.CurrentMetadata)
    return;
  auto &M = *Node.CurrentMetadata;
  M.ChangeTime = *Node.MutationTime;
  if (!ContentsChanged)
    return;
  // Membership comes from the committed object namespace, not textual prefix
  // counts. Held orphans and reused names must not become children again.
  uint16_t Count = 2;
  forEachDirectoryChild(Node, [&](const auto &) { ++Count; });
  M.LinkCount = Count;
  M.Size = uint64_t(Count) * Node.DirectoryEntrySize;
  M.ModificationTime = *Node.MutationTime;
}

DarwinFileMetadata DarwinFiles::createdMetadata(const DirectoryIdentity &Parent,
                                                uint16_t Mode,
                                                uint16_t LinkCount,
                                                uint64_t Size,
                                                uint64_t Blocks) const {
  const auto &Policy = *Options->CreationPolicy;
  DarwinFileMetadata M;
  M.Device = Parent.Device;
  M.GID = Parent.GID;
  M.UID = EffectiveUID;
  M.Inode = NextCreatedInode;
  M.Mode = Mode;
  M.LinkCount = LinkCount;
  M.Size = Size;
  M.Blocks = Blocks;
  M.BlockSize = Policy.BlockSize;
  M.Generation = Policy.Generation;
  M.AccessTime = M.ModificationTime = M.ChangeTime = M.BirthTime = Policy.Time;
  return M;
}

void DarwinFiles::consumeCreatedInode() {
  NextCreatedInode = NextCreatedInode == UINT64_MAX ? 0 : NextCreatedInode + 1;
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
  if (Source.Link && Source.Name->Protected)
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
  if (Target.Link && Target.Name->Protected)
    return unsupported(Result, diagnostic::SymbolicLinkMutation);
  if (Mode == RenameMode::Exclusive &&
      (Target.File || Target.Directory || Target.Link)) {
    // Same-object exclusive rename depends on filesystem case sensitivity.
    // Exact catalogue keys do not supply that missing filesystem property.
    if ((Source.File && Target.File == Source.File) ||
        (Source.Directory && Target.Directory == Source.Directory) ||
        (Source.Link && Target.Link == Source.Link))
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
    if ((Target.Type != Kind::File && Target.Type != Kind::Directory &&
         Target.Type != Kind::SymbolicLink) ||
        (Target.Directory &&
         (!Target.Directory->Linked ||
          (!Target.Directory->Created && !Target.Directory->Exchangeable))))
      return unsupported(Result, diagnostic::RenameSwapKind);
  }
  const auto Parent =
      Source.Name ? Source.Name->Parent : Source.Directory->Parent;
  const auto TargetParent = Target.Link
                                ? Target.Name->Parent
                                : directoryNode(parentPath(Target.Path));
  // Device equality alone does not establish one mount. Only mkdir and
  // declared initial subtrees supply non-mount parent relationships.
  if (Parent->mountAncestor() != TargetParent->mountAncestor())
    return unsupported(Result, diagnostic::RenameMount);
  // Even a same-name native rename performs authorization. The namespace
  // grant excludes known restricted flags, aliases and special parent modes.
  if (!Parent->Mutable || !TargetParent->Mutable)
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
  for (const auto *Metadata : {metadata(Source), metadata(Target)})
    if (Metadata && !SameDevice(Metadata->Device))
      return unsupported(Result, diagnostic::RenameMount);
  // Initial identity remains authoritative even after complete stat becomes
  // unavailable; a prior move must not discard known mount contradictions.
  for (const auto &Link : {Source.Link, Target.Link})
    if (Link && Link->Metadata && !SameDevice(Link->Metadata->Device))
      return unsupported(Result, diagnostic::RenameMount);
  if (Source.Path == Target.Path)
    return returned(0);
  if (Mode == RenameMode::Swap &&
      (!Parent->SwapSupport || !TargetParent->SwapSupport))
    return unsupported(Result, diagnostic::RenameSwapSupport);
  if ((Source.File && Source.File == Target.File) ||
      (Source.Link && Source.Link == Target.Link))
    return returned(0);
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
    const uint64_t SourceOld = Source.Name->PathCharge;
    const uint64_t TargetOld = Target.Name->PathCharge;
    const uint64_t Other = *StorageUsed - SourceOld - TargetOld;
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
    auto Exchange = [&](auto &SourceTable, auto &TargetTable) {
      auto SourceNode = SourceTable.extract(Source.Path);
      auto TargetNode = TargetTable.extract(Target.Path);
      SourceNode.key().swap(SourceKey);
      TargetNode.key().swap(TargetKey);
      SourceTable.insert(std::move(SourceNode));
      TargetTable.insert(std::move(TargetNode));
    };
    if (Source.Link) {
      if (Target.Link)
        Exchange(Links, Links);
      else
        Exchange(Links, Nodes);
      Source.Name->Path.swap(SourceIdentity);
      Source.Name->PathCharge = SourceCharge;
      Source.Name->Parent = TargetParent;
      updateNamespaceMetadata(*Source.Link);
    } else {
      if (Target.Link)
        Exchange(Nodes, Links);
      else
        Exchange(Nodes, Nodes);
      Source.Name->Path.swap(SourceIdentity);
      Source.Name->PathCharge = SourceCharge;
      Source.Name->Parent = TargetParent;
      updateNamespaceMetadata(*Source.File);
    }
    if (Target.Link) {
      Target.Name->Path.swap(TargetIdentity);
      Target.Name->PathCharge = TargetCharge;
      Target.Name->Parent = Parent;
      updateNamespaceMetadata(*Target.Link);
    } else {
      Target.Name->Path.swap(TargetIdentity);
      Target.Name->PathCharge = TargetCharge;
      Target.Name->Parent = Parent;
      updateNamespaceMetadata(*Target.File);
    }
    *StorageUsed = Other + SourceCharge + TargetCharge;
    updateDirectoryMetadata(*Parent, true);
    updateDirectoryMetadata(*TargetParent, true);
    return returned(0);
  }
  // Object bytes/attributes are reclaimable only after the last name and
  // external description/lease. The selected entry has independent name cost.
  const bool ReclaimsFile = Target.File && Target.File->LinkedNames == 1 &&
                            Target.File.use_count() == 2 &&
                            Target.File->Lease.use_count() == 1;
  const uint64_t FileCredit =
      ReclaimsFile ? Target.bytes().size() +
                         Target.File->ExtendedAttributes.DynamicCharge
                   : 0;
  const bool ReclaimsLink = Target.Link && Target.Link->LinkedNames == 1 &&
                            Target.Link.use_count() == 2;
  const uint64_t LinkCredit =
      ReclaimsLink ? Target.Link->dynamicObjectCharge() : 0;
  const auto *ObjectName = Target.File   ? Target.File->Name.get()
                           : Target.Link ? Target.Link->Name.get()
                                         : nullptr;
  const uint64_t NameCredit =
      Target.Name &&
              ((Target.Link &&
                (ReclaimsLink || Target.Link->LinkedNames > 1)) ||
               (Target.File &&
                (ReclaimsFile || Target.File->LinkedNames > 1))) &&
              Target.Name.use_count() == 2 + (ObjectName == Target.Name.get())
          ? Target.Name->PathCharge
          : 0;
  const uint64_t OldCharge = Source.Name->PathCharge;
  const uint64_t Other =
      *StorageUsed - OldCharge - FileCredit - LinkCredit - NameCredit;
  const uint64_t Charge = Target.Path.size() + 1;
  if (Target.Path.size() >= limits::Path || Charge > limits::Bytes - Other)
    return unsupported(Result, diagnostic::RenameLimit);
  std::string NewKey = Target.Path, NewIdentity = Target.Path;
  if (Target.Name)
    reserveDetachedName(Target);
  auto Move = [&](auto &Table) {
    auto Moved = Table.extract(Source.Path);
    if (Target.Name)
      detachName(Target);
    Moved.key().swap(NewKey);
    Table.insert(std::move(Moved));
  };
  if (Source.Link) {
    Move(Links);
    Source.Name->Path.swap(NewIdentity);
    Source.Name->PathCharge = Charge;
    Source.Name->Parent = TargetParent;
    updateNamespaceMetadata(*Source.Link);
  } else {
    Move(Nodes);
    updateNamespaceMetadata(*Source.File);
    Source.Name->Path.swap(NewIdentity);
    Source.Name->PathCharge = Charge;
    Source.Name->Parent = TargetParent;
  }
  // Object and detached-name owners deduct the credited costs exactly once,
  // after this lookup releases its temporary object and identity references.
  *StorageUsed = *StorageUsed - OldCharge + Charge;
  updateDirectoryMetadata(*Parent, true);
  updateDirectoryMetadata(*TargetParent, true);
  Target.File.reset();
  Target.Link.reset();
  Target.Name.reset();
  reclaimUnlinked();
  return returned(0);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::renameSubtrees(Description &Source, Description &Target,
                            const std::shared_ptr<DirectoryNode> &Parent,
                            const std::shared_ptr<DirectoryNode> &TargetParent,
                            bool Swap, ProcessResult &Result) {
  if (!Swap && (Target.Type == Kind::File || Target.Type == Kind::SymbolicLink))
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
  // Protected initial links cannot move indirectly. Other link roots and
  // descendants participate in the same preflight as files and directories.
  if (llvm::any_of(Links, [&](const auto &Entry) {
        return Entry.second->Name->Protected &&
               (Descendant(Entry.second->Name->Parent, Source.Directory) ||
                (Swap &&
                 Descendant(Entry.second->Name->Parent, Target.Directory)));
      }))
    return unsupported(Result, diagnostic::SymbolicLinkDirectoryMove);
  // Reclaim before transaction references are collected. A held orphan file
  // or directory retains its original parent, including an empty target.
  if (auto E = prepareMutation())
    return std::move(E);
  const uint64_t Credit =
      !Swap && Target.Directory && Target.Directory.use_count() == 2
          ? Target.Directory->PathCharge +
                Target.Directory->ExtendedAttributes.DynamicCharge
          : 0;
  struct DirectoryMove {
    std::shared_ptr<DirectoryNode> Node;
    std::string Path, Key;
    bool Linked;
    decltype(Directories)::node_type Entry;
  };
  struct FileMove {
    std::shared_ptr<NameIdentity> Name;
    std::string Path, Key;
    bool Linked;
    decltype(Nodes)::node_type Entry;
  };
  struct LinkMove {
    std::shared_ptr<NameIdentity> Name;
    std::string Path, Key;
    decltype(Links)::node_type Entry;
  };
  std::vector<DirectoryMove> DirectoryMoves;
  std::vector<FileMove> Files;
  std::vector<LinkMove> LinkMoves;
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
  auto MovesName = [&](const std::shared_ptr<NameIdentity> &Name) {
    return Name == Source.Name || Descendant(Name->Parent, Source.Directory) ||
           (Swap && (Name == Target.Name ||
                     Descendant(Name->Parent, Target.Directory)));
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
  auto File = [&](const std::shared_ptr<NameIdentity> &Name,
                  bool Linked) -> const char * {
    if (!MovesName(Name))
      return nullptr;
    auto Path =
        NewPath(Name->Path, Name == Source.Name ||
                                Descendant(Name->Parent, Source.Directory));
    if (!Path)
      return diagnostic::NamespaceAlias;
    if (Path->size() >= limits::Path)
      return diagnostic::RenameLimit;
    OldCharge += Name->PathCharge;
    NewCharge += Path->size() + 1;
    auto Key = Linked ? *Path : std::string();
    Files.push_back({Name, std::move(*Path), std::move(Key), Linked, {}});
    return nullptr;
  };
  for (const auto &[Path, Node] : Directories)
    if (auto Reason = Directory(Node, true))
      return unsupported(Result, Reason);
  for (const auto &Node : UnlinkedDirectories)
    if (auto Reason = Directory(Node, false))
      return unsupported(Result, Reason);
  for (const auto &[Path, Node] : Nodes)
    if (auto Reason = File(Node->Name, true))
      return unsupported(Result, Reason);
  for (const auto &Name : DetachedNames)
    if (auto Reason = File(Name, false))
      return unsupported(Result, Reason);
  for (const auto &[Path, Node] : Links) {
    const auto &Name = Node->Name;
    if (!MovesName(Name))
      continue;
    auto New =
        NewPath(Name->Path, Name == Source.Name ||
                                Descendant(Name->Parent, Source.Directory));
    if (!New)
      return unsupported(Result, diagnostic::NamespaceAlias);
    if (New->size() >= limits::Path)
      return unsupported(Result, diagnostic::RenameLimit);
    // Initial targets and names retain their fixed input reservations. Rekey
    // changes only the separately owned dynamic name charge.
    OldCharge += Name->PathCharge;
    NewCharge += New->size() + 1;
    auto Key = *New;
    LinkMoves.push_back({Name, std::move(*New), std::move(Key), {}});
  }
  const uint64_t Other = *StorageUsed - OldCharge - Credit;
  if (NewCharge > limits::Bytes - Other)
    return unsupported(Result, diagnostic::RenameLimit);
  auto AvailableKey = [&](const std::string &Key) {
    const auto Directory = Directories.find(Key);
    const auto File = Nodes.find(Key);
    const auto Link = Links.find(Key);
    return (Directory == Directories.end() ||
            MovesDirectory(Directory->second) ||
            (!Swap && Directory->second == Target.Directory)) &&
           (File == Nodes.end() || MovesName(File->second->Name)) &&
           (Link == Links.end() || MovesName(Link->second->Name));
  };
  for (const auto &Move : DirectoryMoves)
    if (Move.Linked && !AvailableKey(Move.Key))
      return unsupported(Result, diagnostic::NamespaceAlias);
  for (const auto &Move : Files)
    if (Move.Linked && !AvailableKey(Move.Key))
      return unsupported(Result, diagnostic::NamespaceAlias);
  for (const auto &Move : LinkMoves)
    if (!AvailableKey(Move.Key))
      return unsupported(Result, diagnostic::NamespaceAlias);
  if (!Swap && Target.Directory)
    UnlinkedDirectories.reserve(UnlinkedDirectories.size() + 1);
  // Allocate every path, key and staged node handle before effects. A SWAP
  // keeps both roots linked and supplies no replacement/content/lease credit.
  if (!Swap && Target.Directory) {
    Target.Directory->Linked = false;
    updateDirectoryMetadata(*Target.Directory, false);
    UnlinkedDirectories.push_back(Target.Directory);
    Directories.erase(Target.Path);
  }
  // Opposing subtrees may have identical child suffixes, including mixed
  // root types. Extract all three maps before inserting any new key.
  for (auto &Move : DirectoryMoves)
    if (Move.Linked)
      Move.Entry = Directories.extract(Move.Node->Path);
  for (auto &Move : Files)
    if (Move.Linked)
      Move.Entry = Nodes.extract(Move.Name->Path);
  for (auto &Move : LinkMoves)
    Move.Entry = Links.extract(Move.Name->Path);
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
    Move.Name->Path.swap(Move.Path);
    Move.Name->PathCharge = Move.Name->Path.size() + 1;
  }
  for (auto &Move : LinkMoves) {
    Move.Entry.key().swap(Move.Key);
    Links.insert(std::move(Move.Entry));
    Move.Name->Path.swap(Move.Path);
    Move.Name->PathCharge = Move.Name->Path.size() + 1;
    // Retain the actual parent object as its path moves with the subtree.
    // Rewriting raw targets would change relative, dangling and opaque links.
  }
  if (Source.Directory) {
    Source.Directory->Parent = TargetParent;
    updateDirectoryMetadata(*Source.Directory, false);
  } else if (Source.Link) {
    Source.Name->Parent = TargetParent;
    updateNamespaceMetadata(*Source.Link);
  } else {
    Source.Name->Parent = TargetParent;
    updateNamespaceMetadata(*Source.File);
  }
  if (Swap) {
    if (Target.Directory) {
      Target.Directory->Parent = Parent;
      updateDirectoryMetadata(*Target.Directory, false);
    } else if (Target.Link) {
      Target.Name->Parent = Parent;
      updateNamespaceMetadata(*Target.Link);
    } else {
      Target.Name->Parent = Parent;
      updateNamespaceMetadata(*Target.File);
    }
  }
  updateDirectoryMetadata(*Parent, true);
  updateDirectoryMetadata(*TargetParent, true);
  // The replaced target remains charged until its real final reference is
  // released. Reclaim deducts Credit once, after lookup/plan references end.
  *StorageUsed = *StorageUsed - OldCharge + NewCharge;
  DirectoryMoves.clear();
  Files.clear();
  LinkMoves.clear();
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
  Node->Name->ChargedEntry = true;
  Node->Name->Path = File.Path;
  Node->Name->Parent = directoryNode(Parent);
  Node->Name->PathCharge = Charge;
  if (Options->CreationPolicy) {
    const auto &Policy = *Options->CreationPolicy;
    // Namespace changes invalidate the parent's complete stat observation,
    // but cannot change these supplied device/group fields.
    const auto ParentIdentity = directoryIdentity(Parent);
    if (!ParentIdentity)
      return unsupported(Result, diagnostic::FileCreationParent);
    Node->CurrentMetadata = createdMetadata(
        *ParentIdentity, FileRegularMode | (Mode & 0777 & ~CurrentUmask), 1);
    Node->Policy = &Policy.Mutation;
  }
  // A new object never inherits an old observation/policy at the same name.
  auto Entry = std::make_shared<FileEntry>(Node);
  Nodes.emplace(File.Path, std::move(Entry));
  File.Type = Kind::File;
  File.File = Node;
  File.Name = Node->Name;
  *StorageUsed += Charge;
  updateDirectoryMetadata(*directoryNode(Parent), true);
  if (Options->CreationPolicy)
    consumeCreatedInode();
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
  // Successful content publication releases only runtime attribute growth.
  const uint64_t Other = *StorageUsed - File.bytes().size() -
                         File.File->ExtendedAttributes.DynamicCharge;
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
  // Content mutation does not prove any provider's xattr retention rule.
  invalidateAttributes(Node.ExtendedAttributes);
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
  if (File.Type != Kind::File && File.Type != Kind::Directory &&
      File.Type != Kind::SymbolicLink)
    return {IllegalSeek, true};
  if (Whence == SeekHole || Whence == SeekData) {
    if (Offset > INT64_MAX)
      return {InvalidArgument, true};
    if (File.Type == Kind::SymbolicLink)
      return {NoSuchAddress, true};
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
    Base = File.Type == Kind::Directory ? metadata(File)->Size
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
  if (File.Type == Kind::Directory && !Base) {
    File.Iteration = DirectoryIteration::None;
    File.BulkCursor = 0;
    File.BulkEOF = false;
    File.DirectoryVersion.reset();
  }
  return {Base, false};
}

DarwinFiles::MappingSource DarwinFiles::mappingSource(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint32_t(BadDescriptor);
  if (I->second.Open->Type == Kind::Directory ||
      I->second.Open->Type == Kind::SymbolicLink)
    return uint32_t(InvalidArgument);
  if (I->second.Open->Type != Kind::File)
    return diagnostic::MemoryFileKind;
  const auto &File = *I->second.Open->File;
  return Mapping{File.bytes(), File.Lease,
                 (I->second.Open->Flags & OpenAccessMask) != OpenWriteOnly};
}

bool DarwinFiles::symbolicLinkDescriptor(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  return I != Descriptors.end() && I->second.Open->Type == Kind::SymbolicLink;
}

ServiceResult DarwinFiles::duplicate(const Descriptor &Source, uint32_t Minimum,
                                     bool CloseOnExec) {
  const uint32_t FD = freeDescriptor(Minimum);
  if (FD >= limit())
    return {TooManyFiles, true};
  Descriptors.emplace(FD, Descriptor{Source.Open, CloseOnExec});
  return {FD, false};
}

std::optional<ServiceResult> DarwinFiles::pathconf(const Description &File,
                                                   uint32_t Name,
                                                   ProcessResult &Result) {
  if (File.Type != Kind::File && File.Type != Kind::Directory &&
      File.Type != Kind::SymbolicLink)
    return unsupported(Result, diagnostic::FilePathConfKind);
  // These are the fixed XNU vn_pathconf results, independent of vnode stat,
  // page size and filesystem VNOPs. Other selectors need filesystem knowledge,
  // including invalid selectors whose rejection belongs to that filesystem.
  switch (Name) {
  case 15: // _PC_2_SYMLINKS
  case 16: // _PC_ALLOC_SIZE_MIN
  case 17: // _PC_ASYNC_IO
    return returned(1);
  case 19: // _PC_PRIO_IO
  case 25: // _PC_SYNC_IO
    return returned(0);
  case 20: // _PC_REC_INCR_XFER_SIZE
  case 22: // _PC_REC_MIN_XFER_SIZE
  case 23: // _PC_REC_XFER_ALIGN
    return returned(4096);
  case 21: // _PC_REC_MAX_XFER_SIZE
    return returned(65536);
  case 24: // _PC_SYMLINK_MAX
    return returned(255);
  default:
    return unsupported(Result, diagnostic::FilePathConf);
  }
}

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::handle(ServiceKind Service, const ProcessServiceEvent &Event,
                    ProcessResult &Result) {
  const auto &A = Event.Arguments;
  if (Service == ServiceKind::SetXattr || Service == ServiceKind::FsetXattr ||
      Service == ServiceKind::RemoveXattr ||
      Service == ServiceKind::FremoveXattr)
    return extendedAttributeMutation(Service, Event, Result);
  if (Service == ServiceKind::GetXattr || Service == ServiceKind::FgetXattr ||
      Service == ServiceKind::ListXattr || Service == ServiceKind::FlistXattr)
    return extendedAttributeRead(Service, Event, Result);
  if (Service == ServiceKind::Umask) {
    if (!Options || !Options->InitialUmask)
      return unsupported(Result, diagnostic::FileUmask);
    initializeNamespace();
    const auto Previous = CurrentUmask;
    CurrentUmask = A[0] & 07777;
    return returned(Previous);
  }
  if (Service == ServiceKind::Link)
    return hardLink(A[0], AtCurrentDirectory, A[1], AtCurrentDirectory, true,
                    Result);
  if (Service == ServiceKind::LinkAt) {
    const uint32_t Flags = A[4];
    if (Flags & ~uint32_t(AtFollow))
      return returned(InvalidArgument, true);
    return hardLink(A[1], uint32_t(A[0]), A[3], uint32_t(A[2]),
                    bool(Flags & AtFollow), Result);
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
    return makeDirectory(A[0], AtCurrentDirectory, A[1], Result);
  if (Service == ServiceKind::MkdirAt)
    return makeDirectory(A[1], A[0], A[2], Result);
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
  if (Service == ServiceKind::GetAttrListBulk)
    return bulkAttributes(uint32_t(A[0]), A[1], A[2], A[3], A[4], Result);
  if (Service == ServiceKind::GetAttrList ||
      Service == ServiceKind::GetAttrListAt ||
      Service == ServiceKind::FgetAttrList) {
    const bool At = Service == ServiceKind::GetAttrListAt;
    const bool Held = Service == ServiceKind::FgetAttrList;
    const uint64_t Input = A[At ? 2 : 1];
    const uint64_t Address = A[At ? 3 : 2];
    const uint64_t Size = A[At ? 4 : 3];
    const uint64_t Options = A[At ? 5 : 4];
    const Description *File = nullptr;
    if (Held) {
      auto Found = Descriptors.find(uint32_t(A[0]));
      if (Found == Descriptors.end())
        return returned(BadDescriptor, true);
      File = Found->second.Open.get();
      if (File->Type != Kind::File && File->Type != Kind::Directory &&
          File->Type != Kind::SymbolicLink)
        return unsupported(Result, diagnostic::FileAttributeKind);
    }
    auto Request = readAttributes(Input);
    if (!Request)
      return Request.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Request))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Request))
      return unsupported(Result, *Reason);
    if (Held)
      return attributeList(*File, std::get<AttributeRequest>(*Request), Address,
                           Size, Options, Result);
    auto Resolved = resolvePath(
        A[At ? 1 : 0], At ? uint32_t(A[0]) : uint32_t(AtCurrentDirectory),
        LookupMode::Existing,
        {!(Options & (AttributeNoFollow | AttributeNoFollowAny)),
         bool(Options & AttributeNoFollowAny)});
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    return attributeList(std::get<Description>(*Resolved),
                         std::get<AttributeRequest>(*Request), Address, Size,
                         Options, Result);
  }
  if (Service == ServiceKind::PathConf) {
    auto Resolved = resolvePath(A[0], AtCurrentDirectory);
    if (!Resolved)
      return Resolved.takeError();
    if (auto *Error = std::get_if<uint32_t>(&*Resolved))
      return returned(*Error, true);
    if (auto *Reason = std::get_if<const char *>(&*Resolved))
      return unsupported(Result, *Reason);
    return pathconf(std::get<Description>(*Resolved), uint32_t(A[1]), Result);
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
    const bool Vnode = File.Type == Kind::File ||
                       File.Type == Kind::Directory ||
                       File.Type == Kind::SymbolicLink;
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
  case ServiceKind::FpathConf:
    return pathconf(File, uint32_t(A[1]), Result);
  case ServiceKind::Ftruncate: {
    if ((File.Type != Kind::File && File.Type != Kind::SymbolicLink) ||
        !(File.Flags & OpenAccessMask))
      return returned(InvalidArgument, true);
    if (File.Type == Kind::SymbolicLink) {
      File.Flags |= FileWasWritten;
      return returned(0);
    }
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
    if (File.Type == Kind::File || File.Type == Kind::SymbolicLink)
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
      if (File.Type == Kind::Directory && uint32_t(A[2]) == 2 &&
          !metadata(File))
        return unsupported(Result, diagnostic::FileMetadata);
      if (File.Type == Kind::Directory && uint32_t(A[2]) == 2 &&
          (File.Directory->MetadataInvalidated ||
           (File.Directory->Changed && !File.Directory->CurrentMetadata)))
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
      if (File.ambiguousName())
        return unsupported(Result, diagnostic::HardLinkName);
      const auto Path = File.path();
      if (Path.empty())
        return unsupported(Result, diagnostic::FilePathIdentity);
      return copyout(
          A[2],
          llvm::ArrayRef<uint8_t>(
              reinterpret_cast<const uint8_t *>(Path.data()), Path.size() + 1),
          diagnostic::FilePartialPath, Result);
    }
    case GetFileFlags:
      return returned(File.Flags);
    case SetFileFlags:
      if (uint32_t(A[2]) & ~uint32_t(OpenAccessMask | OpenNonBlocking |
                                     OpenAppend | FileWasWritten))
        return unsupported(Result, diagnostic::FileControl);
      // XNU converts the low32 open-flag request to file flags first. Access
      // bits3 carry into NONBLOCK (and through APPEND); the actual access and
      // write history belong to the existing description, not this request.
      File.Flags = (File.Flags & ~(OpenAppend | OpenNonBlocking)) |
                   ((uint32_t(A[2]) + 1) & (OpenAppend | OpenNonBlocking));
      return returned(File.Type == Kind::SymbolicLink ? NotTerminal : 0,
                      File.Type == Kind::SymbolicLink);
    default:
      return unsupported(Result, diagnostic::FileControl);
    }
  default:
    llvm_unreachable("non-file Darwin service");
  }
}
} // namespace neverd::emulation::darwin_model
