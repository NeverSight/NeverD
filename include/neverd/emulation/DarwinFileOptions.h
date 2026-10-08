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
} // namespace darwin_file_limits

struct DarwinFileTime {
  int64_t Seconds = 0;
  int64_t Nanoseconds = 0;
};

/// Fixed stat64 observations. Regular-file Size must match its bytes; directory
/// Size is an explicit nonnegative observation, not an entry count.
/// These observations do not grant or revoke catalogue access, and reads do
/// not advance timestamps. Unknown metadata is not synthesized from the host.
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
/// ctime and changes link_count to zero. Other metadata is preserved.
/// Requires complete initial metadata, ordinary permissions, flags=0 and
/// link_count=1.
struct DarwinFileMutationPolicy {
  /// Independent of st_blksize and guest page size. Power of two, 512..16 MiB.
  uint32_t AllocationUnit = 0;
  DarwinFileTime Time;
};

/// Opt-in metadata for new regular files in the explicit virtual filesystem.
/// Every mutable parent must have metadata; its Device/GID remain unchanged
/// by namespace mutations. UID is the profile's effective guest user ID.
/// This supplies neither permission enforcement nor native APFS observations.
struct DarwinFileCreationPolicy {
  /// Nonzero and greater than every supplied stat/directory inode. Successful
  /// creations advance this global sequence; UINT64_MAX exhausts it forever.
  uint64_t FirstInode = 0;
  uint32_t BlockSize = 0;
  uint32_t Generation = 0;
  /// Fixed initial atime/mtime/ctime/birthtime, not a host-clock sample.
  DarwinFileTime Time;
  DarwinFileMutationPolicy Mutation;
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

/// Closed initial catalogue, with canonical absolute guest paths. No host
/// filesystem is consulted. Separate opens have independent offsets; dup
/// shares an open description. Ancestor directories are implicit.
struct DarwinFileOptions {
  std::map<std::string, std::vector<uint8_t>> Files;
  /// Absent means unknown input; an explicitly empty stream is EOF.
  std::optional<std::vector<uint8_t>> StandardInput;
  /// Exclusive FD ceiling, including the initially open descriptors 0/1/2.
  uint32_t DescriptorLimit = darwin_file_limits::DefaultDescriptors;
  /// Optional metadata for existing files, directories and symbolic links.
  std::map<std::string, DarwinFileMetadata> Metadata;
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
  /// grants. Namespace changes invalidate the parent's metadata and snapshot.
  /// New directory and runtime-created symbolic-link metadata stay unknown.
  /// Link creation leaves the regular-file inode policy unchanged.
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
  /// Optional virtual regular-file creation metadata; requires InitialUmask.
  /// New directories inherit only the parent's known device/group and do not
  /// consume regular-file inode values. Never applies to existing objects or
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
  /// Mutable directories cannot contain fixed link names. Separate mutable
  /// domains may contain their targets; lookup observes current target names.
  /// Content mutations, descriptors and mappings retain the actual target.
  std::map<std::string, std::vector<uint8_t>> SymbolicLinks;
};
} // namespace neverd::emulation
#endif
