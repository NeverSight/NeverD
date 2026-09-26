#ifndef NEVERD_PIPELINE_NATIVESOURCEHINTS_H
#define NEVERD_PIPELINE_NATIVESOURCEHINTS_H

#include "neverd/ir/SourceTypeHint.h"

#include <map>
#include <optional>
#include <set>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct MedFunc;
struct HighFunc;
struct PipelineFunctionAudit;

/// Independently authenticated, call-only native declarations for one current
/// image/pipeline round. The producer must revalidate the callee body and ABI;
/// existing MedIR hints and persisted PipelineOptions are not authentication.
/// These zero-argument pointer callees retain ordinary call clobbers and
/// effects. They do not certify body/dependency closure or become entry
/// declarations.
struct NativeSourceCalleeContracts {
  const BinaryImage *SourceImage = nullptr;
  std::map<va_t, SourceFunctionTypeHint> ZeroArgumentPointerCallees;
};

/// Collect exact native call ABIs already bound in one MedIR caller. This is
/// physical input evidence for the Boolean difference proof, not a source-body
/// certificate. Conflicting or incomplete bindings contribute no target.
std::map<va_t, SourceFunctionTypeHint>
boundNativeBooleanCallees(const MedFunc &Caller);

/// Exact direct call targets followed by an observed full-word read of the
/// second integer return register in the same block. This is a demand, not a
/// callee ABI proof. Calls, intrinsics and overlapping writes end the scan.
std::set<va_t> observedNativeIntegerPairReturns(const LowFunc &Function,
                                                Arch Architecture);

/// Describe observed scalar machine inputs and a defined result for a native
/// helper. The caller must select an exact local function target and supply IR
/// and audit from the same pipeline/image. This is a candidate for a second
/// source-only pipeline run, not the original C declaration, authentication,
/// a body-completeness certificate, or evidence of semantic equivalence.
///
/// Unknown calls, floating/vector ABI ambiguity, variadics, partial stack
/// slots, and results without a complete carrier on every return path fail
/// closed. A full-width observed parameter at the return register can supply
/// its incoming value; unused placeholders, seeds and PHIs cannot. The bounded
/// CFG proof meets the initial entry fact with backedges and invalidates
/// calls and partial writes, including narrowed self copies.
/// A helper with fully bound calls may instead supply a void source summary,
/// with no usable result; callee results can still be consumed internally.
/// Complete LowIR evidence must
/// prove preservation of incoming register bytes and frame state at every
/// exit. Exact private spills may restore these identities after calls. A
/// frameless immediate-tail helper uses the equivalent no-write proof and
/// rejects stack-derived call arguments or stores. CFG checks still apply,
/// and callers observing a result fail source validation.
/// The final source body and dependency closure must still pass projection
/// validation after the second run. No symbol names participate in inference.
/// Complete integer inputs forwarded to known pointer parameters can refine
/// the source candidate through conflict-free COPY/PHI uses. Physical carriers
/// remain unchanged; this does not modify generic lifting or rewrite types.
/// When Low is supplied from the same pipeline, observed full-width integer
/// entry registers can become explicit auxiliary parameters. Caller-saved
/// inputs may later be overwritten; preserved context inputs require either no
/// preserved non-frame/link writes or a complete native state-restoration proof.
/// Implicit call definitions do not establish entry inputs, including SSA zero.
/// Both callers and definitions must use the resulting source projection;
/// these parameters do not describe an external C or Swift calling convention.
/// ObserveIntegerPair requests a two-field internal record only when both
/// complete eight-byte results can be proved. Otherwise the existing scalar
/// candidate remains available and second-word consumers remain unresolved.
std::optional<SourceFunctionTypeHint> inferNativeSourceTypeHint(
    const BinaryImage &Image, const MedFunc &Med, const HighFunc &High,
    const PipelineFunctionAudit &Audit, std::string &Diagnostic,
    const LowFunc *Low = nullptr, bool ObserveIntegerPair = false,
    const NativeSourceCalleeContracts *CalleeContracts = nullptr);

/// Recheck the complete framed machine contract of a published memory-bearing
/// leaf projection. Requires matching current, explicit Med/High entry ABIs;
/// this does not authenticate final source calls or dependency closure.
bool sourceStackStoreStateContract(const BinaryImage &Image, const LowFunc &Low,
                                   const MedFunc &Med, const HighFunc &High);

/// Extend an inferred native scalar result to two complete integer words only
/// after both registers pass the same return-path proof. The caller must have
/// observed a use of the second word. Existing external/source declarations
/// never acquire this inferred contract. Re-lifting and source validation are
/// required before either word can be published.
std::optional<SourceFunctionTypeHint>
refineNativeIntegerPairReturnHint(const MedFunc &Med, const HighFunc &High,
                                  const PipelineFunctionAudit &Audit);

/// Refine a re-lifted native void or scalar candidate by removing auxiliary
/// register inputs with no occurrence in its complete HighIR body. Canonical
/// parameters and observable auxiliary inputs remain unchanged. This reuses
/// HighIR's existing private-frame cleanup; it does not independently discard
/// stores. The audit must prove that this HighIR came from complete verified
/// lifting. The returned candidate requires another pipeline run and source
/// validation. An inferred eight-byte integer result may also shrink to its
/// defined low word when every return supports that projection and at least
/// one explicitly carries unknown upper padding. This never defines discarded
/// bytes for callers; their complete source validation remains mandatory.
std::optional<SourceFunctionTypeHint>
refineNativeSourceTypeHint(const HighFunc &Function,
                           const PipelineFunctionAudit &Audit);
} // namespace neverd
#endif
