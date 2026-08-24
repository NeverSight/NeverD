//===- EVMABI.cpp - Recovered Ethereum ABI types and signatures ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/runtime/EVMABI.h"

#include "EVMKeccak.h"

#include "neverd/evm/bytecode/EVMOpcodes.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace neverd::evm {
namespace {

constexpr unsigned kProtocolMaximumEventTopics = logTopicCount(Opcode::LOG4);
constexpr unsigned kMinimumABISizedByteWidth = 1;
constexpr unsigned kMinimumABIFixedPointDecimals = 1;
constexpr unsigned kMaximumABIFixedPointDecimals = 80;

constexpr llvm::StringLiteral kCanonicalABIFunctionType = "function";
constexpr std::array kCanonicalABIFixedPointPrefixes = {
    llvm::StringLiteral("fixed"), llvm::StringLiteral("ufixed")};
constexpr llvm::StringLiteral kInvalidABIWidthUnit = "invalid ABI width unit";
constexpr llvm::StringLiteral kNonCanonicalABISignature =
    "is not a canonical ABI signature";
constexpr llvm::StringLiteral kNonCanonicalABIReturnList =
    "declares a non-canonical ABI return list";

llvm::Error signatureError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("evm: known signature: " + Message).str(),
      llvm::inconvertibleErrorCode());
}

bool validSignatureKind(SignatureKind Kind) {
  return static_cast<size_t>(Kind) < kSignatureKindCount;
}

bool validKnownStandard(KnownStandard Standard) {
  return static_cast<size_t>(Standard) < kKnownStandardCount;
}

bool chargeTableText(size_t &Used, size_t Amount) {
  if (Used > kMaxKnownSignatureTableTextBytes ||
      Amount > kMaxKnownSignatureTableTextBytes - Used)
    return false;
  Used += Amount;
  return true;
}

bool isCanonicalUnsignedDecimal(llvm::StringRef Text) {
  return !Text.empty() && (Text.size() == 1 || Text.front() != '0') &&
         llvm::all_of(Text, llvm::isDigit);
}

bool hasCanonicalBoundedValue(llvm::StringRef Text, unsigned Minimum,
                              unsigned Maximum, unsigned Multiple) {
  if (!isCanonicalUnsignedDecimal(Text))
    return false;
  unsigned Value = 0;
  if (Text.getAsInteger(10, Value))
    return false;
  return Value >= Minimum && Value <= Maximum && Value % Multiple == 0;
}

bool isCanonicalABITypeClassSpelling(llvm::StringRef Type,
                                     const ABITypeClassInfo &Info) {
  if (!Type.consume_front(Info.Spelling))
    return false;
  switch (Info.Unit) {
  case ABIWidthUnit::None:
    return Type.empty();
  case ABIWidthUnit::Bits:
    return hasCanonicalBoundedValue(Type, kBitsPerByte, kWordBits,
                                    kBitsPerByte);
  case ABIWidthUnit::Bytes:
    return hasCanonicalBoundedValue(Type, kMinimumABISizedByteWidth, kWordBytes,
                                    1);
  }
  llvm_unreachable(kInvalidABIWidthUnit.data());
}

bool isCanonicalFixedPointABIType(llvm::StringRef Type) {
  for (llvm::StringRef Prefix : kCanonicalABIFixedPointPrefixes) {
    llvm::StringRef Suffix = Type;
    if (!Suffix.consume_front(Prefix))
      continue;
    const auto [Width, Decimals] = Suffix.split('x');
    return !Decimals.contains('x') &&
           hasCanonicalBoundedValue(Width, kBitsPerByte, kWordBits,
                                    kBitsPerByte) &&
           hasCanonicalBoundedValue(Decimals, kMinimumABIFixedPointDecimals,
                                    kMaximumABIFixedPointDecimals, 1);
  }
  return false;
}

bool isCanonicalElementaryABIType(llvm::StringRef Type) {
  if (Type == kCanonicalABIFunctionType)
    return true;
  if (llvm::any_of(abiTypeClassInfos(), [&](const ABITypeClassInfo &Info) {
        return isCanonicalABITypeClassSpelling(Type, Info);
      }))
    return true;
  return isCanonicalFixedPointABIType(Type);
}

