//===- MedABIPassSupport.cpp - ABI-recovery slicing helpers ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Data-flow and stack-offset tracing helpers backing the per-call-site
/// argument scans in recoverCallAbi (MedABIPass.cpp): indirect-target
/// resolution, stack-pointer offset tracing, and cross-block argument-register
/// recovery.  See MedABIPassDetail.h for the shared declarations.
///
//===----------------------------------------------------------------------===//

#include "../../../safety/StackSlotFlow.h"
#include "MedABIPassDetail.h"

#include "neverd/Common.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

static llvm::ArrayRef<uint64_t> integerParamRegs(const TargetRegInfo &TRI,
                                                 bool IsWin64) {
  return IsWin64 && !TRI.Win64ParamRegs.empty() ? TRI.Win64ParamRegs
                                                : TRI.IntParamRegs;
}

static int integerArgIndex(const TargetRegInfo &TRI, uint64_t RegOff,
                           bool IsWin64) {
  llvm::ArrayRef<uint64_t> Regs = integerParamRegs(TRI, IsWin64);
  for (size_t I = 0; I < Regs.size(); ++I)
    if (Regs[I] == RegOff)
      return static_cast<int>(I);
  return -1;
}

// Address = Base + Offset modulo the original address width. Offset owns the
// width; no rewrite may change it or peel a width-changing conversion.
struct ReducedAddress {
  MedVar Base;
  llvm::APInt Offset;
};

static bool sameAddressValue(const MedVar &A, const MedVar &B) {
  return A.Kind == B.Kind && A.Id == B.Id && A.SSAVer == B.SSAVer &&
         A.RegOff == B.RegOff && A.Size == B.Size && A.TheArch == B.TheArch &&
         A.RenameTag == B.RenameTag;
}

// Stop at an opaque value when the next operation cannot be peeled exactly.
// The already-proven suffix remains usable, including when two accesses share
// the same conversion result. Invalid widths and conflicting definitions fail.
static std::optional<ReducedAddress> reduceAddr(const MedBlock &Blk,
                                                const MedVar &V) {
  if (V.Size == 0 || V.Size > 8)
    return std::nullopt;
  ReducedAddress Result{V, llvm::APInt(V.Size * 8u, 0)};
  for (unsigned Depth = 0; Depth <= 128; ++Depth) {
    const MedVar &Cur = Result.Base;
    if (Cur.isConst())
      break;
    const MedOp *Def = nullptr;
    for (const auto &Op : Blk.Ops) {
      // STOREs and control-flow operations have no defining output. Their
      // default Temp/id=0 must not conflict with a real temporary definition.
      if (Op.Output.Size == 0 || Op.Output.Kind != Cur.Kind ||
          Op.Output.Id != Cur.Id || Op.Output.SSAVer != Cur.SSAVer ||
          Op.Output.RegOff != Cur.RegOff)
        continue;
      if (Def || !sameAddressValue(Op.Output, Cur))
        return std::nullopt;
      Def = &Op;
    }
    if (!Def)
      break;

    const MedVar *Next = nullptr;
    switch (Def->Opcode) {
    case NdOp::COPY:
      if (Def->NumInputs == 1 && Def->Inputs[0].Size == Cur.Size)
        Next = &Def->Inputs[0];
      break;
    case NdOp::SUBBYTES:
      if (Def->NumInputs == 2 && Def->Inputs[0].Size == Cur.Size &&
          Def->Inputs[1].isConst() && Def->Inputs[1].ConstVal == 0)
        Next = &Def->Inputs[0];
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB: {
      if (Def->NumInputs != 2)
        break;
      const unsigned C = Def->Inputs[1].isConst() ? 1 : 0;
      const MedVar &Constant = Def->Inputs[C];
      const MedVar &Base = Def->Inputs[1 - C];
      if (!Constant.isConst() || Base.isConst() || Base.Size != Cur.Size ||
          Constant.Size == 0 || Constant.Size > 8 ||
          (Def->Opcode == NdOp::INT_SUB && C != 1))
        break;
      // Match arithmetic Coerce: truncate the constant at its own width,
      // then zero-extend or truncate to the operation's fixed output width.
      llvm::APInt Delta(Constant.Size * 8u, Constant.ConstVal,
                        /*isSigned=*/false, /*implicitTrunc=*/true);
      Delta = Delta.zextOrTrunc(Result.Offset.getBitWidth());
      if (Def->Opcode == NdOp::INT_ADD)
        Result.Offset += Delta;
      else
        Result.Offset -= Delta;
      Next = &Base;
      break;
    }
    default:
      break;
    }
    // Entry live-ins use self-COPY markers. Keep that exact value as the root
    // instead of spending the whole bound repeatedly visiting the marker.
    if (!Next || sameAddressValue(*Next, Cur))
      break;
    Result.Base = *Next;
  }
  return Result;
}

