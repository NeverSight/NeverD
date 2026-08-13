//===- EVMABI.cpp - Recovered Ethereum ABI types and signatures ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/runtime/EVMABI.h"

#include "EVMKeccak.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <vector>

namespace neverd::evm {
namespace {

llvm::Error signatureError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("evm: known signature: " + Message).str(),
      llvm::inconvertibleErrorCode());
}

/// True when every parenthesis and bracket closes the one it opened, and the
/// text holds no character an ABI signature may not contain.
bool isWellFormedSignature(llvm::StringRef Signature) {
  const size_t Open = Signature.find('(');
  if (Open == llvm::StringRef::npos || Open == 0 || !Signature.ends_with(")"))
    return false;
  if (Signature.find_first_of(" \t\n") != llvm::StringRef::npos)
    return false;

  unsigned Depth = 0;
  for (size_t I = Open; I < Signature.size(); ++I) {
    const char C = Signature[I];
    if (C == '(' || C == '[')
      ++Depth;
    else if (C == ')' || C == ']') {
      if (Depth == 0)
        return false;
      --Depth;
    }
  }
  return Depth == 0;
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
  llvm_unreachable("invalid ABI width unit");
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
#define EVM_KNOWN_STANDARD(ID, NAME, SUMMARY)                                  \
  KnownStandardInfo{KnownStandard::ID, NAME, SUMMARY},
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
  // The table is hashed once. Deriving the selector here rather than writing it
  // down is what makes a match meaningful: the entry cannot claim a selector it
  // does not hash to.
  static const std::vector<KnownSignatureInfo> Table = [] {
    std::vector<KnownSignatureInfo> Entries;
    const auto Add = [&](SignatureKind Kind, KnownStandard Standard,
                         llvm::StringLiteral Signature,
                         llvm::StringLiteral Returns) {
      Entries.push_back(KnownSignatureInfo{Kind, Standard, Signature, Returns,
                                           keccak256Selector(Signature),
                                           keccak256Topic(Signature)});
    };
#define EVM_KNOWN_FUNCTION(SIGNATURE, RETURNS, STANDARD)                       \
  Add(SignatureKind::Function, KnownStandard::STANDARD, SIGNATURE, RETURNS);
#define EVM_KNOWN_EVENT(SIGNATURE, STANDARD)                                   \
  Add(SignatureKind::Event, KnownStandard::STANDARD, SIGNATURE, "");
#define EVM_KNOWN_ERROR(SIGNATURE, STANDARD)                                   \
  Add(SignatureKind::Error, KnownStandard::STANDARD, SIGNATURE, "");
#define EVM_LANGUAGE_REVERT(ID, SIGNATURE)                                     \
  Add(SignatureKind::Error, KnownStandard::Solidity, SIGNATURE, "");
#include "neverd/evm/runtime/EVMKnownSignatures.def"
    return Entries;
  }();
  return Table;
}

namespace {

/// The entries of one kind, indexed by selector. A table with two entries of
/// one kind sharing a selector would make the surviving name depend on table
/// order, which \c validateKnownSignatureTables rejects.
const llvm::DenseMap<uint64_t, const KnownSignatureInfo *> &
selectorIndex(SignatureKind Kind) {
  static const auto Build = [](SignatureKind Wanted) {
    llvm::DenseMap<uint64_t, const KnownSignatureInfo *> Index;
    for (const KnownSignatureInfo &Info : knownSignatureInfos())
      if (Info.Kind == Wanted)
        Index.try_emplace(Info.Selector, &Info);
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

const KnownSignatureInfo *findKnownFunction(uint32_t Selector) {
  return findBySelector(SignatureKind::Function, Selector);
}

const KnownSignatureInfo *findKnownError(uint32_t Selector) {
  return findBySelector(SignatureKind::Error, Selector);
}

const KnownSignatureInfo *findKnownEvent(const llvm::APInt &Topic) {
  if (Topic.getBitWidth() != kWordBits)
    return nullptr;
  static const std::vector<const KnownSignatureInfo *> ByTopic = [] {
    std::vector<const KnownSignatureInfo *> Sorted;
    for (const KnownSignatureInfo &Info : knownSignatureInfos())
      if (Info.Kind == SignatureKind::Event)
        Sorted.push_back(&Info);
    llvm::sort(Sorted, [](const KnownSignatureInfo *LHS,
                          const KnownSignatureInfo *RHS) {
      return LHS->Topic.ult(RHS->Topic);
    });
    return Sorted;
  }();

  const auto It = std::lower_bound(
      ByTopic.begin(), ByTopic.end(), Topic,
      [](const KnownSignatureInfo *Info, const llvm::APInt &Value) {
        return Info->Topic.ult(Value);
      });
  return It != ByTopic.end() && (*It)->Topic == Topic ? *It : nullptr;
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
  if (List.empty())
    return Members;

  unsigned Depth = 0;
  size_t Start = 0;
  for (size_t I = 0; I < List.size(); ++I) {
    const char C = List[I];
    if (C == '(' || C == '[')
      ++Depth;
    else if ((C == ')' || C == ']') && Depth != 0)
      --Depth;
    else if (C == ',' && Depth == 0) {
      Members.push_back(List.substr(Start, I - Start));
      Start = I + 1;
    }
  }
  Members.push_back(List.substr(Start));
  return Members;
}

llvm::SmallVector<llvm::StringRef, 8>
signatureArgumentTypes(llvm::StringRef Signature) {
  const size_t Open = Signature.find('(');
  if (Open == llvm::StringRef::npos || !Signature.ends_with(")"))
    return {};
  return splitTypeList(Signature.slice(Open + 1, Signature.size() - 1));
}

llvm::Error validateKnownSignatureTables() {
  llvm::DenseSet<llvm::StringRef> Spellings;

  for (const KnownSignatureInfo &Info : knownSignatureInfos()) {
    if (!isWellFormedSignature(Info.Signature))
      return signatureError("'" + Info.Signature +
                            "' is not a canonical ABI signature");

    // Two entries spelled the same way hash the same way, so the second could
    // never be reached and its standard would silently disappear.
    if (!Spellings.insert(Info.Signature).second)
      return signatureError("'" + Info.Signature + "' is listed twice");

    for (llvm::StringRef Member : splitTypeList(Info.Returns))
      if (Member.empty())
        return signatureError("'" + Info.Signature +
                              "' declares an empty return type");

    // A selector is the leading four bytes of the same digest the topic holds,
    // so the two must never be derived from different text.
    if (Info.Selector != Info.Topic.extractBitsAsZExtValue(
                             kSelectorBits, kWordBits - kSelectorBits))
      return signatureError("'" + Info.Signature +
                            "' has a selector its topic does not begin with");

    // Recovery indexes each kind on its own, because a call selector and a
    // revert-payload selector are read from different places and a collision
    // between the two is harmless. A collision inside one kind is not: which
    // of the two names survived would depend on table order.
    const KnownSignatureInfo *Resolved = nullptr;
    switch (Info.Kind) {
    case SignatureKind::Function:
      Resolved = findKnownFunction(Info.Selector);
      break;
    case SignatureKind::Error:
      Resolved = findKnownError(Info.Selector);
      break;
    case SignatureKind::Event:
      Resolved = findKnownEvent(Info.Topic);
      break;
    }
    if (Resolved != &Info)
      return signatureError("'" + Info.Signature + "' is not the single " +
                            signatureKindName(Info.Kind) +
                            " its hash resolves to");
  }
  return llvm::Error::success();
}

} // namespace neverd::evm
