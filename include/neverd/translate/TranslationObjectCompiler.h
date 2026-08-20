//===- TranslationObjectCompiler.h - Host object emission -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fail-closed LLVM IR to relocatable-host-object boundary used by
/// translation backends.  This API emits and audits an object; it does not
/// link, publish, or execute generated code.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_TRANSLATIONOBJECTCOMPILER_H
#define NEVERD_TRANSLATE_TRANSLATIONOBJECTCOMPILER_H

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/translate/ResolvedHostTarget.h"
#include "neverd/translate/TranslationIRVerifier.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace neverd::translate {

class TranslationTargetMachineV1;

/// Stable failure categories for verified translation-object compilation.
/// Append values without renumbering existing entries.
enum class TranslationObjectCompilerErrorCode : uint8_t {
  InvalidRequest = 0,
  InvalidArtifactPolicy = 1,
  InvalidSemanticPolicy = 2,
  HostTargetResolutionFailed = 3,
  UnsupportedHostArchitecture = 4,
  TargetLookupFailed = 5,
  TargetCPUOrFeatureRejected = 6,
  TargetMachineCreationFailed = 7,
  InputIRVerificationFailed = 8,
  FinalIRVerificationFailed = 9,
  ObjectEmissionUnsupported = 10,
  ObjectEmissionFailed = 11,
  GeneratedCodeBudgetExceeded = 12,
  ArtifactVerificationFailed = 13,
  RuntimeRegistryUnavailable = 14,
};

static_assert(
    static_cast<uint8_t>(TranslationObjectCompilerErrorCode::InvalidRequest) ==
        0 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::InvalidArtifactPolicy) == 1 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::InvalidSemanticPolicy) == 2 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::HostTargetResolutionFailed) == 3 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::UnsupportedHostArchitecture) == 4 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::TargetLookupFailed) == 5 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::TargetCPUOrFeatureRejected) == 6 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::TargetMachineCreationFailed) == 7 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::InputIRVerificationFailed) == 8 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::FinalIRVerificationFailed) == 9 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::ObjectEmissionUnsupported) == 10 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::ObjectEmissionFailed) == 11 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::GeneratedCodeBudgetExceeded) ==
        12 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::ArtifactVerificationFailed) == 13 &&
    static_cast<uint8_t>(
        TranslationObjectCompilerErrorCode::RuntimeRegistryUnavailable) == 14);

