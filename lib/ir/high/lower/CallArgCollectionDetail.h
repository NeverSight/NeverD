//===- CallArgCollectionDetail.h - Per-arch call-argument ABI -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared between CallArgCollection.cpp and the
/// architecture-specific ABI refinements (CallArgCollectionX86.cpp,
/// CallArgCollectionARM.cpp, CallArgCollectionAArch64.cpp) and the
/// calling-convention steps (CallArgCollectionWin64.cpp).  Not public.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_CALLARGCOLLECTIONDETAIL_H
#define NEVERD_IR_HIGH_CALLARGCOLLECTIONDETAIL_H

#include "neverd/Common.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <set>
#include <vector>

namespace neverd {

struct BinaryImage;
class TargetRegInfo;

namespace call_args_detail {

/// Win64 keeps RSI/RDI/XMM6-15; those are not in the SysV CalleeSaveRegs
/// list. Walking past helper CALLs for `mov [rsp+20h], esi` must use the
/// format-aware preserved set, not `isCalleeSaveReg`.
inline bool isCallPreservedReg(const TargetRegInfo &TRI, BinaryFormat Format,
                               uint64_t RegOff, uint16_t Size) {
  if (TRI.isCalleeSaveReg(RegOff))
    return true;
  if (Size == 0)
    return false;
  for (const auto &R : TRI.callPreservedRanges(Format)) {
    if (RegOff >= R.Offset && Size <= R.Bytes &&
        RegOff + static_cast<uint64_t>(Size) <= R.Offset + R.Bytes)
      return true;
  }
  return false;
}

struct CallArgScan {
  const MedFunc *Function = nullptr;
  const std::vector<MedOp> *Ops = nullptr;
  size_t CallIdx = 0;
  uint64_t SpRegOff = 0;
  const TargetRegInfo *TRI = nullptr;
  const BinaryImage *Image = nullptr;
  Arch TheArch = Arch::Unknown;
  /// The calling convention of the call, or null when NeverD has no entry
  /// for it (MedCallConvention.h).
  const CallArgumentConvention *Convention = nullptr;
  int MaxArgs = 0;
  int FirstStackSlot = 0;
  int StoreScanWindow = 0;
  /// The call is a tail jump (CFG building rewrote `jmp callee` as a CALL and
  /// a RETURN of the same instruction): the callee enters on this function's
  /// stack, so its stack arguments sit at the callee-entry offsets from the
  /// entry stack pointer, above the return address on x86, and no argument
  /// was pushed.
  bool TailJump = false;
  /// The call is indirect: as call-ABI recovery does, the scan takes no
  /// prologue save of a register's incoming value for one of its stack
  /// arguments (isIncomingRegisterSave).
  bool Indirect = false;
  /// Register arguments a summarized callee reads (MedOp::CalleeRegisterArgs),
  /// or -1 when the callee has no summary.
  int CalleeRegisterArgs = -1;
  /// Positional arguments its incoming stack reads imply
  /// (MedOp::CalleeStackArgs), or -1 when they are unbounded.
  int CalleeStackArgs = -1;
  /// A checked header variadic prefix is a minimum, not a stack upper bound.
  bool HeaderVariadic = false;
  /// The call is a direct one to code of this image, whose recovered stack
  /// parameters are one per slot.  An import's prototype may instead take an
  /// argument wider than a slot whole.
  bool CalleeInImage = false;
  /// Trailing windows of immediate predecessors of a call-only block.
  /// Each `Before` is the last op index to scan (typically `size()-1`);
  /// the scan stops at the previous CALL, matching same-block setup.
  struct OpWindow {
    const std::vector<MedOp> *Ops = nullptr;
    int Before = -1;
  };
  std::vector<OpWindow> ExtraWindows;
  llvm::function_ref<ExprPtr(const MedVar &)> ToExpr;
  /// The value register argument \p Index holds at the call, for filling a
  /// register slot the call did not visibly write (nullptr when unknown).
  llvm::function_ref<ExprPtr(int)> ReachingRegArg;
  /// Whether argument register \p Index is one of this function's own
  /// parameters, which a forwarder passes through untouched.
  llvm::function_ref<bool(int)> IsOwnParameter;
  /// This function's own stack parameter at argument position \p Index,
  /// which a tail jump passes on unless it stores another value there
  /// (nullptr when it has none).
  llvm::function_ref<ExprPtr(int)> OwnStackParam;
  /// Whether a store to \p Address can be no outgoing argument: it is
  /// below the stack pointer it is made from, through another pointer, or
  /// at a fixed address.
  llvm::function_ref<bool(const MedVar &)> IsNoArgumentStore;
  /// Whether \p Store is one only reads of a PC-rooted value observe: the
  /// get-PC push, or a spill of the GOT address its reloads read.  The GOT
  /// address is never a C argument.
  llvm::function_ref<bool(const MedOp &)> IsPcRootedStore;
  /// Offset of an address from the stack pointer at function entry, when it
  /// resolves through copies and constant adjustments (an `r11 = rsp`
  /// frame); nullopt otherwise.
  llvm::function_ref<std::optional<int64_t>(const MedVar &)> EntryOffsetOf;
  /// The entry offset of the stack pointer the call is made with, where it
  /// is known.
  std::optional<int64_t> CallStackEntryOffset;
  /// The distance of an address above the stack pointer the call is made
  /// with, where both are exact offsets from the entry stack pointer, one
  /// realignment, or the same current stack PHI. A store there occupies the
  /// slot at that distance; an unknown entry coordinate is not zero.
  llvm::function_ref<std::optional<int64_t>(const MedVar &)> CallStackOffsetOf;
  /// Bytes the prologue moved the stack pointer below its entry value.
  int64_t FrameSize = 0;
  /// Entry-relative stack slots the function loads; see MedToHigh.h.
  const std::set<int64_t> *LoadedEntrySlots = nullptr;
  /// Window resolve that peels COPY/SUBBYTES to a defining CALL. Used for
  /// dangling callee-save SSA (`ESI.51`) whose `medvarToExpr` is a clobber.
  llvm::function_ref<ExprPtr(const MedVar &, const std::vector<MedOp> &, int)>
      ResolveWindow;
};

/// What a calling convention's steps of register-argument recovery read and
/// refine for one call (see CallArgPolicy).
struct CallArgContext {
  /// The recovered argument of each position, null where none is known yet.
  std::vector<ExprPtr> &Found;
  int MaxArgs = 0;
  llvm::ArrayRef<uint64_t> ParamRegs;
  const MedBlock &CurBlock;
  /// The function being converted, when there is one.
  const MedFunc *CurMed = nullptr;
  llvm::function_ref<ExprPtr(const MedVar &)> ToExpr;
  llvm::function_ref<ExprPtr(const MedOp &)> OpToExpr;
  /// The argument position a write of an integer register sets, or -1.
  llvm::function_ref<int(uint64_t)> IntegerSlot;
  /// The convention's argument position of a register, or -1.
  llvm::function_ref<int(uint64_t)> RegToArgIdx;
  /// The parameter position of a value of the function being converted.
  llvm::function_ref<int(const MedVar &)> AbiParamIndex;
  /// The SSA value of a register at the entry of the call's block.
  llvm::function_ref<bool(uint64_t, MedVar &)> ReachingAtEntry;
  /// The value of \p V at op \p Before of \p Ops, through the copies and
  /// views a window walk peels.
  llvm::function_ref<ExprPtr(MedVar V, const std::vector<MedOp> &Ops,
                             int Before)>
      FromWindowValue;
  /// Takes position K from a setup write before the call's block: the
  /// predecessor's write of the register reaching the call, or the write
  /// in the fork that dominates a join.  False when there is none.
  llvm::function_ref<bool(int)> TakePrecedingSetup;
  /// Shared indexes for the immutable function being converted.
  llvm::function_ref<const MedOp *(const MedVar &)> UniqueDefinition;
  llvm::function_ref<const PhiNode *(const MedVar &)> UniquePhi;
};

/// The steps of call-argument recovery that belong to one calling
/// convention, each optional.  A convention defines its policy in its own
/// file (CallArgCollectionWin64.cpp) and callArgPolicy() lists it; generic
/// recovery runs a step at its point when the policy has one.
struct CallArgPolicy {
  Arch TheArch = Arch::Unknown;
  /// The image format, or Unknown for every format of the architecture.
  BinaryFormat Format = BinaryFormat::Unknown;
  /// A call alone in a block that has one predecessor can take setup
  /// stores from the end of that predecessor.
  bool ReadsPredecessorWindow = false;
  /// Takes the register-argument writes at the end of that predecessor.
  void (*TakePredecessorRegisters)(CallArgContext &C,
                                   const MedBlock &Pred) = nullptr;
  /// The call's own block writes no argument register: recover what it
  /// passes, and set \p FillLast, the highest position to fill from the
  /// registers reaching the call.
  void (*RecoverCallOnlySetup)(CallArgContext &C, int &FillLast) = nullptr;
  /// Extend the positions up to \p MaxRegArg the block writes with values
  /// that reach the call from before it.
  void (*ExtendWrittenArgs)(CallArgContext &C, int MaxRegArg,
                            int &FillLast) = nullptr;
  /// Name a parameter of the function being converted that an argument
  /// passes through by that parameter.
  void (*ResolvePassThroughParams)(CallArgContext &C) = nullptr;
  /// Fill unwritten register positions with the function's own parameters
  /// for a callee without a summary.  \p HintedCount is the number of
  /// arguments a source binding names, \p SelfCall whether the call is
  /// recursive, and \p OwnParamCount the function's own parameter count.
  void (*FillUnwrittenParams)(CallArgContext &C, size_t HintedCount,
                              bool SelfCall, size_t OwnParamCount) = nullptr;
  /// A recursive call passes exactly the parameters of the signature it
  /// calls.
  bool RecursiveCallsPassOwnSignature = false;
  /// Merge scanned arguments into those a source binding names.
  void (*MergeScannedArgs)(CallArgContext &C, std::vector<ExprPtr> &Hinted,
                           const SourceFunctionTypeHint &Signature) = nullptr;
};

/// The call-argument policy for code of \p A in a \p F image, or null.
const CallArgPolicy *callArgPolicy(Arch A, BinaryFormat F);

/// True only for a same-SSA no-op (`COPY rcx = rcx`).  `COPY rcx.3 = rcx`
/// restores the entry value into a new SSA version and is a real call-arg
/// write — MSVC `__GSHandlerCheck_EH` does this after copy-prop replaces
/// `mov rcx, rbp` with the saved ExceptionRecord.
inline bool isNoopRegisterCopy(const MedOp &Op) {
  return Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
         Op.Output.Kind == MedVar::Reg && Op.Inputs[0].Kind == MedVar::Reg &&
         Op.Output.RegOff == Op.Inputs[0].RegOff &&
         Op.Output.Size == Op.Inputs[0].Size &&
         Op.Output.Id == Op.Inputs[0].Id &&
         Op.Output.SSAVer == Op.Inputs[0].SSAVer;
}

/// True when \p Op ends the setup of a later call's arguments
/// (endsCallArgumentSetup), as MedABIPass's barrier has it.
inline bool endsCallSetup(const MedOp &Op) { return endsCallArgumentSetup(Op); }

void collectSpilledStackArgs(const CallArgScan &Scan,
                             std::vector<ExprPtr> &Found);
/// Collect the stack arguments of \p Scan's call, its stores searched
/// further back when the call fills every integer argument register: only
/// then can a stack argument follow, after the setup of each register.
void collectFullBankStackArgs(const CallArgScan &Scan,
                              std::vector<ExprPtr> &Found);
void collectCallArgsX86(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args);
void collectCallArgsARM(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args);
void collectCallArgsAArch64(const CallArgScan &Scan,
                            std::vector<ExprPtr> &Found,
                            std::vector<ExprPtr> &Args);

} // namespace call_args_detail
} // namespace neverd

#endif // NEVERD_IR_HIGH_CALLARGCOLLECTIONDETAIL_H