// To - From in one finite address domain, or unknown for unrelated bases.
// Constant bases are absolute bit patterns; all other bases require the same
// complete value identity. A zero distance proves equal starting addresses.
static std::optional<llvm::APInt>
reducedAddressDistance(const ReducedAddress &From, const ReducedAddress &To) {
  const unsigned Width = From.Offset.getBitWidth();
  if (Width != To.Offset.getBitWidth())
    return std::nullopt;
  llvm::APInt Distance = To.Offset - From.Offset;
  if (From.Base.isConst() && To.Base.isConst()) {
    Distance += llvm::APInt(Width, To.Base.ConstVal,
                            /*isSigned=*/false, /*implicitTrunc=*/true);
    Distance -= llvm::APInt(Width, From.Base.ConstVal,
                            /*isSigned=*/false, /*implicitTrunc=*/true);
  } else if (!sameAddressValue(From.Base, To.Base)) {
    return std::nullopt;
  }
  return Distance;
}

// Project byte ranges into the address ring only to prove disjointness. This
// is a conservative may-alias model, not a claim that a hardware access wraps.
static bool reducedRangesDisjoint(const llvm::APInt &Distance,
                                  uint16_t FromSize, uint16_t ToSize) {
  if (FromSize == 0 || ToSize == 0)
    return false;
  const unsigned Width = Distance.getBitWidth();
  if (Width < 64 &&
      (FromSize >= (uint64_t{1} << Width) || ToSize >= (uint64_t{1} << Width)))
    return false;
  return Distance.uge(FromSize) && (-Distance).uge(ToSize);
}

static bool preservesSpillAcrossCall(const MedBlock &Blk, int CallIndex,
                                     const MedVar &Address, uint16_t Size,
                                     const AbiSpillContext &Context, int Depth);
static std::optional<int64_t> authenticatedStackOffset(const MedFunc &Func,
                                                       const TargetRegInfo &TRI,
                                                       const MedVar &V,
                                                       unsigned Depth = 0);

// Prove a complete same-block store reaches this load. Unknown calls, aliases,
// partial overwrites and atomic accesses invalidate the earlier stored value.
static std::optional<int>
reachingStore(const MedBlock &Blk, int LoadIndex,
              const AbiSpillContext *Context = nullptr, int Depth = 0) {
  const MedOp &Load = Blk.Ops[LoadIndex];
  const MedVar *Address = safety::detail::memoryAddress(Load);
  if (!Address || Load.Output.Size == 0 ||
      Load.MemoryOrdering != NdMemoryOrdering::None)
    return std::nullopt;
  const auto StackOffset =
      Context ? authenticatedStackOffset(Context->Func, Context->TRI, *Address)
              : std::nullopt;
  const auto Target =
      StackOffset ? std::optional<ReducedAddress>{} : reduceAddr(Blk, *Address);
  if (!StackOffset && !Target)
    return std::nullopt;
  for (int I = LoadIndex - 1; I >= 0; --I) {
    const MedOp &Op = Blk.Ops[I];
    if (Op.Opcode == NdOp::INTRINSIC)
      return std::nullopt;
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
      if (!Context || !preservesSpillAcrossCall(
                          Blk, I, *Address, Load.Output.Size, *Context, Depth))
        return std::nullopt;
      continue;
    }
    if (!safety::detail::isStackMemoryWrite(Op.Opcode))
      continue;
    const MedVar *StoreAddress = safety::detail::memoryAddress(Op);
    const MedVar *Value = safety::detail::storedValue(Op);
    if (!StoreAddress || !Value || Value->Size == 0 ||
        Op.MemoryOrdering != NdMemoryOrdering::None)
      return std::nullopt;
    if (StackOffset) {
      const auto SourceOffset =
          authenticatedStackOffset(Context->Func, Context->TRI, *StoreAddress);
      if (!SourceOffset)
        return std::nullopt;
      if (!safety::detail::stackRangesOverlap(*SourceOffset, Value->Size,
                                              *StackOffset, Load.Output.Size))
        continue;
      return *SourceOffset == *StackOffset && Value->Size == Load.Output.Size
                 ? std::optional<int>(I)
                 : std::nullopt;
    }
    const auto Source = reduceAddr(Blk, *StoreAddress);
    if (!Source)
      return std::nullopt;
    const auto Distance = reducedAddressDistance(*Source, *Target);
    if (!Distance)
      return std::nullopt;
    if (reducedRangesDisjoint(*Distance, Value->Size, Load.Output.Size))
      continue;
    if (Distance->isZero() && Value->Size == Load.Output.Size)
      return I;
    return std::nullopt;
  }
  return std::nullopt;
}

