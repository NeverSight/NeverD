//===- MedLLVMFrameResolve.cpp - Frame/stack-pointer classification ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Frame-derived / stack-pointer classification and dynamic (VLA) stack-
/// allocation recovery for MedLLVMEmitter.  These answer "does this address
/// trace back to the frame / stack pointer?" and rebuild `alloca`s from the
/// `sp - size` idiom, feeding getVar/setVar in MedLLVMVarAccess.cpp.  Shared
/// address tracing remains in MedLLVMAddrResolve.cpp; literal/select,
/// indexed/induction, and code-pointer resolution live in their dedicated
/// translation units.  Every routine here is a MedLLVMEmitter member declared
/// in the shared header, so this is a pure translation-unit split.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedDispatchSlots.h"
#include "neverd/ir/med/MedDynamicStack.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace neverd {

namespace {
// Hashes the query-local keys of canonicalFrameSlotKey's walks. They are
// only probed, inserted and erased, so their order cannot reach an answer.
struct FrameProofKeyHash {
  template <typename Key> size_t operator()(const Key &K) const {
    return llvm::hash_value(K);
  }
};
// {(Id<<32)|SSAVer, Kind} key for the per-function frame-derived memo, matching
// the DefIndex/PhiIndex scheme (lossless for 32-bit Id/SSAVer; the small Kind
// enum never collides with DenseMap's int sentinels).
std::pair<int64_t, int> frameKey(const MedVar &V) {
  return {static_cast<int64_t>(
              (static_cast<uint64_t>(static_cast<uint32_t>(V.Id)) << 32) |
              static_cast<uint32_t>(V.SSAVer)),
          static_cast<int>(V.Kind)};
}
} // namespace

// Recursive worker backed by a visited set, so each distinct (Kind,Id,SSAVer)
// SSA value in the operand DAG is examined at most once.  Without it, a value
// reachable through many shared sub-expressions — e.g. a 16-way switch whose
// merge PHI feeds a loop-carried i64 accumulator, where the i386 register-pair
// halves fan back into the loop-header PHI — is re-expanded along every path,
// which is exponential (observed as a ~50s lift of a single i386 function).
// The visited set makes the walk linear in the value graph and also guarantees
// termination on cyclic SSA (loop back-edges), so no recursion-depth cap is
// needed: the answer is the exact "does this value's address arithmetic trace
// back to the stack pointer", independent of the path/depth at which a node
// happens to be reached.  Definitions/PHIs are resolved through the O(1)
// lookupDef/lookupPhi index instead of rescanning every op in the function,
// which made this the dominant cost of emit() on large functions.
bool MedLLVMEmitter::frameDerivedRec(
    const MedVar &V, llvm::DenseSet<std::pair<int64_t, int>> &Visited) const {
  if (V.isConst() || !CurMedFunc)
    return false;

  if (V.Kind == MedVar::Reg) {
    uint64_t SpOff = getTargetRegInfo(TargetArch).StackPointer;
    if (SpOff != 0 && V.RegOff == SpOff)
      return true;
  }

  // Examine each SSA value once; revisits of a shared node or a back-edge
  // collapse here instead of fanning out into an exponential re-expansion.
  if (!Visited.insert(frameKey(V)).second)
    return false;

  const MedOp *Def = lookupDef(V);
  if (Def && Def->RegistrationRoot != MedOp::RegistrationRootKind::None)
    return true;
  if (!Def) {
    // A PHI fed by any frame-derived predecessor — an induction pointer walking
    // a stack array, `p = PHI(sp-k, p+stride)` — is itself frame-derived.  PHIs
    // live in Blk.Phis, not Blk.Ops, so lookupDef never sees them; without this
    // the induction store address is mistaken for an absolute const-base store,
    // poisoning StoredConstBases and disabling rodata table redirection for the
    // whole function (e.g. clang's table-driven CRC alongside msg[]).
    if (const PhiNode *Phi = lookupPhi(V))
      for (const auto &[PredId, Arg] : Phi->Args)
        if (phiIncomingEdgeFeasible(*Phi, PredId) &&
            frameDerivedRec(Arg, Visited))
          return true;
    return false;
  }

  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
    for (int I = 0; I < Def->NumInputs; ++I)
      if (frameDerivedRec(Def->Inputs[I], Visited))
        return true;
    return false;
  default:
    return false;
  }
}

