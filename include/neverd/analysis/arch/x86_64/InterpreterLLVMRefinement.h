//===- InterpreterLLVMRefinement.h - Native-to-LLVM proofs ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_INTERPRETERLLVMREFINEMENT_H
#define NEVERD_ANALYSIS_INTERPRETERLLVMREFINEMENT_H

#include "neverd/analysis/arch/x86_64/BinaryInterpreterSpecialization.h"
#include "neverd/analysis/arch/x86_64/LLVMInterpreterMachineState.h"

namespace neverd::analysis {

struct InterpreterLLVMRefinementLimits {
  /// Bound serialized input before copying, parsing or hashing it. LLVM's
  /// trusted parser/verifier have no hard CPU/stack/allocation bound here.
  uint64_t MaxIRBytes = 4 * 1024 * 1024;
  uint64_t MaxMachineStateOperations = 65536;
  /// Shared metadata traversal for preparing both canonical entry prefixes.
  uint64_t MaxPreparationItems = 1048576;
  LLVMInterpreterModelLimits LLVMModel;
  /// Independent, explicit budgets; no failed stage raises another's limit.
  LowIRRefinementLimits NativeProof, LLVMProof;
};

struct InterpreterLLVMRefinementPlans {
  /// Untrusted proposals. An absent plan selects complete finite execution.
  std::optional<LowIRLoopRefinementPlan> Native, LLVM;
};

/// Additional entry-state preservation obligations, never a success receipt.
/// Modeled ranges cover only the 16 GPR words in InterpreterMachineStateX64V1.
/// Nonempty ranges may span words; overlaps are unioned without adding bytes.
/// Preparation splits the union at word boundaries and retains mandatory RSP.
/// Opaque architectural state can be checked only by the fresh native premise.
struct InterpreterLLVMRefinementPreservation {
  std::vector<symbolic::SymRegisterRange> ModeledRegisters;
  std::optional<LowIRNativePreservationRequirement> NativeState;
};

struct InterpreterLLVMRefinementModels {
  InterpreterMachineStateModel Residual, LLVM;
  LowIRIndependenceContract Contract;
  std::string LLVMIRDigest, FunctionName;
  /// Effective normalized GPR obligations, including RSP. NativeState remains
  /// a native request and is deliberately absent from the static Contract.
  InterpreterLLVMRefinementPreservation Preservation;
};

/// Snapshot exact textual LLVM IR, select the named function and build fresh
/// models of it and the residual's state wrapper. Both receive an entry-only
/// canonical flag projection owned by UserX64NoFaultV1. Its image is exactly
/// that profile's allowed packed-flag domain; other state words are unchanged.
/// The mandatory contract observes all 17 words, actual status, definedness
/// and written frame bytes, and preserves RSP, the return slot and definedness.
/// An optional frame-entry congruence restricts the same raw RSP input on both
/// sides, without changing its value or granting memory/ABI facts.
/// Frame must be rooted at the raw RSP state word (offset 32, eight bytes).
/// This API only prepares inputs for untrusted loop proposals; it proves
/// nothing. Mutating returned models cannot change what the checker verifies.
/// Additional preservation cannot replace any mandatory observation or entry
/// condition. Range validation and normalization share MaxPreparationItems;
/// the complete proofs retain their independent execution budgets.
llvm::Expected<InterpreterLLVMRefinementModels>
prepareInterpreterLLVMRefinement(
    const LowFunc &Residual, llvm::StringRef LLVMIR,
    llvm::StringRef FunctionName, const LowIRIndependenceFrame &Frame,
    const InterpreterLLVMRefinementLimits &Limits = {},
    const InterpreterLLVMRefinementPreservation &Preservation = {});

enum class InterpreterLLVMRefinementStage : uint8_t {
  Preparation,
  Native,
  LLVM,
  Complete,
};

struct InterpreterLLVMRefinementCertificate {
  static constexpr uint32_t SchemaVersion = 1;
  std::string InputDigest, LLVMIRDigest, FunctionName;
  InterpreterMachineStateProfile Profile =
      InterpreterMachineStateProfile::UserX64NoFaultV1;
  uint32_t FlagsSemanticsVersion = 0;
  InterpreterLLVMRefinementLimits Limits;
  BinaryLowIRRefinementCertificate Native;
  LowIRRefinementCertificate LLVM;
};

struct InterpreterLLVMRefinementResult {
  InterpreterLLVMRefinementStage Stage =
      InterpreterLLVMRefinementStage::Preparation;
  std::string Diagnostic;
  BinaryLowIRRefinementResult Native;
  LowIRRefinementResult LLVM;
  std::optional<InterpreterLLVMRefinementCertificate> Certificate;

  bool proved() const {
    return Stage == InterpreterLLVMRefinementStage::Complete &&
           Certificate.has_value() && Native.proved() && LLVM.proved();
  }
};

/// Freshly prove the actual native image against this identical Residual,
/// then its generated state model against the exact LLVM text. Neither
/// externally supplied models, receipts nor hashes can authorize success.
/// All observations and preservation obligations are mandatory. Native entry
/// constants and image exclusions remain in the native receipt; the source
/// relation covers arbitrary GPRs and canonical flags within the same caller
/// frame, exclusions and optional entry congruence. Extra native restrictions
/// cannot weaken the source relation. Plans never add entry assumptions.
/// Additional modeled preservation applies to both premises. Native opaque
/// preservation requires fresh instruction evidence and SelectedWitness;
/// an AllUndefinedChoices request is refused. Both subreceipts bind their
/// effective contracts; the composite digest binds those fresh identities.
///
/// Requires the explicit normal, nonfaulting, CET-disabled UserX64NoFaultV1
/// profile and its fixed immutable image. The result describes the chosen
/// ISA-allowed witness before the outer return-address pop. It assumes live,
/// initialized frame bytes and accessible, aligned, disjoint state storage
/// with valid LLVM guest-pointer provenance. Lifting, scalar transitions,
/// both model builders, LLVM parsing and the checker/solver remain trusted.
///
/// This is not C-to-LLVM compilation evidence, generated machine-code
/// correctness, physical CPU undefined-bit equality or a portable proof from
/// an untrusted issuer. Digests detect stale inputs; rerun the complete check
/// on changed inputs. Failure never produces the composite certificate.
InterpreterLLVMRefinementResult checkBinaryLLVMRefinement(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowFunc &Residual, llvm::StringRef LLVMIR,
    llvm::StringRef FunctionName, const LowIRIndependenceFrame &Frame,
    const InterpreterLLVMRefinementPlans &Plans = {},
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const InterpreterLLVMRefinementLimits &Limits = {},
    const InterpreterLLVMRefinementPreservation &Preservation = {});

} // namespace neverd::analysis
#endif
