//===- SBFAnchor.cpp - Anchor framework discriminators --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/solana/SBFAnchor.h"

#include "neverd/sbf/SBFConstants.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <limits>

namespace neverd::sbf {
namespace {

/// Anchor joins the namespace prefix and the item name with this separator
/// before hashing, so it is part of the derivation rather than presentation.
constexpr llvm::StringLiteral kAnchorNamespaceSeparator(":");

/// Field and section names of the Anchor IDL JSON schema.
constexpr llvm::StringLiteral kIdlAddressField("address");
constexpr llvm::StringLiteral kIdlMetadataField("metadata");
constexpr llvm::StringLiteral kIdlNameField("name");
constexpr llvm::StringLiteral kIdlVersionField("version");
constexpr llvm::StringLiteral kIdlDiscriminatorField("discriminator");
constexpr llvm::StringLiteral kIdlInstructionsSection("instructions");
constexpr llvm::StringLiteral kIdlAccountsSection("accounts");
constexpr llvm::StringLiteral kIdlEventsSection("events");

llvm::Error idlError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("sbf: anchor idl: " + Message).str(), llvm::inconvertibleErrorCode());
}

/// Read an explicit `discriminator` byte array. Returns std::nullopt when the
/// field is absent so the caller can fall back to deriving it from the name.
llvm::Expected<std::optional<AnchorDiscriminator>>
readDiscriminator(const llvm::json::Object &Item, llvm::StringRef Context) {
  const llvm::json::Array *Bytes = Item.getArray(kIdlDiscriminatorField);
  if (!Bytes)
    return std::optional<AnchorDiscriminator>();
  if (Bytes->size() != kAnchorDiscriminatorLength)
    return idlError(Context + " declares a " + llvm::Twine(Bytes->size()) +
                    "-byte discriminator; only " +
                    llvm::Twine(kAnchorDiscriminatorLength) +
                    "-byte discriminators can be matched against recovered "
                    "code");
  AnchorDiscriminator Result;
  for (size_t Index = 0; Index < Bytes->size(); ++Index) {
    const std::optional<int64_t> Byte = (*Bytes)[Index].getAsInteger();
    if (!Byte || *Byte < 0 || *Byte > std::numeric_limits<uint8_t>::max())
      return idlError(Context + " has a discriminator byte that is not in the "
                                "unsigned byte range");
    Result.Bytes[Index] = static_cast<uint8_t>(*Byte);
  }
  return std::optional<AnchorDiscriminator>(Result);
}

llvm::Error readSection(const llvm::json::Object &Root, llvm::StringRef Section,
                        AnchorNamespace Namespace, AnchorIdl &Idl) {
  const llvm::json::Array *Items = Root.getArray(Section);
  if (!Items)
    return llvm::Error::success();

  for (const llvm::json::Value &Entry : *Items) {
    const llvm::json::Object *Item = Entry.getAsObject();
    if (!Item)
      return idlError("'" + Section + "' must contain only objects");
    const std::optional<llvm::StringRef> Name = Item->getString(kIdlNameField);
    if (!Name)
      return idlError("an entry in '" + Section + "' has no '" + kIdlNameField +
                      "'");

    const std::string Context = (Section + " '" + *Name + "'").str();
    llvm::Expected<std::optional<AnchorDiscriminator>> Explicit =
        readDiscriminator(*Item, Context);
    if (!Explicit) {
      // A discriminator this tool cannot match is recorded and skipped rather
      // than failing the whole document, so the rest stays usable.
      Idl.Skipped.push_back(llvm::toString(Explicit.takeError()));
      continue;
    }

    Idl.Items.push_back(
        {Namespace, Name->str(),
         Explicit->value_or(anchorDiscriminator(Namespace, *Name))});
  }
  return llvm::Error::success();
}

} // namespace

uint64_t AnchorDiscriminator::toWord() const {
  return llvm::support::endian::read64le(Bytes.data());
}

AnchorDiscriminator AnchorDiscriminator::fromWord(uint64_t Word) {
  AnchorDiscriminator Result;
  llvm::support::endian::write64le(Result.Bytes.data(), Word);
  return Result;
}