bool MedLLVMEmitter::varIsFrameDerived(const MedVar &V, int /*Depth*/) const {
  if (V.isConst() || !CurMedFunc)
    return false;

  // The frame-derived property of a value is a pure function of the operand
  // graph, so memoize per top-level query and reuse across the many per-operand
  // queries the op emitter makes for the same function.
  if (FrameDerivedCacheFor != CurMedFunc) {
    FrameDerivedCacheFor = CurMedFunc;
    FrameDerivedCache.clear();
  }
  auto Key = frameKey(V);
  auto It = FrameDerivedCache.find(Key);
  if (It != FrameDerivedCache.end())
    return It->second;

  llvm::DenseSet<std::pair<int64_t, int>> Visited;
  bool Result = frameDerivedRec(V, Visited);
  FrameDerivedCache[Key] = Result;
  return Result;
}

bool MedLLVMEmitter::frameAddressRec(
    const MedVar &V, llvm::DenseSet<std::pair<int64_t, int>> &Visited) const {
  if (V.isConst() || !CurMedFunc)
    return false;

  const MedOp *Def = lookupDef(V);
  const PhiNode *Phi = lookupPhi(V);
  if (V.Kind == MedVar::Reg) {
    const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
    auto sameSSAValue = [](const MedVar &A, const MedVar &B) {
      return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
             A.SSAVer == B.SSAVer && A.RegOff == B.RegOff && A.Size == B.Size;
    };
    const bool IsSelfCopy =
        Def && Def->Opcode == NdOp::COPY && Def->NumInputs == 1 &&
        sameSSAValue(Def->Output, V) && sameSSAValue(Def->Inputs[0], V);
    // A physical register name is not frame provenance after SSA has assigned
    // it a new value.  Optimized i386 routinely reuses EBP as a scalar loop
    // index after the prologue save; treating every EBP version as a frame
    // root makes unrelated rodata indexing look like an opaque stack reload.
    // Only the unmerged live-in (normally the lifter's exact self-COPY) is a
    // root.  Derived SP/FP versions are discovered structurally below.
    const bool IsUnmergedLiveIn = !Phi && (!Def || IsSelfCopy);
    if (IsUnmergedLiveIn &&
        ((TRI.StackPointer != 0 && V.RegOff == TRI.StackPointer) ||
         (TRI.FramePointer != 0 && V.RegOff == TRI.FramePointer)))
      return true;
  }

  if (!Visited.insert(frameKey(V)).second)
    return false;

  if (Phi)
    for (const auto &[PredId, Arg] : Phi->Args)
      if (phiIncomingEdgeFeasible(*Phi, PredId) &&
          frameAddressRec(Arg, Visited))
        return true;

  if (!Def)
    return false;
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
    for (uint8_t I = 0; I < Def->NumInputs; ++I)
      if (frameAddressRec(Def->Inputs[I], Visited))
        return true;
    return false;
  case NdOp::SELECT:
    if (!selectPreservesPointerValues(*Def))
      return false;
    for (uint8_t I = 1; I < Def->NumInputs; ++I)
      if (frameAddressRec(Def->Inputs[I], Visited))
        return true;
    return false;
  default:
    return false;
  }
}

bool MedLLVMEmitter::varMayBeFrameAddress(const MedVar &V) const {
  if (V.isConst() || !CurMedFunc)
    return false;
  if (FrameAddressCacheFor != CurMedFunc) {
    FrameAddressCacheFor = CurMedFunc;
    FrameAddressCache.clear();
  }
  auto Key = frameKey(V);
  if (auto It = FrameAddressCache.find(Key); It != FrameAddressCache.end())
    return It->second;

  llvm::DenseSet<std::pair<int64_t, int>> Visited;
  const bool Result = frameAddressRec(V, Visited);
  FrameAddressCache[Key] = Result;
  return Result;
}

MedDynamicStackLookups MedLLVMEmitter::dynamicStackLookups() const {
  MedDynamicStackLookups L;
  L.Func = CurMedFunc;
  L.StackPointer = getTargetRegInfo(TargetArch).StackPointer;
  L.Definition = [this](const MedVar &V) { return lookupDef(V); };
  L.Phi = [this](const MedVar &V) { return lookupPhi(V); };
  L.ReloadSources = [this](const MedOp &Load, std::vector<MedVar> &Sources) {
    return collectFrameReloadSources(Load, Sources);
  };
  return L;
}

