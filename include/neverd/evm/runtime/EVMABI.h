//===- EVMABI.h - Recovered Ethereum ABI types and signatures -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the two ways NeverD attaches an ABI meaning to recovered bytecode: a
/// type lattice fed by how the code uses a value, and a dictionary of
/// signatures whose Keccak-256 prefix is the selector a call carries.
///
/// The two differ in kind, not only in confidence. A selector lookup supplies
/// a tabulated hash candidate; because four-byte prefixes collide, HighIR
/// function recovery keeps it only when recovered argument use does not
/// contradict its canonical ABI. Everything the lattice reports is an
/// inference from observed use, and says so.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_RUNTIME_EVMABI_H
#define NEVERD_EVM_RUNTIME_EVMABI_H

#include "neverd/evm/EVMConstants.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace neverd::evm {

/// Hostile-input bounds shared by signature, return-list, and public type-list
/// parsing. Keeping them in the public ABI vocabulary lets callers and tests
/// construct exact-boundary inputs without copying parser policy numbers.
#define EVM_ABI_PARSER_LIMIT(NAME, VALUE)                                      \
  inline constexpr std::size_t k##NAME = VALUE;
#include "neverd/evm/runtime/EVMABIParserLimits.def"

#define EVM_ABI_PARSER_LIMIT(NAME, VALUE) static_assert(k##NAME > 0);
#include "neverd/evm/runtime/EVMABIParserLimits.def"

/// Hostile-input bounds for validating caller-provided signature dictionaries.
#define EVM_ABI_TABLE_LIMIT(NAME, VALUE)                                       \
  inline constexpr std::size_t k##NAME = VALUE;
#include "neverd/evm/runtime/EVMABITableLimits.def"

#define EVM_ABI_TABLE_LIMIT(NAME, VALUE) static_assert(k##NAME > 0);
#include "neverd/evm/runtime/EVMABITableLimits.def"

//===----------------------------------------------------------------------===//
// Value types
//===----------------------------------------------------------------------===//

/// How a width joins a type class's spelling.
enum class ABIWidthUnit : uint8_t { None, Bits, Bytes };

enum class ABITypeClass : uint8_t {
#define EVM_ABI_TYPE(ID, SPELLING, WIDTH_UNIT) ID,
#include "neverd/evm/runtime/EVMABITypes.def"
};

struct ABITypeClassInfo {
  ABITypeClass ID;
  llvm::StringLiteral Spelling;
  ABIWidthUnit Unit;
};

llvm::ArrayRef<ABITypeClassInfo> abiTypeClassInfos();
const ABITypeClassInfo &getABITypeClassInfo(ABITypeClass ID);

/// One ABI value type: a class and, when the class is sized, how many bytes of
/// the machine word the value occupies.
class ABIType {
public:
  ABIType() = default;

  /// The type of a value nothing narrowed, which is the whole machine word.
  static ABIType word() { return ABIType(); }
  static ABIType plain(ABITypeClass Class) {
    return ABIType(Class, kWordBytes);
  }
  static ABIType sized(ABITypeClass Class, unsigned ByteWidth) {
    return ABIType(Class, ByteWidth);
  }

  [[nodiscard]] ABITypeClass classID() const { return Class; }
  [[nodiscard]] unsigned byteWidth() const { return ByteWidth; }

  /// The Solidity spelling, such as "address", "uint128", or "bytes4".
  [[nodiscard]] std::string spelling() const;

  friend bool operator==(const ABIType &, const ABIType &) = default;

private:
  ABIType(ABITypeClass Class, unsigned ByteWidth)
      : Class(Class), ByteWidth(ByteWidth) {}

  ABITypeClass Class = ABITypeClass::Unsigned;
  unsigned ByteWidth = kWordBytes;
};

/// How strongly a reported type is established.
enum class ABITypeSource : uint8_t {
#define EVM_ABI_TYPE_SOURCE(ID, NAME, SUMMARY) ID,
#include "neverd/evm/runtime/EVMABITypes.def"
};

