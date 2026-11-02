//===- Signature.h - FLIRT signature data types ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core data types for FLIRT-compatible function signature matching.
///
/// A signature pattern consists of:
///   - Leading bytes with a mask (fixed bytes vs wildcards)
///   - CRC16 checksum over trailing bytes for verification
///   - One or more function name associations with offsets; several names
///     at one offset are aliases, the linkage names one routine has
///   - Optionally, relocated code, import, RTTI or external-data references
///
/// The current loader accepts the text representation of these records.  A
/// PatternModule is one record as a value; a StoredModule is the same record
/// packed into arrays that a large signature set shares.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_SIGNATURE_H
#define NEVERD_SIGS_SIGNATURE_H

#include "llvm/ADT/ArrayRef.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd {
namespace sigs {

/// The sizes and bounds signature parsing and matching work with.
struct SignatureLimits {
#define NEVERD_SIGS_LIMIT(Name, Value) static constexpr size_t Name = Value;
#include "neverd/sigs/SignatureLimits.def"
};

/// The characters of linkage names and linker inputs.
struct LinkerSyntax {
#define NEVERD_SIGS_SYNTAX_CHAR(Name, Value) static constexpr char Name = Value;
#include "neverd/sigs/LinkerSyntax.def"
};

struct PatternByte {
  uint8_t Value = 0;
  bool IsWildcard = false;
};

struct FuncRef {
  uint32_t Offset = 0;
  bool IsData = false;
  bool IsLocalData = false;
  std::string Name;

  FuncRef() = default;
  FuncRef(uint32_t Offset, std::string Name, bool IsData = false,
          bool IsLocalData = false)
      : Offset(Offset), IsData(IsData), IsLocalData(IsLocalData),
        Name(std::move(Name)) {}
};

struct PatternModule {
  std::vector<PatternByte> LeadingBytes;

  uint16_t CRC16 = 0;
  uint8_t CRCLen = 0;

  /// `!refs`: every reference must be confirmed before this byte hit can
  /// become a candidate or support another candidate. This is a NeverD
  /// extension; older readers must reject it, not ignore the requirement.
  bool RequireReferences : 1 = false;
  /// `!caller`: indexed by expected linkage name only. An anchored complete
  /// caller must unanimously name this target at a reference site. A short
  /// caller additionally needs an independent ordinary callee; descendants
  /// never confirm their ancestors or supply that independent evidence.
  /// Complete literal bodies may also participate in a checked two-body x64
  /// code-address fingerprint; neither body alone supplies its identity.
  bool RequireCaller : 1 = false;
  /// `!vtable`: source-owned primary COFF x64 table metadata, never a
  /// function-byte claim. PublicNames names the table; References names its
  /// absolute code-pointer slots. Every pointer is a complete wildcard.
  bool IsVTable : 1 = false;
  /// `!loadcfg`: source configuration fields pairing external pointer storage
  /// (&) with its default function (^). Metadata, never a byte-index claim.
  bool IsLoadConfig : 1 = false;
  /// !layout: complete nonsplittable source code contribution. Never enters
  /// the byte index or supplies individual bodies to reference inference.
  bool IsCodeLayout : 1 = false;

  uint32_t TotalLen = 0;

  std::vector<FuncRef> PublicNames;

  /// The routines or COFF import pointers referenced by `^offset name`
  /// states them.  Offset is where the relocated branch field starts: the
  /// rel32 of an x86 or x64 `call`/`jmp` (E8/E9), and the branch instruction
  /// itself on ARM64 (B/BL) and Thumb-2 (B.W/BL/BLX).  An ARM-state (A32)
  /// B, BL or BLX is stated one byte past its instruction: its offset is
  /// odd, which no Thumb-2 instruction's is, so the offset alone says which
  /// instruction set the branch is in.  Several references at one offset
  /// name the routines that one branch may reach -- a COFF symbol and the
  /// alternate name a link resolves it to where no object defines it, or
  /// the routines two builds of the same bytes call there -- and any of them
  /// confirms it.  The bytes there are wildcards, so matching checks the
  /// target separately; see SignatureDB::apply. COFF x86/x64 additionally
  /// accepts canonical unprefixed conditional rel32 branches (0F 80..8F)
  /// from zero-addend relocations naming explicit function symbols. These
  /// references never create function entries. COFF x86/x64 also supports
  /// the four-byte field of FF 15/FF 25 import calls/jumps, either unprefixed
  /// or with a single canonical REX.W prefix in x64, named
  /// `__imp_<export>`. Only an exact name at the addressed, mapped IAT slot
  /// confirms that reference; absent or different spellings are unknown.
  /// A COFF x64 RIP-relative LEA can reference a primary MSVC vftable by its
  /// exact `??_7...6B@` linkage name. The loader validates RTTI to compare
  /// the mangled type; unsupported or incomplete metadata remains unknown.
  /// `&offset name` explicitly names external data, never a function. Only
  /// complete contextual records may contain it. The source instruction and
  /// independently frozen full-function anchors must agree on its storage.
  std::vector<FuncRef> References;

