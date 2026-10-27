//===- InternalNoReturn.cpp - No-return proofs for internal callees -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The LowIR counterpart of the MedIR no-return proof (MedNoReturn.h), run on
/// a callee lifted on demand so that a single-function decompile can cut the
/// path after a call into an internal routine that never returns, such as a
/// wrapper around a bug check.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/InternalNoReturn.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/libc/LibCNames.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace neverd {

namespace {
/// Whether the user states of some function that it never returns.
bool statesNoReturn(const BinaryImage &Img) {
  return llvm::any_of(Img.UserNoReturn,
                      [](const auto &Stated) { return Stated.second; });
}
} // namespace

InternalNoReturnIndex::InternalNoReturnIndex(
    const BinaryImage &Img, const std::set<va_t> *KnownFuncEntries,
    const libc::NoReturnTargetIndex *NoReturnTargets,
    const detail::AbsoluteRelocationRootIndex *Roots,
    const ExecutableCodeOwnerIndex *CodeOwners)
    : Img(Img), KnownFuncEntries(KnownFuncEntries),
      NoReturnTargets(NoReturnTargets), Roots(Roots), CodeOwners(CodeOwners) {}

void InternalNoReturnIndex::clearMemoizedProofs() {
  std::lock_guard<std::mutex> Lock(Mutex);
  Proofs.clear();
  Screens.clear();
  Instructions.clear();
  Transfers.clear();
  StatedCalls.clear();
}

void InternalNoReturnIndex::trimMemoizedProofs(size_t MaxAnswers) {
  std::lock_guard<std::mutex> Lock(Mutex);
  Instructions.clear();
  Transfers.clear();
  if (Proofs.size() + Screens.size() + StatedCalls.size() > MaxAnswers) {
    Proofs.clear();
    Screens.clear();
    StatedCalls.clear();
  }
}

bool InternalNoReturnIndex::neverReturns(va_t Target, unsigned Depth) const {
  if (const auto Stated = Img.userNoReturnAt(Target))
    return *Stated;
  if (Depth >= limits::kMaxNoReturnProofDepth ||
      !Img.hasExecutableCodeOwnerAt(Target))
    return false;
  const auto Key = std::make_pair(Target, Depth);
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (auto It = Proofs.find(Key); It != Proofs.end())
      return It->second;
  }
  // Two builds may prove the same callee at once; both reach the same
  // answer, so the lock is not held while lifting.
  Decoder Dec;
  bool Proved = false;
  if (Dec.init(Img)) {
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(KnownFuncEntries);
    Builder.setNoReturnTargetIndex(NoReturnTargets);
    Builder.setAbsoluteRelocationRootIndex(Roots);
    Builder.setExecutableCodeOwnerIndex(CodeOwners);
    Builder.setNoReturnCalleeProver(this, Depth + 1);
    Proved = Builder.buildNoReturnProof(Img, Dec, Target,
                                        Img.getFunctionNameAt(Target));
  }
  std::lock_guard<std::mutex> Lock(Mutex);
  return Proofs.emplace(Key, Proved).first->second;
}

bool InternalNoReturnIndex::mayNeverReturn(va_t Target) const {
  // What the user states never returns is rare and deliberate, so a callee
  // whose returns all follow calls into it is worth the proof: it still
  // holds those returns, but no path reaches them.
  return nothingReturns(Target) ||
         (statesNoReturn(Img) &&
          returnsFollowStatedCalls(Target, limits::kMaxNoReturnProofDepth));
}

va_t InternalNoReturnIndex::screenedEnd(va_t Target) const {
  va_t End = Img.getFunctionMetadataEnd(Target, CodeOwners);
  if (const ExceptionFunction *Unwind =
          Img.ExceptionMetadata.findFunctionEntry(Target);
      Unwind && Unwind->Kind == RuntimeFunctionKind::Primary &&
      Unwind->CodeRange.isValid())
    End = Unwind->CodeRange.End;
  const Segment *Seg = Img.getSegmentFor(Target);
  if (End == InvalidVA || End <= Target ||
      End - Target > limits::kMaxNoReturnScreenBytes || !Seg ||
      Target < Seg->VA || End - Seg->VA > Seg->Data.size())
    return InvalidVA;
  return End;
}