struct ABITypeSourceInfo {
  ABITypeSource ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<ABITypeSourceInfo> abiTypeSourceInfos();
llvm::StringRef abiTypeSourceName(ABITypeSource Source);

//===----------------------------------------------------------------------===//
// Inferring a type from observed use
//===----------------------------------------------------------------------===//

/// One way the bytecode can say something about a value.
enum class ABIEvidence : uint8_t {
#define EVM_ABI_EVIDENCE(ID, NAME, SUMMARY) ID,
#include "neverd/evm/runtime/EVMABITypes.def"
};

struct ABIEvidenceInfo {
  ABIEvidence ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

inline constexpr size_t kABIEvidenceCount = 0
#define EVM_ABI_EVIDENCE(ID, NAME, SUMMARY) +1
#include "neverd/evm/runtime/EVMABITypes.def"
    ;

llvm::ArrayRef<ABIEvidenceInfo> abiEvidenceInfos();
llvm::StringRef abiEvidenceName(ABIEvidence Evidence);

/// The number of whole low-order bytes \p Mask keeps, when it keeps exactly a
/// run of them. This is the shape a compiler emits to clean a left-padded
/// argument, so recognizing it recovers the argument's declared width.
std::optional<unsigned> lowByteMaskWidth(const llvm::APInt &Mask);

/// The number of whole high-order bytes \p Mask keeps, which is the shape that
/// cleans a right-padded fixed-size byte string.
std::optional<unsigned> highByteMaskWidth(const llvm::APInt &Mask);

/// The observations one value accumulated, and the type they imply.
class ABIConstraint {
public:
  void observe(ABIEvidence Evidence) {
    Observed.set(static_cast<size_t>(Evidence));
  }
  [[nodiscard]] bool has(ABIEvidence Evidence) const {
    return Observed.test(static_cast<size_t>(Evidence));
  }

  /// Record a mask that keeps \p Bytes bytes of the value.
  ///
  /// A compiler's decoder masks a narrow argument on the word it just loaded,
  /// so the narrowest mask applied directly to that word is the declared width.
  /// A later, wider mask therefore never widens the recovered type.
  void narrowTo(unsigned Bytes);

  [[nodiscard]] unsigned byteWidth() const { return Width; }

  /// True when nothing was observed, which is the difference between a value
  /// reported as a word because it is one and a value reported as a word
  /// because the code never said.
  [[nodiscard]] bool empty() const {
    return Observed.none() && Width == kWordBytes;
  }

