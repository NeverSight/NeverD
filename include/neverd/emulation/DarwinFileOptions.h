//===- DarwinFileOptions.h - Explicit Darwin file inputs --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DARWINFILEOPTIONS_H
#define NEVERD_EMULATION_DARWINFILEOPTIONS_H

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace darwin_file_limits {
inline constexpr uint32_t DefaultDescriptors = 256;
inline constexpr uint32_t Descriptors = 4096;
inline constexpr uint32_t Files = 256;
inline constexpr uint32_t DirectoryEntries = 4096;
inline constexpr uint64_t Bytes = 16 * 1024 * 1024;
inline constexpr uint32_t Path = 1024;
inline constexpr uint32_t Name = 255;
inline constexpr uint32_t ExtendedAttributeName = 127;
inline constexpr uint32_t ExtendedAttributes = 4096;
} // namespace darwin_file_limits

struct DarwinFileTime {
  int64_t Seconds = 0;
  int64_t Nanoseconds = 0;
};

/// One ordinary opaque extended attribute. Names are UTF-8, including slash;
/// bytes are independent of file contents and stat metadata.
struct DarwinExtendedAttribute {
  std::string Name;
  std::vector<uint8_t> Bytes;
};

/// Fixed stat64 observations. Regular-file Size must match its bytes; directory
/// Size is an explicit nonnegative observation, not an entry count.
/// These observations alone do not authorize catalogue access. The explicit
/// static query environments can use Mode/UID/GID; reads do not advance
/// timestamps. Unknown metadata is not synthesized from the host.
struct DarwinFileMetadata {
  int32_t Device = 0;
  uint64_t Inode = 0;
  uint16_t Mode = 0;
  uint16_t LinkCount = 0;
  uint32_t UID = 0;
  uint32_t GID = 0;
  uint64_t Size = 0;
  uint32_t BlockSize = 0;
  uint64_t Blocks = 0;
  uint32_t Flags = 0;
  uint32_t Generation = 0;
  DarwinFileTime AccessTime;
  DarwinFileTime ModificationTime;
  DarwinFileTime ChangeTime;
  DarwinFileTime BirthTime;
};

/// Explicit sparse-unit virtual filesystem contract, not an APFS observation.
/// Initial bytes occupy every allocation unit through EOF, including zeros.
/// Writes allocate touched units; truncate growth creates holes and shrinking
/// drops whole units beyond EOF. A retained partial unit stays allocated.
/// Successful byte mutations use this fixed mtime/ctime; unlink uses it for
/// ctime and projects link_count from the remaining names, including zero.
/// Other metadata is preserved.
/// Requires complete initial metadata, ordinary permissions, flags=0 and
/// link_count=1.
struct DarwinFileMutationPolicy {
  /// Independent of st_blksize and guest page size. Power of two, 512..16 MiB.
  uint32_t AllocationUnit = 0;
  DarwinFileTime Time;
};

/// Explicit virtual mutation of an initial directory's observed stat record.
/// A committed child-name change sets nlink to two plus all linked immediate
/// names, size to nlink * DirectoryEntrySize, and mtime/ctime to Time. Direct
/// moves/removal set only ctime; other observed fields stay object-owned.
/// This does not infer APFS allocation or enforce permissions.
struct DarwinDirectoryMutationPolicy {
  /// Positive and at most 16 MiB; projected stat size is not stored file bytes.
  uint32_t DirectoryEntrySize = 0;
  DarwinFileTime Time;
};

/// Retain an initial symbolic link's complete observed stat across direct
/// namespace moves, changing only ctime to this declared time. Raw target bytes
/// and all other fields stay object-owned; ancestor moves do not change stat.
struct DarwinSymbolicLinkMutationPolicy {
  DarwinFileTime Time;
};