bool MedLLVMEmitter::varIsStackPtrDerived(const MedVar &V, int Depth) const {
  return medStackPointerDerived(V, dynamicStackLookups(), Depth);
}

bool MedLLVMEmitter::varReaches(const MedVar &V, const MedVar &Target,
                                int Depth) const {
  return medValueReaches(V, Target, dynamicStackLookups(), Depth);
}

llvm::Value *
MedLLVMEmitter::tryEmitDynamicStackAlloc(const MedOp &Op,
                                         llvm::IRBuilder<> &Builder) {
  if (!FrameBaseInt)
    return nullptr;
  const MedVar *SizePtr =
      medDynamicStackAllocationSize(Op, dynamicStackLookups());
  if (!SizePtr)
    return nullptr;
  const MedVar &Size = *SizePtr;

  llvm::Value *SizeVal = getVar(Size, Builder);
  if (!SizeVal || !SizeVal->getType()->isIntegerTy())
    return nullptr;
  auto *Dyn = Builder.CreateAlloca(llvm::Type::getInt8Ty(*Ctx), SizeVal, "vla");
  Dyn->setAlignment(llvm::Align(16));
  unsigned Bits = Op.Output.Size > 0 ? Op.Output.Size * 8u : 64u;
  auto *VlaSp =
      Builder.CreatePtrToInt(Dyn, llvm::IntegerType::get(*Ctx, Bits), "vla_sp");
  DynVlaBases[sizeRootKey(Size)] = VlaSp;
  return VlaSp;
}

std::pair<int, int> MedLLVMEmitter::sizeRootKey(const MedVar &V,
                                                int Depth) const {
  return medSizeRootKey(V, dynamicStackLookups(), Depth);
}

std::optional<std::pair<std::pair<int, int>, int64_t>>
MedLLVMEmitter::addrSlotKey(const MedVar &V, int Depth,
                            bool ThroughRegs) const {
  MedDefLookup DefOf;
  if (CurMedFunc)
    DefOf = [this](const MedVar &Value) { return lookupDef(Value); };
  return medAddressSlotKey(V, DefOf, Depth, ThroughRegs);
}

