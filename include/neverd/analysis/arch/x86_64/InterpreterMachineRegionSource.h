//===- InterpreterMachineRegionSource.h - Physical exit C ABI ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checked source consumption of authenticated x64 machine-region results.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_INTERPRETERMACHINEREGIONSOURCE_H
#define NEVERD_ANALYSIS_ARCH_X86_64_INTERPRETERMACHINEREGIONSOURCE_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/analysis/Limits.h"
#include "neverd/analysis/arch/x86_64/InterpreterMachineState.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace neverd::analysis {

enum class InterpreterMachineRegionExitX64V1 : uint64_t {
  None = 0,
  Call = 1,
  Jump = 2,
  Return = 3,
  /// Stop before the trap instruction; no exception has been delivered.
  TrapPending = 4,
};

struct InterpreterMachineRegionSourceOptions {
  /// Keep an authenticated trap as a pre-instruction source exit. Its PC is
  /// the original trap address, not a handler or a post-trap resume address.
  /// The caller owns exception delivery; the source supplies no continuation.
  bool StopBeforeTraps = false;
  /// Choice-source wrapper only: bound both input and generated LowOps,
  /// including guards and rejected exits. No partial source is returned.
  uint64_t MaxChoiceSourceOperations =
      limits::analysis::kInterpreterMachineRegionChoiceSourceMaxOperations;
};

/// Separate storage; earlier machine-source layouts are never extended.
/// The generated function fixes its entry. PC and ExitKind are outputs, not
/// a dispatcher input. A successful transfer commits its real target and
/// physical stack effects, then stops before executing the target. No callee
/// ABI or preservation is assumed. State and query storage retain the prefix's
/// accessibility and guest-memory separation contracts.
struct alignas(8) InterpreterMachineRegionStateX64V1 {
  InterpreterMachineQueryStateX64V2 Core;
  uint64_t PC = 0;
  uint64_t ExitKind = 0;
};

static_assert(std::is_standard_layout_v<InterpreterMachineRegionStateX64V1>);
static_assert(offsetof(InterpreterMachineRegionStateX64V1, Core) == 0);
static_assert(offsetof(InterpreterMachineRegionStateX64V1, PC) == 416);
static_assert(offsetof(InterpreterMachineRegionStateX64V1, ExitKind) == 424);
static_assert(sizeof(InterpreterMachineRegionStateX64V1) == 432);

/// One production in application order within the original instruction.
/// Cursor position, not PC alone, distinguishes repeated dynamic visits.
struct InterpreterArchitecturalChoiceX64V1 {
  uint64_t PC = 0;
  uint64_t Ordinal = 0;
  uint64_t BitCount = 0;
  uint64_t Value = 0;
};

enum class InterpreterMachineRegionChoiceStatusX64V1 : uint64_t {
  Exhausted = 5,
  Mismatch = 6,
};

static_assert(std::is_standard_layout_v<InterpreterArchitecturalChoiceX64V1>);
static_assert(sizeof(InterpreterArchitecturalChoiceX64V1) == 32);
static_assert(offsetof(InterpreterArchitecturalChoiceX64V1, PC) == 0);
static_assert(offsetof(InterpreterArchitecturalChoiceX64V1, Ordinal) == 8);
static_assert(offsetof(InterpreterArchitecturalChoiceX64V1, BitCount) == 16);
static_assert(offsetof(InterpreterArchitecturalChoiceX64V1, Value) == 24);

/// A separate layout, never an implicit extension of the 432-byte state.
/// Choices is a host pointer to ChoiceCount accessible, stable records. The
/// records and state must be disjoint from guest accesses and from each other;
/// the caller owns their lifetime and excludes concurrent mutation. Values
/// must fit BitCount. No native processor's choice is inferred by this ABI.
struct alignas(8) InterpreterMachineRegionChoiceStateX64V1 {
  InterpreterMachineRegionStateX64V1 Core;
  uint64_t Choices = 0;
  uint64_t ChoiceCount = 0;
  uint64_t ChoiceCursor = 0;
};