/// Recursive-descent parser for the canonical ABI type grammar. Signatures and
/// return lists deliberately share it: both describe the same tuple elements,
/// and accepting a spelling in one context but rejecting it in the other would
/// make the declarative dictionary internally contradictory.
class CanonicalABITypeParser {
public:
  explicit CanonicalABITypeParser(llvm::StringRef Text) : Text(Text) {}

  bool parseParenthesizedList(
      llvm::SmallVectorImpl<llvm::StringRef> *Members = nullptr) {
    return consume('(') && parseDelimitedList(')', 0, Members) && atEnd();
  }

  bool parseList(llvm::SmallVectorImpl<llvm::StringRef> *Members = nullptr) {
    if (atEnd())
      return true;
    while (true) {
      const size_t Start = Cursor;
      if (!parseType(0))
        return false;
      if (Members)
        Members->push_back(Text.slice(Start, Cursor));
      if (atEnd())
        return true;
      if (!consume(',') || atEnd())
        return false;
    }
  }

private:
  bool atEnd() const { return Cursor == Text.size(); }

  bool consume(char Expected) {
    if (atEnd() || Text[Cursor] != Expected)
      return false;
    ++Cursor;
    return true;
  }

  bool charge(size_t &Used, size_t Limit) {
    if (Used >= Limit)
      return false;
    ++Used;
    return true;
  }

  bool parseDelimitedList(
      char Close, size_t Depth,
      llvm::SmallVectorImpl<llvm::StringRef> *Members = nullptr) {
    if (consume(Close))
      return true;
    while (true) {
      const size_t Start = Cursor;
      if (!parseType(Depth))
        return false;
      if (Members)
        Members->push_back(Text.slice(Start, Cursor));
      if (consume(Close))
        return true;
      if (!consume(','))
        return false;
    }
  }

  bool parseType(size_t Depth) {
    if (!charge(TypeNodes, kMaxABITypeNodes))
      return false;
    if (consume('(')) {
      if (Depth >= kMaxABITypeNestingDepth ||
          !parseDelimitedList(')', Depth + 1, nullptr))
        return false;
    } else {
      const size_t Start = Cursor;
      while (!atEnd() && llvm::isAlnum(Text[Cursor]))
        ++Cursor;
      if (Start == Cursor ||
          !isCanonicalElementaryABIType(Text.slice(Start, Cursor)))
        return false;
    }

    while (consume('[')) {
      if (!charge(ArrayDimensions, kMaxABIArrayDimensions))
        return false;
      const size_t DimensionStart = Cursor;
      while (!atEnd() && llvm::isDigit(Text[Cursor]))
        ++Cursor;
      const llvm::StringRef Dimension = Text.slice(DimensionStart, Cursor);
      if ((!Dimension.empty() && !isCanonicalUnsignedDecimal(Dimension)) ||
          !consume(']'))
        return false;
    }
    return true;
  }

  llvm::StringRef Text;
  size_t Cursor = 0;
  size_t TypeNodes = 0;
  size_t ArrayDimensions = 0;
};

bool isCanonicalSignatureName(llvm::StringRef Name) {
  if (Name.empty() || (!llvm::isAlpha(Name.front()) && Name.front() != '_' &&
                       Name.front() != '$'))
    return false;
  return llvm::all_of(Name.drop_front(), [](char C) {
    return llvm::isAlnum(C) || C == '_' || C == '$';
  });
}

bool parseCanonicalABISignature(
    llvm::StringRef Signature,
    llvm::SmallVectorImpl<llvm::StringRef> *Arguments = nullptr) {
  const size_t Open = Signature.find('(');
  if (Open == llvm::StringRef::npos ||
      !isCanonicalSignatureName(Signature.take_front(Open)))
    return false;
  CanonicalABITypeParser Parser(Signature.drop_front(Open));
  return Parser.parseParenthesizedList(Arguments);
}

bool isCanonicalABITypeList(llvm::StringRef Types) {
  CanonicalABITypeParser Parser(Types);
  return Parser.parseList();
}

} // namespace