std::optional<med_llvm::SlotKey> MedLLVMEmitter::canonicalFrameSlotKey(
    const MedVar &V, bool RequireEntryStackPointer,
    const ControlValueBindings *Bindings, size_t *SharedProofBudget) const {
  if (!CurMedFunc || V.isConst())
    return std::nullopt;

  const bool Cacheable = !Bindings && !SharedProofBudget &&
                         !FrameProofBudgetForTesting &&
                         FeasibleEdgeState != FeasibleEdgeCacheState::Building;
  const auto QueryKey =
      std::make_pair(addressProvenanceVarKey(V), RequireEntryStackPointer);
  if (Cacheable) {
    if (ClosedFrameSlotCacheFor != CurMedFunc) {
      ClosedFrameSlotCacheFor = CurMedFunc;
      ClosedFrameSlotCache.clear();
    }
    if (auto It = ClosedFrameSlotCache.find(QueryKey);
        It != ClosedFrameSlotCache.end()) {
      ++AddressProvenanceWork.FrameSlotCacheHits;
      return It->second;
    }
  }
  // The walk reads the budget only to stop when it is spent, and once it is
  // spent every step fails and so does the query. A shared-budget walk thus
  // repeats with the same charge under any budget covering its nodes, and runs
  // out under any smaller one. A bound query has its own budget, so its
  // bindings fix its answer.
  const bool SharedBudgetReplayable =
      !Bindings && SharedProofBudget &&
      FeasibleEdgeState != FeasibleEdgeCacheState::Building;
  if (SharedBudgetReplayable) {
    if (SharedBudgetFrameSlotCacheFor != CurMedFunc) {
      SharedBudgetFrameSlotCacheFor = CurMedFunc;
      SharedBudgetFrameSlotCache.clear();
    }
    if (auto It = SharedBudgetFrameSlotCache.find(QueryKey);
        It != SharedBudgetFrameSlotCache.end()) {
      const SharedBudgetFrameSlotProof &Known = It->second;
      if (Known.Complete && Known.Nodes <= *SharedProofBudget) {
        *SharedProofBudget -= Known.Nodes;
        AddressProvenanceWork.FrameSlotNodes += Known.Nodes;
        AddressProvenanceWork.FrameSlotCacheHits += Known.CacheHits;
        ++AddressProvenanceWork.FrameSlotReplays;
        return Known.Slot;
      }
      // A complete walk needs Nodes; one that ran out needs more than Nodes.
      // The cache hits counted before running out are not kept.
      if (Known.Complete ? *SharedProofBudget < Known.Nodes
                         : *SharedProofBudget <= Known.Nodes) {
        AddressProvenanceWork.FrameSlotNodes += *SharedProofBudget;
        *SharedProofBudget = 0;
        ++AddressProvenanceWork.FrameSlotReplays;
        return std::nullopt;
      }
    }
  }
  std::optional<BoundFrameSlotQuery> BoundKey;
  if (Bindings && !SharedProofBudget && !FrameProofBudgetForTesting &&
      FeasibleEdgeState != FeasibleEdgeCacheState::Building) {
    if (BoundFrameSlotCacheFor != CurMedFunc) {
      BoundFrameSlotCacheFor = CurMedFunc;
      BoundFrameSlotCache.clear();
    }
    std::vector<std::pair<AddressProvenanceVarKey, uint64_t>> Bound;
    Bound.reserve(Bindings->size());
    for (const auto &[Value, Bits] : *Bindings)
      Bound.emplace_back(addressProvenanceVarKey(Value), Bits);
    BoundKey.emplace(QueryKey.first, RequireEntryStackPointer,
                     std::move(Bound));
    if (auto It = BoundFrameSlotCache.find(*BoundKey);
        It != BoundFrameSlotCache.end()) {
      ++AddressProvenanceWork.FrameSlotCacheHits;
      return It->second;
    }
  }
  // Building/publishing feasible edges invalidates dependent proofs, including
  // queries already in progress. Do not publish a result from that generation.
  const uint64_t Generation = FrameReloadCacheGeneration;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  const uint64_t SpOff = TRI.StackPointer;
  const uint64_t FpOff = TRI.FramePointer;
  const unsigned PointerSize = TRI.PointerSize;
  std::unordered_set<AddressProvenanceVarKey, FrameProofKeyHash> Active;
  // Both walks share one saturating budget. The recurrence proof used to run
  // outside the slot budget and copied its visited set down every PHI/select
  // arm, re-expanding shared DAGs exponentially.
  size_t OwnProofBudget = FrameProofBudgetForTesting.value_or(8192);
  size_t &RemainingSlotProofNodes =
      SharedProofBudget ? *SharedProofBudget : OwnProofBudget;
  const size_t StartingBudget = RemainingSlotProofNodes;
  size_t ConsumedNodes = 0;
  bool RanOut = false;
  const uint64_t CacheHitsBefore = AddressProvenanceWork.FrameSlotCacheHits;
  auto consumeNode = [&] {
    if (RemainingSlotProofNodes == 0) {
      RanOut = true;
      return false;
    }
    --RemainingSlotProofNodes;
    ++ConsumedNodes;
    ++AddressProvenanceWork.FrameSlotNodes;
    return true;
  };
  std::unordered_map<AddressProvenanceVarKey, med_llvm::SlotKey,
                     FrameProofKeyHash>
      ProvenSlots;
  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };
  auto signedDelta = [&](const MedVar &C,
                         uint16_t FallbackSize) -> std::optional<int64_t> {
    const auto Constant = Bindings      ? traceControlConst(C, Bindings)
                          : C.isConst() ? std::optional<uint64_t>(C.ConstVal)
                                        : std::nullopt;
    if (!Constant)
      return std::nullopt;
    // INT_ADD/INT_SUB wrap at the operation's result width.  A wider literal
    // feeding a 32-bit address therefore represents its signed low 32 bits,
    // not a positive 64-bit displacement.
    const unsigned Bytes = FallbackSize != 0 ? FallbackSize : C.Size;
    if (Bytes == 0 || Bytes > sizeof(uint64_t))
      return std::nullopt;
    const unsigned Bits = Bytes * 8;
    uint64_t Value = *Constant;
    if (Bits < 64) {
      const uint64_t Mask = (uint64_t{1} << Bits) - 1;
      Value &= Mask;
      if (Value & (uint64_t{1} << (Bits - 1)))
        Value |= ~Mask;
    }
    return static_cast<int64_t>(Value);
  };

  // Exact slot identity may pass through a loop-carried stack-pointer PHI
  // whose backedge restores the same architectural SP after temporary call
  // arguments have been popped.  Prove that edge independently as an affine
  // identity of this exact PHI value.  A non-zero net adjustment, a dynamic
  // operand, a lossy cast, or a selectable path that does not reach the target
  // is not an exact slot recurrence.
  auto sameExactValue = [&](const MedVar &A, const MedVar &B) {
    if (A.isConst() || B.isConst())
      return false;
    if (addressProvenanceVarKey(A) == addressProvenanceVarKey(B))
      return true;

    // Low-to-Med keeps the architectural guest-width SP and its wider host
    // container as parallel SSA values.  At a CFG join they are separate PHI
    // nodes (for example ESP.27 and RSP.9 on i386), even though both denote the
    // same guest pointer bits at the same program point.  Treat only such
    // co-located frame-register PHIs as the same affine recurrence anchor; a
    // same-numbered value from another block/lifetime remains distinct.
    if (A.Kind != MedVar::Reg || B.Kind != MedVar::Reg ||
        A.RegOff != B.RegOff || !TRI.isFrameReg(A.RegOff) || PointerSize == 0 ||
        A.Size < PointerSize || B.Size < PointerSize)
      return false;
    const PhiNode *APhi = lookupPhi(A);
    const PhiNode *BPhi = lookupPhi(B);
    if (!APhi || !BPhi)
      return false;
    const MedBlock *Block = lookupPhiBlock(APhi);
    return Block && Block == lookupPhiBlock(BPhi);
  };
  using AffinePair =
      std::pair<AddressProvenanceVarKey, AddressProvenanceVarKey>;
  using AffineMemoKey = std::pair<AffinePair, int>;
  std::unordered_set<AffinePair, FrameProofKeyHash> ActiveAffine;
  std::unordered_map<AffineMemoKey, std::optional<int64_t>, FrameProofKeyHash>
      AffineMemo;
  std::function<std::optional<int64_t>(const MedVar &, const MedVar &, int)>
      affineDeltaTo = [&](const MedVar &Start, const MedVar &Target,
                          int Depth) -> std::optional<int64_t> {
    if (Start.isConst() || Start.Size < PointerSize || Depth > 64 ||
        !consumeNode())
      return std::nullopt;
    if (sameExactValue(Start, Target))
      return int64_t{0};
    const AffinePair Pair{addressProvenanceVarKey(Start),
                          addressProvenanceVarKey(Target)};
    // Include depth because a failure at the recursion limit is not evidence
    // for a shallower query. A cycle avoiding Target has no all-path affine
    // relation to it, whereas reaching Target is handled above.
    const AffineMemoKey MemoKey{Pair, Depth};
    if (auto It = AffineMemo.find(MemoKey); It != AffineMemo.end()) {
      ++AddressProvenanceWork.FrameSlotCacheHits;
      return It->second;
    }
    if (!ActiveAffine.insert(Pair).second)
      return std::nullopt;
    auto Cleanup = llvm::scope_exit([&] { ActiveAffine.erase(Pair); });
    auto Result = [&]() -> std::optional<int64_t> {
      if (const PhiNode *Phi = lookupPhi(Start)) {
        std::optional<int64_t> Common;
        bool SawFeasible = false;
        for (const auto &[PredId, Arg] : Phi->Args) {
          PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, PredId);
          if (Edge == PhiEdgeFeasibility::Infeasible)
            continue;
          if (Edge != PhiEdgeFeasibility::ProvenFeasible)
            return std::nullopt;
          auto Delta = affineDeltaTo(Arg, Target, Depth + 1);
          if (!Delta || (Common && *Common != *Delta))
            return std::nullopt;
          Common = *Delta;
          SawFeasible = true;
        }
        return SawFeasible ? Common : std::nullopt;
      }

      const MedOp *Def = lookupDef(Start);
      if (!Def || Def->NumInputs < 1)
        return std::nullopt;
      if (auto Forwarded = pointerPreservingInput(*Def))
        return affineDeltaTo(*Forwarded, Target, Depth + 1);

      auto addDelta = [&](const MedVar &Base,
                          int64_t Local) -> std::optional<int64_t> {
        auto BaseDelta = affineDeltaTo(Base, Target, Depth + 1);
        if (!BaseDelta)
          return std::nullopt;
        int64_t Result = 0;
        return llvm::AddOverflow(*BaseDelta, Local, Result)
                   ? std::nullopt
                   : std::optional<int64_t>(Result);
      };
      if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
        if (auto Delta = signedDelta(Def->Inputs[1], Def->Output.Size))
          return addDelta(Def->Inputs[0], *Delta);
        if (auto Delta = signedDelta(Def->Inputs[0], Def->Output.Size))
          return addDelta(Def->Inputs[1], *Delta);
        return std::nullopt;
      }
      if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2) {
        auto Delta = signedDelta(Def->Inputs[1], Def->Output.Size);
        if (!Delta)
          return std::nullopt;
        int64_t Negated = 0;
        return llvm::SubOverflow(int64_t{0}, *Delta, Negated)
                   ? std::nullopt
                   : addDelta(Def->Inputs[0], Negated);
      }
      if (selectPreservesPointerValues(*Def)) {
        auto TrueDelta = affineDeltaTo(Def->Inputs[1], Target, Depth + 1);
        auto FalseDelta = affineDeltaTo(Def->Inputs[2], Target, Depth + 1);
        return TrueDelta && FalseDelta && *TrueDelta == *FalseDelta
                   ? TrueDelta
                   : std::nullopt;
      }
      return std::nullopt;
    }();
    AffineMemo.emplace(MemoKey, Result);
    return Result;
  };

  // Cycle detection, rather than the syntactic length of an ESP/RSP
  // round-trip chain, determines termination.  Real i386 prologues with
  // callee-save pushes, a large local frame, and one outgoing call already
  // exceed the legacy depth cap of 24.  Keep an aggregate work bound for
  // hostile/degenerate DAGs while allowing any finite normal chain.
  std::function<std::optional<med_llvm::SlotKey>(const MedVar &, int)> rec =
      [&](const MedVar &Cur, int Depth) -> std::optional<med_llvm::SlotKey> {
    if (Cur.isConst() || Cur.Size < PointerSize || !consumeNode())
      return std::nullopt;

    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Cur);
    if (auto It = ProvenSlots.find(Key); It != ProvenSlots.end()) {
      ++AddressProvenanceWork.FrameSlotCacheHits;
      return It->second;
    }
    if (!Active.insert(Key).second)
      return std::nullopt;
    auto Cleanup = llvm::scope_exit([&] { Active.erase(Key); });
    auto Proof = [&]() -> std::optional<med_llvm::SlotKey> {
      const MedOp *Def = lookupDef(Cur);
      // A runtime definition is not an identity copy of the ordinary entry
      // register. Keep its invocation as the origin of any derived slots.
      if (Def && Def->RegistrationRoot != MedOp::RegistrationRootKind::None)
        return RequireEntryStackPointer ? std::nullopt
                                        : std::optional<med_llvm::SlotKey>(
                                              {{Cur.Id, Cur.SSAVer}, 0});
      // The lifter's entry register seeds are represented as self-copies.  They
      // are roots, not definitions to recurse through.  Only the physical stack
      // pointer is the preferred canonical frame origin.  A physical frame
      // pointer with no provable definition is also an exact origin, but
      // remains distinct from SP: only an explicit affine FP definition may
      // unify them. This admits i386 live-in EBP slots without guessing their
      // SP delta.
      const bool IsSelfCopy = Def && Def->Opcode == NdOp::COPY &&
                              Def->NumInputs >= 1 &&
                              sameVar(Cur, Def->Inputs[0]);
      const bool IsMergedValue = lookupPhi(Cur) != nullptr;
      const bool IsEntryStackPointer =
          Cur.Kind == MedVar::Reg && SpOff != 0 && Cur.RegOff == SpOff &&
          Cur.Size >= PointerSize && !IsMergedValue && (!Def || IsSelfCopy);
      const bool IsLiveInFramePointer =
          Cur.Kind == MedVar::Reg && FpOff != 0 && Cur.RegOff == FpOff &&
          Cur.Size >= PointerSize && !IsMergedValue && (!Def || IsSelfCopy);
      if (IsEntryStackPointer || IsLiveInFramePointer) {
        if (RequireEntryStackPointer && !IsEntryStackPointer)
          return std::nullopt;
        return med_llvm::SlotKey{{Cur.Id, Cur.SSAVer}, 0};
      }

      std::optional<med_llvm::SlotKey> Result;
      if (const PhiNode *Phi = lookupPhi(Cur)) {
        bool SawRootArm = false;
        for (const auto &[PredId, Arg] : Phi->Args) {
          PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, PredId);
          if (Edge == PhiEdgeFeasibility::Infeasible)
            continue;
          if (Edge != PhiEdgeFeasibility::ProvenFeasible) {
            return std::nullopt;
          }
          if (auto Delta = affineDeltaTo(Arg, Cur, 0)) {
            if (*Delta != 0) {
              return std::nullopt;
            }
            continue;
          }
          auto Arm = rec(Arg, Depth + 1);
          if (!Arm || (Result && *Result != *Arm)) {
            return std::nullopt;
          }
          Result = *Arm;
          SawRootArm = true;
        }
        return SawRootArm ? Result : std::nullopt;
      }

      if (Def && Def->NumInputs >= 1) {
        auto addOffset =
            [&](const MedVar &Base,
                int64_t Delta) -> std::optional<med_llvm::SlotKey> {
          auto BaseKey = rec(Base, Depth + 1);
          if (!BaseKey)
            return std::nullopt;
          int64_t Offset = 0;
          if (llvm::AddOverflow(BaseKey->second, Delta, Offset))
            return std::nullopt;
          return med_llvm::SlotKey{BaseKey->first, Offset};
        };
        switch (Def->Opcode) {
        case NdOp::COPY:
        case NdOp::INT_ZEXT:
        case NdOp::INT_SEXT:
          // Width changes preserve a frame address only when neither side drops
          // any target pointer bits.  This still admits 32->64 bookkeeping on a
          // 32-bit target, while rejecting a lossy x86-64 low-register
          // roundtrip.
          if (PointerSize != 0 && Def->Output.Size >= PointerSize &&
              Def->Inputs[0].Size >= PointerSize)
            Result = rec(Def->Inputs[0], Depth + 1);
          break;
        case NdOp::SUBBYTES:
          if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
              Def->Inputs[1].ConstVal == 0 && PointerSize != 0 &&
              Def->Output.Size >= PointerSize &&
              Def->Inputs[0].Size >= PointerSize)
            Result = rec(Def->Inputs[0], Depth + 1);
          break;
        case NdOp::INT_ADD:
          if (Def->NumInputs >= 2 && (Def->Inputs[1].isConst() || Bindings)) {
            if (auto Delta = signedDelta(Def->Inputs[1], Cur.Size))
              Result = addOffset(Def->Inputs[0], *Delta);
          }
          if (!Result && Def->NumInputs >= 2 &&
              (Def->Inputs[0].isConst() || Bindings)) {
            if (auto Delta = signedDelta(Def->Inputs[0], Cur.Size))
              Result = addOffset(Def->Inputs[1], *Delta);
          }
          break;
        case NdOp::INT_SUB:
          if (Def->NumInputs >= 2 && (Def->Inputs[1].isConst() || Bindings)) {
            if (auto Delta = signedDelta(Def->Inputs[1], Cur.Size)) {
              int64_t Negated = 0;
              if (!llvm::SubOverflow(int64_t{0}, *Delta, Negated))
                Result = addOffset(Def->Inputs[0], Negated);
            }
          }
          break;
        default:
          break;
        }
      }
      return Result;
    }();
    // A failed slot walk may depend on an active outer PHI. Cache only closed
    // positive proofs, so traversal order cannot poison a later independent
    // query for the same value.
    if (Proof)
      ProvenSlots.emplace(Key, *Proof);
    return Proof;
  };
  auto Result = rec(V, 0);
  // Every step charges a node first and fails without one, and every failing
  // step fails its query.
  assert((!RanOut || !Result) && "a frame-slot walk that ran out succeeded");
  const bool Published = Generation == FrameReloadCacheGeneration &&
                         FeasibleEdgeState != FeasibleEdgeCacheState::Building;
  if (Cacheable && ClosedFrameSlotCacheFor == CurMedFunc && Published)
    ClosedFrameSlotCache.emplace(QueryKey, Result);
  if (SharedBudgetReplayable && SharedBudgetFrameSlotCacheFor == CurMedFunc &&
      Published) {
    SharedBudgetFrameSlotProof &Known = SharedBudgetFrameSlotCache[QueryKey];
    if (!RanOut)
      Known = {Result, ConsumedNodes,
               AddressProvenanceWork.FrameSlotCacheHits - CacheHitsBefore,
               /*Complete=*/true};
    else if (!Known.Complete)
      Known.Nodes = std::max(Known.Nodes, StartingBudget);
  }
  if (BoundKey && BoundFrameSlotCacheFor == CurMedFunc && Published)
    BoundFrameSlotCache.emplace(std::move(*BoundKey), Result);
  return Result;
}