static_assert(
    std::is_standard_layout_v<InterpreterMachineRegionChoiceStateX64V1>);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V1, Core) == 0);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V1, Choices) ==
              432);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V1, ChoiceCount) ==
              440);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V1,
                       ChoiceCursor) == 448);
static_assert(sizeof(InterpreterMachineRegionChoiceStateX64V1) == 456);

/// Explicit segmented-memory inputs, separate from the V1 allocation.
/// FSBase and GSBase are arbitrary long-mode bases, stable for this region.
/// Their linear addresses may alias each other, the frame and any other guest
/// memory. The entire state and both tapes retain V1's separation contract.
/// No host segment state, operating-system layout or base mutation is implied.
struct alignas(8) InterpreterMachineRegionChoiceStateX64V2 {
  InterpreterMachineRegionChoiceStateX64V1 Core;
  uint64_t FSBase = 0;
  uint64_t GSBase = 0;
};

static_assert(
    std::is_standard_layout_v<InterpreterMachineRegionChoiceStateX64V2>);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V2, Core) == 0);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V2, FSBase) ==
              456);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V2, GSBase) ==
              464);
static_assert(sizeof(InterpreterMachineRegionChoiceStateX64V2) == 472);

/// Separate kernel/PGE allocation with the complete V2 prefix. CR4 is a
/// caller-supplied valid value under X64CR4PGEState's complete execution
/// domain; it is never read from or written to the host processor.
/// State/tape/guest separation and stability requirements apply to the entire
/// allocation.
struct alignas(8) InterpreterMachineRegionChoiceStateX64V3 {
  InterpreterMachineRegionChoiceStateX64V2 Core;
  uint64_t CR4 = 0;
};

enum class InterpreterMachineRegionControlStatusX64V3 : uint64_t {
  /// A non-PGE CR4 change was refused before that instruction's effects.
  Rejected = 7,
};

static_assert(
    std::is_standard_layout_v<InterpreterMachineRegionChoiceStateX64V3>);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V3, Core) == 0);
static_assert(offsetof(InterpreterMachineRegionChoiceStateX64V3, CR4) == 472);
static_assert(sizeof(InterpreterMachineRegionChoiceStateX64V3) == 480);

struct InterpreterMachineRegionSource {
  InterpreterMachineSource Source;
  /// Validated original environment and graph receipt, before source lowering.
  /// Retain its caller-supplied premises with the generated source. It binds
  /// the original recovery result, not subsequent edits to Source.Function.
  SpecializationRawEnvironmentReceipt Environment;
  std::string StateABI = "x64-machine-region-state-v1";
  uint64_t StateBytes = sizeof(InterpreterMachineRegionStateX64V1);
};

/// Validate the complete raw result against independently expected options
/// before consuming its environment marker or physical exit spans. Admit only
/// Call/Jump/Return regions, the user nonfaulting flag profile and the existing
/// scalar/query/XMM source operations. Traps, supplemental code/imports, entry
/// objects and extra architectural banks are refused even when unused.
///
/// Return statuses retain machine-source meanings: 0 is a completed region,
/// 1 an invalid flag-profile execution, 2 an unchanged alignment rejection,
/// 3 query exhaustion and 4 query mismatch. For 3/4, PC names the original
/// unexecuted query and ExitKind is None; preceding effects remain committed.
/// Status 1 supplies no valid continuation. Caller-owned memory premises are
/// not inferred here. Entry alignment is checked before effects. This is not
/// ordinary ABI recovery,
/// call composition, whole-function coverage or native-equivalence proof.
/// Architectural undefined outputs retain LowIR's selected implementation
/// values; native observations still require an independent noninterference
/// proof or an explicit architectural arbitrary-value environment.
/// Authenticated dynamic instruction-alignment guards remain in source and
/// use the C backend's fail-fast checks. Failure supplies no committed-state
/// or exception-continuation contract; valid guest accesses remain required.
/// Emit guest accesses with the C backend's byte copies or alignment-1 alias
/// types; the default listing casts do not supply arbitrary byte aliasing.
llvm::Expected<InterpreterMachineRegionSource>
wrapInterpreterMachineRegionSourceX64(
    const SpecializationResult &Result, const SpecializationOptions &Expected,
    BinaryFormat SourceFormat = BinaryFormat::ELF);

