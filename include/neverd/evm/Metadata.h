//===- Metadata.h - The compiler metadata trailer -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the reader for the CBOR trailer a compiler appends to the code it
/// emits.
///
/// The trailer is not executable and no opcode reads it, so it has to be found
/// and removed before the code is decoded. Removing it without reading it
/// throws away the only self-description a deployed contract carries: which
/// compiler built it, which version, and the content address of the source it
/// was built from.
///
/// Two footer formats are in use and they frame themselves differently, so a
/// reader that knows only one does not fail on the other: it lands two bytes
/// off and silently truncates code. Both are read here, and an input matching
/// neither is left alone.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_METADATA_H
#define NEVERD_EVM_METADATA_H

#include "neverd/evm/EVMConstants.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::evm {

enum class MetadataLanguage : uint8_t {
#define EVM_METADATA_LANGUAGE(ID, NAME, SUMMARY) ID,
#include "neverd/evm/EVMMetadataFields.def"
};

struct MetadataLanguageInfo {
  MetadataLanguage ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<MetadataLanguageInfo> metadataLanguageInfos();
llvm::StringRef metadataLanguageName(MetadataLanguage Language);

/// How a tabulated field's value is meant to be read.
enum class MetadataFieldKind : uint8_t { CompilerVersion, SourceHash, Flag };

enum class MetadataField : uint8_t {
#define EVM_METADATA_FIELD(ID, KEY, KIND, LANGUAGE, NAME, SUMMARY) ID,
#include "neverd/evm/EVMMetadataFields.def"
};

struct MetadataFieldInfo {
  MetadataField ID;
  MetadataFieldKind Kind;
  MetadataLanguage Language;
  /// The map key exactly as the compiler writes it.
  llvm::StringLiteral Key;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<MetadataFieldInfo> metadataFieldInfos();
const MetadataFieldInfo &getMetadataFieldInfo(MetadataField ID);
/// The tabulated field \p Key names, or null when no table entry claims it.
const MetadataFieldInfo *findMetadataField(llvm::StringRef Key);

/// The CBOR value kinds a trailer is allowed to contain. Anything else makes
/// the trailer unreadable, which is reported rather than guessed around.
enum class MetadataValueKind : uint8_t {
#define EVM_METADATA_VALUE_KIND(ID, SPELLING) ID,
#include "neverd/evm/EVMMetadataFields.def"
};

llvm::StringRef metadataValueKindName(MetadataValueKind Kind);

struct MetadataValue {
  MetadataValueKind Kind = MetadataValueKind::Unsigned;
  uint64_t Unsigned = 0;
  bool Boolean = false;
  std::vector<uint8_t> Bytes;
  std::string Text;
  /// The items of an array, or the values of a map in the order the compiler
  /// wrote them, paired by position with Keys.
  std::vector<MetadataValue> Elements;
  /// A map's keys, empty for every other kind. Keys are kept beside the values
  /// rather than in a lookup table so a reader can still tell two builds apart
  /// by the order the compiler chose.
  std::vector<std::string> Keys;

  /// The value \p Key names in a map, or null in any other kind.
  [[nodiscard]] const MetadataValue *lookup(llvm::StringRef Key) const;

