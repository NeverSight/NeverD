//===- Metadata.cpp - The compiler metadata trailer ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Metadata.h"

#include <array>
#include <utility>

namespace neverd::evm {
namespace {

/// The subset of CBOR a compiler trailer uses.
///
/// The trailer is a definite-length map of short text keys whose values are
/// byte strings, text, small integers, and booleans. Nothing here follows an
/// indefinite length, a tag, or a nested container: a trailer that contains one
/// is not a trailer this reader claims to understand, and saying so is better
/// than half-reading it.
namespace cbor {

inline constexpr unsigned kMajorTypeShift = 5;
inline constexpr uint8_t kAdditionalInfoMask = 0x1f;

enum class MajorType : uint8_t {
  Unsigned = 0,
  Negative = 1,
  ByteString = 2,
  Text = 3,
  Array = 4,
  Map = 5,
  Tag = 6,
  Simple = 7,
};

/// Additional-information values that introduce a following length, and the
/// first value that is not an immediate count.
inline constexpr uint8_t kImmediateLimit = 24;
inline constexpr uint8_t kOneByteLength = 24;
inline constexpr uint8_t kTwoByteLength = 25;

/// The simple values a trailer's booleans use.
inline constexpr uint8_t kFalse = 20;
inline constexpr uint8_t kTrue = 21;

class Reader {
public:
  explicit Reader(llvm::ArrayRef<uint8_t> Bytes) : Bytes(Bytes) {}

  [[nodiscard]] bool exhausted() const { return Position == Bytes.size(); }
  [[nodiscard]] bool failed() const { return Failed; }

  /// The head of the next item: its major type and the count or length its
  /// additional information encodes.
  std::optional<std::pair<MajorType, uint64_t>> readHead() {
    if (Failed || Position >= Bytes.size())
      return fail();
    const uint8_t Initial = Bytes[Position++];
    const auto Type = static_cast<MajorType>(Initial >> kMajorTypeShift);
    const uint8_t Info = Initial & kAdditionalInfoMask;
    if (Info < kImmediateLimit)
      return std::make_pair(Type, static_cast<uint64_t>(Info));
    // A trailer's lengths are all small. Accepting only the two shortest
    // encodings keeps a hostile length from naming a span the code cannot hold.
    const size_t Width = Info == kOneByteLength   ? 1
                         : Info == kTwoByteLength ? 2
                                                  : 0;
    if (Width == 0 || Bytes.size() - Position < Width)
      return fail();
    uint64_t Length = 0;
    for (size_t I = 0; I < Width; ++I)
      Length = (Length << kBitsPerByte) | Bytes[Position++];
    return std::make_pair(Type, Length);
  }

  llvm::ArrayRef<uint8_t> readBytes(uint64_t Length) {
    if (Failed || Length > Bytes.size() - Position) {
      fail();
      return {};
    }
    const llvm::ArrayRef<uint8_t> Slice =
        Bytes.slice(Position, static_cast<size_t>(Length));
    Position += static_cast<size_t>(Length);
    return Slice;
  }

private:
  std::nullopt_t fail() {
    Failed = true;
    return std::nullopt;
  }

