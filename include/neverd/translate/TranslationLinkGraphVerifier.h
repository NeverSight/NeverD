//===- TranslationLinkGraphVerifier.h - LinkGraph audit -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fail-closed object-to-LinkGraph audit used before any linker
/// allocation or symbol binding.  Passing this boundary is not permission to
/// link, load, publish, or execute generated code.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONLINKGRAPHVERIFIER_H
#define NEVERD_TRANSLATE_TRANSLATIONLINKGRAPHVERIFIER_H

#include "neverd/translate/TranslationObjectCompiler.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <system_error>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

/// Stable failure categories for the version-1 preallocation graph audit.
/// Append values without renumbering existing entries.
enum class TranslationLinkGraphErrorCode : uint8_t {
  InvalidInput = 0,
  InvalidManifest = 1,
  ObjectGraphCreationFailed = 2,
  GraphTargetMismatch = 3,
  SectionPolicyViolation = 4,
  BlockSymbolManifestMismatch = 5,
  RuntimeSymbolManifestMismatch = 6,
  AbsoluteSymbolRejected = 7,
  EdgePolicyViolation = 8,
};

static_assert(
    static_cast<uint8_t>(TranslationLinkGraphErrorCode::InvalidInput) == 0 &&
    static_cast<uint8_t>(TranslationLinkGraphErrorCode::InvalidManifest) == 1 &&
    static_cast<uint8_t>(
        TranslationLinkGraphErrorCode::ObjectGraphCreationFailed) == 2 &&
    static_cast<uint8_t>(TranslationLinkGraphErrorCode::GraphTargetMismatch) ==
        3 &&
    static_cast<uint8_t>(
        TranslationLinkGraphErrorCode::SectionPolicyViolation) == 4 &&
    static_cast<uint8_t>(
        TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch) == 5 &&
    static_cast<uint8_t>(
        TranslationLinkGraphErrorCode::RuntimeSymbolManifestMismatch) == 6 &&
    static_cast<uint8_t>(
        TranslationLinkGraphErrorCode::AbsoluteSymbolRejected) == 7 &&
    static_cast<uint8_t>(TranslationLinkGraphErrorCode::EdgePolicyViolation) ==
        8);

class TranslationLinkGraphError final
    : public llvm::ErrorInfo<TranslationLinkGraphError> {
public:
  static char ID;

  TranslationLinkGraphError(TranslationLinkGraphErrorCode Code,
                            std::string Detail = {});

  TranslationLinkGraphErrorCode code() const { return Code; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  TranslationLinkGraphErrorCode Code;
  std::string Detail;
};

/// Address-free facts retained after a successful synchronous graph audit.
/// The LinkGraph itself is deliberately destroyed before this value returns.
struct TranslationLinkGraphAuditV1 {
  std::string GraphTriple;
  uint64_t SectionCount = 0;
  uint64_t BlockCount = 0;
  uint64_t DefinedSymbolCount = 0;
  uint64_t ExternalSymbolCount = 0;
  uint64_t EdgeCount = 0;
};

/// Build an LLVM JITLink graph from untrusted relocatable-object bytes and
/// audit it against an independently supplied sealed manifest.  The v1 policy
/// accepts only the little-endian 64-bit AArch64 ELF and Mach-O graph shapes
/// produced by the translation compiler.  This structural audit does not by
/// itself prove instruction semantics or object provenance.  ObjectBytes is
/// never modified.
llvm::Expected<TranslationLinkGraphAuditV1> verifyTranslationLinkGraphV1(
    llvm::ArrayRef<uint8_t> ObjectBytes,
    const ResolvedHostTarget &ExpectedHostTarget,
    llvm::ArrayRef<TranslationObjectSymbolV1> ExpectedBlockSymbols,
    llvm::ArrayRef<TranslationObjectSymbolV1> SealedRuntimeSymbols,
    llvm::StringRef SealedRuntimeRegistryIdentity);

/// Audit an immutable compiler artifact.  Its runtime declaration is first
/// checked against the independently reconstructed v1 runtime registry; graph
/// contents remain authoritative for sections, symbols, and relocations.
llvm::Expected<TranslationLinkGraphAuditV1>
verifyTranslationLinkGraphV1(const TranslationObjectArtifactV1 &Artifact);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONLINKGRAPHVERIFIER_H
