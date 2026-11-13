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

struct AArch64RecoveredUnwindRow {
  uint64_t CodeOffset = 0;
  std::optional<unsigned> CFARegister;
  int64_t CFAOffset = 0;
  // GPR columns and the low and high lanes of V8-V15 at kFirstDSlot
  // and kFirstHighSlot. Unavailable upper lanes are caller-clobbered.
  std::array<AArch64UnwindRegister, aarch64_unwind::kFrameRegisters> Registers;
  dwarf_eh::AArch64RAQueryResult Signing;
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
/// Explicit signing columns retain the loader's separate metadata query.
llvm::Expected<std::vector<AArch64RecoveredUnwindRow>>
recoverDarwinAArch64Unwind(const BinaryImage &Image,
                           const ExceptionFunction &Function,
                           const AArch64UnwindRecoveryLimits &Limits = {});

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

/// Bind the instruction and its recovery row to the language table and its
/// actual load address. A forwarding range contains no cleanup. A protected
/// call owns its landing pad and must keep its interrupted frame on the stack.
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