  std::vector<PatternByte> TailBytes;
};

/// A name a StoredModule gives an offset: one of its public names, or a
/// routine or external data it references. Public names are never data.
struct StoredName {
  uint32_t Offset = 0;
  bool IsData = false;
  bool IsLocalData = false;
  std::string_view Name;

  StoredName() = default;
  StoredName(uint32_t Offset, std::string_view Name, bool IsData = false,
             bool IsLocalData = false)
      : Offset(Offset), IsData(IsData), IsLocalData(IsLocalData), Name(Name) {}
};

/// A PatternModule packed for a large signature set.
///
/// A signature directory states hundreds of thousands of modules, and a
/// PatternModule spends several allocations on each: two bytes per pattern
/// byte, a vector per list, a string per name.  A StoredModule is a small
/// record pointing into memory shared by every module parsed from the same
/// text: a pattern byte is one byte and one bit saying whether the line
/// states it, and the names of a chunk of lines are one array (see
/// PatternNames).  It records exactly what the line says, and
/// SignatureMatcher matches both forms by one set of rules.
struct StoredModule {
  /// The leading bytes, then the tail bytes; an unstated byte is zero.
  const uint8_t *Bytes = nullptr;
  /// Bit I % 8 of byte I / 8 is set when byte I of \ref Bytes is stated.
  const uint8_t *Stated = nullptr;
  /// The public names, then the references.
  const StoredName *Names = nullptr;
  uint32_t LeadingCount = 0;
  uint32_t TailCount = 0;
  uint32_t TotalLen = 0;
  uint32_t PublicNameCount = 0;
  uint32_t ReferenceCount = 0;
  uint16_t CRC16 = 0;
  uint8_t CRCLen = 0;
  bool RequireReferences : 1 = false;
  /// `!caller`: indexed by expected linkage name only. An anchored complete
  /// caller must unanimously name this target at a reference site. A short
  /// caller additionally needs an independent ordinary callee; descendants
  /// never confirm their ancestors or supply that independent evidence.
  /// Complete literal bodies may also participate in a checked two-body x64
  /// code-address fingerprint; neither body alone supplies its identity.
  bool RequireCaller : 1 = false;
  /// `!vtable`: source-owned primary COFF x64 table metadata, never a
  /// function-byte claim. PublicNames names the table; References names its
  /// absolute code-pointer slots. Every pointer is a complete wildcard.
  bool IsVTable : 1 = false;
  /// `!loadcfg`: source configuration fields pairing external pointer storage
  /// (&) with its default function (^). Metadata, never a byte-index claim.
  bool IsLoadConfig : 1 = false;
  /// !layout: complete nonsplittable source code contribution. Never enters
  /// the byte index or supplies individual bodies to reference inference.
  bool IsCodeLayout : 1 = false;

  bool isStated(size_t Byte) const {
    return (Stated[Byte / CHAR_BIT] >> (Byte % CHAR_BIT)) & 1;
  }
  llvm::ArrayRef<StoredName> publicNames() const {
    return {Names, PublicNameCount};
  }
  llvm::ArrayRef<StoredName> references() const {
    return {Names + PublicNameCount, ReferenceCount};
  }
};

/// The names StoredModules point into, for one chunk of lines.  Moving it
/// leaves them where they are.
struct PatternNames {
  std::vector<StoredName> Names;
  std::vector<char> Text;
};

/// Whether \p A is the better of two linkage names one routine has.
///
/// An ELF library defines a routine under several symbols at one address:
/// glibc's `puts` is also `_IO_puts`, `malloc` also `__libc_malloc`. Any of
/// them names the code correctly; the one shown is the one with the fewest
/// leading underscores (the public spelling), then the shorter, then the
/// smaller. This is the one rule everything that picks among aliases uses.
inline bool preferredAliasOrder(std::string_view A, std::string_view B) {
  auto Underscores = [](std::string_view Name) {
    size_t Count = 0;
    while (Count < Name.size() &&
           Name[Count] == LinkerSyntax::ReservedNamePrefix)
      ++Count;
    return Count;
  };
  const size_t UA = Underscores(A), UB = Underscores(B);
  if (UA != UB)
    return UA < UB;
  if (A.size() != B.size())
    return A.size() < B.size();
  return A < B;
}

struct SigMatch {
  uint64_t Address = 0;
  /// The candidate name: of the names the module gives this offset, the one
  /// preferredAliasOrder puts first.
  std::string Name;
  /// The module's other names for the same offset, in that order.
  std::vector<std::string> Aliases;
  std::string LibraryName;
  uint32_t FuncLen = 0;
  /// The module has references and the image confirmed every one of them;
  /// see SignatureDB::apply. This can settle a candidate disputed by other
  /// patterns, but does not prove its original name or nominal source types.
  bool Confirmed = false;
  /// A frozen caller or independently validated source metadata requests this
  /// exact name at this address. Its outgoing constraints have no known
  /// contradiction. Multiple such incoming names are candidate aliases at the
  /// same target; they cannot prove source identity or confirm an ancestor's
  /// references.
  bool CallerConfirmed = false;
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATURE_H
