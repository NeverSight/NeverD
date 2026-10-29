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
#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/ir/med/MedCallTarget.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

bool isAbiRecoveryBarrier(const MedOp &Op) { return endsCallArgumentSetup(Op); }

bool incomingArgumentReachesCall(const AbiCallContext &C, int ArgIndex) {
  if (C.Func.Blocks.empty() || C.CallIdx < 0)
    return false;
  size_t Remaining = limits::kMaxCallSetupStackProofWork;
  std::optional<MedBlockIdIndex> OwnBlocks;
  const MedBlockIdIndex &Blocks =
      C.Blocks ? *C.Blocks : OwnBlocks.emplace(C.Func);
  if (!Blocks.unique())
    return false;
  std::vector<std::pair<const MedBlock *, size_t>> Work{
      {&C.Blk, static_cast<size_t>(C.CallIdx)}};
  std::set<int> Seen;
  bool ReachedEntry = false;
  while (!Work.empty()) {
    auto [B, End] = Work.back();
    Work.pop_back();
    if (!Remaining-- || End > B->Ops.size() || !B->ExceptionalPreds.empty())
      return false;
    // Revisit the call block on a backedge: the entire previous iteration,
    // including its calls, must preserve the incoming value as well.
    if (End == B->Ops.size() && !Seen.insert(B->Id).second)
      continue;
    auto Root = C.Func.ModuleAnalysisRoots.lower_bound(B->StartAddr);
    if (Root != C.Func.ModuleAnalysisRoots.end() &&
        (*Root == B->StartAddr || *Root < B->EndAddr) && *Root != C.Func.Entry)
      return false;
    for (size_t I = 0; I < End; ++I) {
      if (!Remaining--)
        return false;
      const auto &Op = B->Ops[I];
      if (isAbiRecoveryBarrier(Op))
        return false;
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size &&
          C.Layout.registerIndex(Op.Output.RegOff) == ArgIndex &&
          !(Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0] == Op.Output))
        return false;
    }
    if (B == &C.Func.Blocks.front()) {
      ReachedEntry = true;
      if (B->Preds.empty())
        continue;
    } else if (B->Preds.empty())
      return false;
    for (int Id : B->Preds) {
      const MedBlock *P = Blocks.first(Id);
      if (!P || std::count(P->Succs.begin(), P->Succs.end(), B->Id) != 1)
        return false;
      Work.emplace_back(P, P->Ops.size());
    }
  }
  return ReachedEntry;
}

std::map<int64_t, MedVar> callSetupStackStores(const AbiCallContext &C,
                                               CallSetupStackJoins *Joins) {
  using Stores = std::map<int64_t, CallSetupStackValue>;
  if (Joins)
    *Joins = {};
  const int64_t Word = C.Layout.SlotBytes;
  const int64_t Base = C.Layout.CallStackBase;
  if (Word <= 0 || C.MaxArgs <= 0 || Base < 0)
    return {};
  const int64_t Limit = Base + Word * C.MaxArgs;
  // A per-call bound also limits path duplication in diamonds. Exhaustion
  // publishes no partial predecessor proof.
  size_t Remaining = limits::kMaxCallSetupStackProofWork;
  std::optional<MedBlockIdIndex> OwnBlocks;
  const MedBlockIdIndex &Blocks =
      C.Blocks ? *C.Blocks : OwnBlocks.emplace(C.Func);
  if (!Blocks.unique())
    return {};
  std::set<int> Active;
  const auto bytes = [&](const CallSetupStackValue &Value) {
    if (const auto *Scalar = std::get_if<MedVar>(&Value))
      return Scalar->Size;
    return Joins->Nodes[std::get<size_t>(Value)].Bytes;
  };
  auto same = [](const CallSetupStackValue &Left,
                 const CallSetupStackValue &Right) {
    if (Left.index() != Right.index())
      return false;
    if (const auto *Node = std::get_if<size_t>(&Left))
      return *Node == std::get<size_t>(Right);
    const auto &A = std::get<MedVar>(Left), &B = std::get<MedVar>(Right);
    return A.Kind == B.Kind && A.Id == B.Id && A.SSAVer == B.SSAVer &&
           A.Size == B.Size && A.RegOff == B.RegOff && A.TheArch == B.TheArch &&
           A.Provenance == B.Provenance &&
           A.AddressOwnerVA == B.AddressOwnerVA && A.RenameTag == B.RenameTag;
  };
  std::function<Stores(const MedBlock &, size_t, unsigned)> Visit =
      [&](const MedBlock &B, size_t End, unsigned Depth) -> Stores {
    const auto Root = C.Func.ModuleAnalysisRoots.lower_bound(B.StartAddr);
    const bool Independent = Root != C.Func.ModuleAnalysisRoots.end() &&
                             (*Root == B.StartAddr || *Root < B.EndAddr);
    // A root inside an unsplit block could bypass any earlier local store.
    if (Independent && *Root != B.StartAddr)
      return {};
    if (!Remaining || End > B.Ops.size() ||
        Depth >= limits::kCallArgStoreAddressDepth ||
        !Active.insert(B.Id).second)
      return {};
    --Remaining;
    Stores Local;
    std::set<int64_t> Overwritten;
    bool Inherit = true;
    for (size_t I = End; I > 0; --I) {
      if (!Remaining) {
        Inherit = false;
        break;
      }
      --Remaining;
      const MedOp &Op = B.Ops[I - 1];
      if (isAbiRecoveryBarrier(Op) || Op.Opcode == NdOp::ATOMIC_XCHG ||
          Op.Opcode == NdOp::ATOMIC_ADD || Op.Opcode == NdOp::ATOMIC_CMPXCHG ||
          Op.MemoryOrdering != NdMemoryOrdering::None) {
        Inherit = false;
        break;
      }
      if (Op.Opcode != NdOp::STORE)
        continue;
      const auto Offset =
          Op.NumInputs == 2 ? C.CallStackOffset(Op.Inputs[0]) : std::nullopt;
      if (!Offset || !Op.Inputs[1].Size ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          *Offset > INT64_MAX - Op.Inputs[1].Size) {
        Inherit = false; // A may-alias write hides all earlier stores.
        break;
      }
      const int64_t StoreEnd = *Offset + Op.Inputs[1].Size;
      for (int64_t Slot = Base; Slot < Limit; Slot += Word) {
        if (*Offset >= Slot + Word || StoreEnd <= Slot ||
            !Overwritten.insert(Slot).second)
          continue;
        // An aligned scalar keeps its actual width (a Win64 int store is
        // four bytes in an eight-byte slot). Never complete its upper bytes
        // from another store or reinterpret an interior byte as a new slot.
        if (*Offset == Slot && Op.Inputs[1].Size <= Word &&
            (Op.Inputs[1].Size & (Op.Inputs[1].Size - 1)) == 0 &&
            !isIncomingRegisterSave(C.TRI, Op.Inputs[1]))
          Local.emplace(Slot, Op.Inputs[1]);
      }
    }
    Stores Common;
    std::map<int64_t, std::vector<std::pair<int, CallSetupStackValue>>>
        IncomingJoins;
    const bool JoinHere = Joins && B.Preds.size() > 1;
    if (Inherit && !Independent && B.ExceptionalPreds.empty() &&
        !B.Preds.empty() && B.StartAddr != C.Func.Entry) {
      bool First = true;
      std::set<int> Preds;
      for (int Id : B.Preds) {
        const MedBlock *P = Blocks.first(Id);
        if (!Remaining || !Preds.insert(Id).second || !P ||
            std::count(P->Succs.begin(), P->Succs.end(), B.Id) != 1) {
          Common.clear();
          IncomingJoins.clear();
          break;
        }
        --Remaining;
        Stores Incoming = Visit(*P, P->Ops.size(), Depth + 1);
        if (First) {
          if (JoinHere)
            for (const auto &[Offset, Value] : Incoming) {
              if (!Remaining)
                return {};
              --Remaining;
              IncomingJoins[Offset].emplace_back(Id, Value);
            }
          Common = std::move(Incoming);
          First = false;
        } else {
          std::erase_if(Common, [&](const auto &Item) {
            const auto Other = Incoming.find(Item.first);
            return Other == Incoming.end() || !same(Item.second, Other->second);
          });
          for (auto It = IncomingJoins.begin(); It != IncomingJoins.end();) {
            if (!Remaining)
              return {};
            --Remaining;
            const auto Other = Incoming.find(It->first);
            const auto &FirstValue = It->second.front().second;
            if (Other == Incoming.end() ||
                bytes(FirstValue) != bytes(Other->second)) {
              It = IncomingJoins.erase(It);
            } else {
              It->second.emplace_back(Id, Other->second);
              ++It;
            }
          }
        }
        if (Common.empty() && IncomingJoins.empty())
          break;
      }
    }
    for (int64_t Slot : Overwritten) {
      Common.erase(Slot);
      IncomingJoins.erase(Slot);
    }
    Common.insert(Local.begin(), Local.end());
    if (JoinHere) {
      for (const auto &[Offset, Value] : Common)
        IncomingJoins.erase(Offset);
      for (auto &[Offset, Args] : IncomingJoins) {
        if (!Remaining)
          return {};
        --Remaining;
        const size_t Index = Joins->Nodes.size();
        const uint16_t Bytes = bytes(Args.front().second);
        Joins->Nodes.push_back({B.Id, Bytes, std::move(Args)});
        Common.emplace(Offset, Index);
      }
    }
    Active.erase(B.Id);
    return Common;
  };
  const Stores Reaching = Visit(C.Blk, C.CallIdx, 0);
  if (!Remaining) {
    if (Joins)
      *Joins = {};
    return {};
  }
  std::map<int64_t, MedVar> Result;
  for (const auto &[Offset, Value] : Reaching)
    if (const auto *Scalar = std::get_if<MedVar>(&Value))
      Result.emplace(Offset, *Scalar);
    else
      Joins->Slots.emplace(Offset, std::get<size_t>(Value));
  return Result;
}

// Address = Base + Offset modulo the original address width. Offset owns the
// width; no rewrite may change it or peel a width-changing conversion.
struct ReducedAddress {
  MedVar Base;
  llvm::APInt Offset;
};

static bool consumeCallTargetWork(size_t *Remaining) {
  if (!Remaining)
    return true;
  if (!*Remaining)
    return false;
  --*Remaining;
  return true;
}

static bool sameAddressValue(const MedVar &A, const MedVar &B) {
  return A.Kind == B.Kind && A.Id == B.Id && A.SSAVer == B.SSAVer &&
         A.RegOff == B.RegOff && A.Size == B.Size && A.TheArch == B.TheArch &&
         A.RenameTag == B.RenameTag;
}

// Stop at an opaque value when the next operation cannot be peeled exactly.
// The already-proven suffix remains usable, including when two accesses share
// the same conversion result. Invalid widths and conflicting definitions fail.
static std::optional<ReducedAddress>
reduceAddr(const MedBlock &Blk, const MedVar &V, size_t *Remaining = nullptr,
           std::optional<size_t> Before = std::nullopt) {
  if (V.Size == 0 || V.Size > 8)
    return std::nullopt;
  const auto Operations =
      llvm::ArrayRef(Blk.Ops).take_front(Before.value_or(Blk.Ops.size()));
  ReducedAddress Result{V, llvm::APInt(V.Size * 8u, 0)};
  for (unsigned Depth = 0;
       Depth <= ::neverd::limits::ir::kMedABIPassSupportReduceAddrDepthLimit;
       ++Depth) {
    const MedVar &Cur = Result.Base;
    if (Cur.isConst())
      break;
    const MedOp *Def = nullptr;
    for (const auto &Op : Operations) {
      if (!consumeCallTargetWork(Remaining))
        return std::nullopt;
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
      if (!Constant.isConst() || Base.Size != Cur.Size || Constant.Size == 0 ||
          Constant.Size > 8 || (Def->Opcode == NdOp::INT_SUB && C != 1))
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
static std::optional<int64_t>
authenticatedStackOffset(const MedFunc &Func, const TargetRegInfo &TRI,
                         const MedVar &V, unsigned Depth = 0,
                         MedDefSiteIndex *Defs = nullptr);

// Prove a complete same-block store reaches this load. Unknown calls, aliases,
// partial overwrites and atomic accesses invalidate the earlier stored value.
static std::optional<int>
reachingStore(const MedBlock &Blk, int LoadIndex,
              const AbiSpillContext *Context = nullptr, int Depth = 0,
              size_t *Remaining = nullptr) {
  const MedOp &Load = Blk.Ops[LoadIndex];
  const MedVar *Address = safety::detail::memoryAddress(Load);
  if (!Address || Load.Output.Size == 0 ||
      Load.MemoryOrdering != NdMemoryOrdering::None ||
      Load.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return std::nullopt;
  const auto StackOffset =
      Context ? authenticatedStackOffset(Context->Func, Context->TRI, *Address)
              : std::nullopt;
  const auto Target = StackOffset ? std::optional<ReducedAddress>{}
                                  : reduceAddr(Blk, *Address, Remaining);
  if (!StackOffset && !Target)
    return std::nullopt;
  for (int I = LoadIndex - 1; I >= 0; --I) {
    if (!consumeCallTargetWork(Remaining))
      return std::nullopt;
    const MedOp &Op = Blk.Ops[I];
    if (Op.Opcode == NdOp::INTRINSIC && isAbiRecoveryBarrier(Op))
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
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
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
    const auto Source = reduceAddr(Blk, *StoreAddress, Remaining);
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
  if (Depth > ::neverd::limits::ir::
                  kMedABIPassSupportResolveIndirectTargetAddrDepthLimit ||
      (Context && V.Size != Context->TRI.PointerSize))
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

std::optional<MedCallTarget> resolveMedCallTarget(const MedBlock &Block,
                                                  size_t Before,
                                                  const MedVar &Value,
                                                  size_t *WorkRemaining) {
  if (Before > Block.Ops.size() || !Value.Size || Value.Size > 8)
    return std::nullopt;
  MedVar Current = Value;
  size_t OwnWork = limits::ir::kSourceFrameAnalysisRemaining;
  if (!WorkRemaining)
    WorkRemaining = &OwnWork;
  for (unsigned Depth = 0; Depth < limits::kCallTargetSlotDepth; ++Depth) {
    if (!consumeCallTargetWork(WorkRemaining))
      return std::nullopt;
    if (Current.isConst())
      return MedCallTarget{Current.ConstVal, false};
    const MedOp *Definition = nullptr;
    for (size_t I = Before; I-- > 0;) {
      if (!consumeCallTargetWork(WorkRemaining))
        return std::nullopt;
      if (sameAddressValue(Block.Ops[I].Output, Current)) {
        Definition = &Block.Ops[I];
        Before = I;
        break;
      }
    }
    if (!Definition || !Definition->NumInputs ||
        Definition->Output.Size != Value.Size)
      return std::nullopt;
    const auto &Def = *Definition;
    if ((Def.Opcode == NdOp::COPY && Def.NumInputs == 1) ||
        (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs == 2 &&
         Def.Inputs[1].isConst() && Def.Inputs[1].ConstVal == 0)) {
      if (Def.Inputs[0].Size != Value.Size)
        return std::nullopt;
      Current = Def.Inputs[0];
      continue;
    }
    // LEA/ADR arithmetic is a callable value, not pointer storage. Prove its
    // finite-width value before summaries and SSA decide which entry inputs
    // the call reads. Later propagation cannot restore a discarded argument.
    if ((Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
        Def.MemoryOrdering == NdMemoryOrdering::None &&
        Def.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const auto Address =
          reduceAddr(Block, Current, WorkRemaining, Before + 1);
      if (Address && Address->Base.isConst()) {
        llvm::APInt Bits(Address->Offset.getBitWidth(), Address->Base.ConstVal,
                         false, true);
        Bits += Address->Offset;
        return MedCallTarget{Bits.getZExtValue(), false};
      }
      return std::nullopt;
    }
    if (Def.Opcode != NdOp::LOAD || Def.NumInputs != 1 ||
        Def.MemoryOrdering != NdMemoryOrdering::None ||
        Def.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    if (const auto Store = reachingStore(Block, static_cast<int>(Before),
                                         nullptr, 0, WorkRemaining)) {
      Current = *safety::detail::storedValue(Block.Ops[*Store]);
      Before = *Store;
      continue;
    }
    const auto Address =
        reduceAddr(Block, Def.Inputs[0], WorkRemaining, Before);
    if (Address && Address->Base.isConst()) {
      llvm::APInt Slot(Address->Offset.getBitWidth(), Address->Base.ConstVal,
                       false, true);
      Slot += Address->Offset;
      return MedCallTarget{Slot.getZExtValue(), true, Before};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

MedVar PreSSAMedCallTargets::read(MedVar Value) {
  if (!Value.isConst())
    Value.SSAVer =
        Versions[Key{Value.Kind, Value.Id, Value.RegOff, Value.Size}];
  return Value;
}

bool PreSSAMedCallTargets::version(const MedOp &Original,
                                   size_t *WorkRemaining) {
  if (!consumeCallTargetWork(WorkRemaining))
    return false;
  MedOp Op = Original;
  for (unsigned I = 0; I < Op.NumInputs; ++I)
    Op.Inputs[I] = read(Op.Inputs[I]);
  if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
      Op.Opcode == NdOp::INTRINSIC)
    for (auto &[K, Version] : Versions) {
      if (!consumeCallTargetWork(WorkRemaining))
        return false;
      if (std::get<0>(K) == MedVar::Reg &&
          (Op.Opcode == NdOp::INTRINSIC ||
           std::get<2>(K) != getTargetRegInfo(Architecture).StackPointer))
        Version = Next++;
    }
  if (Op.Output.Size && !Op.Output.isConst()) {
    // A narrow write also invalidates the old wider register value. Its
    // missing definition becomes an opaque root, never the preceding bits.
    if (Op.Output.Kind == MedVar::Reg) {
      if (Op.Output.RegOff > UINT64_MAX - Op.Output.Size)
        return false;
      for (auto &[K, Version] : Versions) {
        if (!consumeCallTargetWork(WorkRemaining))
          return false;
        if (std::get<0>(K) == MedVar::Reg &&
            std::get<2>(K) < Op.Output.RegOff + Op.Output.Size &&
            Op.Output.RegOff < std::get<2>(K) + std::get<3>(K))
          Version = Next++;
      }
    }
    Op.Output.SSAVer = Versions[Key{Op.Output.Kind, Op.Output.Id,
                                    Op.Output.RegOff, Op.Output.Size}] = Next++;
  }
  for (auto &Auxiliary : Op.IntrinsicOutputs) {
    if (!consumeCallTargetWork(WorkRemaining))
      return false;
    if (Auxiliary.Size && !Auxiliary.isConst())
      Auxiliary.SSAVer = Versions[Key{Auxiliary.Kind, Auxiliary.Id,
                                      Auxiliary.RegOff, Auxiliary.Size}] =
          Next++;
  }
  Proof.Ops.push_back(std::move(Op));
  return true;
}

std::optional<MedCallTarget>
PreSSAMedCallTargets::resolve(const MedBlock &Block, const MedVar &Value,
                              size_t *WorkRemaining) {
  if (Failed || Architecture == Arch::Unknown ||
      Block.Ops.size() > limits::kMaxCallSetupStackProofWork ||
      Value.Size != getTargetRegInfo(Architecture).PointerSize)
    return std::nullopt;
  size_t OwnWork = limits::ir::kSourceFrameAnalysisRemaining;
  if (!WorkRemaining)
    WorkRemaining = &OwnWork;
  // The versions of earlier operations do not depend on later ones, so the
  // block's prefix is versioned once across queries.
  while (Proof.Ops.size() < Block.Ops.size())
    if (!version(Block.Ops[Proof.Ops.size()], WorkRemaining)) {
      Failed = true;
      return std::nullopt;
    }
  return resolveMedCallTarget(Proof, Proof.Ops.size(), read(Value),
                              WorkRemaining);
}

std::optional<MedCallTarget> resolvePreSSAMedCallTarget(const MedBlock &Block,
                                                        const MedVar &Value,
                                                        Arch Architecture,
                                                        size_t *WorkRemaining) {
  return PreSSAMedCallTargets(Architecture)
      .resolve(Block, Value, WorkRemaining);
}

std::map<size_t, MedCallTarget>
resolveLowBlockCallTargets(const LowBlock &Block, Arch Architecture,
                           size_t *WorkRemaining) {
  std::map<size_t, MedCallTarget> Result;
  if (Architecture == Arch::Unknown ||
      Block.Ops.size() > limits::kMaxCallSetupStackProofWork ||
      std::none_of(Block.Ops.begin(), Block.Ops.end(), [](const LowOp &O) {
        return O.Opcode == NdOp::INDIR_CALL && O.NumInputs == 1 &&
               !O.Inputs[0].isConst();
      }))
    return Result;
  size_t OwnWork = limits::ir::kSourceFrameAnalysisRemaining;
  if (!WorkRemaining)
    WorkRemaining = &OwnWork;
  using Key = std::tuple<VnodeSpace, uint64_t, uint16_t>;
  std::map<Key, int> Identities;
  const auto Convert = [&](const NdVar &V) {
    if (V.isConst())
      return MedVar::makeConst(V.Offset, V.Size, V.Provenance,
                               V.AddressOwnerVA);
    MedVar M;
    M.Size = V.Size;
    M.TheArch = Architecture;
    M.Kind = V.isReg() ? MedVar::Reg : MedVar::Temp;
    if (V.isReg())
      M.RegOff = V.Offset;
    if (!V.isReg() && !V.isTemp())
      return MedVar::makeUnspecified(V.Size, Architecture);
    const auto [It, Inserted] = Identities.try_emplace(
        Key{V.Space, V.Offset, V.Size}, static_cast<int>(Identities.size()));
    M.Id = It->second;
    return M;
  };
  MedBlock Prefix;
  PreSSAMedCallTargets Targets(Architecture);
  for (size_t I = 0; I < Block.Ops.size(); ++I) {
    if (!consumeCallTargetWork(WorkRemaining))
      return {};
    const auto &O = Block.Ops[I];
    if (O.NumInputs > std::size(O.Inputs))
      return {};
    if (O.Opcode == NdOp::INDIR_CALL && O.NumInputs == 1 &&
        !O.Inputs[0].isConst())
      if (const auto Target =
              Targets.resolve(Prefix, Convert(O.Inputs[0]), WorkRemaining))
        Result.emplace(I, *Target);
    MedOp M;
    M.Opcode = O.Opcode;
    M.Addr = O.Addr;
    M.OriginSeq = O.Seq;
    M.MemoryOrdering = O.MemoryOrdering;
    M.MemoryAddressSpace = O.MemoryAddressSpace;
    M.Output = Convert(O.Output);
    for (unsigned J = 0; J < O.NumInputs; ++J)
      M.addInput(Convert(O.Inputs[J]));
    Prefix.Ops.push_back(std::move(M));
  }
  return *WorkRemaining ? Result : std::map<size_t, MedCallTarget>();
}

static int defIndexInBlock(const MedBlock &Blk, int FromIdx, const MedVar &V) {
  for (int J = FromIdx - 1; J >= 0; --J) {
    const auto &O = Blk.Ops[J];
    if (O.Output.Kind == V.Kind && O.Output.Id == V.Id &&
        O.Output.SSAVer == V.SSAVer && O.Output.RegOff == V.RegOff)
      return J;
  }
  return -1;
}

static int peelCopyDefIndex(const MedBlock &Blk, int FromIdx, MedVar &V) {
  int Idx = defIndexInBlock(Blk, FromIdx, V);
  for (int Depth = 0;
       Idx >= 0 &&
       Depth <
           ::neverd::limits::ir::kMedABIPassSupportPeelCopyDefIndexDepthLimit;
       ++Depth) {
    const MedOp &Def = Blk.Ops[Idx];
    if ((Def.Opcode != NdOp::COPY && Def.Opcode != NdOp::INT_ZEXT &&
         Def.Opcode != NdOp::INT_SEXT && Def.Opcode != NdOp::SUBBYTES) ||
        Def.NumInputs < 1)
      return Idx;
    if (Def.Opcode == NdOp::SUBBYTES &&
        (Def.NumInputs < 2 || !Def.Inputs[1].isConst() ||
         Def.Inputs[1].ConstVal != 0))
      return Idx;
    V = Def.Inputs[0];
    Idx = defIndexInBlock(Blk, Idx, V);
  }
  return Idx;
}

std::optional<MedVar> vtableCallObject(const MedBlock &Blk, int CallIdx,
                                       const MedVar &Target) {
  if (CallIdx <= 0 || Target.isConst())
    return std::nullopt;
  MedVar Slot = Target;
  const int SlotIdx = peelCopyDefIndex(Blk, CallIdx, Slot);
  if (SlotIdx < 0)
    return std::nullopt;
  const MedOp &SlotLoad = Blk.Ops[SlotIdx];
  if (SlotLoad.Opcode != NdOp::LOAD || SlotLoad.NumInputs < 1)
    return std::nullopt;
  MedVar Addr = SlotLoad.Inputs[0];
  const int AddrIdx = peelCopyDefIndex(Blk, SlotIdx, Addr);
  if (AddrIdx < 0)
    return std::nullopt;
  const MedOp &Add = Blk.Ops[AddrIdx];
  if ((Add.Opcode != NdOp::INT_ADD && Add.Opcode != NdOp::INT_OR) ||
      Add.NumInputs != 2)
    return std::nullopt;
  const MedVar *Vtbl = nullptr;
  uint64_t Off = 0;
  if (Add.Inputs[1].isConst()) {
    Off = Add.Inputs[1].ConstVal;
    Vtbl = &Add.Inputs[0];
  } else if (Add.Inputs[0].isConst()) {
    Off = Add.Inputs[0].ConstVal;
    Vtbl = &Add.Inputs[1];
  }
  if (!Vtbl || Off == 0)
    return std::nullopt;
  MedVar VtblVal = *Vtbl;
  const int VtblIdx = peelCopyDefIndex(Blk, AddrIdx, VtblVal);
  if (VtblIdx < 0)
    return std::nullopt;
  const MedOp &VtblLoad = Blk.Ops[VtblIdx];
  if (VtblLoad.Opcode != NdOp::LOAD || VtblLoad.NumInputs < 1)
    return std::nullopt;
  return VtblLoad.Inputs[0];
}

std::optional<int>
resolveIndirectTargetArgIdx(const MedBlock &Blk, int FromIdx,
                            const TargetRegInfo &TRI, const MedVar &V,
                            const IntegerArgumentLayout &Layout, int Depth) {
  if (Depth > ::neverd::limits::ir::
                  kMedABIPassSupportResolveIndirectTargetArgIdxDepthLimit ||
      V.isConst())
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
    int ArgIdx = Layout.registerIndex(V.RegOff);
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
      int ArgIdx = Layout.registerIndex(V.RegOff);
      return ArgIdx >= 0 ? std::optional<int>(ArgIdx) : std::nullopt;
    }
    return resolveIndirectTargetArgIdx(Blk, DefIdx, TRI, Def.Inputs[0], Layout,
                                       Depth + 1);
  case NdOp::SUBBYTES:
    return (Def.NumInputs >= 2 && Def.Inputs[1].isConst() &&
            Def.Inputs[1].ConstVal == 0)
               ? resolveIndirectTargetArgIdx(Blk, DefIdx, TRI, Def.Inputs[0],
                                             Layout, Depth + 1)
               : std::nullopt;
  case NdOp::LOAD: {
    const auto Store = reachingStore(Blk, DefIdx);
    if (!Store)
      return std::nullopt;
    return resolveIndirectTargetArgIdx(
        Blk, *Store, TRI, *safety::detail::storedValue(Blk.Ops[*Store]), Layout,
        Depth + 1);
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
      if (Op.Output.Size > 0 && sameDefIdentity(Op.Output, V)) {
        Record(Block, &Op, nullptr);
        if (Site.Ambiguous)
          return Site;
      }
  }
  return Site;
}

// The same sites as findFuncDef, keyed by its identity, built in one scan.
MedDefSite MedDefSiteIndex::find(const MedVar &V) {
  if (!Built) {
    for (const MedBlock &Block : Func.Blocks) {
      auto Record = [&](const MedVar &Output, const MedOp *Op,
                        const PhiNode *Phi) {
        auto [It, Inserted] = Sites.try_emplace(key(Output));
        if (Inserted)
          It->second = {&Block, Op, Phi, false};
        else
          It->second = {nullptr, nullptr, nullptr, true};
      };
      for (const PhiNode &Phi : Block.Phis)
        Record(Phi.Output, nullptr, &Phi);
      for (const MedOp &Op : Block.Ops)
        if (Op.Output.Size > 0)
          Record(Op.Output, &Op, nullptr);
    }
    Built = true;
  }
  const auto It = Sites.find(key(V));
  return It == Sites.end() ? MedDefSite{} : It->second;
}

// Unlike the heuristic argument scanner, call-boundary alias proofs may not
// accept truncation, guessed live-ins, loads, PHIs or overflowing arithmetic.
static std::optional<int64_t> authenticatedStackOffset(const MedFunc &Func,
                                                       const TargetRegInfo &TRI,
                                                       const MedVar &V,
                                                       unsigned Depth,
                                                       MedDefSiteIndex *Defs) {
  if (Depth > ::neverd::limits::ir::
                  kMedABIPassSupportAuthenticatedStackOffsetDepthLimit ||
      V.Size != TRI.PointerSize || V.isConst())
    return std::nullopt;
  const auto Site = Defs ? Defs->find(V) : findFuncDef(Func, V);
  if (Site.Ambiguous || !Site.Op || Site.Op->Output.Size != V.Size ||
      Site.Op->Output.TheArch != V.TheArch ||
      Site.Op->Output.RenameTag != V.RenameTag)
    return std::nullopt;
  const auto &Op = *Site.Op;
  if (V.Kind == MedVar::Reg && V.RegOff == TRI.StackPointer &&
      safety::detail::isAuthenticatedEntryRegisterLiveIn(Func, V))
    return 0;
  if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
    return authenticatedStackOffset(Func, TRI, Op.Inputs[0], Depth + 1, Defs);
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
  auto Base = authenticatedStackOffset(Func, TRI, Op.Inputs[BaseIndex],
                                       Depth + 1, Defs);
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
    // This summary never changes F. Share its definition index across all
    // store-address chains, including definitions after the stores themselves.
    MedDefSiteIndex DefSites(F);
    std::set<va_t> BlockEntries;
    for (const auto &B : F.Blocks)
      BlockEntries.insert(B.StartAddr);
    for (const auto &B : F.Blocks) {
      if (B.Ops.empty() ||
          (B.Succs.empty() && B.Ops.back().Opcode != NdOp::RETURN)) {
        Safe = false;
        break;
      }
      for (const auto &Op : B.Ops) {
        if (isAbiRecoveryBarrier(Op) || Op.Opcode == NdOp::INDIR_BR ||
            Op.Opcode >= NdOp::_COUNT ||
            safety::detail::isAtomicMemoryAccess(Op.Opcode)) {
          Safe = false;
          break;
        }
        if ((Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) &&
            (Op.NumInputs == 0 || !Op.Inputs[0].isConst() ||
             !BlockEntries.count(Op.Inputs[0].ConstVal))) {
          Safe = false;
          break;
        }
        if (Op.Opcode != NdOp::STORE)
          continue;
        const auto *Address = safety::detail::memoryAddress(Op);
        const auto *Value = safety::detail::storedValue(Op);
        auto Offset =
            Address ? authenticatedStackOffset(F, TRI, *Address, 0, &DefSites)
                    : std::nullopt;
        if (!Offset || !Value || Value->Size == 0 ||
            Op.MemoryOrdering != NdMemoryOrdering::None ||
            *Offset > -static_cast<int64_t>(Value->Size)) {
          Safe = false;
          break;
        }
      }
      if (!Safe)
        break;
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
  if (Depth >= ::neverd::limits::ir::
                   kMedABIPassSupportPreservesSpillAcrossCallDepthLimit ||
      !Context.FrameLocalLeafCallees || Context.TRI.TheArch != Arch::AArch64)
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

// One step in the heuristic stack-coordinate chain. Width casts retain the
// existing ABI recovery policy; SUBBYTES only preserves that coordinate when
// it selects byte zero. Keep operand roles so subtraction is never commuted.
struct StackAddressStep {
  const MedVar *Base;
  int64_t Delta;
  bool Subtract;
};

static std::optional<StackAddressStep> parseStackAddressStep(const MedOp &Op) {
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    if (Op.NumInputs == 1)
      return StackAddressStep{&Op.Inputs[0], 0, false};
    return std::nullopt;
  case NdOp::SUBBYTES:
    if (Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
        Op.Inputs[1].ConstVal == 0)
      return StackAddressStep{&Op.Inputs[0], 0, false};
    return std::nullopt;
  case NdOp::INT_ADD:
  case NdOp::INT_SUB: {
    if (Op.NumInputs != 2)
      return std::nullopt;
    const unsigned ConstantIndex = Op.Inputs[1].isConst() ? 1 : 0;
    const auto &Constant = Op.Inputs[ConstantIndex];
    const auto &Base = Op.Inputs[1 - ConstantIndex];
    if (!Constant.isConst() || Base.isConst() ||
        (Op.Opcode == NdOp::INT_SUB && ConstantIndex != 1))
      return std::nullopt;
    return StackAddressStep{&Base, static_cast<int64_t>(Constant.ConstVal),
                            Op.Opcode == NdOp::INT_SUB};
  }
  default:
    return std::nullopt;
  }
}

// stackPtrDelta's answer for a value: the offset, how many levels below the
// value its walk went, and whether the depth cap cut that walk short.
struct StackDeltaWalk {
  std::optional<int64_t> Delta;
  int Height = 0;
  bool Capped = false;
};

// stackPtrDelta's answers within one query, by value identity and depth. The
// depth cap makes an answer depend on the depth it was asked at; within that,
// a PHI DAG shares each answer instead of re-walking it once per path.
// Most queries hold a few answers, so the first ones stay inline.
using StackDeltaMemo =
    llvm::SmallDenseMap<std::tuple<int, int, int, uint64_t, int>,
                        StackDeltaWalk, 8>;

static StackDeltaWalk stackPtrDeltaAt(const MedFunc &Func,
                                      const TargetRegInfo &TRI, const MedVar &V,
                                      int Depth, MedDefSiteIndex *Defs,
                                      StackDeltaMemo &Memo);

std::optional<int64_t> stackPtrDelta(const MedFunc &Func,
                                     const TargetRegInfo &TRI, const MedVar &V,
                                     int Depth, MedDefSiteIndex *Defs,
                                     BinaryFormat Format) {
  StackDeltaMemo Memo;
  const auto Delta = stackPtrDeltaAt(Func, TRI, V, Depth, Defs, Memo).Delta;
  if (Delta || Depth)
    return Delta;
  // Reuse the entry-offset owner's complete loop proof when the local ABI
  // walk cannot resolve mutually dependent stack PHIs. Calls and outgoing
  // stores must share that origin instead of losing later stack arguments.
  return entryStackOffset(Func, V, TRI.TheArch, Format, Func.EntryKind);
}

static StackDeltaWalk stackPtrDeltaUncached(const MedFunc &Func,
                                            const TargetRegInfo &TRI,
                                            const MedVar &V, int Depth,
                                            MedDefSiteIndex *Defs,
                                            StackDeltaMemo &Memo) {
  StackDeltaWalk Walk;
  auto Below = [&](const MedVar &Next) {
    const StackDeltaWalk Step =
        stackPtrDeltaAt(Func, TRI, Next, Depth + 1, Defs, Memo);
    Walk.Height = std::max(Walk.Height, Step.Height + 1);
    Walk.Capped |= Step.Capped;
    return Step.Delta;
  };
  const MedDefSite Site = Defs ? Defs->find(V) : findFuncDef(Func, V);
  if (Site.Ambiguous)
    return Walk;
  if (Site.Phi) {
    std::optional<int64_t> Common;
    for (const auto &[Pred, Incoming] : Site.Phi->Args) {
      (void)Pred;
      if (sameDefIdentity(Incoming, V))
        continue;
      auto Delta = Below(Incoming);
      if (!Delta || (Common && *Common != *Delta))
        return Walk;
      Common = *Delta;
    }
    Walk.Delta = Common;
    return Walk;
  }
  if (!Site.Op) {
    // No function-wide definition: the live-in stack pointer is the zero
    // reference; anything else is not SP-derived.
    if (V.Kind == MedVar::Reg && V.RegOff == TRI.StackPointer)
      Walk.Delta = 0;
    return Walk;
  }
  const MedOp *Def = Site.Op;
  const MedBlock &DefBlock = *Site.Block;

  if (Def->Opcode == NdOp::COPY && Def->NumInputs == 1) {
    const MedVar &In = Def->Inputs[0];
    // The entry stack-pointer self-copy (`COPY ESP, ESP`, the live-in marker)
    // is the zero reference; following it would recurse forever to the depth
    // cap and report the whole push chain as un-traceable, collapsing every
    // pushed argument onto slot 0.
    if (In.Kind == MedVar::Reg && In.RegOff == TRI.StackPointer &&
        In.Id == V.Id && In.SSAVer == V.SSAVer) {
      Walk.Delta = 0;
      return Walk;
    }
  }
  if (auto Step = parseStackAddressStep(*Def)) {
    if (auto Base = Below(*Step->Base))
      Walk.Delta = safety::detail::checkedStackOffset(*Base, Step->Delta,
                                                      Step->Subtract);
    return Walk;
  }
  if (Def->Opcode == NdOp::LOAD) {
    const int LoadIndex = static_cast<int>(Def - DefBlock.Ops.data());
    if (const auto Store = reachingStore(DefBlock, LoadIndex))
      Walk.Delta = Below(*safety::detail::storedValue(DefBlock.Ops[*Store]));
    return Walk;
  }
  return Walk;
}

static StackDeltaWalk stackPtrDeltaAt(const MedFunc &Func,
                                      const TargetRegInfo &TRI, const MedVar &V,
                                      int Depth, MedDefSiteIndex *Defs,
                                      StackDeltaMemo &Memo) {
  if (V.isConst())
    return {};
  if (Depth > ::neverd::limits::ir::kMedABIPassSupportStackPtrDeltaAtDepthLimit)
    return {std::nullopt, 0, /*Capped=*/true};
  // An answer no cap shaped serves any query whose walk stays under the cap,
  // so the calls of one function share their stack-pointer chains.
  if (Defs)
    if (const auto *Known = Defs->stackDelta(V, TRI.StackPointer);
        Known &&
        Depth + Known->Height <=
            ::neverd::limits::ir::kMedABIPassSupportStackPtrDeltaAtDepthLimit)
      return {Known->Delta, Known->Height, false};
  // The answer reads V only through its definition identity.
  const auto Key = std::make_tuple(static_cast<int>(V.Kind), V.Id, V.SSAVer,
                                   V.RegOff, Depth);
  if (const auto It = Memo.find(Key); It != Memo.end())
    return It->second;
  const auto Walk = stackPtrDeltaUncached(Func, TRI, V, Depth, Defs, Memo);
  Memo.try_emplace(Key, Walk);
  if (Defs && !Walk.Capped)
    Defs->rememberStackDelta(V, TRI.StackPointer, {Walk.Delta, Walk.Height});
  return Walk;
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
  if (Depth > ::neverd::limits::ir::
                  kMedABIPassSupportDerivesFromFrameRegDepthLimit ||
      V.isConst())
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
  auto Step = parseStackAddressStep(*Def);
  return Step && derivesFromFrameReg(Blk, TRI, *Step->Base, Depth + 1);
}

// Identifies an SSA stack-pointer value (Id, version, register offset).
static SpOffsetKey spOffsetKey(const MedVar &V) {
  return {V.Id, V.SSAVer, V.RegOff};
}

// Records, for each stack-pointer value on the call-site SP's definition chain,
// its byte offset *above* the call SP (call SP = 0, each earlier `push`'s SP
// one slot higher). Copies, width casts and byte-zero SUBBYTES keep the
// offset; a `sub`/`add` of a constant shifts it. Used to place pushed
// arguments relative to the call SP when the absolute entry-relative delta is
// unavailable (a post-loop push chain whose SP threads a loop-carried PHI,
// where the chain round-trips ESP<->RSP and never reaches the entry SP as a
// constant).
void buildCallSpOffsets(const MedFunc &Func, const TargetRegInfo &TRI,
                        const MedVar &V, int64_t Off,
                        std::map<SpOffsetKey, int64_t> &Map, int Depth,
                        MedDefSiteIndex *Defs) {
  if (Depth > ::neverd::limits::ir::
                  kMedABIPassSupportBuildCallSpOffsetsDepthLimit ||
      V.isConst())
    return;
  if (!Map.emplace(spOffsetKey(V), Off).second)
    return; // already visited (e.g. the entry SP self-copy)
  const MedDefSite Site = Defs ? Defs->find(V) : findFuncDef(Func, V);
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
  auto Step = parseStackAddressStep(*Def);
  if (!Step)
    return;
  // Walk from the result to its base by reversing the operation, without
  // negating a possibly minimum signed displacement.
  auto BaseOffset =
      safety::detail::checkedStackOffset(Off, Step->Delta, !Step->Subtract);
  if (BaseOffset)
    buildCallSpOffsets(Func, TRI, *Step->Base, *BaseOffset, Map, Depth + 1,
                       Defs);
}

// Offset of store-address \p V above the call SP, resolved against the call
// SP's offset map.  Follows \p V's definition chain (offset-preserving casts,
// and constant add/sub) until it reaches a value on the call SP chain.  nullopt
// when the address is not stack-pointer derived.
std::optional<int64_t> relStackOff(const MedFunc &Func,
                                   const TargetRegInfo &TRI, const MedVar &V,
                                   const std::map<SpOffsetKey, int64_t> &Map,
                                   int Depth, MedDefSiteIndex *Defs) {
  if (Depth > ::neverd::limits::ir::kMedABIPassSupportRelStackOffDepthLimit ||
      V.isConst())
    return std::nullopt;
  if (auto It = Map.find(spOffsetKey(V)); It != Map.end())
    return It->second;
  const MedDefSite Site = Defs ? Defs->find(V) : findFuncDef(Func, V);
  if (Site.Ambiguous)
    return std::nullopt;
  if (Site.Phi) {
    std::optional<int64_t> Common;
    for (const auto &[Pred, Incoming] : Site.Phi->Args) {
      (void)Pred;
      auto Rel = relStackOff(Func, TRI, Incoming, Map, Depth + 1, Defs);
      if (!Rel || (Common && *Common != *Rel))
        return std::nullopt;
      Common = *Rel;
    }
    return Common;
  }
  const MedOp *Def = Site.Op;
  if (!Def)
    return std::nullopt;
  auto Step = parseStackAddressStep(*Def);
  if (!Step)
    return std::nullopt;
  auto Base = relStackOff(Func, TRI, *Step->Base, Map, Depth + 1, Defs);
  if (!Base)
    return std::nullopt;
  return safety::detail::checkedStackOffset(*Base, Step->Delta, Step->Subtract);
}

const RelocationEntry *RelocationSymbolIndex::at(va_t Address) const {
  if (!Named) {
    Named.emplace();
    for (size_t I = 0; I < Img.Relocations.size(); ++I)
      if (!Img.Relocations[I].SymbolName.empty())
        Named->emplace_back(Img.Relocations[I].Address, I);
    std::sort(Named->begin(), Named->end());
  }
  const auto It = std::lower_bound(Named->begin(), Named->end(),
                                   std::make_pair(Address, size_t{0}));
  return It != Named->end() && It->first == Address
             ? &Img.Relocations[It->second]
             : nullptr;
}

std::string relocCalleeName(const BinaryImage &Img, va_t InsnAddr,
                            const RelocationSymbolIndex *Index) {
  // The branch relocation sits at the instruction's displacement field: at
  // the instruction address itself for AArch64/ARM `bl` (the imm is in the
  // opcode word), but one byte in for x86 `call rel32` (the 0xE8 opcode
  // precedes the 4-byte displacement).  Accept both so a relocatable-object
  // external call is named on x86 too (otherwise its placeholder-0 target
  // resolves to whatever symbol sits at VA 0 — e.g. the function itself,
  // lifting a libc call into a bogus self-recursion).  The first such
  // relocation in the image's order names it.
  const RelocationEntry *Rel = nullptr;
  if (Index) {
    const RelocationEntry *At = Index->at(InsnAddr);
    const RelocationEntry *After = Index->at(InsnAddr + 1);
    Rel = !At || (After && After < At) ? After : At;
  } else {
    for (const auto &Candidate : Img.Relocations)
      if ((Candidate.Address == InsnAddr ||
           Candidate.Address == InsnAddr + 1) &&
          !Candidate.SymbolName.empty()) {
        Rel = &Candidate;
        break;
      }
  }
  if (!Rel)
    return {};
  llvm::StringRef Name = stripLeadingUnderscores(Rel->SymbolName);
  if (libc::isKnownFunction(Name))
    return Name.str();
  return Rel->SymbolName;
}

// The value an argument-register-defining op contributes to its slot: the
// source of a copy, or — for a sub-register sync that narrows the register's
// own wider value (`EDI := low32(RDI)` emitted after a 64-bit write) — that
// wider value, so a struct or other wide argument keeps its high bits instead
// of the truncated sub-register view.  Otherwise the op's output itself.
static MedVar argRegSourceValue(const MedOp &Op) {
  if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1)
    return Op.Inputs[0];
  if (Op.Opcode == NdOp::SUBBYTES && Op.OriginSeq < 0 && Op.NumInputs >= 2 &&
      Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal == 0 &&
      Op.Inputs[0].Kind == MedVar::Reg &&
      Op.Inputs[0].RegOff == Op.Output.RegOff &&
      Op.Inputs[0].Size > Op.Output.Size)
    return Op.Inputs[0];
  return Op.Output;
}

// Resolve synthetic low-register observations back to the whole argument the
// register holds: its nearest write, where that writes the same source.  SSA
// syncs a low register where an instruction reads it, so the write may be an
// earlier instruction's (`lea rdi, [rip + s]` before `mov ebx, edi`).
// Multiple sibling aliases may intervene, but a real partial write or a call
// ends the proof. Source identity includes constant provenance and owner as
// well as the exact register/temporary SSA occurrence; a temporary the block
// copies a value into stands for that value (`lea` computes the address into
// one, copies it to the register and syncs the low bytes from it).
int wholeArgRegWriteInBlock(const MedBlock &Blk, int J,
                            const TargetRegInfo &TRI,
                            const IntegerArgumentLayout &Layout) {
  const MedOp &Op = Blk.Ops[J];
  // Only a sub-register sync whose source is not the register itself needs the
  // sibling lookup; everything else (a plain copy, or `subpiece(Xn, 0)`) is
  // already resolved by argRegSourceValue.
  if (Op.Opcode != NdOp::SUBBYTES || Op.NumInputs < 2 ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].ConstVal != 0 ||
      Op.Output.Kind != MedVar::Reg ||
      (Op.Inputs[0].Kind == MedVar::Reg &&
       Op.Inputs[0].RegOff == Op.Output.RegOff))
    return -1;
  const MedVar &Src = Op.Inputs[0];
  const int ArgIdx = Layout.registerIndex(Op.Output.RegOff);
  if (Op.OriginSeq >= 0 || Src.Size <= Op.Output.Size || ArgIdx < 0)
    return -1;
  auto matches = [](const MedVar &Candidate, const MedVar &Value) {
    return sameAddressValue(Candidate, Value) &&
           (!Value.isConst() ||
            (Candidate.ConstVal == Value.ConstVal &&
             Candidate.Provenance == Value.Provenance &&
             Candidate.AddressOwnerVA == Value.AddressOwnerVA));
  };
  // The value \p V holds where the block copied one into it before the sync.
  auto valueOf = [&](const MedVar &V) -> const MedVar & {
    if (!V.isConst())
      for (int K = J - 1; K >= 0; --K) {
        const MedOp &D = Blk.Ops[K];
        if (D.Opcode == NdOp::COPY && D.NumInputs == 1 &&
            sameAddressValue(D.Output, V))
          return D.Inputs[0];
      }
    return V;
  };
  auto sameSource = [&](const MedVar &Candidate) {
    return matches(Candidate, Src) || matches(valueOf(Candidate), valueOf(Src));
  };
  const uint64_t Register = Layout.Registers[ArgIdx];
  for (int K = J - 1; K >= 0; --K) {
    const MedOp &W = Blk.Ops[K];
    if (isAbiRecoveryBarrier(W))
      break;
    if (W.Output.Kind != MedVar::Reg || W.Output.Size == 0)
      continue;
    const bool Overlaps = W.Output.RegOff < Register + TRI.FullRegWidth &&
                          W.Output.RegOff + W.Output.Size > Register;
    if (!Overlaps)
      continue;
    // Several synthetic aliases can follow the same full-register write.
    // They are observations, not successive writes of narrower arguments.
    if (W.Opcode == NdOp::SUBBYTES && W.OriginSeq < 0 && W.NumInputs == 2 &&
        W.Output.RegOff == Register && W.Output.Size < Src.Size &&
        W.Inputs[1].isConst() && W.Inputs[1].ConstVal == 0 &&
        sameSource(W.Inputs[0]))
      continue;
    if (W.Opcode == NdOp::COPY && W.Output.RegOff == Register &&
        W.Output.Size == Src.Size && W.NumInputs == 1 &&
        sameSource(W.Inputs[0]))
      return K;
    break;
  }
  return -1;
}

MedVar argRegSourceValueInBlock(const MedBlock &Blk, int J,
                                const TargetRegInfo &TRI,
                                const IntegerArgumentLayout &Layout) {
  if (wholeArgRegWriteInBlock(Blk, J, TRI, Layout) >= 0)
    return Blk.Ops[J].Inputs[0];
  return argRegSourceValue(Blk.Ops[J]);
}

const PhiNode *selectAuthoritativeArgPhi(const MedFunc &Func,
                                         const MedBlock &Block,
                                         const TargetRegInfo &TRI, int ArgIdx,
                                         const IntegerArgumentLayout &Layout) {
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
        Layout.registerIndex(Phi.Output.RegOff) != ArgIdx)
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

const PhiNode *argPhiReachingCall(const MedFunc &Func, const MedBlock &Block,
                                  int CallIdx, const TargetRegInfo &TRI,
                                  int ArgIdx,
                                  const IntegerArgumentLayout &Layout,
                                  BinaryFormat Format) {
  if (ArgIdx < 0 || ArgIdx >= static_cast<int>(Layout.Registers.size()))
    return nullptr;
  const uint64_t ArgReg = Layout.Registers[ArgIdx];
  const auto ArgBytes = static_cast<uint16_t>(TRI.PointerSize);
  auto writesArgument = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.Size > 0 &&
           Layout.registerIndex(V.RegOff) == ArgIdx;
  };
  const int End = std::min(CallIdx, static_cast<int>(Block.Ops.size()));
  for (int J = 0; J < End; ++J) {
    const MedOp &Op = Block.Ops[J];
    if (writesArgument(Op.Output) ||
        llvm::any_of(Op.IntrinsicOutputs, writesArgument) ||
        callClobbersRegister(Op, ArgReg, ArgBytes, TRI, Format))
      return nullptr;
  }
  return selectAuthoritativeArgPhi(Func, Block, TRI, ArgIdx, Layout);
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
std::optional<MedVar>
findReachingArgReg(const MedFunc &Func, const TargetRegInfo &TRI, Arch TheArch,
                   int BlockId, int ArgIdx, const IntegerArgumentLayout &Layout,
                   bool AllowUnknownLiveIn, bool *FromLiveIn, bool *FoundDef,
                   int CallIdx, BinaryFormat Format,
                   const MedBlockIdIndex *Blocks) {
  std::optional<MedBlockIdIndex> OwnBlocks;
  const MedBlockIdIndex &ById = Blocks ? *Blocks : OwnBlocks.emplace(Func);

  const uint64_t ArgReg =
      ArgIdx >= 0 && ArgIdx < static_cast<int>(Layout.Registers.size())
          ? Layout.Registers[ArgIdx]
          : 0;
  const auto ArgBytes = static_cast<uint16_t>(TRI.PointerSize);
  // The nearest write to the register before \p UpTo, or what the nearest
  // call that may change the register left there (\p Left).  In the call's
  // own block another barrier to call-setup recovery (an intrinsic not
  // proven to keep the setup) ends the search there, as the caller's
  // in-block scan stops at it.
  auto scanOps = [&](const MedBlock &B, int UpTo, bool &Left,
                     bool CallBlock = false) -> std::optional<MedVar> {
    for (int J = UpTo - 1; J >= 0; --J) {
      const auto &Op = B.Ops[J];
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0 &&
          Layout.registerIndex(Op.Output.RegOff) == ArgIdx)
        return argRegSourceValueInBlock(B, J, TRI, Layout);
      for (const MedVar &Aux : Op.IntrinsicOutputs)
        if (Aux.Kind == MedVar::Reg && Aux.Size > 0 &&
            Layout.registerIndex(Aux.RegOff) == ArgIdx)
          return Aux;
      if (ArgReg)
        if (auto Value =
                valueLeftByCall(Func, Op, ArgReg, ArgBytes, TRI, Format)) {
          Left = true;
          return Value;
        }
      if (CallBlock && Op.Opcode == NdOp::INTRINSIC && isAbiRecoveryBarrier(Op))
        return std::nullopt;
    }
    return std::nullopt;
  };

  if (FromLiveIn)
    *FromLiveIn = false;
  if (FoundDef)
    *FoundDef = false;

  const MedBlock *LastCallBlock = ById.last(BlockId);
  if (!LastCallBlock)
    return std::nullopt;
  const MedBlock &CallBlock = *LastCallBlock;
  // The caller took a write in the call's block before its call-boundary
  // stop; whatever reaches the call also passes the call at that stop.
  if (CallIdx >= 0) {
    bool Left = false;
    if (auto V = scanOps(
            CallBlock,
            std::min(CallIdx, static_cast<int>(CallBlock.Ops.size())), Left,
            /*CallBlock=*/true)) {
      if (FoundDef)
        *FoundDef = !Left;
      return V;
    }
  }
  if (const PhiNode *Phi =
          selectAuthoritativeArgPhi(Func, CallBlock, TRI, ArgIdx, Layout)) {
    if (FoundDef)
      *FoundDef = true;
    return Phi->Output;
  }

  // A write or PHI ends a path back from the call, and so does a call that
  // may change the register, or an exception handler's entry, where the
  // unwinder leaves caller-saved registers unspecified.  The nearest write
  // on any path wins, and the incoming value does where a path reaches the
  // function's entry: only where calls end every path is the value theirs.
  std::optional<MedVar> FirstLeft;
  bool ReachedEntry = false;
  std::set<int> Visited{BlockId};
  std::vector<int> Work;
  auto continuePaths = [&](const MedBlock &B) {
    if (!B.ExceptionalPreds.empty() && !FirstLeft)
      FirstLeft = MedVar::makeUnspecified(ArgBytes, TheArch);
    if (B.Preds.empty() && B.ExceptionalPreds.empty())
      ReachedEntry = true;
    Work.insert(Work.end(), B.Preds.begin(), B.Preds.end());
  };
  continuePaths(CallBlock);
  while (!Work.empty()) {
    int BId = Work.back();
    Work.pop_back();
    if (!Visited.insert(BId).second)
      continue;
    const MedBlock *Pred = ById.last(BId);
    if (!Pred)
      continue;
    const MedBlock &B = *Pred;
    bool Left = false;
    if (auto V = scanOps(B, static_cast<int>(B.Ops.size()), Left)) {
      if (!Left) {
        if (FoundDef)
          *FoundDef = true;
        return V;
      }
      if (!FirstLeft)
        FirstLeft = V;
      continue;
    }
    if (const PhiNode *Phi =
            selectAuthoritativeArgPhi(Func, B, TRI, ArgIdx, Layout)) {
      if (FoundDef)
        *FoundDef = true;
      return Phi->Output;
    }
    continuePaths(B);
  }
  if (FirstLeft && !ReachedEntry)
    return FirstLeft;

  // No in-function definition reaches the call: the value is the live-in
  // (incoming parameter), as when a caller forwards its own argument straight
  // to the callee (`return callee(a)`), leaving no write for the scans to find.
  // Recover it only when this argument register is a real parameter of the
  // current function, so an uninitialised caller-saved register is never
  // invented as an argument.
  llvm::ArrayRef<uint64_t> ParamRegs = Layout.Registers;
  if (ArgIdx >= 0 && ArgIdx < static_cast<int>(ParamRegs.size())) {
    uint64_t Reg = ParamRegs[ArgIdx];
    for (const auto &P : Func.Params)
      if ((P.Kind == MedVar::Reg || P.Kind == MedVar::Param) &&
          P.RegOff == Reg) {
        // The parameter itself, named by its register as a promoted
        // forwarder parameter is: no register variable of this function
        // need hold the incoming value, so a register id would name another
        // value.
        MedVar V;
        V.Kind = MedVar::Param;
        V.Id = -1;
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

std::optional<MedVar> uniquePredNonNullGuard(const MedFunc &Func,
                                             const MedBlock &Blk) {
  auto blockById = [&](int Id) -> const MedBlock * {
    for (const auto &B : Func.Blocks)
      if (B.Id == Id)
        return &B;
    return nullptr;
  };
  auto uniquePred = [&](const MedBlock &B) -> const MedBlock * {
    const MedBlock *Found = nullptr;
    for (int PredId : B.Preds) {
      const MedBlock *Cand = blockById(PredId);
      if (!Cand)
        continue;
      if (Found)
        return nullptr;
      Found = Cand;
    }
    return Found;
  };
  auto lastPointerTest = [&](const MedBlock &Pred) -> std::optional<MedVar> {
    const MedVar *Best = nullptr;
    for (const auto &Op : Pred.Ops) {
      if (Op.Opcode == NdOp::LOAD && Op.Output.Size >= 4 &&
          !Op.Output.isConst())
        Best = &Op.Output;
      if (Op.Opcode == NdOp::INT_AND && Op.NumInputs >= 2 &&
          Op.Inputs[0].Size >= 4 && !Op.Inputs[0].isConst() &&
          Op.Inputs[0].Kind == Op.Inputs[1].Kind &&
          Op.Inputs[0].Id == Op.Inputs[1].Id &&
          Op.Inputs[0].SSAVer == Op.Inputs[1].SSAVer)
        Best = &Op.Inputs[0];
      if ((Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL) &&
          Op.NumInputs >= 2) {
        const MedVar *Tested = nullptr;
        if (Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal == 0)
          Tested = &Op.Inputs[0];
        else if (Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == 0)
          Tested = &Op.Inputs[1];
        if (Tested && Tested->Size >= 4 && !Tested->isConst())
          Best = Tested;
      }
    }
    if (Best)
      return *Best;
    return std::nullopt;
  };
  const MedBlock *Cur = uniquePred(Blk);
  if (!Cur) {
    const MedBlock *CondPred = nullptr;
    for (int PredId : Blk.Preds) {
      const MedBlock *Cand = blockById(PredId);
      if (!Cand || Cand->Ops.empty() ||
          Cand->Ops.back().Opcode != NdOp::COND_BR)
        continue;
      if (CondPred) {
        CondPred = nullptr;
        break;
      }
      CondPred = Cand;
    }
    Cur = CondPred;
  }
  for (unsigned I = 0; I < 8 && Cur; ++I) {
    if (Cur->Ops.empty() || Cur->Ops.back().Opcode != NdOp::COND_BR ||
        Cur->Ops.back().NumInputs < 2) {
      Cur = uniquePred(*Cur);
      continue;
    }
    const MedOp &Term = Cur->Ops.back();
    MedVar Cond = Term.Inputs[0].isConst() ? Term.Inputs[1] : Term.Inputs[0];
    auto defInPred = [&](const MedVar &V) -> const MedOp * {
      const MedOp *Def = nullptr;
      for (const auto &Op : Cur->Ops) {
        if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
            Op.Output.SSAVer == V.SSAVer && Op.Output.RegOff == V.RegOff)
          Def = &Op;
      }
      return Def;
    };
    std::set<std::tuple<int, int, int, uint64_t>> Seen;
    for (unsigned Depth = 0;
         Depth < ::neverd::limits::ir::
                     kMedABIPassSupportUniquePredNonNullGuardDepthLimit;
         ++Depth) {
      if (!Seen.insert({static_cast<int>(Cond.Kind), Cond.Id, Cond.SSAVer,
                        Cond.RegOff})
               .second)
        break;
      const MedOp *Def = defInPred(Cond);
      if (!Def) {
        if (Cond.Size >= 4 && !Cond.isConst())
          return Cond;
        break;
      }
      if (Def->Opcode == NdOp::BOOL_NOT && Def->NumInputs >= 1) {
        Cond = Def->Inputs[0];
        continue;
      }
      if ((Def->Opcode == NdOp::INT_EQUAL ||
           Def->Opcode == NdOp::INT_NOTEQUAL) &&
          Def->NumInputs >= 2) {
        const MedVar *Tested = nullptr;
        if (Def->Inputs[1].isConst() && Def->Inputs[1].ConstVal == 0)
          Tested = &Def->Inputs[0];
        else if (Def->Inputs[0].isConst() && Def->Inputs[0].ConstVal == 0)
          Tested = &Def->Inputs[1];
        if (Tested && Tested->Size >= 4 && !Tested->isConst())
          return *Tested;
        break;
      }
      if (Def->Opcode == NdOp::INT_AND && Def->NumInputs >= 2 &&
          Def->Inputs[0].Size >= 4 && !Def->Inputs[0].isConst() &&
          Def->Inputs[0].Kind == Def->Inputs[1].Kind &&
          Def->Inputs[0].Id == Def->Inputs[1].Id &&
          Def->Inputs[0].SSAVer == Def->Inputs[1].SSAVer)
        return Def->Inputs[0];
      if ((Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
           Def->Opcode == NdOp::INT_SEXT) &&
          Def->NumInputs >= 1) {
        Cond = Def->Inputs[0];
        continue;
      }
      if (Def->Opcode == NdOp::LOAD && Def->Output.Size >= 4)
        return Def->Output;
      if (Def->Output.Size >= 4 && !Def->Output.isConst())
        return Def->Output;
      break;
    }
    if (auto Fallback = lastPointerTest(*Cur))
      return Fallback;
    Cur = uniquePred(*Cur);
  }
  return std::nullopt;
}

} // namespace neverd
