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
enum class MetadataValueKind : uint8_t { Unsigned, ByteString, Text, Boolean };

struct MetadataValue {
  MetadataValueKind Kind = MetadataValueKind::Unsigned;
  uint64_t Unsigned = 0;
  bool Boolean = false;
  std::vector<uint8_t> Bytes;
  std::string Text;
};

/// One key of the trailer, in the order the compiler wrote it.
struct MetadataEntry {
  std::string Key;
  /// Null when no table entry claims the key, which is how a field a newer
  /// compiler introduced still reaches a reader.
  const MetadataFieldInfo *Field = nullptr;
  MetadataValue Value;
};

/// The whole trailer, read.
struct ContractMetadata {
  /// Where the trailer starts, and how many bytes it occupies including the
  /// two that record its length. Together these say exactly which bytes are
  /// not code.
  size_t Offset = 0;
  size_t Size = 0;
  std::vector<MetadataEntry> Entries;

  [[nodiscard]] const MetadataEntry *find(MetadataField Field) const;

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
/// Returns nothing unless the last two bytes give a length that lands on a
/// well-formed CBOR map holding at least one tabulated key. That condition is
/// what keeps code whose last bytes happen to look like a length from being
/// mistaken for a trailer and truncated.
std::optional<ContractMetadata>
findContractMetadata(llvm::ArrayRef<uint8_t> Code);

} // namespace neverd::evm

#endif // NEVERD_EVM_METADATA_H
