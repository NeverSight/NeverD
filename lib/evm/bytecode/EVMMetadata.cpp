//===- EVMMetadata.cpp - The compiler metadata trailer ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/bytecode/EVMMetadata.h"

#include <array>
#include <utility>

namespace neverd::evm {
namespace {

/// The subset of CBOR a compiler trailer uses.
///
/// A trailer holds short text keys, byte strings, small integers, booleans,
/// and — since a compiler started describing the code layout alongside the
/// build — arrays and maps nested inside each other. Nothing here follows an
/// indefinite length or a tag: a trailer that contains one is not a trailer
/// this reader claims to understand, and saying so is better than half-reading
/// it.
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

/// The most items one container may declare, and the most containers one value
/// may nest. A footer is a handful of small items inside at most a sequence, a
/// map, and a version array; these bounds only keep a corrupt count from naming
/// a huge container and a hostile one from recursing.
inline constexpr uint64_t kMaxItems = 32;
inline constexpr unsigned kMaxDepth = 4;

class Reader {
public:
  explicit Reader(llvm::ArrayRef<uint8_t> Bytes) : Bytes(Bytes) {}

  [[nodiscard]] bool exhausted() const { return Position == Bytes.size(); }
  [[nodiscard]] bool failed() const { return Failed; }

  /// Read one value, which must be one of the kinds a trailer may hold.
  std::optional<MetadataValue> readValue(unsigned Depth = 0) {
    if (Depth > kMaxDepth)
      return fail();
    const auto Head = readHead();
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
      const llvm::ArrayRef<uint8_t> Payload = readBytes(Argument);
      if (Failed)
        return std::nullopt;
      Value.Kind = MetadataValueKind::ByteString;
      Value.Bytes.assign(Payload.begin(), Payload.end());
      return Value;
    }
    case MajorType::Text: {
      const llvm::ArrayRef<uint8_t> Payload = readBytes(Argument);
      if (Failed)
        return std::nullopt;
      Value.Kind = MetadataValueKind::Text;
      Value.Text.assign(Payload.begin(), Payload.end());
      return Value;
    }
    case MajorType::Array: {
      if (Argument > kMaxItems)
        return fail();
      Value.Kind = MetadataValueKind::Array;
      Value.Elements.reserve(static_cast<size_t>(Argument));
      for (uint64_t I = 0; I < Argument; ++I) {
        auto Item = readValue(Depth + 1);
        if (!Item)
          return std::nullopt;
        Value.Elements.push_back(std::move(*Item));
      }
      return Value;
    }
    case MajorType::Map: {
      if (Argument > kMaxItems)
        return fail();
      Value.Kind = MetadataValueKind::Map;
      Value.Keys.reserve(static_cast<size_t>(Argument));
      Value.Elements.reserve(static_cast<size_t>(Argument));
      for (uint64_t I = 0; I < Argument; ++I) {
        // Every key a compiler writes is text. A key of any other kind is a
        // document with a different meaning, not this one with a surprise in
        // it.
        const auto KeyHead = readHead();
        if (!KeyHead || KeyHead->first != MajorType::Text)
          return fail();
        const llvm::ArrayRef<uint8_t> KeyBytes = readBytes(KeyHead->second);
        if (Failed)
          return std::nullopt;
        auto Item = readValue(Depth + 1);
        if (!Item)
          return std::nullopt;
        Value.Keys.emplace_back(KeyBytes.begin(), KeyBytes.end());
        Value.Elements.push_back(std::move(*Item));
      }
      return Value;
    }
    case MajorType::Simple:
      if (Argument != kFalse && Argument != kTrue)
        return fail();
      Value.Kind = MetadataValueKind::Boolean;
      Value.Boolean = Argument == kTrue;
      return Value;
    case MajorType::Negative:
    case MajorType::Tag:
      return fail();
    }
    return fail();
  }

private:
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

  std::nullopt_t fail() {
    Failed = true;
    return std::nullopt;
  }

  llvm::ArrayRef<uint8_t> Bytes;
  size_t Position = 0;
  bool Failed = false;
};

} // namespace cbor

/// A release version has three components, whether they are written as one
/// byte each or as one number each.
inline constexpr size_t kVersionComponents = 3;

