//===- TranslationArtifactVerifier.h - Audit generated objects -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fail-closed validation boundary for post-codegen translation
/// artifacts. Validation never links, rewrites, or executes the object.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONARTIFACTVERIFIER_H
#define NEVERD_TRANSLATE_TRANSLATIONARTIFACTVERIFIER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <cstdint>
#include <string>
#include <system_error>

namespace llvm {
class Triple;
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

/// Stable categories returned by the generated-artifact validation boundary.
enum class TranslationArtifactViolation : uint8_t {
  InvalidPolicy = 0,
  MalformedObject = 1,
  UnsupportedObjectFormat = 2,
  ObjectFormatMismatch = 3,
  HostArchitectureMismatch = 4,
  UnsupportedArtifactKind = 5,
  ExecutableWritableSection = 6,
  ExceptionUnwindMetadata = 7,
  StaticInitializer = 8,
  ThreadLocalStorage = 9,
  IndirectSymbol = 10,
  DynamicSymbolNotAllowed = 11,
  ExternalSymbolNotAllowed = 12,
  RelocationTargetNotAllowed = 13,
  RelocationTypeNotAllowed = 14,
  DynamicRelocation = 15,
  UnsupportedSection = 16,
  PreemptibleDefinition = 17,
  UnsupportedLoadCommand = 18,
  RequiredBlockMissing = 19,
  InvalidBlockDefinition = 20,
  UnexpectedBlockDefinition = 21,
};

/// A typed verifier failure. Callers inspect reason() rather than parsing the
/// diagnostic emitted by log(). ItemName and Detail are diagnostic only.
class TranslationArtifactVerificationError final
    : public llvm::ErrorInfo<TranslationArtifactVerificationError> {
public:
  static char ID;

  TranslationArtifactVerificationError(TranslationArtifactViolation Reason,
                                       std::string ItemName,
                                       std::string Detail = {});

  TranslationArtifactViolation reason() const { return Reason; }
  llvm::StringRef itemName() const { return ItemName; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  TranslationArtifactViolation Reason;
  std::string ItemName;
  std::string Detail;
};

/// Version-1 fail-closed policy for a generated translation object.
///
/// Array elements and their strings are borrowed for the duration of a
/// verifier call. RequiredBlockSymbols must contain at least one exact symbol
/// name; names are unique and disjoint from AllowedRuntimeSymbols. Every
/// required symbol must define a non-zero-size function in an executable
/// section with non-preemptible linkage. An externally resolvable code
/// definition not named by the manifest is rejected.
///
/// AllowedRuntimeSymbols identifies the only undefined runtime symbols that
/// generated code may reference. Such references must use a format- and
/// architecture-proven direct PC-relative call or branch. Absolute, data,
/// GOT/PLT, stub, and section-relative materializations are rejected.
struct TranslationArtifactPolicyV1 {
  explicit TranslationArtifactPolicyV1(
      llvm::ArrayRef<llvm::StringRef> RequiredBlocks,
      llvm::ArrayRef<llvm::StringRef> AllowedSymbols = {})
      : RequiredBlockSymbols(RequiredBlocks),
        AllowedRuntimeSymbols(AllowedSymbols) {}

  llvm::ArrayRef<llvm::StringRef> RequiredBlockSymbols;
  llvm::ArrayRef<llvm::StringRef> AllowedRuntimeSymbols;
};

/// Verify one object against the version-1 block manifest and runtime-helper
/// boundary. Unlike the compatibility overloads below, this path requires a
/// non-empty RequiredBlockSymbols manifest.
llvm::Error
verifyTranslationArtifact(llvm::MemoryBufferRef Artifact,
                          const llvm::Triple &ExpectedHostTriple,
                          const TranslationArtifactPolicyV1 &Policy);

/// Byte-view convenience overload for the version-1 policy.
llvm::Error
verifyTranslationArtifact(llvm::ArrayRef<uint8_t> ArtifactBytes,
                          const llvm::Triple &ExpectedHostTriple,
                          const TranslationArtifactPolicyV1 &Policy);

/// Compatibility entry point for callers that provide only a runtime-symbol
/// allowlist. It preserves the pre-policy behavior and does not require a block
/// manifest. New generated-code paths should use TranslationArtifactPolicyV1.
/// Validation never links, rewrites, or executes the object.
///
/// The object format and architecture must exactly match ExpectedHostTriple.
/// Undefined symbols are admitted only by exact membership in
/// AllowedRuntimeSymbols; dynamic symbols are forbidden. Every relocation must
/// use a positively recognized encoding and resolve either to a non-preemptible
/// definition in this object or to one of those runtime symbols. Prefix
/// matching and platform spelling rewrites are deliberately unsupported.
///
/// The verifier rejects executable-and-writable sections, exception/unwind
/// metadata, static constructors, TLS, indirect symbols, IFUNCs, dynamic
/// relocations, preemptible definitions, GOT/PLT indirection, and object
/// encodings or Mach-O load commands that LLVM cannot classify precisely.
llvm::Error verifyTranslationArtifact(
    llvm::MemoryBufferRef Artifact, const llvm::Triple &ExpectedHostTriple,
    llvm::ArrayRef<llvm::StringRef> AllowedRuntimeSymbols = {});

/// Byte-view convenience overload. ArtifactBytes is borrowed for the duration
/// of the call.
llvm::Error verifyTranslationArtifact(
    llvm::ArrayRef<uint8_t> ArtifactBytes,
    const llvm::Triple &ExpectedHostTriple,
    llvm::ArrayRef<llvm::StringRef> AllowedRuntimeSymbols = {});

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONARTIFACTVERIFIER_H
