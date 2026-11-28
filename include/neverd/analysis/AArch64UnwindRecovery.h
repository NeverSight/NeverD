//===- AArch64UnwindRecovery.h - Implicit Darwin unwind rows -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recover incoming-return signing from reachable instructions and the owning
/// frame's declared register locations or surviving register and stack values.
/// Resolve outside signal handlers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_AARCH64UNWINDRECOVERY_H
#define NEVERD_ANALYSIS_AARCH64UNWINDRECOVERY_H

#include "neverd/loader/ReturnAddressSigning.h"
#include "neverd/runtime/DarwinArm64eUnwindRow.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {
namespace aarch64_unwind {
#define NEVERD_UNWIND_VALUE(Type, Name, Value)                                 \
  inline constexpr Type Name = Value;
#include "neverd/analysis/AArch64UnwindRecovery.def"
} // namespace aarch64_unwind

struct AArch64UnwindRegister {
  enum Kind { SameValue, AtCFA, Undefined } Location = SameValue;
  int64_t Offset = 0;
  bool operator==(const AArch64UnwindRegister &) const = default;
};

/// Every incoming path put its live LR's own signing PC in this GPR.
/// PCs are image addresses, prior to the runtime slide.
struct AArch64LiveSigningModifier {
  unsigned Register;
  dwarf_eh::AArch64RAKey Key;
  llvm::SmallVector<va_t, 2> SigningPCs;
  bool operator==(const AArch64LiveSigningModifier &) const = default;
};

struct AArch64RecoveredUnwindRow {
  uint64_t CodeOffset = 0;
  std::optional<unsigned> CFARegister;
  int64_t CFAOffset = 0;
  // GPR columns and the low and high lanes of V8-V15 at kFirstDSlot
  // and kFirstHighSlot. Unavailable upper lanes are caller-clobbered.
  std::array<AArch64UnwindRegister, aarch64_unwind::kFrameRegisters> Registers;
  dwarf_eh::AArch64RAQueryResult Signing;
  /// Live frame lower bound, relative to the incoming SP, when proved.
  std::optional<int64_t> StackOffset;
  /// Live FP relative to incoming SP, independently of the chosen CFA base.
  std::optional<int64_t> FramePointerOffset;
  /// One authenticated entry instruction tail-branches before entering a scope.
  bool IsForwardingEntry = false;
  std::optional<AArch64LiveSigningModifier> LiveSigningModifier;
};

struct AArch64UnwindRecoveryLimits {
  size_t MaxInstructions = aarch64_unwind::kMaxInstructions;
  size_t MaxTransitions = aarch64_unwind::kMaxTransitions;
  size_t MaxStackCells = aarch64_unwind::kMaxStackCells;
};

/// Rows start at exact instruction boundaries and extend to the next row or
/// frame end. Unknown joins and unreachable instructions remain Unknown.
/// A DWARF row with a proven CFA retains its declared register locations.
/// Compact and partial-prologue/epilogue rows use live incoming values or
/// active frame saves. A declared undefined register stays undefined.
/// A range consisting of forwarding entries gets one live-LR row per branch.
/// Intervening bytes have no recovery rule. Explicit signing columns retain
/// the loader's separate metadata query.
/// Coalesced unwind-only compact ranges use independent known function
/// entries; returned offsets remain relative to the shared metadata range.
llvm::Expected<std::vector<AArch64RecoveredUnwindRow>>
recoverDarwinAArch64Unwind(const BinaryImage &Image,
                           const ExceptionFunction &Function,
                           const AArch64UnwindRecoveryLimits &Limits = {});

/// Effective source rows retained while installing a classic B-key/SP object.
/// They include remembered-state restores. Epilogues are post-AUT directives.
struct AArch64ClassicReturnCFI {
  struct SigningRow {
    uint64_t Offset;
    bool Signed;
  };
  llvm::SmallVector<SigningRow, 2> SigningRows;
  llvm::SmallVector<uint64_t, 1> Epilogues;
};

/// Replay the relocated frame with the validated input object's signing rows.
/// At a dynamic epilogue include every in-frame PC compatible with the source
/// unwind contract and the restored caller state.
llvm::Expected<std::vector<AArch64RecoveredUnwindRow>>
recoverDarwinAArch64ObjectUnwind(const BinaryImage &Image,
                                 const ExceptionFunction &Function,
                                 const AArch64ClassicReturnCFI &Source);

/// The caller has already checked that Offset belongs to the recovered frame.
const AArch64RecoveredUnwindRow &
aarch64UnwindRowAt(llvm::ArrayRef<AArch64RecoveredUnwindRow> Rows,
                   uint64_t Offset);

struct AArch64SystemCallerPlan {
  NDDarwinArm64eUnwindRow Frame{};
  std::array<NDDarwinArm64eSavedGPR, NDArm64ePreservedGPRCount> Saved{};
  size_t Count = 0;
  NDDarwinArm64eVectorSaves Vectors{};
};

/// Bind a recovered row to its loaded frame address outside a signal handler.
/// Unknown signing, unrepresentable registers and out-of-frame signing PCs
/// cannot initialize a native cursor. Vectors.Complete additionally reports
/// whether the ordinary Darwin call ABI can resume execution.
std::optional<AArch64SystemCallerPlan>
prepareDarwinArm64eSystemCaller(const AArch64RecoveredUnwindRow &Row,
                                ExceptionAddressRange Range,
                                va_t LoadedFrameStart);

/// A snapshot at this exact instruction can resolve a live modifier at a
/// control-flow join. The GPR must contain one of the proven, slid signing PCs.
std::optional<AArch64SystemCallerPlan> prepareDarwinArm64eSystemCaller(
    const AArch64RecoveredUnwindRow &Row, ExceptionAddressRange Range,
    va_t LoadedFrameStart, llvm::ArrayRef<va_t> InterruptedGPRs);

/// Bind the instruction and its recovery row to the language table and its
/// actual load address. A forwarding range contains no cleanup. A protected
/// call keeps the interrupted frame. A transparent prefix must belong to its
/// protected range or lead straight back to the same call from its return edge.
/// An explicitly guarded non-call can supply its own return edge when both
/// instruction boundaries have the same complete recovery row.
/// A live LR additionally needs matching system DWARF or compact frameless
/// rows. Delivery authenticates its recovered recipe, adapts a separate LR
/// column to Darwin's unwind ABI and restores the original for the landing pad.
std::optional<NDDarwinArm64eExceptionSite>
prepareDarwinArm64eExceptionSite(const BinaryImage &Image,
                                 const ExceptionFunction &Function,
                                 llvm::ArrayRef<AArch64RecoveredUnwindRow> Rows,
                                 va_t PC, va_t LoadedFrameStart);

struct AArch64AnalyzedUnwindSigning {
  AArch64UnwindSigningQuery Metadata;
  std::optional<dwarf_eh::AArch64RAQueryResult> InstructionStateAtPC;
};

/// Preserve metadata provenance even when instruction recovery succeeds.
AArch64AnalyzedUnwindSigning
analyzeAArch64UnwindSigning(const BinaryImage &Image,
                            const ExceptionFunction &Function, va_t PC,
                            const AArch64UnwindRecoveryLimits &Limits = {});

} // namespace neverd

#endif // NEVERD_ANALYSIS_AARCH64UNWINDRECOVERY_H
