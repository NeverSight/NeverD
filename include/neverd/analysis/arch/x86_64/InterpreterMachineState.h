//===- InterpreterMachineState.h - Explicit recovery source ABI -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETERMACHINESTATE_H
#define NEVERD_ANALYSIS_INTERPRETERMACHINESTATE_H

#include "neverd/analysis/InterpreterEntryAlignment.h"
#include "neverd/analysis/InterpreterMachineStateProfile.h"
#include "neverd/analysis/InterpreterModel.h"
#include "neverd/analysis/Limits.h"
#include "neverd/analysis/LowIRUndefinedIndependence.h"
#include "neverd/ir/SourceTypeHint.h"
#include "neverd/ir/low/LowIR.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace neverd::analysis {

/// Recovery-only source ABI. This is not the translated-block runtime ABI.
/// GPR order is RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8 ... R15. Guest
/// addresses are integers naming the original fixed mappings. State storage
/// must be disjoint from every guest memory access and remain accessible for
/// the complete call. Generated source requires a little-endian 64-bit host.
struct alignas(8) InterpreterMachineStateX64V1 {
  uint64_t GPR[16] = {};
  uint64_t RFlags = 2;
};

static_assert(std::is_standard_layout_v<InterpreterMachineStateX64V1>);
static_assert(sizeof(InterpreterMachineStateX64V1) == 136);
static_assert(offsetof(InterpreterMachineStateX64V1, RFlags) == 128);

/// One ordered guest CPU query, including both input selectors. Answers are
/// independent across occurrences, including repeated identical selectors.
struct InterpreterCPUQueryX64V1 {
  uint32_t Leaf = 0;
  uint32_t Subleaf = 0;
  uint32_t Answers[4] = {}; // EAX, EBX, ECX, EDX.
};

/// Opt-in source ABI. Queries is an integer address of QueryCount accessible,
/// immutable query records. The complete array must not wrap or alias state
/// storage or guest memory. Count zero permits a null Queries pointer. The
/// caller owns this environment; no host CPU instruction supplies an answer.
struct alignas(8) InterpreterMachineQueryStateX64V1 {
  InterpreterMachineStateX64V1 Machine;
  uint64_t Queries = 0;
  uint64_t QueryCount = 0;
  uint64_t QueryCursor = 0;
};

static_assert(std::is_standard_layout_v<InterpreterCPUQueryX64V1>);
static_assert(sizeof(InterpreterCPUQueryX64V1) == 24);
static_assert(std::is_standard_layout_v<InterpreterMachineQueryStateX64V1>);
static_assert(offsetof(InterpreterMachineQueryStateX64V1, Machine) == 0);
static_assert(sizeof(InterpreterMachineQueryStateX64V1) == 160);

/// A separate source layout that also binds every byte of XMM0 through XMM15.
/// The query prefix retains its v1 layout. XMM words are low then high halves;
/// only eight-byte state alignment is required. Wider vector banks, partial
/// register operands and floating-point environment operations remain refused.
struct alignas(8) InterpreterMachineQueryStateX64V2 {
  InterpreterMachineQueryStateX64V1 Core;
  uint64_t XMM[16][2] = {};
};

static_assert(std::is_standard_layout_v<InterpreterMachineQueryStateX64V2>);
static_assert(offsetof(InterpreterMachineQueryStateX64V2, XMM) == 160);
static_assert(sizeof(InterpreterMachineQueryStateX64V2) == 416);

/// Check the caller-supplied entry state against the fixed user-mode profile.
/// Generated source returns a nonzero status for the same invalid states.
llvm::Error
validateInterpreterMachineStateX64V1(const InterpreterMachineStateX64V1 &State);

struct InterpreterMachineSource {
  LowFunc Function;
  /// One state pointer parameter and an unsigned 64-bit status: zero means
  /// success; nonzero means a flag-profile, entry-domain or query violation.
  /// Invalid executions may have guest memory effects; no rollback is promised
  /// and their output state does not certify guest semantics.
  SourceFunctionTypeHint SourceABI;
};

/// Layout of generated return writeback; all choices preserve the same state
/// and status contract. Existing overloads retain inline returns. SharedGPRExit
/// keeps packed-flags stores at the original exits and shares the GPR stores
/// when a fresh, complete ordinary join can be formed. Otherwise it falls back
/// to inline returns. The LLVM source route retains its own tail merging.
enum class InterpreterMachineStateLayout { InlineReturns, SharedGPRExit };