// Resolve an indirect call target to a constant code address when the call goes
// through a function pointer that provably holds a known function (`fp = &F;
// ...; fp(...)`).  Follows COPY / width-cast chains and a single stack-slot
// store/load round trip (the pointer parked in a local), matching the load's
// address to a prior store's address by structural equality (reduceAddr /
// reducedAddressDistance). Returns the address, or 0 when not provable -- a
// conservative best effort within the call's own block that never changes
// behavior unless the target is proven, so non-resolvable indirect calls keep
// their existing (heuristic) handling.
va_t resolveIndirectTargetAddr(const MedBlock &Blk, int FromIdx,
                               const MedVar &V, int Depth,
                               const AbiSpillContext *Context) {
  if (Depth > 16 || (Context && V.Size != Context->TRI.PointerSize))
    return 0;
  if (V.isConst())
    return V.ConstVal;
  int DefIdx = -1;
  for (int J = FromIdx - 1; J >= 0; --J) {
    const auto &O = Blk.Ops[J];
    if (O.Output.Kind == V.Kind && O.Output.Id == V.Id &&
        O.Output.SSAVer == V.SSAVer && O.Output.RegOff == V.RegOff) {
      DefIdx = J;
      break;
    }
  }
  if (DefIdx < 0)
    return 0;
  const MedOp &Def = Blk.Ops[DefIdx];
  if (Context && Def.Output.Size != V.Size)
    return 0;
  switch (Def.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def.NumInputs >= 1
               ? resolveIndirectTargetAddr(Blk, DefIdx, Def.Inputs[0],
                                           Depth + 1, Context)
               : 0;
  case NdOp::SUBBYTES:
    return (Def.NumInputs >= 2 && Def.Inputs[1].isConst() &&
            Def.Inputs[1].ConstVal == 0)
               ? resolveIndirectTargetAddr(Blk, DefIdx, Def.Inputs[0],
                                           Depth + 1, Context)
               : 0;
  case NdOp::LOAD: {
    const auto Store = reachingStore(Blk, DefIdx, Context, Depth);
    if (!Store)
      return 0;
    return resolveIndirectTargetAddr(
        Blk, *Store, *safety::detail::storedValue(Blk.Ops[*Store]), Depth + 1,
        Context);
  }
  default:
    return 0;
  }
}

std::optional<int> resolveIndirectTargetArgIdx(const MedBlock &Blk, int FromIdx,
                                               const TargetRegInfo &TRI,
                                               const MedVar &V, bool IsWin64,
                                               int Depth) {
  if (Depth > 16 || V.isConst())
    return std::nullopt;

  int DefIdx = -1;
  for (int J = FromIdx - 1; J >= 0; --J) {
    const auto &O = Blk.Ops[J];
    if (O.Output.Kind == V.Kind && O.Output.Id == V.Id &&
        O.Output.SSAVer == V.SSAVer && O.Output.RegOff == V.RegOff) {
      DefIdx = J;
      break;
    }
  }
  if (DefIdx < 0) {
    if (V.Kind != MedVar::Reg && V.Kind != MedVar::Param)
      return std::nullopt;
    int ArgIdx = integerArgIndex(TRI, V.RegOff, IsWin64);
    return ArgIdx >= 0 ? std::optional<int>(ArgIdx) : std::nullopt;
  }

  const MedOp &Def = Blk.Ops[DefIdx];
  switch (Def.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    if (Def.NumInputs < 1)
      return std::nullopt;
    // Entry live-ins are represented by a self-copy.  Treat that as the
    // provenance root instead of recursing through the identical SSA value.
    if (Def.Inputs[0].Kind == V.Kind && Def.Inputs[0].Id == V.Id &&
        Def.Inputs[0].SSAVer == V.SSAVer && Def.Inputs[0].RegOff == V.RegOff) {
      int ArgIdx = integerArgIndex(TRI, V.RegOff, IsWin64);
      return ArgIdx >= 0 ? std::optional<int>(ArgIdx) : std::nullopt;
    }
    return resolveIndirectTargetArgIdx(Blk, DefIdx, TRI, Def.Inputs[0], IsWin64,
                                       Depth + 1);
  case NdOp::SUBBYTES:
    return (Def.NumInputs >= 2 && Def.Inputs[1].isConst() &&
            Def.Inputs[1].ConstVal == 0)
               ? resolveIndirectTargetArgIdx(Blk, DefIdx, TRI, Def.Inputs[0],
                                             IsWin64, Depth + 1)
               : std::nullopt;
  case NdOp::LOAD: {
    const auto Store = reachingStore(Blk, DefIdx);
    if (!Store)
      return std::nullopt;
    return resolveIndirectTargetArgIdx(
        Blk, *Store, TRI, *safety::detail::storedValue(Blk.Ops[*Store]),
        IsWin64, Depth + 1);
  }
  default:
    return std::nullopt;
  }
}

// Offset of \p V relative to the function-entry stack pointer, following the SP
// definition chain through constant add/sub decrements (cdecl `push`/`sub esp`)
// and width casts.  Lets the stack-argument scan place each pushed argument at
// its true distance from the call-site SP even when successive `push`es
// retarget a moving stack pointer.  nullopt when \p V does not derive from the
// SP.
//
// SSA definitions are function-wide.  In particular, clang often computes an
// outgoing call-frame base in a predecessor and emits the STOREs and CALL in a
// successor.  Looking only in the use block loses that exact definition and
// silently drops every cdecl argument.
struct MedDefSite {
  const MedBlock *Block = nullptr;
  const MedOp *Op = nullptr;
  const PhiNode *Phi = nullptr;
  bool Ambiguous = false;
};

static bool sameDefIdentity(const MedVar &A, const MedVar &B) {
  return A.Kind == B.Kind && A.Id == B.Id && A.SSAVer == B.SSAVer &&
         A.RegOff == B.RegOff;
}

static MedDefSite findFuncDef(const MedFunc &Func, const MedVar &V) {
  MedDefSite Site;
  auto Record = [&](const MedBlock &Block, const MedOp *Op,
                    const PhiNode *Phi) {
    if (Site.Op || Site.Phi) {
      Site = {};
      Site.Ambiguous = true;
      return;
    }
    Site.Block = &Block;
    Site.Op = Op;
    Site.Phi = Phi;
  };

  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      if (sameDefIdentity(Phi.Output, V)) {
        Record(Block, nullptr, &Phi);
        if (Site.Ambiguous)
          return Site;
      }
    for (const MedOp &Op : Block.Ops)
      if (sameDefIdentity(Op.Output, V)) {
        Record(Block, &Op, nullptr);
        if (Site.Ambiguous)
          return Site;
      }
  }
  return Site;
}