bool MedLLVMEmitter::varIsReloadedStackPtr(const MedVar &V, int Depth) const {
  return medReloadedStackPointer(V, dynamicStackLookups(), Depth);
}

llvm::Value *
MedLLVMEmitter::tryResolveDynVlaAddr(const MedOp &Op,
                                     llvm::IRBuilder<> & /*Builder*/) {
  if (DynVlaBases.empty())
    return nullptr;
  llvm::Value *Base = nullptr;
  medTryDynamicStackAddressSizes(
      Op, dynamicStackLookups(), [&](const MedVar &Size) {
        auto It = DynVlaBases.find(sizeRootKey(Size));
        if (It == DynVlaBases.end())
          return false;
        Base = It->second;
        return true;
      });
  return Base;
}

bool MedLLVMEmitter::isStackProbeCall(const MedOp &Op) const {
  if (!Img || !CurMedFunc)
    return false;
  if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
      Op.NumInputs < 1)
    return false;

  auto findDef = [&](const MedVar &V) { return lookupDef(V); };

  // Resolve a var to a constant address, threading copy/width casts and a
  // constant-offset add (the folded `GOT_base + slot_offset` form a GOT load
  // computes its address from).
  std::function<bool(const MedVar &, uint64_t &, int)> constAddr =
      [&](const MedVar &V, uint64_t &Out, int Depth) -> bool {
    if (Depth > limits::kMaxStackPtrTraceDepth)
      return false;
    if (V.isConst()) {
      Out = V.ConstVal;
      return true;
    }
    const MedOp *D = findDef(V);
    if (!D || D->NumInputs < 1)
      return false;
    switch (D->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return constAddr(D->Inputs[0], Out, Depth + 1);
    case NdOp::SUBBYTES:
      return D->NumInputs >= 2 && D->Inputs[1].isConst() &&
             D->Inputs[1].ConstVal == 0 &&
             constAddr(D->Inputs[0], Out, Depth + 1);
    case NdOp::INT_ADD: {
      uint64_t A = 0, B = 0;
      if (D->NumInputs >= 2 && constAddr(D->Inputs[0], A, Depth + 1) &&
          constAddr(D->Inputs[1], B, Depth + 1)) {
        Out = A + B;
        return true;
      }
      return false;
    }
    default:
      return false;
    }
  };

  // Find the pointer-slot the call target is loaded from: an indirect call's
  // target is `LOAD <slot>` (possibly threaded through copies).
  uint64_t SlotAddr = 0;
  bool HaveSlot = false;
  bool LoadedFromStorage = false;
  const MedVar &Tgt = Op.Inputs[0];
  const MedOp *D = findDef(Tgt);
  for (int Guard = 0; D && Guard <= limits::kMaxStackPtrTraceDepth; ++Guard) {
    if (D->Opcode == NdOp::COPY && D->NumInputs >= 1) {
      D = findDef(D->Inputs[0]);
      continue;
    }
    if (D->Opcode == NdOp::LOAD && D->NumInputs >= 1 &&
        D->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      HaveSlot = constAddr(D->Inputs[0], SlotAddr, 0);
      LoadedFromStorage = HaveSlot;
    }
    break;
  }
  // A direct call to the routine's own address (no GOT indirection).
  if (!HaveSlot && Tgt.isConst()) {
    SlotAddr = Tgt.ConstVal;
    HaveSlot = true;
  }
  if (!HaveSlot)
    return false;

  std::string Name;
  if (LoadedFromStorage) {
    if (auto It = EffectiveImportStorageSlots.find(SlotAddr);
        It != EffectiveImportStorageSlots.end())
      Name = It->second.Name;
  } else if (const Import *Imp = Img->findImportAt(SlotAddr)) {
    Name = Imp->Name;
  }
  if (Name.empty())
    return false;
  return isDarwinStackProbeName(Name);
}

} // namespace neverd
