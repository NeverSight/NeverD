//===- InternalNoReturn.h - No-return proofs for callees --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Proves that an internal function never returns, so that a direct call to
/// it ends its block during CFG construction like a call to a routine the
/// name list knows.  Only a proof cuts the path: no path of the callee may
/// reach a return, and every one must reach an architectural trap or a call
/// that is itself known or proved never to return.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_INTERNALNORETURN_H
#define NEVERD_IR_LOW_INTERNALNORETURN_H

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

/// Memoized no-return proofs for the internal functions of one unchanged
/// image, shared by the CFG builds of one pipeline run.  A proof lifts the
/// callee with the given CFG settings and this prover, so the callee's own
/// calls are proved in turn, up to limits::kMaxNoReturnProofDepth; a callee
/// past that depth counts as returning.  The answer for a callee and depth
/// does not depend on which build asked first.
class InternalNoReturnIndex final : public NoReturnCalleeProver {
public:
  /// The pointers are borrowed for the lifetime of the index, as by
  /// CFGBuilder's setters of the same names.
  InternalNoReturnIndex(const BinaryImage &Img,
                        const std::set<va_t> *KnownFuncEntries,
                        const libc::NoReturnTargetIndex *NoReturnTargets,
                        const detail::AbsoluteRelocationRootIndex *Roots,
                        const ExecutableCodeOwnerIndex *CodeOwners);

  bool neverReturns(va_t Target, unsigned Depth) const override;

  /// True when the code range unwind or symbol metadata gives the function at
  /// \p Target holds no instruction that returns, decoded in order from its
  /// start.  Without such a range in an x64, ARM or ARM64 PE image, whose
  /// functions but leaves have unwind entries, true when straight-line flow
  /// from the start ends where nothing resumes it (entryFlowNeverResumes),
  /// as a __fastfail stub's does.  Otherwise without such a range, past
  /// limits::kMaxNoReturnScreenBytes, or where a byte does not decode, the
  /// function may return.  Where the user
  /// states of some function that it never returns, also true when each
  /// return in that range follows a call that cannot come back
  /// (returnsFollowStatedCalls): the function still holds its return,
  /// although no path reaches it.  Memoized.
  bool mayNeverReturn(va_t Target) const override;

  /// Begin another run against the same immutable inputs, preserving this
  /// prover's identity but not accumulating memo entries across selections.
  /// The previous run's workers must have joined before calling this.
  void clearMemoizedProofs();
  /// Begin another run against the same immutable inputs, keeping the
  /// answers earlier runs found: they depend only on those inputs.  The
  /// screened instructions and transfers, which grow with every function a
  /// run screens, are dropped, and so are the answers once there are more
  /// than \p MaxAnswers.  The previous run's workers must have joined.
  void trimMemoizedProofs(size_t MaxAnswers);

private:
  /// What the screen reads of an instruction, decoded without lifting.
  struct ScreenedInstruction {
    va_t Addr = InvalidVA;
    /// Where a direct call goes, else InvalidVA.
    va_t CallTarget = InvalidVA;
    /// Whether it returns to the caller; nullopt where no lifter tells.
    std::optional<bool> Returns;
    /// A jump, return or trap: what follows is reached only from elsewhere.
    bool Ends = false;
  };
  /// The direct branches of a screened range, lifted.
  struct ScreenedTransfers {
    /// False when the range holds an indirect branch or a byte that does not
    /// lift, so its branch targets are not all known.
    bool Known = false;
    /// Targets within the range, and those that leave it (tail calls).
    std::set<va_t> Inside;
    std::set<va_t> Leaving;
  };

  /// The end of the code range unwind or symbol metadata gives the function
  /// at \p Target, where that range is screened, else InvalidVA.
  va_t screenedEnd(va_t Target) const;
  /// Decodes the screened code range of the function at \p Target in order,
  /// handing each instruction to \p Visit until it answers false.  False
  /// when there is no such range or a byte does not decode.
  bool decodeScreenedRange(
      va_t Target,
      llvm::function_ref<bool(Decoder &, const DecodedInsn &)> Visit) const;
  /// Whether the screened range of the function at \p Target holds no
  /// instruction that returns.  Memoized.
  bool nothingReturns(va_t Target) const;
  /// Whether straight-line flow from \p Target, within
  /// limits::kMaxNoReturnScreenBytes, reaches an architectural trap that
  /// never resumes, or a call stated or known by name never to return,
  /// before any instruction branches or returns.
  bool entryFlowNeverResumes(va_t Target) const;
  /// The instructions of the screened range of the function at \p Target;
  /// empty where a byte does not decode.  Memoized.
  const std::vector<ScreenedInstruction> &
  screenedInstructions(va_t Target) const;
  /// The direct branches of the screened range of the function at \p Target.
  /// Memoized.
  const ScreenedTransfers &screenedTransfers(va_t Target) const;
  /// Whether the function at \p Target holds a return, and each one follows
  /// the nearest call before it in straight-line code with nothing else
  /// branching in between, a call into a function that cannot come back
  /// (cannotComeBack).  Memoized.
  bool returnsFollowStatedCalls(va_t Target, unsigned Levels) const;
  /// Whether a call from \p Caller to \p Callee cannot come back because of
  /// what the user states: \p Callee is stated never to return, or within
  /// \p Levels further calls its returns follow such calls, or it holds no
  /// return and calls or jumps to such a function.
  bool cannotComeBack(va_t Callee, va_t Caller, unsigned Levels) const;

  const BinaryImage &Img;
  const std::set<va_t> *KnownFuncEntries;
  const libc::NoReturnTargetIndex *NoReturnTargets;
  const detail::AbsoluteRelocationRootIndex *Roots;
  const ExecutableCodeOwnerIndex *CodeOwners;
  mutable std::mutex Mutex;
  mutable std::map<std::pair<va_t, unsigned>, bool> Proofs;
  mutable std::map<va_t, bool> Screens;
  mutable std::map<va_t, std::vector<ScreenedInstruction>> Instructions;
  mutable std::map<va_t, ScreenedTransfers> Transfers;
  mutable std::map<std::pair<va_t, unsigned>, bool> StatedCalls;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_INTERNALNORETURN_H
