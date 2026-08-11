//===- ABI.h - Recovered Ethereum ABI types and signatures ----*- C++ -*-===//
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
/// The two differ in kind, not only in confidence. A selector match exhibits a
/// preimage, so it settles the name and the argument types. Everything the
/// lattice reports is an inference from observed use, and says so.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_ABI_H
#define NEVERD_EVM_ABI_H

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

//===----------------------------------------------------------------------===//
// Value types
//===----------------------------------------------------------------------===//

/// How a width joins a type class's spelling.
enum class ABIWidthUnit : uint8_t { None, Bits, Bytes };

enum class ABITypeClass : uint8_t {
#define EVM_ABI_TYPE(ID, SPELLING, WIDTH_UNIT) ID,
#include "neverd/evm/EVMABITypes.def"
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
#include "neverd/evm/EVMABITypes.def"
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
#include "neverd/evm/EVMABITypes.def"
};

struct ABIEvidenceInfo {
  ABIEvidence ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

inline constexpr size_t kABIEvidenceCount = 0
#define EVM_ABI_EVIDENCE(ID, NAME, SUMMARY) +1
#include "neverd/evm/EVMABITypes.def"
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
#include "neverd/evm/EVMKnownSignatures.def"
};

struct SignatureKindInfo {
  SignatureKind ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<SignatureKindInfo> signatureKindInfos();
llvm::StringRef signatureKindName(SignatureKind Kind);

enum class KnownStandard : uint8_t {
#define EVM_KNOWN_STANDARD(ID, NAME, SUMMARY) ID,
#include "neverd/evm/EVMKnownSignatures.def"
};

struct KnownStandardInfo {
  KnownStandard ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<KnownStandardInfo> knownStandardInfos();
const KnownStandardInfo &getKnownStandardInfo(KnownStandard ID);

/// One tabulated signature together with what its text hashes to.
struct KnownSignatureInfo {
  SignatureKind Kind = SignatureKind::Function;
  KnownStandard Standard = KnownStandard::Common;
  /// The canonical spelling, such as "transfer(address,uint256)".
  llvm::StringLiteral Signature;
  /// The return list the owning standard declares, empty when it declares
  /// none. A selector covers the arguments only, so this is a claim of the
  /// dictionary rather than something the match proves.
  llvm::StringLiteral Returns;
  /// The leading four bytes of the digest, which a call places at the start of
  /// its calldata and a custom error at the start of its revert payload.
  uint32_t Selector = 0;
  /// The whole digest, which an event places in its first topic.
  llvm::APInt Topic;

  /// The part before the argument list.
  [[nodiscard]] llvm::StringRef name() const;
};

llvm::ArrayRef<KnownSignatureInfo> knownSignatureInfos();

/// The tabulated function, custom error, or event that hashes to the given
/// value, or null when the dictionary does not know it.
const KnownSignatureInfo *findKnownFunction(uint32_t Selector);
const KnownSignatureInfo *findKnownError(uint32_t Selector);
const KnownSignatureInfo *findKnownEvent(const llvm::APInt &Topic);

//===----------------------------------------------------------------------===//
// The revert payloads the language reserves for itself
//===----------------------------------------------------------------------===//

enum class LanguageRevert : uint8_t {
#define EVM_LANGUAGE_REVERT(ID, SIGNATURE) ID,
#include "neverd/evm/EVMKnownSignatures.def"
};

/// The dictionary entry for a payload the language emits on its own behalf.
/// Recovery compares against these rather than against a spelling, so the
/// signature text stays in one file.
const KnownSignatureInfo &getLanguageRevertInfo(LanguageRevert Which);

enum class PanicCode : uint8_t {
#define EVM_PANIC_CODE(ID, CODE, NAME, SUMMARY) ID = (CODE),
#include "neverd/evm/EVMKnownSignatures.def"
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

/// The comma-separated members of \p List, split only at the commas that
/// separate top-level members so that a tuple stays one member.
llvm::SmallVector<llvm::StringRef, 8> splitTypeList(llvm::StringRef List);

/// The argument types of \p Signature, in declaration order.
llvm::SmallVector<llvm::StringRef, 8>
signatureArgumentTypes(llvm::StringRef Signature);

/// Report a signature the tables spell in a form the ABI does not accept, or
/// two entries of one kind that hash to the same value.
llvm::Error validateKnownSignatureTables();

} // namespace neverd::evm

#endif // NEVERD_EVM_ABI_H