  llvm::ArrayRef<uint8_t> Bytes;
  size_t Position = 0;
  bool Failed = false;
};

/// Read one value, which must be one of the four kinds a trailer may hold.
std::optional<MetadataValue> readValue(Reader &In) {
  const auto Head = In.readHead();
  if (!Head)
    return std::nullopt;
  const auto [Type, Argument] = *Head;
  MetadataValue Value;
  switch (Type) {
  case MajorType::Unsigned:
    Value.Kind = MetadataValueKind::Unsigned;
    Value.Unsigned = Argument;
    return Value;
  case MajorType::ByteString: {
    const llvm::ArrayRef<uint8_t> Payload = In.readBytes(Argument);
    if (In.failed())
      return std::nullopt;
    Value.Kind = MetadataValueKind::ByteString;
    Value.Bytes.assign(Payload.begin(), Payload.end());
    return Value;
  }
  case MajorType::Text: {
    const llvm::ArrayRef<uint8_t> Payload = In.readBytes(Argument);
    if (In.failed())
      return std::nullopt;
    Value.Kind = MetadataValueKind::Text;
    Value.Text.assign(Payload.begin(), Payload.end());
    return Value;
  }
  case MajorType::Simple:
    if (Argument != kFalse && Argument != kTrue)
      return std::nullopt;
    Value.Kind = MetadataValueKind::Boolean;
    Value.Boolean = Argument == kTrue;
    return Value;
  case MajorType::Negative:
  case MajorType::Array:
  case MajorType::Map:
  case MajorType::Tag:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace cbor

/// The number of map keys a trailer may declare. Solidity writes at most a
/// handful; this only stops a corrupt count from naming a huge one.
inline constexpr uint64_t kMaxMetadataEntries = 32;

/// A release version is written as one byte per component.
inline constexpr size_t kVersionTripleBytes = 3;

} // namespace

llvm::ArrayRef<MetadataLanguageInfo> metadataLanguageInfos() {
  static const std::array Table = {
#define EVM_METADATA_LANGUAGE(ID, NAME, SUMMARY)                               \
  MetadataLanguageInfo{MetadataLanguage::ID, NAME, SUMMARY},
#include "neverd/evm/EVMMetadataFields.def"
  };
  return Table;
}

llvm::StringRef metadataLanguageName(MetadataLanguage Language) {
  return metadataLanguageInfos()[static_cast<size_t>(Language)].Name;
}

llvm::ArrayRef<MetadataFieldInfo> metadataFieldInfos() {
  static const std::array Table = {
#define EVM_METADATA_FIELD(ID, KEY, KIND, LANGUAGE, NAME, SUMMARY)             \
  MetadataFieldInfo{MetadataField::ID, MetadataFieldKind::KIND,                \
                    MetadataLanguage::LANGUAGE, KEY, NAME, SUMMARY},
#include "neverd/evm/EVMMetadataFields.def"
  };
  return Table;
}

const MetadataFieldInfo &getMetadataFieldInfo(MetadataField ID) {
  return metadataFieldInfos()[static_cast<size_t>(ID)];
}

const MetadataFieldInfo *findMetadataField(llvm::StringRef Key) {
  for (const MetadataFieldInfo &Info : metadataFieldInfos())
    if (Info.Key == Key)
      return &Info;
  return nullptr;
}

const MetadataEntry *ContractMetadata::find(MetadataField Field) const {
  for (const MetadataEntry &Entry : Entries)
    if (Entry.Field && Entry.Field->ID == Field)
      return &Entry;
  return nullptr;
}

MetadataLanguage ContractMetadata::language() const {
  for (const MetadataEntry &Entry : Entries)
    if (Entry.Field && Entry.Field->Kind == MetadataFieldKind::CompilerVersion)
      return Entry.Field->Language;
  return MetadataLanguage::Unknown;
}

std::string ContractMetadata::compilerVersion() const {
  for (const MetadataEntry &Entry : Entries) {
    if (!Entry.Field || Entry.Field->Kind != MetadataFieldKind::CompilerVersion)
      continue;
    // A release writes one byte per version component; a prerelease writes the
    // whole spelling as text because it does not fit three bytes.
    if (Entry.Value.Kind == MetadataValueKind::Text)
      return Entry.Value.Text;
    if (Entry.Value.Kind != MetadataValueKind::ByteString ||
        Entry.Value.Bytes.size() != kVersionTripleBytes)
      continue;
    std::string Version;
    for (uint8_t Component : Entry.Value.Bytes) {
      if (!Version.empty())
        Version += '.';
      Version += std::to_string(Component);
    }
    return Version;
  }
  return {};
}

const MetadataEntry *ContractMetadata::sourceHash() const {
  for (const MetadataEntry &Entry : Entries)
    if (Entry.Field && Entry.Field->Kind == MetadataFieldKind::SourceHash)
      return &Entry;
  return nullptr;
}

std::optional<ContractMetadata>
findContractMetadata(llvm::ArrayRef<uint8_t> Code) {
  if (Code.size() <= kMetadataLengthBytes)
    return std::nullopt;
  size_t Declared = 0;
  for (size_t I = Code.size() - kMetadataLengthBytes; I < Code.size(); ++I)
    Declared = (Declared << kBitsPerByte) | Code[I];
  if (Declared == 0 || Declared + kMetadataLengthBytes > Code.size())
    return std::nullopt;

  ContractMetadata Metadata;
  Metadata.Offset = Code.size() - Declared - kMetadataLengthBytes;
  Metadata.Size = Declared + kMetadataLengthBytes;

  cbor::Reader In(Code.slice(Metadata.Offset, Declared));
  const auto Head = In.readHead();
  if (!Head || Head->first != cbor::MajorType::Map ||
      Head->second > kMaxMetadataEntries)
    return std::nullopt;

  bool Tabulated = false;
  for (uint64_t I = 0; I < Head->second; ++I) {
    const auto KeyHead = In.readHead();
    if (!KeyHead || KeyHead->first != cbor::MajorType::Text)
      return std::nullopt;
    const llvm::ArrayRef<uint8_t> KeyBytes = In.readBytes(KeyHead->second);
    if (In.failed())
      return std::nullopt;
    auto Value = cbor::readValue(In);
    if (!Value)
      return std::nullopt;

    MetadataEntry Entry;
    Entry.Key.assign(KeyBytes.begin(), KeyBytes.end());
    Entry.Field = findMetadataField(Entry.Key);
    Entry.Value = std::move(*Value);
    Tabulated |= Entry.Field != nullptr;
    Metadata.Entries.push_back(std::move(Entry));
  }

  // A trailer ends exactly where its declared length says. Trailing bytes mean
  // the length and the map disagree, and a map naming no tabulated key is not
  // evidence that these bytes are a trailer at all.
  if (!In.exhausted() || !Tabulated)
    return std::nullopt;
  return Metadata;
}

} // namespace neverd::evm