bool InternalNoReturnIndex::decodeScreenedRange(
    va_t Target,
    llvm::function_ref<bool(Decoder &, const DecodedInsn &)> Visit) const {
  const va_t End = screenedEnd(Target);
  const Segment *Seg = Img.getSegmentFor(Target);
  Decoder Dec;
  if (End == InvalidVA || !Dec.init(Img))
    return false;
  for (va_t At = Target; At < End;) {
    DecodedInsn Insn;
    const int Size =
        Dec.selectMode(Img, At)
            ? Dec.decodeOne(Seg->Data.data() + (At - Seg->VA),
                            static_cast<size_t>(End - At), At, Insn)
            : 0;
    if (Size <= 0)
      return false;
    if (!Visit(Dec, Insn))
      break;
    At += static_cast<va_t>(Size);
  }
  return true;
}

bool InternalNoReturnIndex::nothingReturns(va_t Target) const {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (auto It = Screens.find(Target); It != Screens.end())
      return It->second;
  }
  bool Returns = false;
  // Straight-line flow ends at a terminator other than a resumable trap, and
  // at a call to a routine stated or known by name never to return; padding
  // after either does not resume it. A return there is reached only by a
  // branch: unless one in the range targets it, nothing reaches it.
  const auto CallNeverReturns = [&](va_t Callee) {
    if (const auto Stated = Img.userNoReturnAt(Callee))
      return *Stated;
    return libc::isNoReturnTarget(Img, Callee, NoReturnTargets);
  };
  // Each such return, with the padding before it: a branch to either reaches
  // the return.
  std::vector<va_t> Unreached, Padding;
  bool FlowEnded = false;
  const bool Decoded =
      decodeScreenedRange(Target, [&](Decoder &Dec, const DecodedInsn &Insn) {
        const std::optional<bool> InsnReturns = Dec.returnsToCaller(Insn);
        const bool Ended = FlowEnded && Insn.Addr != Target;
        if (InsnReturns && *InsnReturns && Ended) {
          Unreached.insert(Unreached.end(), Padding.begin(), Padding.end());
          Unreached.push_back(Insn.Addr);
          Padding.clear();
          return true;
        }
        Returns = !InsnReturns || *InsnReturns;
        if (Returns)
          return false;
        const va_t Callee = Dec.directCallTarget(Insn);
        const bool Pads = Ended && Dec.isAlignmentNop(Insn);
        if (Pads)
          Padding.push_back(Insn.Addr);
        else
          Padding.clear();
        FlowEnded =
            (Dec.isFunctionTerminator(Insn) && !Dec.isResumableTrap(Insn)) ||
            (Callee != InvalidVA && CallNeverReturns(Callee)) || Pads;
        return true;
      });
  if (Decoded && !Returns && !Unreached.empty()) {
    const ScreenedTransfers &Branches = screenedTransfers(Target);
    Returns = !Branches.Known || llvm::any_of(Unreached, [&](va_t At) {
      return Branches.Inside.count(At);
    });
  }
  std::lock_guard<std::mutex> Lock(Mutex);
  return Screens.emplace(Target, Decoded && !Returns).first->second;
}

const std::vector<InternalNoReturnIndex::ScreenedInstruction> &
InternalNoReturnIndex::screenedInstructions(va_t Target) const {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (auto It = Instructions.find(Target); It != Instructions.end())
      return It->second;
  }
  std::vector<ScreenedInstruction> Found;
  if (!decodeScreenedRange(Target, [&](Decoder &Dec, const DecodedInsn &Insn) {
        Found.push_back({Insn.Addr, Dec.directCallTarget(Insn),
                         Dec.returnsToCaller(Insn),
                         Dec.isFunctionTerminator(Insn)});
        return true;
      }))
    Found.clear();
  std::lock_guard<std::mutex> Lock(Mutex);
  return Instructions.emplace(Target, std::move(Found)).first->second;
}