llvm::ArrayRef<AnchorNamespaceInfo> anchorNamespaceInfos() {
  static const std::array Table = {
#define SBF_ANCHOR_NAMESPACE(ID, PREFIX, SPELLING)                             \
  AnchorNamespaceInfo{AnchorNamespace::ID, PREFIX, SPELLING},
#include "neverd/sbf/solana/SBFAnchorNamespaces.def"
  };
  return Table;
}

llvm::StringRef anchorNamespaceSpelling(AnchorNamespace Namespace) {
  for (const AnchorNamespaceInfo &Info : anchorNamespaceInfos())
    if (Info.ID == Namespace)
      return Info.Spelling;
  return "unknown";
}

AnchorDiscriminator anchorDiscriminator(AnchorNamespace Namespace,
                                        llvm::StringRef Name) {
  llvm::StringRef Prefix;
  for (const AnchorNamespaceInfo &Info : anchorNamespaceInfos())
    if (Info.ID == Namespace)
      Prefix = Info.Prefix;

  llvm::SHA256 Hash;
  Hash.update(Prefix);
  Hash.update(kAnchorNamespaceSeparator);
  Hash.update(Name);

  AnchorDiscriminator Result;
  const std::array<uint8_t, 32> Digest = Hash.final();
  llvm::copy(llvm::ArrayRef(Digest).take_front(kAnchorDiscriminatorLength),
             Result.Bytes.begin());
  return Result;
}

llvm::ArrayRef<AnchorNameInfo> anchorNameInfos() {
  static const std::vector<AnchorNameInfo> Table = {
#define SBF_ANCHOR_NAME(NAMESPACE, NAME)                                       \
  AnchorNameInfo{AnchorNamespace::NAMESPACE, NAME,                             \
                 anchorDiscriminator(AnchorNamespace::NAMESPACE, NAME)},
#include "neverd/sbf/solana/SBFAnchorNames.def"
  };
  return Table;
}

const AnchorNameInfo *findAnchorName(const AnchorDiscriminator &Value) {
  // The dictionary is large enough that a scan per lookup would dominate
  // recovery, so it is indexed once. Building the index requires the table to
  // be injective, which SBFAnchorTests checks against the same derivation.
  static const llvm::DenseMap<uint64_t, const AnchorNameInfo *> ByWord = [] {
    llvm::DenseMap<uint64_t, const AnchorNameInfo *> Index;
    for (const AnchorNameInfo &Info : anchorNameInfos())
      Index.try_emplace(Info.Discriminator.toWord(), &Info);
    return Index;
  }();

  auto It = ByWord.find(Value.toWord());
  return It == ByWord.end() ? nullptr : It->second;
}

llvm::ArrayRef<AnchorErrorRangeInfo> anchorErrorRangeInfos() {
  static const std::array Table = {
#define SBF_ANCHOR_ERROR_RANGE(ID, NAME, FIRST, SUMMARY)                       \
  AnchorErrorRangeInfo{AnchorErrorRange::ID, NAME, FIRST, SUMMARY},
#include "neverd/sbf/solana/SBFAnchorErrors.def"
  };
  return Table;
}