//===----------------------------------------------------------------------===//
// Value types
//===----------------------------------------------------------------------===//

llvm::ArrayRef<ABITypeClassInfo> abiTypeClassInfos() {
  static const std::array Table = {
#define EVM_ABI_TYPE(ID, SPELLING, WIDTH_UNIT)                                 \
  ABITypeClassInfo{ABITypeClass::ID, SPELLING, ABIWidthUnit::WIDTH_UNIT},
#include "neverd/evm/runtime/EVMABITypes.def"
  };
  return Table;
}

const ABITypeClassInfo &getABITypeClassInfo(ABITypeClass ID) {
  return abiTypeClassInfos()[static_cast<size_t>(ID)];
}

std::string ABIType::spelling() const {
  const ABITypeClassInfo &Info = getABITypeClassInfo(Class);
  switch (Info.Unit) {
  case ABIWidthUnit::None:
    return Info.Spelling.str();
  case ABIWidthUnit::Bits:
    return Info.Spelling.str() + std::to_string(ByteWidth * kBitsPerByte);
  case ABIWidthUnit::Bytes:
    return Info.Spelling.str() + std::to_string(ByteWidth);
  }
  llvm_unreachable(kInvalidABIWidthUnit.data());
}

llvm::ArrayRef<ABITypeSourceInfo> abiTypeSourceInfos() {
  static const std::array Table = {
#define EVM_ABI_TYPE_SOURCE(ID, NAME, SUMMARY)                                 \
  ABITypeSourceInfo{ABITypeSource::ID, NAME, SUMMARY},
#include "neverd/evm/runtime/EVMABITypes.def"
  };
  return Table;
}

llvm::StringRef abiTypeSourceName(ABITypeSource Source) {
  return abiTypeSourceInfos()[static_cast<size_t>(Source)].Name;
}

//===----------------------------------------------------------------------===//
// Inferring a type from observed use
//===----------------------------------------------------------------------===//

llvm::ArrayRef<ABIEvidenceInfo> abiEvidenceInfos() {
  static const std::array Table = {
#define EVM_ABI_EVIDENCE(ID, NAME, SUMMARY)                                    \
  ABIEvidenceInfo{ABIEvidence::ID, NAME, SUMMARY},
#include "neverd/evm/runtime/EVMABITypes.def"
  };
  return Table;
}

llvm::StringRef abiEvidenceName(ABIEvidence Evidence) {
  return abiEvidenceInfos()[static_cast<size_t>(Evidence)].Name;
}

std::optional<unsigned> lowByteMaskWidth(const llvm::APInt &Mask) {
  if (Mask.getBitWidth() != kWordBits || Mask.isZero() || Mask.isAllOnes())
    return std::nullopt;
  const unsigned Bits = Mask.getActiveBits();
  if (Bits % kBitsPerByte != 0 ||
      Mask != llvm::APInt::getLowBitsSet(kWordBits, Bits))
    return std::nullopt;
  return Bits / kBitsPerByte;
}

std::optional<unsigned> highByteMaskWidth(const llvm::APInt &Mask) {
  if (Mask.getBitWidth() != kWordBits || Mask.isZero() || Mask.isAllOnes())
    return std::nullopt;
  const unsigned Zeros = Mask.countr_zero();
  if (Zeros % kBitsPerByte != 0)
    return std::nullopt;
  const unsigned Bits = kWordBits - Zeros;
  if (Mask != llvm::APInt::getHighBitsSet(kWordBits, Bits))
    return std::nullopt;
  return Bits / kBitsPerByte;
}

void ABIConstraint::narrowTo(unsigned Bytes) {
  if (Bytes == 0 || Bytes > kWordBytes)
    return;
  Width = std::min(Width, Bytes);
}

ABIType ABIConstraint::resolve() const {
  // These tests are a precedence rather than a sequence of independent
  // questions: one argument can be shifted and then added, and it is the
  // arithmetic that names it. Signedness comes first because no unsigned type
  // needs sign extension, and the byte-string tests come last because every
  // numeric type is also a legal operand of a shift.
  if (has(ABIEvidence::SignExtended) || has(ABIEvidence::SignedCompare))
    return ABIType::sized(ABITypeClass::Signed, Width);
  if (has(ABIEvidence::HighByteMask))
    return ABIType::sized(ABITypeClass::FixedBytes, Width);
  if (has(ABIEvidence::CallTarget) || Width == kAddressBytes)
    return ABIType::plain(ABITypeClass::Address);
  if (has(ABIEvidence::BooleanTest) && Width == 1)
    return ABIType::plain(ABITypeClass::Bool);
  if (has(ABIEvidence::Arithmetic))
    return ABIType::sized(ABITypeClass::Unsigned, Width);
  if (has(ABIEvidence::Bitwise) || has(ABIEvidence::BitShift))
    return ABIType::sized(ABITypeClass::FixedBytes, Width);
  return ABIType::sized(ABITypeClass::Unsigned, Width);
}

//===----------------------------------------------------------------------===//
// The signature dictionary
//===----------------------------------------------------------------------===//

llvm::ArrayRef<SignatureKindInfo> signatureKindInfos() {
  static const std::array Table = {
#define EVM_SIGNATURE_KIND(ID, NAME, SUMMARY)                                  \
  SignatureKindInfo{SignatureKind::ID, NAME, SUMMARY},
#include "neverd/evm/runtime/EVMKnownSignatures.def"
  };
  return Table;
}

llvm::StringRef signatureKindName(SignatureKind Kind) {
  return signatureKindInfos()[static_cast<size_t>(Kind)].Name;
}

llvm::ArrayRef<KnownStandardInfo> knownStandardInfos() {
  static const std::array Table = {
#define EVM_KNOWN_STANDARD(ID, NAME, SUMMARY, MINIMUM_INDEPENDENT_SELECTORS)   \
  KnownStandardInfo{                                                           \
      KnownStandard::ID, NAME, SUMMARY,                                        \
      StandardSelectorEvidenceCount{MINIMUM_INDEPENDENT_SELECTORS}},
#include "neverd/evm/runtime/EVMKnownSignatures.def"
  };
  return Table;
}

const KnownStandardInfo &getKnownStandardInfo(KnownStandard ID) {
  return knownStandardInfos()[static_cast<size_t>(ID)];
}

llvm::StringRef KnownSignatureInfo::name() const {
  return signatureName(Signature);
}

llvm::ArrayRef<KnownSignatureInfo> knownSignatureInfos() {
  // The table is hashed once. Function variants intentionally collapse to one
  // canonical spelling here: standards and return lists are not selector
  // identity, and remain in knownFunctionVariantInfos().
  static const std::vector<KnownSignatureInfo> Table = [] {
    std::vector<KnownSignatureInfo> Entries;
    const auto AddFunction = [&](llvm::StringLiteral Signature) {
      if (llvm::any_of(Entries, [&](const KnownSignatureInfo &Info) {
            return Info.Kind == SignatureKind::Function &&
                   Info.Signature == Signature;
          }))
        return;
      Entries.push_back(KnownSignatureInfo{
          SignatureKind::Function, Signature, std::nullopt, std::nullopt,
          keccak256Selector(Signature), keccak256Topic(Signature)});
    };
    const auto AddEvent = [&](KnownStandard Standard,
                              llvm::StringLiteral Signature,
                              EventIndexedArgumentCount IndexedArguments) {
      Entries.push_back(KnownSignatureInfo{
          SignatureKind::Event, Signature,
          KnownEventMetadata{Standard, IndexedArguments}, std::nullopt,
          keccak256Selector(Signature), keccak256Topic(Signature)});
    };
    const auto AddError = [&](KnownStandard Standard,
                              llvm::StringLiteral Signature) {
      Entries.push_back(KnownSignatureInfo{
          SignatureKind::Error, Signature, std::nullopt,
          KnownErrorMetadata{Standard}, keccak256Selector(Signature),
          keccak256Topic(Signature)});
    };
#define EVM_KNOWN_FUNCTION(SIGNATURE, RETURNS, STANDARD) AddFunction(SIGNATURE);
#define EVM_NONINDEPENDENT_FUNCTION(SIGNATURE, RETURNS, STANDARD)              \
  AddFunction(SIGNATURE);
#define EVM_KNOWN_EVENT(SIGNATURE, INDEXED_ARGUMENTS, STANDARD)                \
  AddEvent(KnownStandard::STANDARD, SIGNATURE,                                 \
           EventIndexedArgumentCount{INDEXED_ARGUMENTS});
#define EVM_KNOWN_ERROR(SIGNATURE, STANDARD)                                   \
  AddError(KnownStandard::STANDARD, SIGNATURE);
#define EVM_LANGUAGE_REVERT(ID, SIGNATURE)                                     \
  AddError(KnownStandard::Solidity, SIGNATURE);
#include "neverd/evm/runtime/EVMKnownSignatures.def"
    return Entries;
  }();
  return Table;
}

llvm::ArrayRef<KnownFunctionVariantInfo> knownFunctionVariantInfos() {
  static const std::vector<KnownFunctionVariantInfo> Table = [] {
    std::vector<KnownFunctionVariantInfo> Entries;
    const auto Add = [&](llvm::StringLiteral Signature,
                         llvm::StringLiteral Returns, KnownStandard Standard,
                         FunctionSelectorEvidence Evidence) {
      const auto It = llvm::find_if(
          knownSignatureInfos(), [&](const KnownSignatureInfo &Info) {
            return Info.Kind == SignatureKind::Function &&
                   Info.Signature == Signature;
          });
      assert(It != knownSignatureInfos().end() &&
             "every function variant has a canonical signature");
      Entries.push_back(
          KnownFunctionVariantInfo{&*It, Standard, Returns, Evidence});
    };
#define EVM_KNOWN_FUNCTION(SIGNATURE, RETURNS, STANDARD)                       \
  Add(SIGNATURE, RETURNS, KnownStandard::STANDARD,                             \
      FunctionSelectorEvidence::Independent);
#define EVM_NONINDEPENDENT_FUNCTION(SIGNATURE, RETURNS, STANDARD)              \
  Add(SIGNATURE, RETURNS, KnownStandard::STANDARD,                             \
      FunctionSelectorEvidence::NonIndependent);
#include "neverd/evm/runtime/EVMKnownSignatures.def"
    return Entries;
  }();
  return Table;
}

llvm::SmallVector<const KnownFunctionVariantInfo *, 2>
knownFunctionVariants(const KnownSignatureInfo &Function) {
  llvm::SmallVector<const KnownFunctionVariantInfo *, 2> Variants;
  if (Function.Kind != SignatureKind::Function)
    return Variants;
  for (const KnownFunctionVariantInfo &Variant : knownFunctionVariantInfos())
    if (Variant.Function == &Function)
      Variants.push_back(&Variant);
  return Variants;
}

namespace {

/// The entries of one kind, indexed by selector. Null is an ambiguity sentinel:
/// lookup stays fail-closed even before validation reports a malformed table.
const llvm::DenseMap<uint32_t, const KnownSignatureInfo *> &
selectorIndex(SignatureKind Kind) {
  static const auto Build = [](SignatureKind Wanted) {
    llvm::DenseMap<uint32_t, const KnownSignatureInfo *> Index;
    for (const KnownSignatureInfo &Info : knownSignatureInfos()) {
      if (Info.Kind != Wanted)
        continue;
      const auto [It, Inserted] = Index.try_emplace(Info.Selector, &Info);
      if (!Inserted)
        It->second = nullptr;
    }
    return Index;
  };
  static const auto Functions = Build(SignatureKind::Function);
  static const auto Errors = Build(SignatureKind::Error);
  return Kind == SignatureKind::Function ? Functions : Errors;
}

const KnownSignatureInfo *findBySelector(SignatureKind Kind,
                                         uint32_t Selector) {
  const auto &Index = selectorIndex(Kind);
  const auto It = Index.find(Selector);
  return It == Index.end() ? nullptr : It->second;
}

} // namespace