/// Explicit trap policy for the same state layout and return statuses.
/// StopBeforeTraps admits only traps authenticated by the raw-region owner.
/// A TrapPending exit commits all earlier effects and the complete pre-trap
/// state, without executing the trap, pushing an exception frame or entering
/// a handler. The default overload still refuses all trap declarations.
llvm::Expected<InterpreterMachineRegionSource>
wrapInterpreterMachineRegionSourceX64(
    const SpecializationResult &Result, const SpecializationOptions &Expected,
    const InterpreterMachineRegionSourceOptions &SourceOptions,
    BinaryFormat SourceFormat = BinaryFormat::ELF);

/// Requires FreshScalarV1 and the separate 456-byte choice state. Other region
/// premises and statuses are unchanged. Status 5 means choice exhaustion;
/// status 6 means a PC/ordinal/width/value mismatch. Both stop before the
/// original instruction with ExitKind=None, preceding effects committed and
/// its entry ChoiceCursor unchanged. All of an instruction's records are
/// checked before any of its effects. Advance only after the whole instruction;
/// false guards still consume records. Copies and spills consume none.
/// This supplies executable raw-region source, not native-equivalence proof.
llvm::Expected<InterpreterMachineRegionSource>
wrapInterpreterMachineRegionChoiceSourceX64V1(
    const SpecializationResult &Result, const SpecializationOptions &Expected,
    const InterpreterMachineRegionSourceOptions &SourceOptions = {},
    BinaryFormat SourceFormat = BinaryFormat::ELF);

/// Requires the separate 472-byte state. In addition to V1 operations, admits
/// authenticated X64SegmentedReads/Writes of 1/2/4/8 bytes at 64-bit offsets.
/// Base plus offset wraps at 64 bits before the ordinary byte-aliasing access.
/// Caller-owned nonfaulting memory premises still apply. FSBase/GSBase remain
/// unchanged on every supplied exit; statuses and instruction reservations
/// retain V1 semantics. Other architectural banks and base-changing operations
/// are refused. This API does not obtain bases from the host or infer them.
llvm::Expected<InterpreterMachineRegionSource>
wrapInterpreterMachineRegionChoiceSourceX64V2(
    const SpecializationResult &Result, const SpecializationOptions &Expected,
    const InterpreterMachineRegionSourceOptions &SourceOptions = {},
    BinaryFormat SourceFormat = BinaryFormat::ELF);

/// Explicit KernelX64NoFaultV1 and X64CR4PGEState source, using the separate
/// 480-byte V3 state. All V2 scalar/segmented operations retain their ordinary
/// memory premises. Kernel flag transitions use the shared CPL0 rules;
/// other architectural banks and the SMAP flag profile remain unsupported.
/// Full CR4 reads and PGE-only writes use the raw owner's value transition.
/// Instruction choice reservations precede effects. Status 7 names the original
/// unexecuted CR4 write in PC, with ExitKind None, unchanged CR4 and choice
/// cursor, and all preceding instructions committed. Reservation exhaustion
/// or mismatch has priority over this control check. A successful region
/// commits full CR4; other status meanings remain those of V1/V2.
/// This is a scalar source abstraction in the declared stable-translation
/// domain, without host control transfers, TLB effects or native proof.
llvm::Expected<InterpreterMachineRegionSource>
wrapInterpreterMachineRegionChoiceSourceX64V3(
    const SpecializationResult &Result, const SpecializationOptions &Expected,
    const InterpreterMachineRegionSourceOptions &SourceOptions = {},
    BinaryFormat SourceFormat = BinaryFormat::ELF);

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_INTERPRETERMACHINEREGIONSOURCE_H
