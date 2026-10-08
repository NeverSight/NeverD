//===- DarwinFiles.h - Darwin file descriptions ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINFILES_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINFILES_H

#include "DarwinKernel.h"

#include "llvm/ADT/BitVector.h"

#include <variant>

namespace neverd::emulation::darwin_model {
llvm::Error validateFileOptions(const DarwinFileOptions &Options);

class DarwinFiles {
public:
  DarwinFiles(GuestMemory &Memory,
              const std::optional<DarwinFileOptions> &Options,
              uint64_t OutputLimit = process_defaults::Output,
              uint32_t EffectiveUID = value::UserID);
  llvm::Expected<std::optional<ServiceResult>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ProcessResult &Result);
  struct Mapping {
    llvm::ArrayRef<uint8_t> Bytes;
    /// The VM owner retains this lease until every mapped range is unmapped.
    std::shared_ptr<const unsigned> Lease;
    bool Readable;
  };
  /// Current regular-file bytes, BSD errno, or an unsupported object kind.
  /// Looking up a mapping source never changes the open description's cursor.
  using MappingSource = std::variant<Mapping, uint32_t, const char *>;
  MappingSource mappingSource(uint32_t FD) const;

private:
  enum class Kind {
    Input,
    Output,
    Error,
    File,
    Directory,
    SymbolicLink,
    Missing
  };
  enum class LookupMode {
    Existing,
    CreateFile,
    CreateDirectory,
    DeleteFile,
    DeleteDirectory,
    RenameTarget,
    RenameDirectoryTarget
  };
  // Retaining a terminal link and refusing required expansion are separate
  // namei policies. AT_SYMLINK_NOFOLLOW_ANY uses both; ordinary open uses only
  // NoExpansion, while exclusive creation retains the terminal entry.
  struct LinkPolicy {
    bool FollowFinal;
    bool NoExpansion;
  };
  enum class Terminal { Ordinary, Dot, DotDot };
  enum class RenameMode { Replace, Exclusive, Swap };
  struct DirectoryIdentity {
    int32_t Device;
    uint32_t GID;
  };
  struct DirectoryNode {
    std::string Path;
    std::shared_ptr<DirectoryNode> Parent;
    std::optional<DirectoryIdentity> Identity;
    const DarwinFileMetadata *Metadata = nullptr;
    const DarwinDirectoryContents *Snapshot = nullptr;
    uint64_t PathCharge = 0;
    bool Created = false;
    bool Linked = true;
    bool Changed = false;
    bool Mutable = false;
    bool Removable = false;
    bool Movable = false;
    bool Exchangeable = false;
    bool NonMount = false;
    /// Initial objects keep their direct declaration. mkdir inherits its
    /// parent's capability; moving the object does not replace this grant.
    bool SwapSupport = false;
    /// Only mkdir or an explicit initial-subtree declaration establishes a
    /// non-mount parent relation. Neither device equality nor SWAP does so.
    const DirectoryNode *mountAncestor() const {
      const auto *Node = this;
      while (Node && (Node->Created || Node->NonMount))
        Node = Node->Parent.get();
      return Node;
    }
  };
  struct Contents {
    llvm::ArrayRef<uint8_t> Initial;
    std::optional<std::vector<uint8_t>> Modified;
    const DarwinFileMetadata *InitialMetadata = nullptr;
    std::optional<DarwinFileMetadata> CurrentMetadata;
    const DarwinFileMutationPolicy *Policy = nullptr;
    std::optional<llvm::BitVector> Allocated;
    bool Writable = false;
    /// Last linked name. All descriptions follow rename; unlink retains it.
    std::string Path;
    /// Retain the parent object, including after unlink/name reuse. Moving an
    /// ancestor also changes paths of its held orphan files and mappings.
    std::shared_ptr<DirectoryNode> Parent;
    uint64_t PathCharge = 0;
    bool MetadataInvalidated = false;
    std::shared_ptr<const unsigned> Lease = std::make_shared<const unsigned>(0);
    llvm::ArrayRef<uint8_t> bytes() const {
      return Modified ? llvm::ArrayRef<uint8_t>(*Modified) : Initial;
    }
    const DarwinFileMetadata *metadata() const {
      return CurrentMetadata ? &*CurrentMetadata : InitialMetadata;
    }
  };
  struct LinkNode {
    llvm::ArrayRef<uint8_t> Initial;
    std::optional<std::vector<uint8_t>> CreatedTarget;
    const DarwinFileMetadata *Metadata = nullptr;
    std::string Path;
    std::shared_ptr<DirectoryNode> Parent;
    bool Protected = false;
    llvm::ArrayRef<uint8_t> bytes() const {
      return CreatedTarget ? llvm::ArrayRef<uint8_t>(*CreatedTarget) : Initial;
    }
  };
  struct Description {
    Kind Type;
    llvm::ArrayRef<uint8_t> Input;
    uint64_t Offset = 0;
    const DarwinFileMetadata *Metadata = nullptr;
    std::string Path;
    std::shared_ptr<Contents> File;
    uint32_t Flags = 0;
    Terminal FinalComponent = Terminal::Ordinary;
    std::shared_ptr<DirectoryNode> Directory;
    bool FinalParentUnlinked = false;
    std::shared_ptr<LinkNode> Link;
    llvm::ArrayRef<uint8_t> bytes() const {
      return File ? File->bytes() : Link ? Link->bytes() : Input;
    }
  };
  struct Descriptor {
    std::shared_ptr<Description> Open;
    bool CloseOnExec = false;
  };
  struct Buffer {
    uint64_t Address;
    uint64_t Size;
  };
  using VectorInput = std::variant<std::vector<Buffer>, uint32_t, const char *>;
  using Pathname = std::variant<std::string, uint32_t>;
  using Lookup = std::variant<Description, uint32_t, const char *>;
  using DirectoryLookup =
      std::variant<std::shared_ptr<DirectoryNode>, uint32_t, const char *>;
  GuestMemory &Memory;
  const std::optional<DarwinFileOptions> &Options;
  const uint64_t OutputLimit;
  const uint32_t EffectiveUID;
  std::map<uint32_t, Descriptor> Descriptors;
  std::map<std::string, std::shared_ptr<Contents>> Nodes;
  std::map<std::string, std::shared_ptr<LinkNode>> Links;
  std::vector<std::shared_ptr<Contents>> Unlinked;
  std::map<std::string, std::shared_ptr<DirectoryNode>> Directories;
  std::vector<std::shared_ptr<DirectoryNode>> UnlinkedDirectories;
  bool NamespaceReady = false;
  uint64_t NextCreatedInode = 0;
  uint16_t CurrentUmask = 0;
  std::optional<uint64_t> StorageUsed;
  uint32_t FixedEntries = 0;
  std::shared_ptr<DirectoryNode> CurrentDirectory;

