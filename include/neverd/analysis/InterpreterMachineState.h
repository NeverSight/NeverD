//===- InterpreterMachineState.h - Explicit recovery source ABI -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETERMACHINESTATE_H
#define NEVERD_ANALYSIS_INTERPRETERMACHINESTATE_H

#include "neverd/analysis/InterpreterEntryAlignment.h"
#include "neverd/analysis/InterpreterMachineStateProfile.h"
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

/// Check the caller-supplied entry state against the fixed user-mode profile.
/// Generated source returns a nonzero status for the same invalid states.
llvm::Error
validateInterpreterMachineStateX64V1(const InterpreterMachineStateX64V1 &State);

struct InterpreterMachineSource {
  LowFunc Function;
  /// One state pointer parameter and an unsigned 64-bit status: zero means
  /// success; nonzero means a flag-profile or entry-domain violation. Invalid
  /// executions may have guest memory effects; no rollback is promised and
  /// their output state does not certify guest semantics.
  SourceFunctionTypeHint SourceABI;
};

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

/// Analysis model of the same source wrapper. Register bytes [0, 136) hold the
/// state object's input and output bytes in InterpreterMachineStateX64V1 order.
/// RETURN carries the wrapper's status, separately from the guest RAX field.
/// Guest memory operations retain their original meaning. The state object is
/// abstracted only under the wrapper's accessible, aligned, disjoint-storage
/// precondition; this is not a model of arbitrary host-memory aliasing.
struct InterpreterMachineStateModel {
  LowFunc Function;
  /// Complete deterministic LowIR records for this generated model. These
  /// are not architecture-undefined-output evidence for the original binary.
  std::vector<LowIRUndefinedInstruction> Instructions;
};

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
    uint64_t MaxOperations = 65536);
llvm::Expected<InterpreterMachineStateModel> modelInterpreterMachineStateX64(
    const LowFunc &Residual, InterpreterMachineStateProfile Profile,
    uint64_t MaxOperations,
    std::optional<InterpreterEntryAlignment> EntryAlignment);

} // namespace neverd::analysis

#endif