llvm::ArrayRef<AnchorErrorInfo> anchorErrorInfos() {
  static const std::array Table = {
#define SBF_ANCHOR_ERROR(ID, CODE, MESSAGE)                                    \
  AnchorErrorInfo{AnchorError::ID, #ID, CODE, MESSAGE},
#include "neverd/sbf/solana/SBFAnchorErrors.def"
  };
  return Table;
}

const AnchorErrorRangeInfo &getAnchorErrorRangeInfo(AnchorErrorRange ID) {
  return anchorErrorRangeInfos()[static_cast<size_t>(ID)];
}

uint32_t anchorCustomErrorOffset() {
  return getAnchorErrorRangeInfo(AnchorErrorRange::Custom).First;
}

AnchorErrorClassification classifyAnchorError(uint64_t Code) {
  AnchorErrorClassification Result;
  if (Code > std::numeric_limits<uint32_t>::max())
    return Result;
  const auto Narrow = static_cast<uint32_t>(Code);

  // The bands are listed in ascending order, so the last one the code reaches
  // is the one that contains it.
  for (const AnchorErrorRangeInfo &Range : anchorErrorRangeInfos())
    if (Narrow >= Range.First)
      Result.Range = &Range;
  if (!Result.Range)
    return Result;

  for (const AnchorErrorInfo &Info : anchorErrorInfos())
    if (Info.Code == Narrow) {
      Result.Known = &Info;
      break;
    }

  if (Result.Range->ID == AnchorErrorRange::Custom)
    Result.CustomOrdinal = Narrow - anchorCustomErrorOffset();
  return Result;
}

llvm::Error validateAnchorErrorTable() {
  const auto Fail = [](llvm::Twine Message) {
    return llvm::make_error<llvm::StringError>(
        ("sbf: anchor errors: " + Message).str(),
        llvm::inconvertibleErrorCode());
  };

  uint32_t Previous = 0;
  for (const AnchorErrorRangeInfo &Range : anchorErrorRangeInfos()) {
    // classifyAnchorError picks the last band a code reaches, which only names
    // the right one while the bands ascend.
    if (Range.First <= Previous && Range.ID != AnchorErrorRange::Instruction)
      return Fail("band '" + Range.Name + "' starts at " +
                  llvm::Twine(Range.First) + ", which does not follow " +
                  llvm::Twine(Previous));
    Previous = Range.First;
  }

  llvm::DenseSet<uint32_t> Codes;
  for (const AnchorErrorInfo &Info : anchorErrorInfos()) {
    if (!Codes.insert(Info.Code).second)
      return Fail("code " + llvm::Twine(Info.Code) + " is listed twice");
    if (Info.Code >= anchorCustomErrorOffset())
      return Fail("'" + Info.Name + "' claims code " + llvm::Twine(Info.Code) +
                  ", which belongs to a program's own enumeration");
    const AnchorErrorClassification Classified = classifyAnchorError(Info.Code);
    if (Classified.Known != &Info)
      return Fail("'" + Info.Name + "' is not what code " +
                  llvm::Twine(Info.Code) + " classifies to");
  }
  return llvm::Error::success();
}

const AnchorIdlItem *AnchorIdl::find(const AnchorDiscriminator &Value) const {
  for (const AnchorIdlItem &Item : Items)
    if (Item.Discriminator == Value)
      return &Item;
  return nullptr;
}

llvm::Expected<AnchorIdl> parseAnchorIdl(llvm::StringRef Text) {
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Text);
  if (!Parsed)
    return Parsed.takeError();
  const llvm::json::Object *Root = Parsed->getAsObject();
  if (!Root)
    return idlError("the document must be a JSON object");

  AnchorIdl Idl;

  // The modern schema nests name and version under `metadata`; the legacy
  // schema keeps them at the top level.
  const llvm::json::Object *Metadata = Root->getObject(kIdlMetadataField);
  const llvm::json::Object &Names = Metadata ? *Metadata : *Root;
  Idl.Name = Names.getString(kIdlNameField).value_or("").str();
  Idl.Version = Names.getString(kIdlVersionField).value_or("").str();

  if (std::optional<llvm::StringRef> Address =
          Root->getString(kIdlAddressField)) {
    llvm::Expected<Pubkey> Key = parsePubkey(*Address);
    if (!Key)
      return Key.takeError();
    Idl.Address = *Key;
  }

  if (llvm::Error E = readSection(*Root, kIdlInstructionsSection,
                                  AnchorNamespace::Instruction, Idl))
    return std::move(E);
  if (llvm::Error E = readSection(*Root, kIdlAccountsSection,
                                  AnchorNamespace::Account, Idl))
    return std::move(E);
  if (llvm::Error E =
          readSection(*Root, kIdlEventsSection, AnchorNamespace::Event, Idl))
    return std::move(E);

  return Idl;
}

} // namespace neverd::sbf