  /// The type the observations imply, and how it was reached.
  [[nodiscard]] ABIType resolve() const;
  [[nodiscard]] ABITypeSource source() const {
    return empty() ? ABITypeSource::Default : ABITypeSource::Dataflow;
  }

private:
  std::bitset<kABIEvidenceCount> Observed;
  unsigned Width = kWordBytes;
};

//===----------------------------------------------------------------------===//
// The signature dictionary
//===----------------------------------------------------------------------===//

enum class SignatureKind : uint8_t {
#define EVM_SIGNATURE_KIND(ID, NAME, SUMMARY) ID,
#include "neverd/evm/runtime/EVMKnownSignatures.def"
};

inline constexpr size_t kSignatureKindCount = 0
#define EVM_SIGNATURE_KIND(ID, NAME, SUMMARY) +1
#include "neverd/evm/runtime/EVMKnownSignatures.def"
    ;

struct SignatureKindInfo {
  SignatureKind ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<SignatureKindInfo> signatureKindInfos();
llvm::StringRef signatureKindName(SignatureKind Kind);

enum class KnownStandard : uint8_t {
#define EVM_KNOWN_STANDARD(ID, NAME, SUMMARY, MINIMUM_INDEPENDENT_SELECTORS) ID,
#include "neverd/evm/runtime/EVMKnownSignatures.def"
};

inline constexpr size_t kKnownStandardCount = 0
#define EVM_KNOWN_STANDARD(ID, NAME, SUMMARY, MINIMUM_INDEPENDENT_SELECTORS) +1
#include "neverd/evm/runtime/EVMKnownSignatures.def"
    ;

/// Strong count of distinct, ABI-compatible function selectors required to
/// recognize a standard without stronger event, storage, or proxy evidence.
enum class StandardSelectorEvidenceCount : uint8_t {};

[[nodiscard]] constexpr unsigned
standardSelectorEvidenceCount(StandardSelectorEvidenceCount Count) {
  return static_cast<unsigned>(Count);
}

/// One selector is only a four-byte hash prefix and cannot establish a whole
/// interface, regardless of how specific its tabulated spelling appears.
inline constexpr unsigned kMinimumIndependentSelectorsForStandard = 2;

struct KnownStandardInfo {
  KnownStandard ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
  /// Distinct compatible function selectors required when no full event topic
  /// or specification-fixed storage/proxy evidence identifies the standard.
  StandardSelectorEvidenceCount MinimumIndependentSelectors{};
};

llvm::ArrayRef<KnownStandardInfo> knownStandardInfos();
const KnownStandardInfo &getKnownStandardInfo(KnownStandard ID);

/// Strong count of event arguments carried in indexed topics.
enum class EventIndexedArgumentCount : uint8_t {};

[[nodiscard]] constexpr unsigned
eventIndexedArgumentCount(EventIndexedArgumentCount Count) {
  return static_cast<unsigned>(Count);
}

/// A non-anonymous event contributes one signature topic before its indexed
/// arguments. Every event in the known-signature table is identified through
/// that signature topic and is therefore non-anonymous.
inline constexpr unsigned kEventSignatureTopicCount = 1;

struct KnownEventMetadata {
  KnownStandard Standard = KnownStandard::Common;
  EventIndexedArgumentCount IndexedArguments{};

  [[nodiscard]] constexpr unsigned totalTopicCount() const {
    return kEventSignatureTopicCount +
           eventIndexedArgumentCount(IndexedArguments);
  }

  friend bool operator==(const KnownEventMetadata &,
                         const KnownEventMetadata &) = default;
};

/// The standard which declares one known custom error. Errors are kept in the
/// same canonical-signature table because their selector lookup has the same
/// collision rules as functions, but they never contribute interface evidence.
struct KnownErrorMetadata {
  KnownStandard Standard = KnownStandard::Common;
};

/// One tabulated signature together with what its text hashes to.
struct KnownSignatureInfo {
  SignatureKind Kind = SignatureKind::Function;
  /// The canonical spelling, such as "transfer(address,uint256)".
  llvm::StringLiteral Signature;
  /// Present only for events. The count is part of the declared ABI even
  /// though it does not participate in the event signature hash.
  std::optional<KnownEventMetadata> Event;
  /// Present only for custom errors. Unlike a function membership, this is
  /// naming metadata and is never interface evidence.
  std::optional<KnownErrorMetadata> Error;
  /// The leading four bytes of the digest, which a call places at the start of
  /// its calldata and a custom error at the start of its revert payload.
  uint32_t Selector = 0;
  /// The whole digest, which an event places in its first topic.
  llvm::APInt Topic;

  /// The part before the argument list.
  [[nodiscard]] llvm::StringRef name() const;
};

llvm::ArrayRef<KnownSignatureInfo> knownSignatureInfos();

/// Whether a function selector can independently support recognizing a
/// standard. Shared spellings are useful membership declarations, but cannot
/// distinguish any of the standards which declare them.
enum class FunctionSelectorEvidence : uint8_t { NonIndependent, Independent };

/// One standard's declaration of a canonical function signature.
///
/// Function spelling and selector identity deliberately live in
/// \c KnownSignatureInfo, while return types and standard membership live here:
/// the selector hashes only the name and argument list. In particular,
/// ERC-20 and ERC-721 give some identical selectors different return lists.
struct KnownFunctionVariantInfo {
  const KnownSignatureInfo *Function = nullptr;
  KnownStandard Standard = KnownStandard::Common;
  llvm::StringLiteral Returns;
  FunctionSelectorEvidence Evidence = FunctionSelectorEvidence::NonIndependent;