  uint32_t limit() const;
  uint32_t freeDescriptor(uint32_t Minimum = 0) const;
  void initializeNamespace();
  llvm::Error prepareMutation();
  void reclaimUnlinked();
  std::shared_ptr<DirectoryNode> directoryNode(const std::string &Path);
  std::shared_ptr<DirectoryNode> initialDirectoryNode(const std::string &Path);
  uint32_t dynamicEntries() const;
  bool mutableDirectory(const std::string &Path) const;
  std::optional<DirectoryIdentity> directoryIdentity(const std::string &Path);
  llvm::Expected<Pathname> readPath(uint64_t Address);
  DirectoryLookup directoryDescriptor(uint32_t FD) const;
  llvm::Expected<std::optional<Lookup>> directoryPrefix(uint64_t Address,
                                                        uint32_t DirectoryFD);
  llvm::Expected<Lookup> resolvePath(uint64_t Address, uint32_t DirectoryFD,
                                     LookupMode Mode = LookupMode::Existing,
                                     LinkPolicy Links = {true, false},
                                     bool CheckDirectoryPrefix = true);
  llvm::Expected<std::optional<ServiceResult>>
  open(uint64_t Address, uint32_t Flags, uint32_t DirectoryFD, uint32_t Mode,
       ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  access(uint64_t Path, uint32_t DirectoryFD, uint32_t Mode,
         ProcessResult &Result, LinkPolicy Links = {true, false});
  llvm::Expected<std::optional<ServiceResult>>
  status(const Description &File, uint64_t Address, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  statusPath(uint64_t Path, uint64_t Address, uint32_t DirectoryFD,
             ProcessResult &Result, LinkPolicy Links = {true, false});
  llvm::Expected<std::optional<ServiceResult>>
  readLink(uint64_t Path, uint64_t Address, uint64_t Size, uint32_t DirectoryFD,
           ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  makeSymbolicLink(uint64_t Target, uint64_t Path, uint32_t DirectoryFD,
                   ProcessResult &Result);
  static std::optional<uint32_t> rootRemovalError(const Description &File);
  llvm::Expected<std::optional<ServiceResult>> unlink(uint64_t Path,
                                                      uint32_t DirectoryFD,
                                                      ProcessResult &Result,
                                                      bool NoExpansion = false);
  llvm::Expected<std::optional<ServiceResult>>
  makeDirectory(uint64_t Path, uint32_t DirectoryFD, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  removeDirectory(uint64_t Path, uint32_t DirectoryFD, ProcessResult &Result,
                  bool NoExpansion = false);
  llvm::Expected<std::optional<ServiceResult>>
  rename(uint64_t SourcePath, uint32_t SourceDirectory, uint64_t TargetPath,
         uint32_t TargetDirectory, RenameMode Mode, ProcessResult &Result,
         bool NoExpansion = false);
  llvm::Expected<std::optional<ServiceResult>>
  renameSubtrees(Description &Source, Description &Target,
                 const std::shared_ptr<DirectoryNode> &Parent,
                 const std::shared_ptr<DirectoryNode> &TargetParent, bool Swap,
                 ProcessResult &Result);
  void updateNamespaceMetadata(Contents &Node, bool Removed);
  llvm::Expected<std::optional<ServiceResult>>
  create(Description &File, uint32_t Mode, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  copyout(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
          const char *PartialDiagnostic, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  read(Description &File, llvm::ArrayRef<Buffer> Buffers, uint64_t Count,
       uint64_t Offset, bool Positioned, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  write(Description &File, llvm::ArrayRef<Buffer> Buffers, uint64_t Count,
        uint64_t Offset, bool Positioned, ProcessResult &Result);
  llvm::Expected<VectorInput> readVectors(uint64_t Address, uint32_t Count);
  llvm::Expected<std::optional<ServiceResult>>
  capture(Description &File, llvm::ArrayRef<Buffer> Buffers, uint64_t Count,
          bool Vectored, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  resize(Description &File, uint64_t Size, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  admitMutation(const Description &File, uint64_t Size, ProcessResult &Result);
  void publish(Description &File, std::vector<uint8_t> Bytes,
               std::optional<std::pair<uint64_t, uint64_t>> Written = {});
  llvm::Expected<std::optional<ServiceResult>>
  directory(Description &File, uint64_t Address, uint64_t Count,
            uint64_t Position, ProcessResult &Result);
  ServiceResult seek(Description &File, uint64_t Offset, uint32_t Whence);
  ServiceResult duplicate(const Descriptor &Source, uint32_t Minimum,
                          bool CloseOnExec);
};
} // namespace neverd::emulation::darwin_model
#endif
