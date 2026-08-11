//===- Anchor.h - Anchor framework discriminators -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines Anchor's discriminator derivation and the two ways NeverD attaches a
/// name to a recovered discriminator: a dictionary of names that recur across
/// deployed programs, and an IDL supplied by the operator.
///
/// Anchor prefixes an item name with its namespace, hashes the result with
/// SHA-256, and keeps the leading eight bytes. Recovery therefore cannot invert
/// a discriminator; it can only confirm a candidate name.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANCHOR_H
#define NEVERD_SBF_ANCHOR_H

#include "neverd/sbf/Pubkey.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::sbf {

/// Anchor keeps this many leading bytes of the namespaced SHA-256 hash.
inline constexpr size_t kAnchorDiscriminatorLength = 8;

struct AnchorDiscriminator {
  std::array<uint8_t, kAnchorDiscriminatorLength> Bytes{};

  /// The discriminator as the little-endian 64-bit word an SBF program
  /// compares against, which is how it appears in a recovered immediate.
  uint64_t toWord() const;
  static AnchorDiscriminator fromWord(uint64_t Word);

  friend bool operator==(const AnchorDiscriminator &,
                         const AnchorDiscriminator &) = default;
  friend std::strong_ordering
  operator<=>(const AnchorDiscriminator &,
              const AnchorDiscriminator &) = default;
};

enum class AnchorNamespace : uint8_t {
#define SBF_ANCHOR_NAMESPACE(ID, PREFIX, SPELLING) ID,
#include "neverd/sbf/SBFAnchorNamespaces.def"
};

struct AnchorNamespaceInfo {
  AnchorNamespace ID;
  llvm::StringLiteral Prefix;
  llvm::StringLiteral Spelling;
};

llvm::ArrayRef<AnchorNamespaceInfo> anchorNamespaceInfos();
llvm::StringRef anchorNamespaceSpelling(AnchorNamespace Namespace);

/// Derive the discriminator Anchor would emit for \p Name in \p Namespace.
AnchorDiscriminator anchorDiscriminator(AnchorNamespace Namespace,
                                        llvm::StringRef Name);

struct AnchorNameInfo {
  AnchorNamespace Namespace;
  llvm::StringLiteral Name;
  AnchorDiscriminator Discriminator;
};

/// The built-in candidate dictionary, hashed once on first use.
llvm::ArrayRef<AnchorNameInfo> anchorNameInfos();

/// Look up a discriminator in the built-in dictionary. Returns null when the
/// program's names are not among the recurring ones.
const AnchorNameInfo *findAnchorName(const AnchorDiscriminator &Value);

/// One named item recovered from an operator-supplied IDL.
struct AnchorIdlItem {
  AnchorNamespace Namespace = AnchorNamespace::Instruction;
  std::string Name;
  AnchorDiscriminator Discriminator;
};

/// An Anchor IDL document reduced to what binary recovery can use.
struct AnchorIdl {
  std::string Name;
  std::string Version;
  std::optional<Pubkey> Address;
  std::vector<AnchorIdlItem> Items;
  /// Items the document declared but recovery cannot match, each with the
  /// reason. Reported rather than dropped, so a partial IDL is never mistaken
  /// for a complete one.
  std::vector<std::string> Skipped;

  const AnchorIdlItem *find(const AnchorDiscriminator &Value) const;
};

/// Parse an Anchor IDL JSON document. Accepts both the modern layout, which
/// stores an explicit `discriminator` byte array, and the legacy layout, whose
/// discriminators are derived from item names.
llvm::Expected<AnchorIdl> parseAnchorIdl(llvm::StringRef Text);

//===----------------------------------------------------------------------===//
// Error codes
//===----------------------------------------------------------------------===//

/// The band a returned failure code falls in. A program built against a newer
/// framework can return a code no table lists, and its band still says what
/// kind of check rejected the call.
enum class AnchorErrorRange : uint8_t {
#define SBF_ANCHOR_ERROR_RANGE(ID, NAME, FIRST, SUMMARY) ID,
#include "neverd/sbf/SBFAnchorErrors.def"
};

struct AnchorErrorRangeInfo {
  AnchorErrorRange ID;
  llvm::StringLiteral Name;
  /// Lowest code the band covers. The band ends where the next one begins.
  uint32_t First;
  llvm::StringLiteral Summary;
};

enum class AnchorError : uint16_t {
#define SBF_ANCHOR_ERROR(ID, CODE, MESSAGE) ID,
#include "neverd/sbf/SBFAnchorErrors.def"
};

struct AnchorErrorInfo {
  AnchorError ID;
  /// The framework's own variant name, which is what a program log prints.
  llvm::StringLiteral Name;
  uint32_t Code;
  llvm::StringLiteral Message;
};

llvm::ArrayRef<AnchorErrorRangeInfo> anchorErrorRangeInfos();
llvm::ArrayRef<AnchorErrorInfo> anchorErrorInfos();
const AnchorErrorRangeInfo &getAnchorErrorRangeInfo(AnchorErrorRange ID);

/// The lowest code a program's own error enumeration can use.
uint32_t anchorCustomErrorOffset();

/// What a returned failure code means.
struct AnchorErrorClassification {
  /// Null when the code is below every band, which means it is not an Anchor
  /// failure code at all.
  const AnchorErrorRangeInfo *Range = nullptr;
  /// Set when the framework declares exactly this code.
  const AnchorErrorInfo *Known = nullptr;
  /// Position within the program's own enumeration, set only for codes in the
  /// custom band. Anchor numbers those in declaration order, so the ordinal is
  /// meaningful even though the name is not recoverable without an IDL.
  std::optional<uint32_t> CustomOrdinal;

  /// True when the code says something a report should carry.
  bool isMeaningful() const { return Range != nullptr; }
};

AnchorErrorClassification classifyAnchorError(uint64_t Code);

/// Report a band out of order, a code outside the band it is listed under, or
/// a duplicated code.
llvm::Error validateAnchorErrorTable();

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANCHOR_H