// Unlike the heuristic argument scanner, call-boundary alias proofs may not
// accept truncation, guessed live-ins, loads, PHIs or overflowing arithmetic.
static std::optional<int64_t> authenticatedStackOffset(const MedFunc &Func,
                                                       const TargetRegInfo &TRI,
                                                       const MedVar &V,
                                                       unsigned Depth) {
  if (Depth > 128 || V.Size != TRI.PointerSize || V.isConst())
    return std::nullopt;
  const auto Site = findFuncDef(Func, V);
  if (Site.Ambiguous || !Site.Op || Site.Op->Output.Size != V.Size ||
      Site.Op->Output.TheArch != V.TheArch ||
      Site.Op->Output.RenameTag != V.RenameTag)
    return std::nullopt;
  const auto &Op = *Site.Op;
  if (V.Kind == MedVar::Reg && V.RegOff == TRI.StackPointer &&
      safety::detail::isAuthenticatedEntryRegisterLiveIn(Func, V))
    return 0;
  if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
    return authenticatedStackOffset(Func, TRI, Op.Inputs[0], Depth + 1);
  if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
      Op.NumInputs != 2)
    return std::nullopt;
  unsigned BaseIndex = 0;
  if (Op.Opcode == NdOp::INT_ADD && Op.Inputs[0].isConst())
    BaseIndex = 1;
  const auto &Constant = Op.Inputs[1 - BaseIndex];
  if (Constant.Size > TRI.PointerSize)
    return std::nullopt;
  auto Delta = safety::detail::signedStackConstant(Constant);
  // AArch64 stack adjustments carry narrow positive immediates. Accept only
  // those whose sign and zero extensions agree; never narrow the base pointer.
  if (!Delta ||
      (Constant.Size < TRI.PointerSize &&
       (*Delta < 0 || static_cast<uint64_t>(*Delta) != Constant.ConstVal)))
    return std::nullopt;
  auto Base =
      authenticatedStackOffset(Func, TRI, Op.Inputs[BaseIndex], Depth + 1);
  if (!Base || !Delta)
    return std::nullopt;
  return safety::detail::checkedStackOffset(*Base, *Delta,
                                            Op.Opcode == NdOp::INT_SUB);
}

std::set<va_t> findFrameLocalLeafCallees(const std::vector<MedFunc> &Funcs,
                                         Arch TheArch) {
  const auto &TRI = getTargetRegInfo(TheArch);
  std::set<va_t> Result, Seen, Duplicates;
  for (const auto &F : Funcs) {
    if (!Seen.insert(F.Entry).second)
      Duplicates.insert(F.Entry);
    bool Safe = !F.Blocks.empty();
    std::set<va_t> BlockEntries;
    for (const auto &B : F.Blocks)
      BlockEntries.insert(B.StartAddr);
    for (const auto &B : F.Blocks) {
      if (B.Ops.empty() ||
          (B.Succs.empty() && B.Ops.back().Opcode != NdOp::RETURN))
        Safe = false;
      for (const auto &Op : B.Ops) {
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::INDIR_BR ||
            Op.Opcode >= NdOp::_COUNT ||
            safety::detail::isAtomicMemoryAccess(Op.Opcode)) {
          Safe = false;
          continue;
        }
        if ((Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) &&
            (Op.NumInputs == 0 || !Op.Inputs[0].isConst() ||
             !BlockEntries.count(Op.Inputs[0].ConstVal)))
          Safe = false;
        if (Op.Opcode != NdOp::STORE)
          continue;
        const auto *Address = safety::detail::memoryAddress(Op);
        const auto *Value = safety::detail::storedValue(Op);
        auto Offset =
            Address ? authenticatedStackOffset(F, TRI, *Address) : std::nullopt;
        if (!Offset || !Value || Value->Size == 0 ||
            Op.MemoryOrdering != NdMemoryOrdering::None ||
            *Offset > -static_cast<int64_t>(Value->Size))
          Safe = false;
      }
    }
    if (Safe)
      Result.insert(F.Entry);
  }
  for (va_t Entry : Duplicates)
    Result.erase(Entry);
  return Result;
}

static bool preservesSpillAcrossCall(const MedBlock &Blk, int CallIndex,
                                     const MedVar &Address, uint16_t Size,
                                     const AbiSpillContext &Context,
                                     int Depth) {
  if (Depth >= 16 || !Context.FrameLocalLeafCallees ||
      Context.TRI.TheArch != Arch::AArch64)
    return false;
  auto Offset = authenticatedStackOffset(Context.Func, Context.TRI, Address);
  if (!Offset || Size == 0 || *Offset > -static_cast<int64_t>(Size))
    return false;
  // Require an explicit same-block call SP; do not guess across CFG edges.
  std::optional<int64_t> CallSP;
  for (int I = CallIndex - 1; I >= 0; --I) {
    const auto &Out = Blk.Ops[I].Output;
    if (Out.Kind == MedVar::Reg && Out.RegOff == Context.TRI.StackPointer) {
      CallSP = authenticatedStackOffset(Context.Func, Context.TRI, Out);
      break;
    }
  }
  if (!CallSP || *Offset < *CallSP)
    return false;
  const auto &Call = Blk.Ops[CallIndex];
  if (Call.NumInputs == 0)
    return false;
  const va_t Target = resolveIndirectTargetAddr(Blk, CallIndex, Call.Inputs[0],
                                                Depth + 1, &Context);
  return Target && Context.FrameLocalLeafCallees->count(Target);
}

