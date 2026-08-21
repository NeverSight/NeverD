//===- TranslationObjectRequest.h - x86-64 to AArch64 object slice -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publishes the first complete guest-byte to audited-object translation
/// request.  Version 1 is deliberately narrow: x86-64 guest bytes, explicit
/// AArch64 output, and fail-closed unsupported instructions.  The host target
/// is either an explicit AOT target or the resolved native AArch64 JIT target.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONOBJECTREQUEST_H
#define NEVERD_TRANSLATE_TRANSLATIONOBJECTREQUEST_H

#include "neverd/translate/TranslationBlockLowerer.h"
#include "neverd/translate/TranslationObjectCompiler.h"
#include "neverd/translate/TranslationTargetMachine.h"
#include "neverd/translate/X86TranslationBlockBuilder.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

inline constexpr uint32_t kTranslationObjectRequestSchemaV1 = 1;
inline constexpr uint32_t kTranslationObjectWrapperCacheIdentityVersionV1 = 1;
inline constexpr uint32_t kTranslationObjectWrapperCacheIdentityVersionV2 = 2;
inline constexpr uint32_t kTranslationObjectWrapperCacheIdentityVersion =
    kTranslationObjectWrapperCacheIdentityVersionV2;

/// Stable failures for the complete request boundary.  Nested typed codes are
/// retained where a lower stage supplied them.  Append without renumbering.
enum class TranslationObjectRequestErrorCode : uint8_t {
  InvalidRequest = 0,
  GuestStateRejected = 1,
  RuntimeCreationFailed = 2,
  TargetMachineCreationFailed = 3,
  BlockBuilderCreationFailed = 4,
  BlockConstructionFailed = 5,
  InstructionBudgetExceeded = 6,
  BlockLoweringFailed = 7,
  ObjectCompilationFailed = 8,
  GeneratedCodeBudgetExceeded = 9,
  ArtifactVerificationFailed = 10,
};

static_assert(
    static_cast<uint8_t>(TranslationObjectRequestErrorCode::InvalidRequest) ==
        0 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::GuestStateRejected) == 1 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::RuntimeCreationFailed) == 2 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::TargetMachineCreationFailed) == 3 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::BlockBuilderCreationFailed) == 4 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::BlockConstructionFailed) == 5 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::InstructionBudgetExceeded) == 6 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::BlockLoweringFailed) == 7 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::ObjectCompilationFailed) == 8 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded) == 9 &&
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::ArtifactVerificationFailed) == 10);

class TranslationObjectRequestError final
    : public llvm::ErrorInfo<TranslationObjectRequestError> {
public:
  static char ID;

  TranslationObjectRequestError(
      TranslationObjectRequestErrorCode Code, std::string Detail = {},
      std::optional<TranslationTargetMachineErrorCode> TargetCode =
          std::nullopt,
      std::optional<X86TranslationBlockBuilderErrorCode> BuilderCode =
          std::nullopt,
      std::optional<TranslationBlockLoweringErrorCode> LoweringCode =
          std::nullopt,
      std::optional<TranslationObjectCompilerErrorCode> CompilerCode =
          std::nullopt,
      std::optional<uint64_t> CompilerBudgetObserved = std::nullopt,
      std::optional<uint64_t> CompilerBudgetLimit = std::nullopt,
      std::optional<uint64_t> GuestInstructionCount = std::nullopt,
      std::optional<uint64_t> BuilderGuestPC = std::nullopt,
      std::optional<RuntimeMemoryFaultKindV1> BuilderMemoryFault = std::nullopt,
      std::optional<GuestMemoryFault> BuilderMemoryFaultDetails = std::nullopt,
      std::optional<uint64_t> LoweringGuestPC = std::nullopt);

  TranslationObjectRequestErrorCode code() const { return Code; }
  llvm::StringRef detail() const { return Detail; }
  std::optional<TranslationTargetMachineErrorCode> targetCode() const {
    return TargetCode;
  }
  std::optional<X86TranslationBlockBuilderErrorCode> builderCode() const {
    return BuilderCode;
  }
  /// Exact instruction address reported by block construction or translation
  /// budget exhaustion.  Other request failures do not publish a guest PC.
  std::optional<uint64_t> builderGuestPC() const { return BuilderGuestPC; }
  /// Exact instruction-fetch fault reported by block construction.  A missing
  /// value distinguishes non-memory construction failures.
  std::optional<RuntimeMemoryFaultKindV1> builderMemoryFault() const {
    return BuilderMemoryFault;
  }
  /// Complete checked instruction-fetch failure.  This retains the exact
  /// execute range and access width instead of collapsing it to a category.
  const std::optional<GuestMemoryFault> &builderMemoryFaultDetails() const {
    return BuilderMemoryFaultDetails;
  }
  std::optional<TranslationBlockLoweringErrorCode> loweringCode() const {
    return LoweringCode;
  }
  /// Exact guest instruction address reported by block lowering.  This is
  /// diagnostic provenance; the session's resumable PC remains the last
  /// committed runtime-state PC when an entire block fails to lower.
  std::optional<uint64_t> loweringGuestPC() const { return LoweringGuestPC; }
  std::optional<TranslationObjectCompilerErrorCode> compilerCode() const {
    return CompilerCode;
  }
  std::optional<uint64_t> compilerBudgetObserved() const {
    return CompilerBudgetObserved;
  }
  std::optional<uint64_t> compilerBudgetLimit() const {
    return CompilerBudgetLimit;
  }
  std::optional<uint64_t> guestInstructionCount() const {
    return GuestInstructionCount;
  }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  TranslationObjectRequestErrorCode Code;
  std::string Detail;
  std::optional<TranslationTargetMachineErrorCode> TargetCode;
  std::optional<X86TranslationBlockBuilderErrorCode> BuilderCode;
  std::optional<uint64_t> BuilderGuestPC;
  std::optional<RuntimeMemoryFaultKindV1> BuilderMemoryFault;
  std::optional<GuestMemoryFault> BuilderMemoryFaultDetails;
  std::optional<TranslationBlockLoweringErrorCode> LoweringCode;
  std::optional<uint64_t> LoweringGuestPC;
  std::optional<TranslationObjectCompilerErrorCode> CompilerCode;
  std::optional<uint64_t> CompilerBudgetObserved;
  std::optional<uint64_t> CompilerBudgetLimit;
  std::optional<uint64_t> GuestInstructionCount;
};