  [[nodiscard]] bool contributesIndependentSelectorEvidence() const {
    return Evidence == FunctionSelectorEvidence::Independent;
  }
};

/// Every per-standard function declaration in the declarative signature table.
llvm::ArrayRef<KnownFunctionVariantInfo> knownFunctionVariantInfos();

/// All declarations of \p Function. A shared canonical function can have
/// different return lists, so callers must inspect the whole result or first
/// establish a standard; selecting the first variant is never sound.
llvm::SmallVector<const KnownFunctionVariantInfo *, 2>
knownFunctionVariants(const KnownSignatureInfo &Function);

/// The tabulated function or custom-error candidate that hashes to the given
/// selector, or null when the dictionary has none. A four-byte match is not a
/// unique preimage; function recovery must reject a candidate contradicted by
/// recovered argument use, and errors remain names rather than interface
/// evidence.
const KnownSignatureInfo *findKnownFunction(uint32_t Selector);
const KnownSignatureInfo *findKnownError(uint32_t Selector);

/// The unique entry of \p Kind carrying \p Selector, or null when none or more
/// than one exists. The latter is the fail-closed rule used by the runtime
/// indexes and is exposed so table validation can be tested with synthetic
/// collision sets.
const KnownSignatureInfo *
findUniqueKnownSignature(llvm::ArrayRef<KnownSignatureInfo> Signatures,
                         SignatureKind Kind, uint32_t Selector);

/// The unique event variant whose signature topic and total LOG topic count
/// both match, or null when no variant matches or multiple standards declare
/// the same observable layout.
const KnownSignatureInfo *findKnownEvent(const llvm::APInt &Topic,
                                         unsigned TotalTopics);

//===----------------------------------------------------------------------===//
// The revert payloads the language reserves for itself
//===----------------------------------------------------------------------===//

enum class LanguageRevert : uint8_t {
#define EVM_LANGUAGE_REVERT(ID, SIGNATURE) ID,
#include "neverd/evm/runtime/EVMKnownSignatures.def"
};

/// The dictionary entry for a payload the language emits on its own behalf.
/// Recovery compares against these rather than against a spelling, so the
/// signature text stays in one file.
const KnownSignatureInfo &getLanguageRevertInfo(LanguageRevert Which);

enum class PanicCode : uint8_t {
#define EVM_PANIC_CODE(ID, CODE, NAME, SUMMARY) ID = (CODE),
#include "neverd/evm/runtime/EVMKnownSignatures.def"
};

struct PanicCodeInfo {
  PanicCode ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<PanicCodeInfo> panicCodeInfos();

/// What the given panic code means, or null when the language assigns it no
/// meaning. A program built against a newer compiler can panic with a code no
/// table lists, and reporting the number is then the honest answer.
const PanicCodeInfo *findPanicCode(uint64_t Code);

/// The part of a signature before its argument list.
llvm::StringRef signatureName(llvm::StringRef Signature);

/// The canonical ABI members of \p List, split only at top-level commas so a
/// tuple stays one member. Returns an empty vector when the list is empty or
/// malformed.
llvm::SmallVector<llvm::StringRef, 8> splitTypeList(llvm::StringRef List);

/// The argument types of canonical \p Signature, in declaration order, or an
/// empty vector when the signature is malformed.
llvm::SmallVector<llvm::StringRef, 8>
signatureArgumentTypes(llvm::StringRef Signature);

/// Report malformed canonical signatures or return-type lists, dangling or
/// duplicate function variants, incorrectly independent shared selectors, and
/// selector collisions.
llvm::Error validateKnownSignatureTables();

/// Validate an explicit pair of tables. This is the same invariant checker as
/// the process-wide dictionary and permits focused tests of rejected layouts.
llvm::Error validateKnownSignatureTables(
    llvm::ArrayRef<KnownSignatureInfo> Signatures,
    llvm::ArrayRef<KnownFunctionVariantInfo> FunctionVariants);

} // namespace neverd::evm

#endif // NEVERD_EVM_RUNTIME_EVMABI_H