std::optional<int64_t> stackPtrDelta(const MedFunc &Func,
                                     const TargetRegInfo &TRI, const MedVar &V,
                                     int Depth) {
  if (Depth > 128 || V.isConst())
    return std::nullopt;

  const MedDefSite Site = findFuncDef(Func, V);
  if (Site.Ambiguous)
    return std::nullopt;
  if (Site.Phi) {
    std::optional<int64_t> Common;
    for (const auto &[Pred, Incoming] : Site.Phi->Args) {
      (void)Pred;
      if (sameDefIdentity(Incoming, V))
        continue;
      auto Delta = stackPtrDelta(Func, TRI, Incoming, Depth + 1);
      if (!Delta || (Common && *Common != *Delta))
        return std::nullopt;
      Common = *Delta;
    }
    return Common;
  }
  if (!Site.Op) {
    // No function-wide definition: the live-in stack pointer is the zero
    // reference; anything else is not SP-derived.
    if (V.Kind == MedVar::Reg && V.RegOff == TRI.StackPointer)
      return 0;
    return std::nullopt;
  }
  const MedOp *Def = Site.Op;
  const MedBlock &DefBlock = *Site.Block;

  switch (Def->Opcode) {
  case NdOp::COPY:
    if (Def->NumInputs >= 1) {
      const MedVar &In = Def->Inputs[0];
      // The entry stack-pointer self-copy (`COPY ESP, ESP`, the live-in marker)
      // is the zero reference; following it would recurse forever to the depth
      // cap and report the whole push chain as un-traceable, collapsing every
      // pushed argument onto slot 0.
      if (In.Kind == MedVar::Reg && In.RegOff == TRI.StackPointer &&
          In.Id == V.Id && In.SSAVer == V.SSAVer)
        return 0;
      return stackPtrDelta(Func, TRI, In, Depth + 1);
    }
    return std::nullopt;
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def->NumInputs >= 1
               ? stackPtrDelta(Func, TRI, Def->Inputs[0], Depth + 1)
               : std::nullopt;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB: {
    if (Def->NumInputs < 2)
      return std::nullopt;
    std::optional<int64_t> Base;
    int64_t K = 0;
    bool HaveK = false;
    for (uint8_t I = 0; I < Def->NumInputs; ++I) {
      if (Def->Inputs[I].isConst()) {
        K = static_cast<int64_t>(Def->Inputs[I].ConstVal);
        HaveK = true;
      } else if (!Base) {
        Base = stackPtrDelta(Func, TRI, Def->Inputs[I], Depth + 1);
      }
    }
    if (!Base || !HaveK)
      return std::nullopt;
    return Def->Opcode == NdOp::INT_ADD ? *Base + K : *Base - K;
  }
  case NdOp::LOAD: {
    const int LoadIndex = static_cast<int>(Def - DefBlock.Ops.data());
    const auto Store = reachingStore(DefBlock, LoadIndex);
    if (!Store)
      return std::nullopt;
    return stackPtrDelta(Func, TRI,
                         *safety::detail::storedValue(DefBlock.Ops[*Store]),
                         Depth + 1);
  }
  default:
    return std::nullopt;
  }
}

// Whether \p V is, or derives from (through copies, width casts and a constant
// add/sub), a frame register -- the stack OR frame pointer.  Unlike
// stackPtrDelta this also accepts frame-pointer (x29) relative addresses, which
// clang -O0 uses for stack buffers once the frame is large enough.  Used to
// confirm an AArch64 indirect-call x8 value is a genuine stack buffer pointer
// (the sret result slot, `add x8, sp/fp, #k`) rather than an unrelated scratch
// value, so the hidden sret-buffer argument is recovered only for real
// indirect-sret call sites.
bool derivesFromFrameReg(const MedBlock &Blk, const TargetRegInfo &TRI,
                         const MedVar &V, int Depth) {
  if (Depth > 64 || V.isConst())
    return false;
  if (V.Kind == MedVar::Reg && TRI.isFrameReg(V.RegOff))
    return true;
  const MedOp *Def = nullptr;
  for (const auto &Op : Blk.Ops)
    if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
        Op.Output.SSAVer == V.SSAVer && Op.Output.RegOff == V.RegOff) {
      Def = &Op;
      break;
    }
  if (!Def)
    return false;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def->NumInputs >= 1 &&
           derivesFromFrameReg(Blk, TRI, Def->Inputs[0], Depth + 1);
  case NdOp::INT_ADD:
  case NdOp::INT_SUB: {
    // A frame-relative buffer is `frame_reg +/- constant`: one operand derives
    // from a frame register, the other is a constant displacement.
    bool HaveBase = false, HaveConst = false;
    for (uint8_t I = 0; I < Def->NumInputs; ++I) {
      if (Def->Inputs[I].isConst())
        HaveConst = true;
      else if (derivesFromFrameReg(Blk, TRI, Def->Inputs[I], Depth + 1))
        HaveBase = true;
    }
    return HaveBase && HaveConst;
  }
  default:
    return false;
  }
}