/// Synchronous borrowed-state request.
///
/// State must remain alive and must not be mutated until
/// compileTranslationObjectRequestV1() returns.  Options and Semantic are
/// owned copies.  The translation runtime copies guest memory before decoding,
/// and the returned descriptor owns its exact executable bytes and generation
/// bindings.  The default semantic policy removes caller resource ceilings.
class TranslationObjectRequestV1 final {
public:
  TranslationObjectRequestV1(const GuestState &State, uint64_t EntryPC,
                             TranslationOptions Options,
                             TranslationSemanticPolicyV1 Semantic =
                                 TranslationSemanticPolicyV1::unlimited())
      : State(&State), EntryPC(EntryPC), Options(std::move(Options)),
        Semantic(std::move(Semantic)) {}
  TranslationObjectRequestV1(
      GuestState &&, uint64_t, TranslationOptions,
      TranslationSemanticPolicyV1 = TranslationSemanticPolicyV1::unlimited()) =
      delete;
  TranslationObjectRequestV1(
      const GuestState &&, uint64_t, TranslationOptions,
      TranslationSemanticPolicyV1 = TranslationSemanticPolicyV1::unlimited()) =
      delete;

  const GuestState &guestState() const { return *State; }
  uint64_t entryPC() const { return EntryPC; }
  const TranslationOptions &options() const { return Options; }
  const TranslationSemanticPolicyV1 &semanticPolicy() const { return Semantic; }

private:
  const GuestState *State;
  uint64_t EntryPC;
  TranslationOptions Options;
  TranslationSemanticPolicyV1 Semantic;
};

/// Owned trusted descriptor, audited relocatable object, and whole-request
/// identity.  cacheIdentity() includes executable generations in addition to
/// the object compiler's IR-derived request identity.  The emitted bytes keep
/// their distinct artifactCacheKey() identity.
class TranslationObjectResultV1 final {
public:
  const TranslationBlockDescriptorV1 &descriptor() const { return Descriptor; }
  const TranslationObjectArtifactV1 &artifact() const { return Artifact; }
  llvm::StringRef cacheIdentity() const { return CacheIdentity; }

private:
  friend llvm::Expected<TranslationObjectResultV1>
  compileTranslationObjectRequestV1(const TranslationObjectRequestV1 &);

  TranslationObjectResultV1(TranslationBlockDescriptorV1 Descriptor,
                            TranslationObjectArtifactV1 Artifact,
                            std::string CacheIdentity)
      : Descriptor(std::move(Descriptor)), Artifact(std::move(Artifact)),
        CacheIdentity(std::move(CacheIdentity)) {}

  TranslationBlockDescriptorV1 Descriptor;
  TranslationObjectArtifactV1 Artifact;
  std::string CacheIdentity;
};

/// Decode and lift one exact x86-64 block from Request.guestState(), lower it
/// using the same resolved AArch64 target machine later used for object
/// emission, run the owned proof-gated semantic policy (unlimited by default)
/// with LLVM's requested optimization pipeline, and return an audited
/// relocatable object.  AOT accepts an explicit AArch64 target; JIT accepts
/// only a native target whose resolved process architecture is AArch64.  This
/// API does not link, load, publish, or execute the artifact.
llvm::Expected<TranslationObjectResultV1>
compileTranslationObjectRequestV1(const TranslationObjectRequestV1 &Request);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONOBJECTREQUEST_H
