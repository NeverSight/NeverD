//===- DarwinFiles.h - Darwin file descriptions ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINFILES_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINFILES_H

#include "DarwinKernel.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <set>
#include <utility>
#include <variant>

namespace neverd::emulation::darwin_model {
llvm::Error validateFileOptions(const DarwinFileOptions &Options);
bool validExtendedAttributeName(llvm::StringRef Name);
bool ordinaryExtendedAttributeName(llvm::StringRef Name);

class DarwinFiles {
public:
  DarwinFiles(
      GuestMemory &Memory, const std::optional<DarwinFileOptions> &Options,
      uint64_t OutputLimit = process_defaults::Output,
      uint32_t EffectiveUID = value::UserID,
      const std::optional<DarwinCredentials> &Credentials = std::nullopt);
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
  /// Admission fact for modes whose known symbolic vnode rejects mapping.
  /// This query borrows the descriptor without retaining or changing it.
  bool symbolicLinkDescriptor(uint32_t FD) const;

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
  enum class Subject { Real, Effective };
  using Authorization = std::variant<std::monostate, uint32_t, const char *>;
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
  struct AttributeState {
    const std::vector<DarwinExtendedAttribute> *Initial = nullptr;
    std::optional<std::vector<DarwinExtendedAttribute>> Modified;
    uint64_t InitialCharge = 0;
    uint64_t DynamicCharge = 0;
    uint32_t ExtraCount = 0;
    bool Mutable = false;
    bool Invalidated = false;
    const std::vector<DarwinExtendedAttribute> *get() const {
      return Invalidated ? nullptr : Modified ? &*Modified : Initial;
    }
    void attach(const std::vector<DarwinExtendedAttribute> &Attributes,
                bool Grant) {
      Initial = &Attributes;
      Mutable = Grant;
      for (const auto &Attribute : Attributes)
        InitialCharge += Attribute.Name.size() + 1 + Attribute.Bytes.size();
    }
  };
  struct DirectoryNode {
    std::string Path;
    std::shared_ptr<DirectoryNode> Parent;
    std::optional<DirectoryIdentity> Identity;
    const DarwinFileMetadata *Metadata = nullptr;
    AttributeState ExtendedAttributes;
    std::optional<DarwinFileMetadata> CurrentMetadata;
    uint32_t DirectoryEntrySize = 0;
    const DarwinFileTime *MutationTime = nullptr;
    const DarwinDirectoryContents *Snapshot = nullptr;
    const DarwinDirectoryEnumerationPolicy *EnumerationPolicy = nullptr;
    /// Bulk authorization belongs to an initial object, never its children.
    bool BulkAttributes = false;
    uint64_t EnumerationVersion = 0;
    uint64_t PathCharge = 0;
    bool Created = false;
    bool Linked = true;
    bool Changed = false;
    bool MetadataInvalidated = false;
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
  /// A namespace identity owns no file/link object. Objects may retain it
  /// after unlink without introducing an object -> entry -> object cycle.
  struct NameIdentity {
    std::string Path;
    std::shared_ptr<DirectoryNode> Parent;
    uint64_t PathCharge = 0;
    bool Protected = false;
    /// Initial regular names and every runtime name consume an entry. Initial
    /// symbolic names retain their independent fixed input reservation.
    bool ChargedEntry = false;
  };
  struct NamedObject {
    /// Lifetime bookkeeping only. Multiple-name vnode observations require a
    /// separately proved contract; this internal selection is not that proof.
    std::shared_ptr<NameIdentity> Name = std::make_shared<NameIdentity>();
    const std::string *InitialPath = nullptr;
    uint32_t LinkedNames = 1;
    bool HadMultipleNames = false;
  };
  struct Contents : NamedObject {
    llvm::ArrayRef<uint8_t> Initial;
    std::optional<std::vector<uint8_t>> Modified;
    const DarwinFileMetadata *InitialMetadata = nullptr;
    AttributeState ExtendedAttributes;
    std::optional<DarwinFileMetadata> CurrentMetadata;
    const DarwinFileMutationPolicy *Policy = nullptr;
    std::optional<llvm::BitVector> Allocated;
    bool Writable = false;
    bool MetadataInvalidated = false;
    std::shared_ptr<const unsigned> Lease = std::make_shared<const unsigned>(0);
    llvm::ArrayRef<uint8_t> bytes() const {
      return Modified ? llvm::ArrayRef<uint8_t>(*Modified) : Initial;
    }
    const DarwinFileMetadata *metadata() const {
      return CurrentMetadata ? &*CurrentMetadata : InitialMetadata;
    }
  };
  struct LinkNode : NamedObject {
    llvm::ArrayRef<uint8_t> Initial;
    std::optional<std::vector<uint8_t>> CreatedTarget;
    const DarwinFileMetadata *Metadata = nullptr;
    AttributeState ExtendedAttributes;
    std::optional<DarwinFileMetadata> CurrentMetadata;
    const DarwinFileTime *MutationTime = nullptr;
    bool Created = false;
    bool MetadataInvalidated = false;
    llvm::ArrayRef<uint8_t> bytes() const {
      return CreatedTarget ? llvm::ArrayRef<uint8_t>(*CreatedTarget) : Initial;
    }
    uint64_t dynamicObjectCharge() const {
      return (CreatedTarget ? CreatedTarget->size() : 0) +
             ExtendedAttributes.DynamicCharge;
    }
  };
  template <typename ObjectType> struct NamedEntry {
    std::shared_ptr<ObjectType> Object;
    std::shared_ptr<NameIdentity> Name;
    explicit NamedEntry(std::shared_ptr<ObjectType> Node)
        : Object(std::move(Node)), Name(Object->Name) {}
    NamedEntry(std::shared_ptr<ObjectType> Node,
               std::shared_ptr<NameIdentity> Identity)
        : Object(std::move(Node)), Name(std::move(Identity)) {}
  };
  using FileEntry = NamedEntry<Contents>;
  using LinkEntry = NamedEntry<LinkNode>;
  enum class DirectoryIteration { None, Entries, Bulk };
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
    std::shared_ptr<NameIdentity> Name;
    std::optional<uint64_t> DirectoryVersion;
    DirectoryIteration Iteration = DirectoryIteration::None;
    uint64_t BulkCursor = 0;
    bool BulkEOF = false;
    llvm::ArrayRef<uint8_t> bytes() const {
      return File ? File->bytes() : Link ? Link->bytes() : Input;
    }
    llvm::StringRef path() const {
      return Name ? Name->Path : Directory ? Directory->Path : Path;
    }
    bool ambiguousName() const {
      return File ? File->HadMultipleNames : Link && Link->HadMultipleNames;
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
  /// Explicit immutable knowledge, separate from the scalar observation
  /// fallback used by legacy creation metadata.
  const std::optional<DarwinCredentials> Credentials;
  std::map<uint32_t, Descriptor> Descriptors;
  std::map<std::string, std::shared_ptr<FileEntry>> Nodes;
  std::map<std::string, std::shared_ptr<LinkEntry>> Links;
  std::vector<std::shared_ptr<Contents>> Unlinked;
  std::vector<std::shared_ptr<LinkNode>> UnlinkedLinks;
  /// Names can outlive unlink independently of their object's other names.
  std::vector<std::shared_ptr<NameIdentity>> DetachedNames;
  std::map<std::string, std::shared_ptr<DirectoryNode>> Directories;
  std::vector<std::shared_ptr<DirectoryNode>> UnlinkedDirectories;
  bool NamespaceReady = false;
  std::set<std::string> InitialAliases;
  uint64_t NextCreatedInode = 0;
  uint16_t CurrentUmask = 0;
  std::optional<uint64_t> StorageUsed;
  uint32_t FixedEntries = 0;
  uint32_t AttributeSlots = 0;
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
  bool queryEnvironment() const;
  const char *authorizationScope() const;
  std::optional<bool> groupMembership(uint32_t GID, Subject User) const;
  Authorization authorize(const DarwinFileMetadata *Metadata, uint32_t Actions,
                          Subject User) const;
  llvm::Expected<Lookup> resolvePath(uint64_t Address, uint32_t DirectoryFD,
                                     LookupMode Mode = LookupMode::Existing,
                                     LinkPolicy Links = {true, false},
                                     bool CheckDirectoryPrefix = true,
                                     Subject User = Subject::Real);
  llvm::Expected<std::optional<ServiceResult>>
  open(uint64_t Address, uint32_t Flags, uint32_t DirectoryFD, uint32_t Mode,
       ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  access(uint64_t Path, uint32_t DirectoryFD, uint32_t Mode,
         ProcessResult &Result, LinkPolicy Links = {true, false},
         Subject User = Subject::Real);
  std::optional<ServiceResult> pathconf(const Description &File, uint32_t Name,
                                        ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  status(const Description &File, uint64_t Address, ProcessResult &Result);
  const DarwinFileMetadata *metadata(const Description &File) const;
  // Complete records have one validity decision, distinct from retained
  // identity observations used for namespace consistency.
  using StatusMetadata = std::variant<const DarwinFileMetadata *, const char *>;
  StatusMetadata statusMetadata(const Description &File) const;
  struct AttributeRequest {
    uint16_t BitmapCount;
    std::array<uint32_t, 5> Masks;
  };
  using AttributeInput = std::variant<AttributeRequest, uint32_t, const char *>;
  using AttributeRecord = std::variant<std::vector<uint8_t>, const char *>;
  static bool supportedAttributeMask(uint32_t Mask);
  AttributeRecord attributeRecord(
      const Description &File, uint32_t Mask,
      std::optional<llvm::StringRef> EntryName = std::nullopt) const;
  llvm::Expected<std::optional<ServiceResult>>
  bulkAttributes(uint32_t FD, uint64_t Input, uint64_t Address, uint64_t Size,
                 uint64_t Options, ProcessResult &Result);
  llvm::Expected<AttributeInput> readAttributes(uint64_t Address);
  llvm::Expected<Pathname> readExtendedAttributeName(uint64_t Address);
  const std::vector<DarwinExtendedAttribute> *
  extendedAttributes(const Description &File) const;
  AttributeState *attributeState(const Description &File) const;
  void invalidateAttributes(AttributeState &State);
  void invalidateAttributeMetadata(Description &File);
  llvm::Expected<std::optional<ServiceResult>>
  extendedAttributeRead(ServiceKind Service, const ProcessServiceEvent &Event,
                        ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  extendedAttributeMutation(ServiceKind Service,
                            const ProcessServiceEvent &Event,
                            ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  extendedAttributeResult(const Description &File,
                          std::optional<llvm::StringRef> Name, uint64_t Address,
                          uint64_t Size, uint32_t Position, uint32_t Flags,
                          bool Held, ProcessResult &Result);
  llvm::Expected<std::optional<ServiceResult>>
  attributeList(const Description &File, const AttributeRequest &Request,
                uint64_t Address, uint64_t Size, uint64_t Options,
                ProcessResult &Result);
  struct DirectoryEntryIdentity {
    llvm::StringRef Name;
    uint8_t Type;
    std::optional<uint64_t> Inode;
  };
  /// Linked membership and inode identity are independent of complete stat
  /// validity. No allocation occurs while counting committed child names.
  void forEachDirectoryChild(
      const DirectoryNode &Node,
      llvm::function_ref<void(const DirectoryEntryIdentity &)> Visit) const;
  std::optional<DarwinDirectoryContents>
  enumerationContents(const DirectoryNode &Node) const;
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
  hardLink(uint64_t SourcePath, uint32_t SourceDirectory, uint64_t TargetPath,
           uint32_t TargetDirectory, bool FollowFinal, ProcessResult &Result);
  void reserveDetachedName(const Description &File);
  void detachName(Description &File);
  llvm::Expected<std::optional<ServiceResult>>
  makeDirectory(uint64_t Path, uint32_t DirectoryFD, uint32_t Mode,
                ProcessResult &Result);
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
  void updateNamespaceMetadata(Contents &Node);
  void updateNamespaceMetadata(LinkNode &Node);
  void updateDirectoryMetadata(DirectoryNode &Node, bool ContentsChanged);
  DarwinFileMetadata createdMetadata(const DirectoryIdentity &Parent,
                                     uint16_t Mode, uint16_t LinkCount,
                                     uint64_t Size = 0,
                                     uint64_t Blocks = 0) const;
  void consumeCreatedInode();
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
