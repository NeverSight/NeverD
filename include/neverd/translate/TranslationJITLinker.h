//===- TranslationJITLinker.h - Sealed in-process linking -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fail-closed boundary that turns one compiler-owned native
/// translation artifact into finalized in-process code.  The linker resolves
/// runtime symbols only through an explicitly supplied sealed registry.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONJITLINKER_H
#define NEVERD_TRANSLATE_TRANSLATIONJITLINKER_H

#include "neverd/translate/RuntimeGuestState.h"
#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/RuntimeSymbolRegistry.h"
#include "neverd/translate/TranslationObjectCompiler.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

class TranslationObjectResultV1;

/// Stable failure categories for the version-1 sealed in-process linker.
/// Append values without renumbering existing entries.
enum class TranslationJITLinkerErrorCode : uint8_t {
  InvalidArtifact = 0,
  ArtifactTargetNotNative = 1,
  UnsupportedProcessTarget = 2,
  ProcessTargetMismatch = 3,
  RuntimeRegistryMismatch = 4,
  ArtifactAuditFailed = 5,
  LinkGraphAuditFailed = 6,
  LinkGraphCreationFailed = 7,
  PrePruneAuditFailed = 8,
  PostPruneAuditFailed = 9,
  PostAllocationAuditFailed = 10,
  RuntimeSymbolLookupFailed = 11,
  ResolutionAuditFailed = 12,
  PreFixupAuditFailed = 13,
  PostFixupAuditFailed = 14,
  FinalizationFailed = 15,
  EntryPointUnavailable = 16,
  InvocationRejected = 17,
  UnloadFailed = 18,
};

static_assert(
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::InvalidArtifact) == 0 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::ArtifactTargetNotNative) == 1 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::UnsupportedProcessTarget) == 2 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::ProcessTargetMismatch) == 3 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::RuntimeRegistryMismatch) == 4 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::ArtifactAuditFailed) ==
        5 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::LinkGraphAuditFailed) ==
        6 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::LinkGraphCreationFailed) == 7 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::PrePruneAuditFailed) ==
        8 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::PostPruneAuditFailed) ==
        9 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::PostAllocationAuditFailed) == 10 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::RuntimeSymbolLookupFailed) == 11 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::ResolutionAuditFailed) == 12 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::PreFixupAuditFailed) ==
        13 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::PostFixupAuditFailed) ==
        14 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::FinalizationFailed) ==
        15 &&
    static_cast<uint8_t>(
        TranslationJITLinkerErrorCode::EntryPointUnavailable) == 16 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::InvocationRejected) ==
        17 &&
    static_cast<uint8_t>(TranslationJITLinkerErrorCode::UnloadFailed) == 18);

class TranslationJITLinkerError final
    : public llvm::ErrorInfo<TranslationJITLinkerError> {
public:
  static char ID;

  TranslationJITLinkerError(TranslationJITLinkerErrorCode Code,
                            std::string Detail = {});

  TranslationJITLinkerErrorCode code() const { return Code; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  TranslationJITLinkerErrorCode Code;
  std::string Detail;
};

/// Completed audit boundaries in a successful receipt.  Bits are append-only.
enum class TranslationJITLinkAuditStageV1 : uint32_t {
  RawArtifact = 1U << 0,
  PreallocationGraph = 1U << 1,
  PrePrune = 1U << 2,
  PostPrune = 1U << 3,
  PostAllocation = 1U << 4,
  Resolved = 1U << 5,
  PreFixup = 1U << 6,
  PostFixup = 1U << 7,
  Finalized = 1U << 8,
};

struct TranslationJITLinkAuditReceiptV1 {
  static constexpr uint32_t SchemaVersion = 1;

  uint32_t CompletedStages = 0;
  std::string ArtifactIdentity;
  std::string RuntimeRegistryIdentity;
  std::string HostTriple;
  std::string HostCPU;
  uint64_t ManifestBlockCount = 0;
  uint64_t RuntimeReferenceCount = 0;
  uint64_t StubCount = 0;
  uint64_t GOTEntryCount = 0;
  uint64_t FinalSectionCount = 0;
  uint64_t FinalBlockCount = 0;
  uint64_t FinalEdgeCount = 0;
  bool InvocationCredentialBound = false;

  bool completed(TranslationJITLinkAuditStageV1 Stage) const {
    return (CompletedStages & static_cast<uint32_t>(Stage)) != 0;
  }
};

/// Move-only owner of one finalized native translation block.
///
/// No instance is returned until every graph and final-content audit has
/// succeeded and JITLink has finalized memory protections.  unload() provides
/// deterministic release; destruction performs the same release as a
/// best-effort fallback.
class LinkedTranslationBlockV1 final {
public:
  LinkedTranslationBlockV1(LinkedTranslationBlockV1 &&) noexcept;
  LinkedTranslationBlockV1 &operator=(LinkedTranslationBlockV1 &&) noexcept;
  LinkedTranslationBlockV1(const LinkedTranslationBlockV1 &) = delete;
  LinkedTranslationBlockV1 &
  operator=(const LinkedTranslationBlockV1 &) = delete;
  ~LinkedTranslationBlockV1();

  bool isLoaded() const noexcept;
  const TranslationJITLinkAuditReceiptV1 &auditReceipt() const {
    return Receipt;
  }

  /// Validate both host ABI records and require the guest RIP to equal the
  /// bound credential entry PC before invoking the finalized block.
  /// Concurrent unload waits for an invocation already holding the internal
  /// execution lease; once unload begins no new invocation is admitted.
  llvm::Expected<uint32_t> invoke(RuntimeGuestStateX86_64V1 &State,
                                  RuntimeCallFrameV1 &Runtime) const;

  /// Synchronously revoke invocation and release executable memory.  This is
  /// idempotent.  A deallocation error still leaves the block revoked because
  /// the memory-manager API consumes the finalized allocation handle.
  llvm::Error unload();

private:
  struct Impl;

  friend llvm::Expected<LinkedTranslationBlockV1>
  linkTranslationObjectV1(const TranslationObjectArtifactV1 &,
                          const RuntimeSymbolRegistryV1 &);
  friend llvm::Expected<LinkedTranslationBlockV1>
  linkTranslationObjectV1(const TranslationObjectResultV1 &,
                          const RuntimeSymbolRegistryV1 &,
                          const RuntimeCodeCredentialV1 &);

  LinkedTranslationBlockV1(std::unique_ptr<Impl> State,
                           TranslationJITLinkAuditReceiptV1 Receipt);

  std::unique_ptr<Impl> State;
  TranslationJITLinkAuditReceiptV1 Receipt;
};

/// Audit-link one compiler-owned native AArch64 artifact against Registry.
///
/// The artifact is re-audited as raw object bytes and as the exact LinkGraph
/// passed to JITLink.  Registry is the sole symbol source; process and dynamic
/// loader lookup are never consulted.  No invocation credential is bound, so
/// invoke() on the returned block always fails closed.
llvm::Expected<LinkedTranslationBlockV1>
linkTranslationObjectV1(const TranslationObjectArtifactV1 &Artifact,
                        const RuntimeSymbolRegistryV1 &Registry);

/// Validate one trusted translation result, link its artifact, and bind the
/// sole manifest entry to one dispatcher credential.  The credential entry PC
/// must exactly match the validated block descriptor.  invoke() is available
/// only on a block returned through this overload.
llvm::Expected<LinkedTranslationBlockV1>
linkTranslationObjectV1(const TranslationObjectResultV1 &Object,
                        const RuntimeSymbolRegistryV1 &Registry,
                        const RuntimeCodeCredentialV1 &Credential);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONJITLINKER_H