/// Explicit virtual metadata rules for new links and directories. Link blocks
/// round raw-target bytes up to SymbolicLinkAllocationUnit (in 512-byte
/// blocks). Directory nlink is two plus its immediate linked names of every
/// kind; size is nlink * DirectoryEntrySize, even while an empty removed
/// directory is held. Directory blocks stay fixed. These rules are not inferred
/// APFS observations.
struct DarwinNamespaceCreationPolicy {
  /// Independent of st_blksize and page size. Power of two, 512..16 MiB.
  uint32_t SymbolicLinkAllocationUnit = 0;
  /// Positive, at most 16 MiB; does not charge virtual stat size as file bytes.
  uint32_t DirectoryEntrySize = 0;
  uint64_t DirectoryBlocks = 0;
};

/// Opt-in metadata for new objects in the explicit virtual filesystem.
/// Every mutable parent must have metadata; its Device/GID remain unchanged
/// by namespace mutations. UID is the profile's effective guest user ID.
/// This supplies neither permission enforcement nor native APFS observations.
struct DarwinFileCreationPolicy {
  /// Nonzero and greater than every supplied stat/directory inode. Successful
  /// regular creations, and link/directory creations when Namespace is present,
  /// advance this global sequence; UINT64_MAX exhausts it forever.
  uint64_t FirstInode = 0;
  uint32_t BlockSize = 0;
  uint32_t Generation = 0;
  /// Fixed initial atime/mtime/ctime/birthtime, not a host-clock sample.
  DarwinFileTime Time;
  DarwinFileMutationPolicy Mutation;
  /// Omission preserves unknown new-link/directory metadata and consumes no
  /// inode for those kinds. When present, mkdir and symlink use ordinary mode
  /// bits masked by umask; child namespace changes set parent mtime/ctime and
  /// direct moves set object ctime to Mutation.Time. Other fields are
  /// preserved.
  std::optional<DarwinNamespaceCreationPolicy> Namespace;
};

/// One observed directory record. NextOffset is the enumeration cursor after
/// this entry, distinct from the optional d_seekoff observation. Cookies are
/// local to a snapshot and need not increase; zero is reserved for rewind.
struct DarwinDirectoryEntry {
  std::string Name;
  uint64_t Inode = 0;
  uint8_t Type = 0;
  uint64_t NextOffset = 0;
  uint64_t SeekOffset = 0;
  /// Additional payload minimum when starting at this record, if any.
  uint32_t MinimumBufferSize = 0;
};

/// Complete ordered snapshot, including . and .. and every catalogue child.
/// No host inode, filesystem order or cursor is inferred from path names.
struct DarwinDirectoryContents {
  std::vector<DarwinDirectoryEntry> Entries;
  /// Explicit positive payload minimum, including at EOF. Extended syscall
  /// flags are excluded from this count; each next record must also fit whole.
  uint32_t MinimumBufferSize = 0;
};

/// Explicit virtual enumeration: . and .. precede unsigned-byte-sorted live
/// names; cookies are local ordinals and d_seekoff is the declared constant.
/// Namespace changes require a zero rewind before reusing a cursor. Held
/// removed directories enumerate no records. No APFS order/cookie is inferred.
struct DarwinDirectoryEnumerationPolicy {
  /// Positive payload minimum, including at EOF; at most 128 MiB.
  uint32_t MinimumBufferSize = 0;
  /// Additional minimum when starting at the first record; at most 128 MiB.
  uint32_t InitialMinimumBufferSize = 0;
  uint64_t SeekOffset = 0;
  /// Explicit virtual TYPE bulk enumeration: sorted live child names, local
  /// ordinals and independent iteration/EOF state. Does not infer APFS cookies.
  /// This initial object grant is not inherited by newly created directories.
  /// The two buffer minima and SeekOffset above apply only to getdirentries64.
  bool BulkAttributes = false;
};

/// Immutable ordinary local authorization for access/faccessat only.
/// Declares no ACL, MAC, additional kauth listener, entitlement or other
/// permission bypass; a writable/executable nonopaque local mount with
/// ownership enabled; flags=0 and no special mode bits. Actual checks require
/// explicit credentials, metadata and a nonzero selected UID. Root and general
/// vnode authorization remain unsupported. Other vnode operations are
/// closed; existing opaque standard streams remain independent.
enum class DarwinFileAuthorization {
  /// Actual checks require a matching owner UID; nonowners stay unsupported.
  StaticOwnerQueries,
  /// Same closed ordinary environment, with nonowner queries when whole-mask
  /// group/world outcomes agree or explicit credentials prove membership.
  /// A missing in-credential group is usually unknown external membership;
  /// the real-credential displacement may make an explicit list authoritative.
  /// Selected root and general vnode operations remain unsupported.
  StaticOrdinaryQueries
};

/// Closed initial catalogue, with canonical absolute guest paths. No host
/// filesystem is consulted. Separate opens have independent offsets; dup
/// shares an open description. Ancestor directories are implicit.
/// Constructed regular descriptions model successful ordinary poll filter
/// attachment, no revocation and no MAC/provider refusal. Arbitrary native
/// vnode registration failures and security lifecycle are outside this model.
struct DarwinFileOptions {
  std::map<std::string, std::vector<uint8_t>> Files;
  /// Absence retains existence-only queries without permission enforcement.
  /// This declaration never infers missing credentials or metadata.
  std::optional<DarwinFileAuthorization> Authorization;
  /// Absent means unknown input; an explicitly empty stream is EOF.
  std::optional<std::vector<uint8_t>> StandardInput;
  /// Exclusive FD ceiling, including the initially open descriptors 0/1/2.
  uint32_t DescriptorLimit = darwin_file_limits::DefaultDescriptors;
  /// Optional metadata for existing files, directories and symbolic links.
  std::map<std::string, DarwinFileMetadata> Metadata;
  /// Complete ordered initial observations for existing objects. Omission is
  /// unknown; an empty vector declares a known empty list. Namespace changes
  /// retain these observations on the object, including held removed objects.
  /// Content writes/truncate invalidate them; new objects start unknown.
  /// Protected system attributes, ResourceFork, FinderInfo and decmpfs are
  /// excluded. Neither permissions nor host attributes are inferred.
  std::map<std::string, std::vector<DarwinExtendedAttribute>>
      ExtendedAttributes;
  /// Independent ordinary xattr mutation grants on initial objects with a
  /// complete observed list. Created objects and reused names do not inherit
  /// them. Replacement keeps its slot, deletion removes it, creation appends.
  /// Initial bytes/count remain reserved; runtime excess shares the catalogue
  /// limits. Success invalidates complete stat, without changing enumeration.
  std::set<std::string> MutableExtendedAttributes;
  /// Explicit directories, including empty ones; root and ancestors are
  /// implicit.
  std::set<std::string> Directories;
  /// Absent means unknown, not the host CWD. Must name an existing directory.
  std::optional<std::string> WorkingDirectory;
  /// Absent snapshots remain unknown, even for an explicit empty directory.
  std::map<std::string, DarwinDirectoryContents> DirectoryContents;
  /// Explicit mutable regular files. Other catalogue entries stay read-only.
  /// Mutations are process-local; they never change these input bytes.
  std::set<std::string> WritableFiles;
  /// Optional per-file virtual metadata policy. Without one, post-mutation
  /// metadata remains unknown. A failed nonempty copyin can invalidate even
  /// configured metadata; later successful mutations do not restore it.
  std::map<std::string, DarwinFileMutationPolicy> MutationPolicies;
  /// Explicit authority to change immediate names in these directories.
  /// Newly created files have writable contents; new directories inherit
  /// namespace mutation authority. Existing objects retain their separate
  /// grants. Namespace changes invalidate initial parent metadata and
  /// snapshots; created parents use explicit Namespace metadata when supplied.
  /// New link and directory metadata stay unknown without that policy, and
  /// their creations then leave the regular-file inode sequence unchanged.
  /// Removed directories retain their object and original parent while held
  /// by directory FDs, CWD or children.
  /// Regular-file rename may cross these created descendants of one initial
  /// directory object. Distinct initial parents need explicit mount knowledge;
  /// matching Device observations do not grant cross-parent rename.
  std::set<std::string> MutableDirectories;
  /// Removal authority for explicit initial Directories entries other than
  /// root. Declares an ordinary non-mount object with one namespace identity;
  /// its immediate parent must be mutable. Known flags, aliases or conflicting
  /// parent/target devices reject admission. Fixed initial costs stay reserved
  /// after removal; old observations never describe a reused name.
  std::set<std::string> RemovableDirectories;
  /// Declared RENAME_SWAP support in these explicit mutable initial directory
  /// domains. Created descendants inherit the original object's declaration.
  /// Omission remains unknown; matching devices or mutable grants alone do not
  /// supply filesystem support. Each reference has a fixed path+NUL charge.
  std::set<std::string> SwapRenameDirectories;
  /// Optional virtual creation metadata; requires InitialUmask. Without its
  /// Namespace extension, directories inherit only known device/group and
  /// links/directories consume no inode. Never applies to initial objects or
  /// reads any host environment. Input stays unchanged.
  std::optional<DarwinFileCreationPolicy> CreationPolicy;
  /// Initial process mask, including all 07777 bits returned by Darwin umask.
  /// Independent of creation authority. Absence is unknown, not a host default.
  std::optional<uint16_t> InitialUmask;
  /// Ordinary rename authority for explicit non-root initial directory roots.
  /// Their complete initial subtree is declared non-mount and uniquely named;
  /// the immediate parent must be mutable. Descendants keep their own grants.
  /// Fixed input costs stay reserved, while moved names acquire dynamic
  /// charges. This does not authorize initial-directory SWAP or directory
  /// removal.
  std::set<std::string> MovableDirectories;
  /// SWAP operand authority for explicit non-root initial directory objects.
  /// Declares the same ordinary non-mount, unique initial subtree as Movable,
  /// while keeping ordinary rename and removal grants separate. The immediate
  /// parent must be mutable; both actual parents still need SWAP support for
  /// distinct-object exchange. Fixed references charge path+NUL independently.
  std::set<std::string> ExchangeableDirectories;
  /// Fixed initial symbolic-link names and exact nonempty, non-NUL target
  /// bytes. Targets need not exist and are never normalized at admission.
  /// Mutable directories can contain initial link names only with an explicit
  /// MutableSymbolicLinks grant. Lookup observes current target names.
  /// Content mutations, descriptors and mappings retain the actual target.
  std::map<std::string, std::vector<uint8_t>> SymbolicLinks;
  /// Optional per-object virtual enumeration for admitted initial directories
  /// with nonzero observed inodes. mkdir inherits the actual parent's policy;
  /// existing descendants retain their own declarations. Every listed child
  /// and the actual parent must have a known nonzero inode. This is independent
  /// of full stat validity, namespace grants and creation metadata. A directory
  /// cannot also declare an immutable DirectoryContents snapshot.
  std::map<std::string, DarwinDirectoryEnumerationPolicy>
      DirectoryEnumerationPolicies;
  /// Optional mutation authority for admitted initial directory metadata with
  /// a nonzero inode. Retain the complete initial record until a genuine
  /// committed change. This follows the object through moves/name reuse and
  /// is independent of namespace grants, creation and enumeration policies.
  /// mkdir descendants use their separately declared creation policy.
  std::map<std::string, DarwinDirectoryMutationPolicy>
      DirectoryMutationPolicies;
  /// Namespace mutation authority for explicit initial symbolic links. Their
  /// actual parents must be mutable; this declares ordinary unique objects and
  /// rejects known flags, aliases and conflicting parent devices. It grants no
  /// access to referents. Fixed input costs remain reserved after removal;
  /// moved names acquire separate dynamic costs. Omission keeps links
  /// protected.
  std::set<std::string> MutableSymbolicLinks;
  /// Optional complete-stat retention for granted initial links with nonzero
  /// observed inodes. Without a policy, a direct move makes full stat unknown
  /// but retains inode identity for enumeration. This follows the object and
  /// never transfers to a reused name or new symlink object.
  std::map<std::string, DarwinSymbolicLinkMutationPolicy>
      SymbolicLinkMutationPolicies;
};
} // namespace neverd::emulation
#endif