/// Turn a read map into the keys of a compiler map, or nothing when it is not
/// one. A map naming no tabulated key is not evidence that these bytes are a
/// trailer at all.
std::optional<std::vector<MetadataEntry>> readCompilerMap(MetadataValue Value) {
  if (Value.Kind != MetadataValueKind::Map)
    return std::nullopt;

  std::vector<MetadataEntry> Entries;
  Entries.reserve(Value.Keys.size());
  bool Tabulated = false;
  for (size_t I = 0; I < Value.Keys.size(); ++I) {
    MetadataEntry Entry;
    Entry.Key = std::move(Value.Keys[I]);
    Entry.Field = findMetadataField(Entry.Key);
    Entry.Value = std::move(Value.Elements[I]);
    Tabulated |= Entry.Field != nullptr;
    Entries.push_back(std::move(Entry));
  }
  if (!Tabulated)
    return std::nullopt;
  return Entries;
}

/// Read the footer at the end of \p Code under one framing.
std::optional<ContractMetadata> readFooter(llvm::ArrayRef<uint8_t> Code,
                                           const MetadataContainerInfo &Info) {
  if (Code.size() <= kMetadataLengthBytes)
    return std::nullopt;
  size_t Declared = 0;
  for (size_t I = Code.size() - kMetadataLengthBytes; I < Code.size(); ++I)
    Declared = (Declared << kBitsPerByte) | Code[I];

  // The same two bytes mean different things to the two compilers, so how far
  // back the footer starts follows from the framing rather than from the
  // number. Reading one framing's bytes under the other lands two bytes off.
  const size_t Total =
      Info.LengthCountsItself ? Declared : Declared + kMetadataLengthBytes;
  if (Total <= kMetadataLengthBytes || Total > Code.size())
    return std::nullopt;

  ContractMetadata Metadata;
  Metadata.Container = Info.ID;
  Metadata.Offset = Code.size() - Total;
  Metadata.Size = Total;

  cbor::Reader In(Code.slice(Metadata.Offset, Total - kMetadataLengthBytes));
  auto Value = In.readValue();
  // A trailer ends exactly where its declared length says. Bytes left over
  // mean the length and the CBOR disagree about which bytes are not code.
  if (!Value || !In.exhausted())
    return std::nullopt;

  switch (Info.ID) {
  case MetadataContainer::SolidityMap: {
    auto Entries = readCompilerMap(std::move(*Value));
    if (!Entries)
      return std::nullopt;
    Metadata.Entries = std::move(*Entries);
    return Metadata;
  }
  case MetadataContainer::VyperSequence: {
    if (Value->Kind != MetadataValueKind::Array ||
        Value->Elements.size() < kMinMetadataSequenceLength ||
        Value->Elements.size() > kMaxMetadataSequenceLength)
      return std::nullopt;
    const size_t Length = Value->Elements.size();
    for (size_t I = 0; I < Length; ++I) {
      // Counting back from the map is what lets one table read both the
      // sequence a release wrote and the longer one its successor wrote.
      const MetadataSequenceElementInfo *Element =
          findMetadataSequenceElement(Length - 1 - I);
      if (!Element || Element->ValueKind != Value->Elements[I].Kind)
        return std::nullopt;
      if (Element->ValueKind == MetadataValueKind::Map) {
        auto Entries = readCompilerMap(std::move(Value->Elements[I]));
        if (!Entries)
          return std::nullopt;
        Metadata.Entries = std::move(*Entries);
        continue;
      }
      Metadata.Sequence.push_back({Element, std::move(Value->Elements[I])});
    }
    return Metadata;
  }
  }
  return std::nullopt;
}

} // namespace

llvm::ArrayRef<MetadataLanguageInfo> metadataLanguageInfos() {
  static const std::array Table = {
#define EVM_METADATA_LANGUAGE(ID, NAME, SUMMARY)                               \
  MetadataLanguageInfo{MetadataLanguage::ID, NAME, SUMMARY},
#include "neverd/evm/bytecode/EVMMetadataFields.def"
  };
  return Table;
}

llvm::StringRef metadataLanguageName(MetadataLanguage Language) {
  return metadataLanguageInfos()[static_cast<size_t>(Language)].Name;
}