  /// The release this value spells, in the three forms a compiler writes one:
  /// a three-byte string, an array of three numbers, or free text. Empty when
  /// the value is not a version at all.
  [[nodiscard]] std::string versionString() const;
};

/// One key of the compiler map, in the order the compiler wrote it.
struct MetadataEntry {
  std::string Key;
  /// Null when no table entry claims the key, which is how a field a newer
  /// compiler introduced still reaches a reader.
  const MetadataFieldInfo *Field = nullptr;
  MetadataValue Value;
};

/// How a footer frames itself, which decides where the CBOR starts.
enum class MetadataContainer : uint8_t {
#define EVM_METADATA_CONTAINER(ID, SPELLING, LENGTH_COUNTS_ITSELF, SUMMARY) ID,
#include "neverd/evm/EVMMetadataFields.def"
};

struct MetadataContainerInfo {
  MetadataContainer ID;
  llvm::StringLiteral Name;
  /// True when the two trailing bytes count themselves, so that they give the
  /// distance back to the start of the footer rather than the length of the
  /// CBOR alone.
  bool LengthCountsItself;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<MetadataContainerInfo> metadataContainerInfos();
const MetadataContainerInfo &getMetadataContainerInfo(MetadataContainer ID);
llvm::StringRef metadataContainerName(MetadataContainer Container);

/// A member of a sequence footer, named by its distance from the compiler map
/// rather than its index, because that distance is what stayed fixed when a
/// release added a member at the front.
enum class MetadataSequenceElement : uint8_t {
#define EVM_METADATA_SEQUENCE_ELEMENT(ID, POSITION_FROM_END, VALUE_KIND,       \
                                      REQUIRED, NAME, SUMMARY)                 \
  ID,
#include "neverd/evm/EVMMetadataFields.def"
};

struct MetadataSequenceElementInfo {
  MetadataSequenceElement ID;
  /// Zero for the compiler map the sequence ends in.
  uint8_t PositionFromEnd;
  MetadataValueKind ValueKind;
  /// False for a member only newer releases write, which is what makes a
  /// shorter sequence a legal older footer.
  bool Required;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<MetadataSequenceElementInfo> metadataSequenceElementInfos();
const MetadataSequenceElementInfo &
getMetadataSequenceElementInfo(MetadataSequenceElement ID);
/// The member declared \p PositionFromEnd places before the compiler map, or
/// null when the table declares none that far back.
const MetadataSequenceElementInfo *
findMetadataSequenceElement(size_t PositionFromEnd);

/// The sequence lengths the table describes. A sequence outside this range
/// belongs to a format this reader does not claim to understand.
inline constexpr size_t kMinMetadataSequenceLength = [] {
  size_t Count = 0;
#define EVM_METADATA_SEQUENCE_ELEMENT(ID, POSITION_FROM_END, VALUE_KIND,       \
                                      REQUIRED, NAME, SUMMARY)                 \
  if (REQUIRED)                                                                \
    ++Count;
#include "neverd/evm/EVMMetadataFields.def"
  return Count;
}();

inline constexpr size_t kMaxMetadataSequenceLength = [] {
  size_t Count = 0;
#define EVM_METADATA_SEQUENCE_ELEMENT(ID, POSITION_FROM_END, VALUE_KIND,       \
                                      REQUIRED, NAME, SUMMARY)                 \
  ++Count;
#include "neverd/evm/EVMMetadataFields.def"
  return Count;
}();

static_assert(kMinMetadataSequenceLength > 0);
static_assert(kMinMetadataSequenceLength <= kMaxMetadataSequenceLength);

/// One member of a sequence footer ahead of the compiler map, in written
/// order.
struct MetadataSequenceEntry {
  /// Never null: a member the table does not describe makes the whole footer
  /// unreadable rather than partially read.
  const MetadataSequenceElementInfo *Element = nullptr;
  MetadataValue Value;
};

/// The whole trailer, read.
struct ContractMetadata {
  MetadataContainer Container = MetadataContainer::SolidityMap;
  /// Where the trailer starts, and how many bytes it occupies including the
  /// two that record its length. Together these say exactly which bytes are
  /// not code.
  size_t Offset = 0;
  size_t Size = 0;
  /// The keys of the compiler map, which for a map footer is the whole trailer
  /// and for a sequence footer is its last member.
  std::vector<MetadataEntry> Entries;
  /// The members a sequence footer carries ahead of that map. Empty for a map
  /// footer.
  std::vector<MetadataSequenceEntry> Sequence;

  [[nodiscard]] const MetadataEntry *find(MetadataField Field) const;
  [[nodiscard]] const MetadataSequenceEntry *
  find(MetadataSequenceElement Element) const;

  /// The value a sequence member holds when it is present and has the kind the
  /// table declares for it, and nothing otherwise.
  [[nodiscard]] std::optional<uint64_t>
  sequenceUnsigned(MetadataSequenceElement Element) const;

  /// The language the trailer names, and Unknown when it names none.
  [[nodiscard]] MetadataLanguage language() const;

  /// The compiler release, spelled the way the compiler spells it: "0.8.26"
  /// for a release triple, and the recorded text for a prerelease build. Empty
  /// when the trailer carries no version.
  [[nodiscard]] std::string compilerVersion() const;

  /// The tabulated content address the trailer carries, null when it carries
  /// none.
  [[nodiscard]] const MetadataEntry *sourceHash() const;
};

/// Read the trailer at the end of \p Code.
///
/// Returns nothing unless the last two bytes give a length that lands, under
/// one of the tabulated framings, on well-formed CBOR of the shape that
/// framing declares, ending in a map that holds at least one tabulated key.
/// That condition is what keeps code whose last bytes happen to look like a
/// length from being mistaken for a trailer and truncated.
std::optional<ContractMetadata>
findContractMetadata(llvm::ArrayRef<uint8_t> Code);

} // namespace neverd::evm

#endif // NEVERD_EVM_METADATA_H