const InternalNoReturnIndex::ScreenedTransfers &
InternalNoReturnIndex::screenedTransfers(va_t Target) const {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (auto It = Transfers.find(Target); It != Transfers.end())
      return It->second;
  }
  ScreenedTransfers Found;
  const va_t End = screenedEnd(Target);
  bool Unknown = false;
  const bool Decoded =
      decodeScreenedRange(Target, [&](Decoder &Dec, const DecodedInsn &Insn) {
        std::vector<LowOp> Ops;
        // A byte no lifter reads leaves the range's branches unknown, as it
        // ends a CFG build.
        try {
          Dec.liftToLow(Insn, Ops);
        } catch (const UnliftedInstruction &) {
          Unknown = true;
          return false;
        }
        for (const LowOp &Op : Ops) {
          if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
              Op.Inputs[0].isConst() &&
              isX86BoundedLeafPending(
                  static_cast<Intrinsic>(Op.Inputs[0].Offset))) {
            // The screen has no incoming-register proof. A pending ENCLU or
            // GETSEC may still be a nonlocal machine transition.
            Unknown = true;
          } else if (Op.Opcode == NdOp::INDIR_BR) {
            Unknown = true;
          } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
            if (Op.NumInputs == 0 || !Op.Inputs[0].isConst()) {
              Unknown = true;
              continue;
            }
            const va_t To = Op.Inputs[0].Offset;
            (To >= Target && To < End ? Found.Inside : Found.Leaving)
                .insert(To);
          }
        }
        return !Unknown;
      });
  Found.Known = Decoded && !Unknown;
  std::lock_guard<std::mutex> Lock(Mutex);
  return Transfers.emplace(Target, std::move(Found)).first->second;
}

bool InternalNoReturnIndex::returnsFollowStatedCalls(va_t Target,
                                                     unsigned Levels) const {
  if (Levels == 0)
    return false;
  const auto Key = std::make_pair(Target, Levels);
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (auto It = StatedCalls.find(Key); It != StatedCalls.end())
      return It->second;
  }
  const std::vector<ScreenedInstruction> &Insns = screenedInstructions(Target);
  // Each return, with the call it follows.
  std::vector<std::pair<size_t, size_t>> Followed;
  bool Holds = true;
  for (size_t I = 0; Holds && I < Insns.size(); ++I) {
    if (!Insns[I].Returns) {
      Holds = false;
      break;
    }
    if (!*Insns[I].Returns)
      continue;
    size_t Call = I;
    while (Call > 0 && Insns[Call - 1].CallTarget == InvalidVA &&
           !Insns[Call - 1].Ends)
      --Call;
    Holds = Call > 0 && Insns[Call - 1].CallTarget != InvalidVA &&
            cannotComeBack(Insns[Call - 1].CallTarget, Target, Levels - 1);
    if (Holds)
      Followed.emplace_back(Call - 1, I);
  }
  Holds = Holds && !Followed.empty();
  // No branch may enter between a call and the return after it.
  if (Holds) {
    const ScreenedTransfers &Branches = screenedTransfers(Target);
    Holds = Branches.Known &&
            llvm::none_of(Followed, [&](const std::pair<size_t, size_t> &At) {
              for (size_t K = At.first + 1; K <= At.second; ++K)
                if (Branches.Inside.count(Insns[K].Addr))
                  return true;
              return false;
            });
  }
  std::lock_guard<std::mutex> Lock(Mutex);
  return StatedCalls.emplace(Key, Holds).first->second;
}

bool InternalNoReturnIndex::cannotComeBack(va_t Callee, va_t Caller,
                                           unsigned Levels) const {
  if (const auto Stated = Img.userNoReturnAt(Callee))
    return *Stated;
  if (Callee == Caller || !Img.hasExecutableCodeOwnerAt(Callee))
    return false;
  if (returnsFollowStatedCalls(Callee, Levels))
    return true;
  if (Levels == 0 || !nothingReturns(Callee))
    return false;
  // A function with no return leaves by its calls and jumps only, one of
  // which goes where the user's statement leads.
  const auto leads = [&](va_t To) {
    return cannotComeBack(To, Callee, Levels - 1);
  };
  return llvm::any_of(screenedInstructions(Callee),
                      [&](const ScreenedInstruction &Insn) {
                        return Insn.CallTarget != InvalidVA &&
                               leads(Insn.CallTarget);
                      }) ||
         llvm::any_of(screenedTransfers(Callee).Leaving, leads);
}

} // namespace neverd