llvm::ArrayRef<MetadataFieldInfo> metadataFieldInfos() {
  static const std::array Table = {
#define EVM_METADATA_FIELD(ID, KEY, KIND, LANGUAGE, NAME, SUMMARY)             \
  MetadataFieldInfo{MetadataField::ID,                                         \
                    MetadataFieldKind::KIND,                                   \
                    MetadataLanguage::LANGUAGE,                                \
                    KEY,                                                       \
                    NAME,                                                      \
                    SUMMARY},
#include "neverd/evm/bytecode/EVMMetadataFields.def"
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

llvm::StringRef metadataValueKindName(MetadataValueKind Kind) {
  switch (Kind) {
#define EVM_METADATA_VALUE_KIND(ID, SPELLING)                                  \
  case MetadataValueKind::ID:                                                  \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/bytecode/EVMMetadataFields.def"
  }
  return kUnknownName;
}

llvm::ArrayRef<MetadataContainerInfo> metadataContainerInfos() {
  static const std::array Table = {
#define EVM_METADATA_CONTAINER(ID, SPELLING, LENGTH_COUNTS_ITSELF, SUMMARY)    \
  MetadataContainerInfo{MetadataContainer::ID, SPELLING,                       \
                        (LENGTH_COUNTS_ITSELF), SUMMARY},
#include "neverd/evm/bytecode/EVMMetadataFields.def"
  };
  return Table;
}

const MetadataContainerInfo &
getMetadataContainerInfo(MetadataContainer Container) {
  return metadataContainerInfos()[static_cast<size_t>(Container)];
}

llvm::StringRef metadataContainerName(MetadataContainer Container) {
  return getMetadataContainerInfo(Container).Name;
}

llvm::ArrayRef<MetadataSequenceElementInfo> metadataSequenceElementInfos() {
  static const std::array Table = {
#define EVM_METADATA_SEQUENCE_ELEMENT(ID, POSITION_FROM_END, VALUE_KIND,       \
                                      REQUIRED, NAME, SUMMARY)                 \
  MetadataSequenceElementInfo{MetadataSequenceElement::ID,                     \
                              (POSITION_FROM_END),                             \
                              MetadataValueKind::VALUE_KIND,                   \
                              (REQUIRED),                                      \
                              NAME,                                            \
                              SUMMARY},
#include "neverd/evm/bytecode/EVMMetadataFields.def"
  };
  return Table;
}

const MetadataSequenceElementInfo &
getMetadataSequenceElementInfo(MetadataSequenceElement ID) {
  return metadataSequenceElementInfos()[static_cast<size_t>(ID)];
}

const MetadataSequenceElementInfo *
findMetadataSequenceElement(size_t PositionFromEnd) {
  for (const MetadataSequenceElementInfo &Info : metadataSequenceElementInfos())
    if (Info.PositionFromEnd == PositionFromEnd)
      return &Info;
  return nullptr;
}

const MetadataValue *MetadataValue::lookup(llvm::StringRef Key) const {
  if (Kind != MetadataValueKind::Map)
    return nullptr;
  for (size_t I = 0; I < Keys.size(); ++I)
    if (Keys[I] == Key)
      return &Elements[I];
  return nullptr;
}

std::string MetadataValue::versionString() const {
  // A prerelease does not fit three components and is written as the whole
  // spelling; a release is written as one byte per component by one compiler
  // and one number per component by the other.
  if (Kind == MetadataValueKind::Text)
    return Text;

  std::string Version;
  const auto Append = [&Version](uint64_t Component) {
    if (!Version.empty())
      Version += '.';
    Version += std::to_string(Component);
  };

  if (Kind == MetadataValueKind::ByteString &&
      Bytes.size() == kVersionComponents) {
    for (uint8_t Component : Bytes)
      Append(Component);
    return Version;
  }
  if (Kind == MetadataValueKind::Array &&
      Elements.size() == kVersionComponents) {
    for (const MetadataValue &Component : Elements) {
      if (Component.Kind != MetadataValueKind::Unsigned)
        return {};
      Append(Component.Unsigned);
    }
    return Version;
  }
  return {};
}

const MetadataEntry *ContractMetadata::find(MetadataField Field) const {
  for (const MetadataEntry &Entry : Entries)
    if (Entry.Field && Entry.Field->ID == Field)
      return &Entry;
  return nullptr;
}

const MetadataSequenceEntry *
ContractMetadata::find(MetadataSequenceElement Element) const {
  for (const MetadataSequenceEntry &Entry : Sequence)
    if (Entry.Element && Entry.Element->ID == Element)
      return &Entry;
  return nullptr;
}

std::optional<uint64_t>
ContractMetadata::sequenceUnsigned(MetadataSequenceElement Element) const {
  const MetadataSequenceEntry *Entry = find(Element);
  if (!Entry || Entry->Value.Kind != MetadataValueKind::Unsigned)
    return std::nullopt;
  return Entry->Value.Unsigned;
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
    if (std::string Version = Entry.Value.versionString(); !Version.empty())
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
  for (const MetadataContainerInfo &Info : metadataContainerInfos())
    if (auto Metadata = readFooter(Code, Info))
      return Metadata;
  return std::nullopt;
}

} // namespace neverd::evm
