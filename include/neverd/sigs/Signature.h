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
///   - Optionally, the routines the function branches to directly
///
/// The current loader accepts the text representation of these records.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_SIGNATURE_H
#define NEVERD_SIGS_SIGNATURE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neverd {
namespace sigs {

struct PatternByte {
  uint8_t Value = 0;
  bool IsWildcard = false;
};

struct FuncRef {
  uint32_t Offset = 0;
  std::string Name;
};

struct PatternModule {
  std::vector<PatternByte> LeadingBytes;

  uint16_t CRC16 = 0;
  uint8_t CRCLen = 0;

  uint32_t TotalLen = 0;

  std::vector<FuncRef> PublicNames;

  /// The routines the function branches to directly, as `^offset name`
  /// states them.  Offset is where the relocated branch field starts: the
  /// rel32 of an x86 or x64 `call`/`jmp` (E8/E9), and the branch instruction
  /// itself on ARM64 (B/BL) and Thumb-2 (B.W/BL/BLX).  The bytes there are
  /// wildcards, so matching checks the target separately; see
  /// SignatureDB::apply.
  std::vector<FuncRef> References;

  std::vector<PatternByte> TailBytes;
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
    while (Count < Name.size() && Name[Count] == '_')
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
  /// The routine's name: of the names the module gives this offset, the one
  /// preferredAliasOrder puts first.
  std::string Name;
  /// The module's other names for the same offset, in that order.
  std::vector<std::string> Aliases;
  std::string LibraryName;
  uint32_t FuncLen = 0;
  /// The module has references and the image confirmed every one of them;
  /// see SignatureDB::apply.  Such a match settles an address that other
  /// matches name differently.
  bool Confirmed = false;
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATURE_H