const KnownSignatureInfo *
findUniqueKnownSignature(llvm::ArrayRef<KnownSignatureInfo> Signatures,
                         SignatureKind Kind, uint32_t Selector) {
  const KnownSignatureInfo *Match = nullptr;
  for (const KnownSignatureInfo &Info : Signatures) {
    if (Info.Kind != Kind || Info.Selector != Selector)
      continue;
    if (Match)
      return nullptr;
    Match = &Info;
  }
  return Match;
}

const KnownSignatureInfo *findKnownFunction(uint32_t Selector) {
  return findBySelector(SignatureKind::Function, Selector);
}

const KnownSignatureInfo *findKnownError(uint32_t Selector) {
  return findBySelector(SignatureKind::Error, Selector);
}

const KnownSignatureInfo *findKnownEvent(const llvm::APInt &Topic,
                                         unsigned TotalTopics) {
  if (Topic.getBitWidth() != kWordBits)
    return nullptr;
  static const std::vector<const KnownSignatureInfo *> ByTopic = [] {
    std::vector<const KnownSignatureInfo *> Sorted;
    for (const KnownSignatureInfo &Info : knownSignatureInfos())
      if (Info.Kind == SignatureKind::Event)
        Sorted.push_back(&Info);
    llvm::sort(Sorted, [](const KnownSignatureInfo *LHS,
                          const KnownSignatureInfo *RHS) {
      if (LHS->Topic != RHS->Topic)
        return LHS->Topic.ult(RHS->Topic);
      const unsigned LHSTopics = LHS->Event->totalTopicCount();
      const unsigned RHSTopics = RHS->Event->totalTopicCount();
      if (LHSTopics != RHSTopics)
        return LHSTopics < RHSTopics;
      if (LHS->Signature != RHS->Signature)
        return LHS->Signature.compare(RHS->Signature) < 0;
      return static_cast<uint8_t>(LHS->Event->Standard) <
             static_cast<uint8_t>(RHS->Event->Standard);
    });
    return Sorted;
  }();

  const auto It = std::lower_bound(
      ByTopic.begin(), ByTopic.end(), Topic,
      [](const KnownSignatureInfo *Info, const llvm::APInt &Value) {
        return Info->Topic.ult(Value);
      });
  const KnownSignatureInfo *Match = nullptr;
  for (auto Candidate = It;
       Candidate != ByTopic.end() && (*Candidate)->Topic == Topic;
       ++Candidate) {
    if ((*Candidate)->Event->totalTopicCount() != TotalTopics)
      continue;
    if (Match)
      return nullptr;
    Match = *Candidate;
  }
  return Match;
}

const KnownSignatureInfo &getLanguageRevertInfo(LanguageRevert Which) {
  static const std::array Selectors = {
#define EVM_LANGUAGE_REVERT(ID, SIGNATURE) keccak256Selector(SIGNATURE),
#include "neverd/evm/runtime/EVMKnownSignatures.def"
  };
  const KnownSignatureInfo *Info =
      findKnownError(Selectors[static_cast<size_t>(Which)]);
  assert(Info && "the language's own payloads are always in the error table");
  return *Info;
}

llvm::ArrayRef<PanicCodeInfo> panicCodeInfos() {
  static const std::array Table = {
#define EVM_PANIC_CODE(ID, CODE, NAME, SUMMARY)                                \
  PanicCodeInfo{PanicCode::ID, NAME, SUMMARY},
#include "neverd/evm/runtime/EVMKnownSignatures.def"
  };
  return Table;
}

const PanicCodeInfo *findPanicCode(uint64_t Code) {
  for (const PanicCodeInfo &Info : panicCodeInfos())
    if (static_cast<uint64_t>(Info.ID) == Code)
      return &Info;
  return nullptr;
}

llvm::StringRef signatureName(llvm::StringRef Signature) {
  return Signature.take_while([](char C) { return C != '('; });
}

llvm::SmallVector<llvm::StringRef, 8> splitTypeList(llvm::StringRef List) {
  llvm::SmallVector<llvm::StringRef, 8> Members;
  CanonicalABITypeParser Parser(List);
  if (!Parser.parseList(&Members))
    Members.clear();
  return Members;
}