/// Typed compiler failure.  Detail is diagnostic and is never a substitute
/// for inspecting code().
class TranslationObjectCompilerError final
    : public llvm::ErrorInfo<TranslationObjectCompilerError> {
public:
  static char ID;

  TranslationObjectCompilerError(
      TranslationObjectCompilerErrorCode Code, std::string Detail = {},
      std::optional<uint64_t> BudgetObserved = std::nullopt,
      std::optional<uint64_t> BudgetLimit = std::nullopt);

  TranslationObjectCompilerErrorCode code() const { return Code; }
  llvm::StringRef detail() const { return Detail; }
  std::optional<uint64_t> budgetObserved() const { return BudgetObserved; }
  std::optional<uint64_t> budgetLimit() const { return BudgetLimit; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  TranslationObjectCompilerErrorCode Code;
  std::string Detail;
  std::optional<uint64_t> BudgetObserved;
  std::optional<uint64_t> BudgetLimit;
};

/// Why a semantic/LLVM convergence invocation stopped.  The module report
/// retains the most severe stop seen across all function-pass invocations.
enum class TranslationSemanticStopV1 : uint8_t {
  NotRun = 0,
  Stable = 1,
  CycleDetected = 2,
  RoundBudgetExhausted = 3,
};

static_assert(
    static_cast<uint8_t>(TranslationSemanticStopV1::NotRun) == 0 &&
    static_cast<uint8_t>(TranslationSemanticStopV1::Stable) == 1 &&
    static_cast<uint8_t>(TranslationSemanticStopV1::CycleDetected) == 2 &&
    static_cast<uint8_t>(TranslationSemanticStopV1::RoundBudgetExhausted) == 3);

/// Aggregate telemetry from semantic simplification embedded in compilation.
/// Proof is the most conservative synthesis-proof disposition observed.
/// Exact derivational MBA rewrites do not manufacture a solver proof and
/// therefore leave Proof as NotRun when no synthesis query was needed.
struct TranslationSemanticReportV1 {
  bool Changed = false;
  uint64_t Rewrites = 0;
  uint64_t SearchWork = 0;
  solver::ProofStats ProofWork;
  uint64_t FunctionPassInvocations = 0;
  unsigned MaxRounds = 0;
  TranslationSemanticStopV1 Stop = TranslationSemanticStopV1::NotRun;
  solver::ProofStatus Proof = solver::ProofStatus::NotRun;
};

/// Caller-owned semantic resource policy.
///
/// MaxRounds applies independently to each function-pass invocation.  Zero
/// removes that caller limit; exact state repetition still stops a cycle.
/// Built-in proof is the only accepted provider for object compilation, and a
/// callback is rejected because neither its behavior nor identity is suitable
/// for a deterministic precompile cache key.
struct TranslationSemanticPolicyV1 {
  SymSimplifyOptions Simplify;
  unsigned MaxRounds = 0;

  /// Safe finite defaults for latency-sensitive callers.
  static TranslationSemanticPolicyV1 bounded();

  /// Remove MBA arity/work, synthesis-work, bit-blast, SAT, and convergence
  /// ceilings.  Memory-safety limits and synthesis grammar choices remain
  /// explicit in Simplify and may be changed by the caller.  Stochastic search
  /// remains disabled so an artifact is derived reproducibly.
  static TranslationSemanticPolicyV1 unlimited();
};

/// Version-1 policy borrowed for the duration of one compiler call.
///
/// RequiredBlockSymbols names LLVM IR definitions.  The compiler validates a
/// complete manifest, canonicalizes its order, and uses LLVM's target mangler
/// to form the exact object-level manifest.  Runtime helpers are not supplied
/// by the caller: the sealed v1 registry is the sole allowlist.
struct TranslationObjectPolicyV1 {
  uint64_t StateSize = 0;
  llvm::ArrayRef<TranslationIRMemorySlot> StateSlots;
  llvm::ArrayRef<llvm::StringRef> RequiredBlockSymbols;
  TranslationSemanticPolicyV1 Semantic =
      TranslationSemanticPolicyV1::unlimited();
};

/// One canonical LLVM IR symbol and the exact symbol emitted into the object.
/// These may differ on targets with a global symbol prefix.
struct TranslationObjectSymbolV1 {
  std::string IRName;
  std::string ObjectName;

  friend bool operator==(const TranslationObjectSymbolV1 &,
                         const TranslationObjectSymbolV1 &) = default;
};

/// Immutable result of verified relocatable-object emission.
class TranslationObjectArtifactV1 final {
public:
  static constexpr uint32_t CacheIdentityVersion = 1;
  /// Manual schema for the exact IR optimization and object-emission recipe.
  /// Bump whenever pass ordering, sealing, or target-machine policy changes.
  static constexpr uint32_t PipelineSchemaVersion = 4;

  llvm::ArrayRef<uint8_t> bytes() const { return Bytes; }
  const ResolvedHostTarget &hostTarget() const { return HostTarget; }
  const TranslationSemanticReportV1 &semanticReport() const {
    return SemanticReport;
  }
  /// True only after the target-aware LLVM default pipeline completed.  This
  /// is execution telemetry, not a claim that LLVM or semantic passes rewrote
  /// the module.
  bool llvmOptimizationPipelineRan() const { return LLVMPipelineRan; }
  llvm::ArrayRef<TranslationObjectSymbolV1> blockSymbols() const {
    return BlockSymbols;
  }
  llvm::ArrayRef<TranslationObjectSymbolV1> runtimeSymbols() const {
    return RuntimeSymbols;
  }

  /// Address-free ABI-shape identity of the sole registry admitted while
  /// compiling this object.  A future linker must require an exact match
  /// before binding any runtime symbol.
  llvm::StringRef runtimeRegistryIdentity() const {
    return RuntimeRegistryIdentity;
  }

  /// Key available before optimization/code generation.  It covers the input
  /// bitcode, normalized request, target, semantic resources, ABI, and policy.
  llvm::StringRef requestCacheKey() const { return RequestCacheKey; }

  /// Identity of this audited artifact.  In addition to requestCacheKey(), it
  /// covers final verified IR and emitted object bytes; it is not a promise
  /// that a future compiler version produces the same bytes.
  llvm::StringRef artifactCacheKey() const { return ArtifactCacheKey; }

private:
  friend llvm::Expected<TranslationObjectArtifactV1>
  compileTranslationObjectV1(const llvm::Module &, const TranslationOptions &,
                             const TranslationObjectPolicyV1 &);
  friend llvm::Expected<TranslationObjectArtifactV1>
  compileTranslationObjectWithTargetV1(const llvm::Module &,
                                       const TranslationOptions &,
                                       const TranslationObjectPolicyV1 &,
                                       TranslationTargetMachineV1 &);

  TranslationObjectArtifactV1(
      std::vector<uint8_t> Bytes, ResolvedHostTarget HostTarget,
      TranslationSemanticReportV1 SemanticReport, bool LLVMPipelineRan,
      std::vector<TranslationObjectSymbolV1> BlockSymbols,
      std::vector<TranslationObjectSymbolV1> RuntimeSymbols,
      std::string RuntimeRegistryIdentity, std::string RequestCacheKey,
      std::string ArtifactCacheKey)
      : Bytes(std::move(Bytes)), HostTarget(std::move(HostTarget)),
        SemanticReport(std::move(SemanticReport)),
        LLVMPipelineRan(LLVMPipelineRan), BlockSymbols(std::move(BlockSymbols)),
        RuntimeSymbols(std::move(RuntimeSymbols)),
        RuntimeRegistryIdentity(std::move(RuntimeRegistryIdentity)),
        RequestCacheKey(std::move(RequestCacheKey)),
        ArtifactCacheKey(std::move(ArtifactCacheKey)) {}

  std::vector<uint8_t> Bytes;
  ResolvedHostTarget HostTarget;
  TranslationSemanticReportV1 SemanticReport;
  bool LLVMPipelineRan = false;
  std::vector<TranslationObjectSymbolV1> BlockSymbols;
  std::vector<TranslationObjectSymbolV1> RuntimeSymbols;
  std::string RuntimeRegistryIdentity;
  std::string RequestCacheKey;
  std::string ArtifactCacheKey;
};

/// Compile Module through a clone, verify the final IR, emit a relocatable
/// host object, and audit it against the exact v1 symbol boundary.  Module is
/// never modified on success or failure.  This function does not link or run
/// the returned bytes.
llvm::Expected<TranslationObjectArtifactV1>
compileTranslationObjectV1(const llvm::Module &Module,
                           const TranslationOptions &Options,
                           const TranslationObjectPolicyV1 &Policy);

/// Variant for a caller that already owns the exact target machine used to
/// lower Module.  Target must have been created from Options.  Reusing this
/// boundary prevents a lowerer and emitter from independently selecting target
/// defaults or DataLayouts.
llvm::Expected<TranslationObjectArtifactV1>
compileTranslationObjectWithTargetV1(const llvm::Module &Module,
                                     const TranslationOptions &Options,
                                     const TranslationObjectPolicyV1 &Policy,
                                     TranslationTargetMachineV1 &Target);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_TRANSLATIONOBJECTCOMPILER_H