// Identifies an SSA stack-pointer value (Id, version, register offset).
static SpOffsetKey spOffsetKey(const MedVar &V) {
  return {V.Id, V.SSAVer, V.RegOff};
}

// Records, for each stack-pointer value on the call-site SP's definition chain,
// its byte offset *above* the call SP (call SP = 0, each earlier `push`'s SP
// one slot higher).  Offset-preserving casts (SUBBYTES/ZEXT/SEXT/COPY) keep the
// offset; a `sub`/`add` of a constant shifts it.  Used to place pushed
// arguments relative to the call SP when the absolute entry-relative delta is
// unavailable (a post-loop push chain whose SP threads a loop-carried PHI,
// where the chain round-trips ESP<->RSP and never reaches the entry SP as a
// constant).
void buildCallSpOffsets(const MedFunc &Func, const TargetRegInfo &TRI,
                        const MedVar &V, int64_t Off,
                        std::map<SpOffsetKey, int64_t> &Map, int Depth) {
  if (Depth > 128 || V.isConst())
    return;
  if (!Map.emplace(spOffsetKey(V), Off).second)
    return; // already visited (e.g. the entry SP self-copy)
  const MedDefSite Site = findFuncDef(Func, V);
  if (Site.Ambiguous)
    return;
  // A PHI can merge different concrete stack deltas.  The relative fallback
  // deliberately stops at that boundary: values already mapped on the
  // call-side chain remain usable, while path-specific predecessors cannot be
  // assigned the same offset without an equality proof.
  if (Site.Phi)
    return;
  const MedOp *Def = Site.Op;
  if (!Def)
    return;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    if (Def->NumInputs >= 1)
      buildCallSpOffsets(Func, TRI, Def->Inputs[0], Off, Map, Depth + 1);
    break;
  case NdOp::INT_SUB:
    // V = base - C  =>  base sits C higher  =>  base offset = Off + C.
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst())
      buildCallSpOffsets(Func, TRI, Def->Inputs[0],
                         Off + static_cast<int64_t>(Def->Inputs[1].ConstVal),
                         Map, Depth + 1);
    break;
  case NdOp::INT_ADD:
    // V = base + C  =>  base sits C lower  =>  base offset = Off - C.
    if (Def->NumInputs >= 2) {
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (Def->Inputs[I].isConst())
          for (uint8_t J = 0; J < Def->NumInputs; ++J)
            if (!Def->Inputs[J].isConst())
              buildCallSpOffsets(
                  Func, TRI, Def->Inputs[J],
                  Off - static_cast<int64_t>(Def->Inputs[I].ConstVal), Map,
                  Depth + 1);
    }
    break;
  default:
    break;
  }
}

// Offset of store-address \p V above the call SP, resolved against the call
// SP's offset map.  Follows \p V's definition chain (offset-preserving casts,
// and constant add/sub) until it reaches a value on the call SP chain.  nullopt
// when the address is not stack-pointer derived.
std::optional<int64_t> relStackOff(const MedFunc &Func,
                                   const TargetRegInfo &TRI, const MedVar &V,
                                   const std::map<SpOffsetKey, int64_t> &Map,
                                   int Depth) {
  if (Depth > 128 || V.isConst())
    return std::nullopt;
  if (auto It = Map.find(spOffsetKey(V)); It != Map.end())
    return It->second;
  const MedDefSite Site = findFuncDef(Func, V);
  if (Site.Ambiguous)
    return std::nullopt;
  if (Site.Phi) {
    std::optional<int64_t> Common;
    for (const auto &[Pred, Incoming] : Site.Phi->Args) {
      (void)Pred;
      auto Rel = relStackOff(Func, TRI, Incoming, Map, Depth + 1);
      if (!Rel || (Common && *Common != *Rel))
        return std::nullopt;
      Common = *Rel;
    }
    return Common;
  }
  const MedOp *Def = Site.Op;
  if (!Def)
    return std::nullopt;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Def->NumInputs >= 1
               ? relStackOff(Func, TRI, Def->Inputs[0], Map, Depth + 1)
               : std::nullopt;
  case NdOp::INT_SUB:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst())
      if (auto B = relStackOff(Func, TRI, Def->Inputs[0], Map, Depth + 1))
        return *B - static_cast<int64_t>(Def->Inputs[1].ConstVal);
    return std::nullopt;
  case NdOp::INT_ADD:
    if (Def->NumInputs >= 2) {
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (Def->Inputs[I].isConst())
          for (uint8_t J = 0; J < Def->NumInputs; ++J)
            if (!Def->Inputs[J].isConst())
              if (auto B =
                      relStackOff(Func, TRI, Def->Inputs[J], Map, Depth + 1))
                return *B + static_cast<int64_t>(Def->Inputs[I].ConstVal);
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

std::string relocCalleeName(const BinaryImage &Img, va_t InsnAddr) {
  for (const auto &Rel : Img.Relocations) {
    // The branch relocation sits at the instruction's displacement field: at
    // the instruction address itself for AArch64/ARM `bl` (the imm is in the
    // opcode word), but one byte in for x86 `call rel32` (the 0xE8 opcode
    // precedes the 4-byte displacement).  Accept both so a relocatable-object
    // external call is named on x86 too (otherwise its placeholder-0 target
    // resolves to whatever symbol sits at VA 0 — e.g. the function itself,
    // lifting a libc call into a bogus self-recursion).
    if ((Rel.Address != InsnAddr && Rel.Address != InsnAddr + 1) ||
        Rel.SymbolName.empty())
      continue;
    llvm::StringRef Name = stripLeadingUnderscores(Rel.SymbolName);
    if (libc::isKnownFunction(Name))
      return Name.str();
    return Rel.SymbolName;
  }
  return {};
}

// The value an argument-register-defining op contributes to its slot: the
// source of a copy, or — for a sub-register sync that narrows the register's
// own wider value (`EDI := low32(RDI)` emitted after a 64-bit write) — that
// wider value, so a struct or other wide argument keeps its high bits instead
// of the truncated sub-register view.  Otherwise the op's output itself.
static MedVar argRegSourceValue(const MedOp &Op) {
  if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1)
    return Op.Inputs[0];
  if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
      Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal == 0 &&
      Op.Inputs[0].Kind == MedVar::Reg &&
      Op.Inputs[0].RegOff == Op.Output.RegOff &&
      Op.Inputs[0].Size > Op.Output.Size)
    return Op.Inputs[0];
  return Op.Output;
}