llvm::SmallVector<llvm::StringRef, 8>
signatureArgumentTypes(llvm::StringRef Signature) {
  llvm::SmallVector<llvm::StringRef, 8> Arguments;
  if (!parseCanonicalABISignature(Signature, &Arguments))
    Arguments.clear();
  return Arguments;
}

llvm::Error validateKnownSignatureTables() {
  if (signatureKindInfos().size() != kSignatureKindCount)
    return signatureError("signature-kind table size does not match its enum");
  for (size_t Index = 0; Index < signatureKindInfos().size(); ++Index)
    if (static_cast<size_t>(signatureKindInfos()[Index].ID) != Index)
      return signatureError("signature kind is not at its enum table index");
  if (knownStandardInfos().size() != kKnownStandardCount)
    return signatureError("standard table size does not match its enum");
  for (size_t Index = 0; Index < knownStandardInfos().size(); ++Index) {
    const KnownStandardInfo &Info = knownStandardInfos()[Index];
    if (static_cast<size_t>(Info.ID) != Index)
      return signatureError("'" + Info.Name +
                            "' is not at its enum table index");
    if (standardSelectorEvidenceCount(Info.MinimumIndependentSelectors) <
        kMinimumIndependentSelectorsForStandard)
      return signatureError("'" + Info.Name +
                            "' trusts fewer than two function selectors");
  }

  return validateKnownSignatureTables(knownSignatureInfos(),
                                      knownFunctionVariantInfos());
}