/// Wrap a complete, call-free residual into an explicit machine-state source
/// function. Loads and stores bind every supported native input/output. Guest
/// register identities are relocated, so a guest RSP never becomes a compiler
/// private host frame. Guest constants become raw numeric values; source
/// emission must not supply an image for rebasing guest memory accesses.
/// Optional EntryAlignment checks entry RSP before guest accesses or state
/// writes. A rejected alignment returns status 2 with the state unchanged.
/// Ordinary scalar LowIR remains owned by LowToMed and its
/// existing source backends; this wrapper does not evaluate those operations.
///
/// State is committed at a native RETURN boundary, before the architectural
/// return-address pop. Fault/unwind equivalence and binary replacement are not
/// certified. Every guest memory access must succeed, and state storage must
/// not alias guest memory. Dynamic POPFQ images outside the profile set a
/// sticky failure status. Such execution is not a valid recovered result.
/// Every guest write range must be disjoint from the entry return-address
/// slot; the native recovery boundary validates root-derived writes and
/// requires the caller to guarantee this for computed external addresses.
/// Exact PUSHFQ/POPFQ intrinsics are projected through user-mode packed flags;
/// all other intrinsics, calls, indirect branches, and unknown registers fail.
llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineStateX64(
    const LowFunc &Residual, BinaryFormat SourceFormat = BinaryFormat::ELF,
    InterpreterMachineStateProfile Profile =
        InterpreterMachineStateProfile::UserX64NoFaultV1);
llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineStateX64(
    const LowFunc &Residual, BinaryFormat SourceFormat,
    InterpreterMachineStateProfile Profile,
    std::optional<InterpreterEntryAlignment> EntryAlignment);
llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineStateX64(
    const LowFunc &Residual, BinaryFormat SourceFormat,
    InterpreterMachineStateProfile Profile,
    std::optional<InterpreterEntryAlignment> EntryAlignment,
    InterpreterMachineStateLayout Layout);

/// Wrap the scalar machine domain with the explicit ordered CPU-query ABI.
/// Canonical CPUID bundles consume exactly one matching record and clear the
/// four result registers' high halves. Exhaustion returns status 3; selector
/// mismatch returns status 4. Both stop before that query, commit the current
/// machine state, and leave QueryCursor unchanged. Earlier guest effects are
/// not rolled back. Entry alignment still rejects before any state write.
/// Other intrinsic and raw-environment restrictions remain unchanged. This
/// interface supplies source, not a native equivalence certificate or a
/// hardware serialization/concurrency model. The existing v1 wrappers continue
/// to refuse CPU queries; they do not infer or read this extended layout.
llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineQueryStateX64(
    const LowFunc &Residual, BinaryFormat SourceFormat = BinaryFormat::ELF,
    InterpreterMachineStateProfile Profile =
        InterpreterMachineStateProfile::UserX64NoFaultV1,
    std::optional<InterpreterEntryAlignment> EntryAlignment = std::nullopt,
    InterpreterMachineStateLayout Layout =
        InterpreterMachineStateLayout::InlineReturns);

/// The same ordered-query contract with complete XMM input/output storage.
/// Whole 128-bit register operands and instruction-local temporary byte views
/// are translated through ordinary scalar LowIR. Unbound temporary bytes and
/// function-temporary aliases fail. Constant alignment checks require an exact
/// proof; dynamic checks still fail. The old wrappers never read this layout.
llvm::Expected<InterpreterMachineSource> wrapInterpreterMachineQueryStateX64V2(
    const LowFunc &Residual, BinaryFormat SourceFormat = BinaryFormat::ELF,
    InterpreterMachineStateProfile Profile =
        InterpreterMachineStateProfile::UserX64NoFaultV1,
    std::optional<InterpreterEntryAlignment> EntryAlignment = std::nullopt,
    InterpreterMachineStateLayout Layout =
        InterpreterMachineStateLayout::InlineReturns);

/// Generate the source wrapper and its register-state model through the same
/// scalar, lane, flag and control-flow rules. No rewritten-wrapper pattern
/// recognition, C compilation or equivalence proof is performed here. Invalid
/// entry flags and dynamic rejected flag writes retain the sticky status.
/// Observations, entry domain, frame contract and proof budgets remain the
/// caller's responsibility. The model by itself supplies no native/source
/// certificate. MaxOperations bounds input metadata and generated operations.
/// Analysis models retain only the executable graph and declared entry roots;
/// diagnostic strings and source/relocation provenance are not copied.
llvm::Expected<InterpreterMachineStateModel> modelInterpreterMachineStateX64(
    const LowFunc &Residual,
    InterpreterMachineStateProfile Profile =
        InterpreterMachineStateProfile::UserX64NoFaultV1,
    uint64_t MaxOperations =
        ::neverd::limits::analysis::kInterpreterMachineStateMaxOperations);
llvm::Expected<InterpreterMachineStateModel> modelInterpreterMachineStateX64(
    const LowFunc &Residual, InterpreterMachineStateProfile Profile,
    uint64_t MaxOperations,
    std::optional<InterpreterEntryAlignment> EntryAlignment);
llvm::Expected<InterpreterMachineStateModel> modelInterpreterMachineStateX64(
    const LowFunc &Residual, InterpreterMachineStateProfile Profile,
    uint64_t MaxOperations,
    std::optional<InterpreterEntryAlignment> EntryAlignment,
    InterpreterMachineStateLayout Layout);

} // namespace neverd::analysis

#endif