// argRegSourceValue with block context: resolves a low-half sub-register sync
// whose source is a *temporary* (`SUBBYTES Wn = subpiece(t, 0)`, not the
// `subpiece(Xn, 0)` of the register itself that argRegSourceValue already
// widens) to the full-width value of a paired full-register write of the same
// source (`COPY Xn = t`) appearing just before it in the block.  The backward
// register scan meets the 32-bit sync before the 64-bit write, so without this
// a pointer written whole to an argument register and then synced to its 32-bit
// view -- a FILE* loaded into x1 right before an external `fputs` -- would be
// recovered as the truncated low 32 bits, yielding a wild pointer at run time.
MedVar argRegSourceValueInBlock(const MedBlock &Blk, int J,
                                const TargetRegInfo &TRI, bool IsWin64) {
  const MedOp &Op = Blk.Ops[J];
  MedVar Base = argRegSourceValue(Op);
  // Only a sub-register sync whose source is not the register itself needs the
  // sibling lookup; everything else (a plain copy, or `subpiece(Xn, 0)`) is
  // already resolved by argRegSourceValue.
  if (Op.Opcode != NdOp::SUBBYTES || Op.NumInputs < 2 ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].ConstVal != 0 ||
      Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg ||
      (Op.Inputs[0].Kind == MedVar::Reg &&
       Op.Inputs[0].RegOff == Op.Output.RegOff))
    return Base;
  const MedVar &Src = Op.Inputs[0];
  const int ArgIdx = integerArgIndex(TRI, Op.Output.RegOff, IsWin64);
  for (int K = J - 1; K >= 0; --K) {
    const MedOp &W = Blk.Ops[K];
    if (W.Output.Kind != MedVar::Reg ||
        integerArgIndex(TRI, W.Output.RegOff, IsWin64) != ArgIdx)
      continue;
    // The nearest earlier write to this same argument register: when it is a
    // wider write of the same source value, it is the full-width definition the
    // sub-register sync mirrors.
    if (W.Output.Size > Op.Output.Size && W.NumInputs >= 1 &&
        !W.Inputs[0].isConst() && W.Inputs[0].Kind == Src.Kind &&
        W.Inputs[0].Id == Src.Id && W.Inputs[0].SSAVer == Src.SSAVer)
      return argRegSourceValue(W);
    break; // only the nearest earlier write to this register is the pair
  }
  return Base;
}

const PhiNode *selectAuthoritativeArgPhi(const MedFunc &Func,
                                         const MedBlock &Block,
                                         const TargetRegInfo &TRI, int ArgIdx,
                                         bool IsWin64) {
  // AArch64 may carry both Wn and Xn PHIs for one ABI argument.  Ordinarily the
  // full-width view is authoritative, but a wide alias synthesized only by a
  // loop back-edge is undef on the first iteration.  Prefer a PHI whose value
  // is genuinely defined on every function-entry edge, then prefer width.
  auto entryEdgesSeeded = [&](const PhiNode &Phi) {
    for (const auto &A : Phi.Args) {
      const MedBlock *Pred = nullptr;
      for (const auto &B : Func.Blocks)
        if (B.Id == A.first) {
          Pred = &B;
          break;
        }
      if (!Pred)
        return false;
      if (!Pred->Preds.empty())
        continue;
      if (A.second.isConst())
        continue;

      bool Defined = false;
      for (const auto &P : Pred->Phis)
        if (P.Output.Id == A.second.Id && P.Output.SSAVer == A.second.SSAVer) {
          Defined = true;
          break;
        }
      if (!Defined)
        for (const auto &O : Pred->Ops)
          if (O.Output.Id == A.second.Id &&
              O.Output.SSAVer == A.second.SSAVer && O.Output.Size > 0) {
            Defined = true;
            break;
          }
      if (!Defined)
        for (const auto &P : Func.Params)
          if ((A.second.Kind == MedVar::Reg ||
               A.second.Kind == MedVar::Param) &&
              P.RegOff == A.second.RegOff && P.Size == A.second.Size) {
            Defined = true;
            break;
          }
      if (!Defined)
        return false;
    }
    return true; // no undef value on a function-entry predecessor
  };

  const PhiNode *Best = nullptr;
  bool BestSeeded = false;
  for (const auto &Phi : Block.Phis) {
    if (Phi.Output.Kind != MedVar::Reg ||
        integerArgIndex(TRI, Phi.Output.RegOff, IsWin64) != ArgIdx)
      continue;
    const bool PhiSeeded = entryEdgesSeeded(Phi);
    if (!Best ||
        (PhiSeeded != BestSeeded ? PhiSeeded
                                 : Phi.Output.Size > Best->Output.Size)) {
      Best = &Phi;
      BestSeeded = PhiSeeded;
    }
  }
  return Best;
}