llvm::Error validateKnownSignatureTables(
    llvm::ArrayRef<KnownSignatureInfo> Signatures,
    llvm::ArrayRef<KnownFunctionVariantInfo> FunctionVariants) {
  if (Signatures.size() > kMaxKnownSignatureTableEntries)
    return signatureError("signature table exceeds its entry limit");
  if (FunctionVariants.size() > kMaxKnownFunctionVariantTableEntries)
    return signatureError("function variant table exceeds its entry limit");
  size_t TableTextBytes = 0;
  for (const KnownSignatureInfo &Info : Signatures)
    if (!chargeTableText(TableTextBytes, Info.Signature.size()))
      return signatureError("signature table text exceeds its byte limit");
  for (const KnownFunctionVariantInfo &Variant : FunctionVariants)
    if (!chargeTableText(TableTextBytes, Variant.Returns.size()))
      return signatureError("signature table text exceeds its byte limit");

  std::set<const KnownSignatureInfo *> CanonicalFunctions;
  std::set<std::tuple<llvm::StringRef, KnownStandard, unsigned>> EventVariants;
  std::set<std::pair<llvm::StringRef, KnownStandard>> ErrorVariants;
  std::map<std::pair<SignatureKind, uint32_t>, const KnownSignatureInfo *>
      SelectorOwners;

  for (const KnownSignatureInfo &Info : Signatures) {
    if (!validSignatureKind(Info.Kind))
      return signatureError("'" + Info.Signature +
                            "' names an invalid signature kind");
    switch (Info.Kind) {
    case SignatureKind::Function:
      if (Info.Event || Info.Error)
        return signatureError("'" + Info.Signature +
                              "' function carries non-function metadata");
      break;
    case SignatureKind::Event:
      if (!Info.Event || Info.Error)
        return signatureError("'" + Info.Signature +
                              "' event has invalid event metadata");
      if (!validKnownStandard(Info.Event->Standard))
        return signatureError("'" + Info.Signature +
                              "' event names an invalid standard");
      break;
    case SignatureKind::Error:
      if (Info.Event || !Info.Error)
        return signatureError("'" + Info.Signature +
                              "' error has invalid error metadata");
      if (!validKnownStandard(Info.Error->Standard))
        return signatureError("'" + Info.Signature +
                              "' error names an invalid standard");
      break;
    }

    llvm::SmallVector<llvm::StringRef, 8> Arguments;
    if (!parseCanonicalABISignature(Info.Signature, &Arguments))
      return signatureError("'" + Info.Signature + "' " +
                            kNonCanonicalABISignature);
    if (Info.Topic.getBitWidth() != kWordBits)
      return signatureError("'" + Info.Signature +
                            "' carries a non-word-sized topic");

    const uint32_t DerivedSelector = keccak256Selector(Info.Signature);
    const llvm::APInt DerivedTopic = keccak256Topic(Info.Signature);
    if (Info.Selector != DerivedSelector || Info.Topic != DerivedTopic)
      return signatureError("'" + Info.Signature +
                            "' carries metadata derived from different text");

    switch (Info.Kind) {
    case SignatureKind::Function:
      CanonicalFunctions.insert(&Info);
      break;
    case SignatureKind::Event: {
      const unsigned IndexedArguments =
          eventIndexedArgumentCount(Info.Event->IndexedArguments);
      if (IndexedArguments > Arguments.size())
        return signatureError("'" + Info.Signature +
                              "' indexes more arguments than it declares");
      if (Info.Event->totalTopicCount() > kProtocolMaximumEventTopics)
        return signatureError("'" + Info.Signature +
                              "' requires more topics than LOG4 permits");
      const auto Variant = std::make_tuple(Info.Signature, Info.Event->Standard,
                                           IndexedArguments);
      if (!EventVariants.insert(Variant).second)
        return signatureError("'" + Info.Signature +
                              "' repeats an event variant");
      break;
    }
    case SignatureKind::Error:
      if (!ErrorVariants
               .insert(std::make_pair(Info.Signature, Info.Error->Standard))
               .second)
        return signatureError("'" + Info.Signature +
                              "' repeats an error variant");
      break;
    }

    // Recovery indexes calls and revert payloads separately. A collision
    // within either domain is ambiguous, and lookup must return no candidate
    // regardless of the order of declarations.
    if (Info.Kind != SignatureKind::Event) {
      const auto [Owner, Inserted] =
          SelectorOwners.try_emplace({Info.Kind, Info.Selector}, &Info);
      if (!Inserted)
        return signatureError("'" + Info.Signature + "' is not the single " +
                              signatureKindName(Info.Kind) +
                              " its hash resolves to");
      (void)Owner;
    }
  }

  std::set<std::pair<const KnownSignatureInfo *, KnownStandard>> Memberships;
  std::map<const KnownSignatureInfo *, std::set<KnownStandard>> Owners;
  std::set<const KnownSignatureInfo *> IndependentlyMarkedFunctions;
  for (const KnownFunctionVariantInfo &Variant : FunctionVariants) {
    if (!Variant.Function || !CanonicalFunctions.contains(Variant.Function))
      return signatureError("function variant has no canonical signature");
    if (!validKnownStandard(Variant.Standard))
      return signatureError("'" + Variant.Function->Signature +
                            "' function variant names an invalid standard");
    if (Variant.Evidence != FunctionSelectorEvidence::NonIndependent &&
        Variant.Evidence != FunctionSelectorEvidence::Independent)
      return signatureError("'" + Variant.Function->Signature +
                            "' function variant has invalid evidence");
    if (!Memberships.insert({Variant.Function, Variant.Standard}).second)
      return signatureError("'" + Variant.Function->Signature +
                            "' repeats a function variant for '" +
                            getKnownStandardInfo(Variant.Standard).Name + "'");
    Owners[Variant.Function].insert(Variant.Standard);
    if (Variant.contributesIndependentSelectorEvidence())
      IndependentlyMarkedFunctions.insert(Variant.Function);
    if (!isCanonicalABITypeList(Variant.Returns))
      return signatureError("'" + Variant.Function->Signature + "' " +
                            kNonCanonicalABIReturnList);
  }

  for (const KnownSignatureInfo *Function : CanonicalFunctions) {
    const auto OwnerIt = Owners.find(Function);
    if (OwnerIt == Owners.end())
      return signatureError("'" + Function->Signature +
                            "' has no function variant");
    if (OwnerIt->second.size() >= 2 &&
        IndependentlyMarkedFunctions.contains(Function))
      return signatureError("'" + Function->Signature +
                            "' is shared but marked independent");
  }

  return llvm::Error::success();
}

} // namespace neverd::evm