// Value reaching argument register \p ArgIdx at the call in (\p BlockId,
// \p OpIdx), found by walking the CFG backwards into predecessor blocks: the
// nearest write to that argument register, then a block PHI for it.  Returns
// the reaching value or nullopt when the register is never set on any path to
// the call.  Recovers a register argument materialised in a block that
// *dominates* the call rather than the call's own block — e.g. a loop-invariant
// count set before a vectorised loop whose predicated branches push the call
// downstream, which the in-block and in-block-PHI scans (call block only)
// cannot see.
//
// When \p AllowUnknownLiveIn is set the caller has bounded this index to the
// callee's arity, so a parameter register with no reaching definition is the
// incoming argument of a forwarder and is recovered as a live-in even when it
// is not yet a recorded parameter of the function.
std::optional<MedVar> findReachingArgReg(const MedFunc &Func,
                                         const TargetRegInfo &TRI, Arch TheArch,
                                         int BlockId, int ArgIdx, bool IsWin64,
                                         bool AllowUnknownLiveIn,
                                         bool *FromLiveIn) {
  std::map<int, const MedBlock *> ById;
  for (const auto &B : Func.Blocks)
    ById[B.Id] = &B;

  auto scanBlock = [&](const MedBlock &B, int UpTo) -> std::optional<MedVar> {
    for (int J = UpTo - 1; J >= 0; --J) {
      const auto &Op = B.Ops[J];
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0 &&
          integerArgIndex(TRI, Op.Output.RegOff, IsWin64) == ArgIdx)
        return argRegSourceValueInBlock(B, J, TRI, IsWin64);
    }
    if (const PhiNode *Phi =
            selectAuthoritativeArgPhi(Func, B, TRI, ArgIdx, IsWin64))
      return Phi->Output;
    return std::nullopt;
  };

  auto It = ById.find(BlockId);
  if (It == ById.end())
    return std::nullopt;
  // The call block's straight-line ops were already handled by the caller (with
  // its call-boundary stop); only its PHIs and the predecessor chain remain.
  if (const PhiNode *Phi =
          selectAuthoritativeArgPhi(Func, *It->second, TRI, ArgIdx, IsWin64))
    return Phi->Output;

  std::set<int> Visited{BlockId};
  std::vector<int> Work(It->second->Preds.begin(), It->second->Preds.end());
  while (!Work.empty()) {
    int BId = Work.back();
    Work.pop_back();
    if (!Visited.insert(BId).second)
      continue;
    auto BIt = ById.find(BId);
    if (BIt == ById.end())
      continue;
    if (auto V =
            scanBlock(*BIt->second, static_cast<int>(BIt->second->Ops.size())))
      return V;
    Work.insert(Work.end(), BIt->second->Preds.begin(),
                BIt->second->Preds.end());
  }

  // No in-function definition reaches the call: the value is the live-in
  // (incoming parameter), as when a caller forwards its own argument straight
  // to the callee (`return callee(a)`), leaving no write for the scans to find.
  // Recover it only when this argument register is a real parameter of the
  // current function, so an uninitialised caller-saved register is never
  // invented as an argument.
  llvm::ArrayRef<uint64_t> ParamRegs = integerParamRegs(TRI, IsWin64);
  if (ArgIdx >= 0 && ArgIdx < static_cast<int>(ParamRegs.size())) {
    uint64_t Reg = ParamRegs[ArgIdx];
    for (const auto &P : Func.Params)
      if ((P.Kind == MedVar::Reg || P.Kind == MedVar::Param) &&
          P.RegOff == Reg) {
        MedVar V;
        V.Kind = MedVar::Reg;
        V.RegOff = Reg;
        V.SSAVer = 0;
        V.Size = P.Size > 0 ? P.Size : static_cast<uint16_t>(TRI.PointerSize);
        V.TheArch = TheArch;
        return V;
      }
    if (AllowUnknownLiveIn) {
      MedVar V;
      V.Kind = MedVar::Reg;
      V.RegOff = Reg;
      V.SSAVer = 0;
      V.Size = static_cast<uint16_t>(TRI.PointerSize);
      V.TheArch = TheArch;
      if (FromLiveIn)
        *FromLiveIn = true;
      return V;
    }
  }
  return std::nullopt;
}

} // namespace neverd
