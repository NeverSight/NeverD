//===- MedLLVMSpillPredicate.cpp - Stack-spill predicates ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stack-slot predicates for MedLLVMEmitter's global-data resolution:
/// whether a slot's address escapes, whether a matching-key load reloads
/// it, and whether such a reload is used locally.  Together they decide
/// whether a spilled global base keeps its original VA or is symbolized
/// at the store, plus the value-VA folding those walks rely on.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/object/SectionNames.h"

#define DEBUG_TYPE "neverd-med-llvm-global-data"
#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

void MedLLVMEmitter::ensureAddrPredCache() const {
  if (AddrPredCacheFor == CurMedFunc)
    return;
  AddrPredCacheFor = CurMedFunc;
  SlotAddressEscapesCache.clear();
  SlotMatchingKeyLoadCache.clear();
  SlotReloadUsedLocallyCache.clear();
  WritableDataSegCache.clear();
  PtrTableUniqueSegCache.clear();
}

bool MedLLVMEmitter::stackSlotAddressEscapes(const MedVar &SlotAddr) const {
  if (!CurMedFunc)
    return false;
  // Prefer the common entry-SP coordinate so an FP-relative load/store and an
  // SP-relative address passed to a callee compare equal. Preserve the legacy
  // identity key as a conservative fallback for frames whose root cannot be
  // proven affine (notably a live-in frame pointer).
  auto keyOf = [&](const MedVar &V) {
    if (auto Canonical = canonicalFrameSlotKey(V))
      return Canonical;
    return addrSlotKey(V, /*Depth=*/0, /*ThroughRegs=*/true);
  };
  auto Target = keyOf(SlotAddr);
  if (!Target)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotAddressEscapesCache.find(*Target);
      It != SlotAddressEscapesCache.end())
    return It->second;
  bool Result = false;
  // The slot's address passed as a call argument: a callee may write an
  // already- symbolized pointer through it (the escaping output-pointer shape,
  // #475).
  for (const auto &CI : CurMedFunc->CallInfos) {
    for (const auto &Arg : CI.Args)
      if (auto K = keyOf(Arg); K && *K == *Target) {
        Result = true;
        break;
      }
    if (Result)
      break;
  }
  // The slot's address stored to memory escapes the same way.
  if (!Result)
    for (const auto &Blk : CurMedFunc->Blocks) {
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
          if (auto K = keyOf(Op.Inputs[1]); K && *K == *Target) {
            Result = true;
            break;
          }
      if (Result)
        break;
    }
  SlotAddressEscapesCache[*Target] = Result;
  return Result;
}

bool MedLLVMEmitter::frameSlotHasMatchingKeyLoad(
    const MedVar &StoreAddr) const {
  if (!CurMedFunc)
    return false;
  auto keyOf = [&](const MedVar &V) {
    if (auto Canonical = canonicalFrameSlotKey(V))
      return Canonical;
    return addrSlotKey(V);
  };
  auto SK = keyOf(StoreAddr);
  if (!SK)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotMatchingKeyLoadCache.find(*SK);
      It != SlotMatchingKeyLoadCache.end())
    return It->second;
  bool Result = false;
  for (const auto &Blk : CurMedFunc->Blocks) {
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
        if (auto LK = keyOf(Op.Inputs[0]); LK && *LK == *SK) {
          Result = true;
          break;
        }
    if (Result)
      break;
  }
  SlotMatchingKeyLoadCache[*SK] = Result;
  return Result;
}

bool MedLLVMEmitter::frameAccessesProvenDisjoint(const MedVar &A,
                                                 uint16_t ASize,
                                                 const MedVar &B,
                                                 uint16_t BSize) const {
  const unsigned PointerBytes = getTargetRegInfo(TargetArch).PointerSize;
  if (!CurMedFunc || PointerBytes == 0 || PointerBytes > sizeof(uint64_t) ||
      ASize == 0 || BSize == 0)
    return false;

  // Intermediate endpoint arithmetic can exceed int64_t by one pointer-width
  // modulus plus an access size.  APInt keeps these proofs portable to MSVC,
  // which has no native 128-bit integer extension, without weakening the
  // overflow checks.
  static constexpr unsigned WideArithmeticBits = 128;
  auto wideSigned = [](int64_t Value) {
    return llvm::APInt(WideArithmeticBits, static_cast<uint64_t>(Value),
                       /*isSigned=*/true);
  };
  auto wideUnsigned = [](uint64_t Value) {
    return llvm::APInt(WideArithmeticBits, Value);
  };

  std::map<int, const MedBlock *> BlocksById;
  for (const MedBlock &Block : CurMedFunc->Blocks)
    if (!BlocksById.emplace(Block.Id, &Block).second)
      return false;

  struct UnsignedRange {
    uint64_t Min = 0;
    uint64_t Max = 0;
  };
  auto maskForSize = [](uint16_t Size) -> std::optional<uint64_t> {
    if (Size == 0 || Size > sizeof(uint64_t))
      return std::nullopt;
    return Size == sizeof(uint64_t) ? std::numeric_limits<uint64_t>::max()
                                    : (uint64_t{1} << (Size * 8)) - 1;
  };
  auto sameValue = [&](const MedVar &Left, const MedVar &Right) {
    return !Left.isConst() && !Right.isConst() &&
           addressProvenanceVarKey(Left) == addressProvenanceVarKey(Right);
  };
  auto constantValue = [&](const MedVar &Value) -> std::optional<uint64_t> {
    if (!Value.isConst() || controlConstantMayRelocate(Value))
      return std::nullopt;
    auto Mask = maskForSize(Value.Size);
    return Mask ? std::optional<uint64_t>(Value.ConstVal & *Mask)
                : std::nullopt;
  };
  auto signedConstant = [&](const MedVar &Value,
                            uint16_t Width) -> std::optional<int64_t> {
    auto Raw = constantValue(Value);
    auto Mask = maskForSize(Width);
    if (!Raw || !Mask)
      return std::nullopt;
    const unsigned Bits = Width * 8;
    uint64_t Result = *Raw & *Mask;
    if (Bits < 64 && (Result & (uint64_t{1} << (Bits - 1))))
      Result |= ~*Mask;
    return static_cast<int64_t>(Result);
  };

  std::function<std::optional<int64_t>(const MedVar &, const MedVar &,
                                       std::set<AddressProvenanceVarKey>)>
      affineStepTo = [&](const MedVar &Start, const MedVar &Root,
                         std::set<AddressProvenanceVarKey> Seen)
      -> std::optional<int64_t> {
    if (Start.isConst())
      return std::nullopt;
    if (sameValue(Start, Root))
      return int64_t{0};
    if (!Seen.insert(addressProvenanceVarKey(Start)).second)
      return std::nullopt;
    const MedOp *Def = lookupDef(Start);
    if (!Def || Def->NumInputs < 1 || Def->Output.Size != Root.Size)
      return std::nullopt;
    if (Def->Opcode == NdOp::COPY && Def->Inputs[0].Size == Def->Output.Size)
      return affineStepTo(Def->Inputs[0], Root, std::move(Seen));
    if (Def->Opcode == NdOp::SUBBYTES && Def->NumInputs >= 2 &&
        Def->Inputs[0].Size == Def->Output.Size &&
        constantValue(Def->Inputs[1]) == std::optional<uint64_t>(0))
      return affineStepTo(Def->Inputs[0], Root, std::move(Seen));
    if ((Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB) ||
        Def->NumInputs < 2)
      return std::nullopt;

    const MedVar *Base = &Def->Inputs[0];
    const MedVar *DeltaValue = &Def->Inputs[1];
    if (Def->Opcode == NdOp::INT_ADD && Def->Inputs[0].isConst()) {
      Base = &Def->Inputs[1];
      DeltaValue = &Def->Inputs[0];
    }
    auto Delta = signedConstant(*DeltaValue, Def->Output.Size);
    auto BaseDelta = affineStepTo(*Base, Root, std::move(Seen));
    if (!Delta || !BaseDelta)
      return std::nullopt;
    int64_t SignedDelta = *Delta;
    if (Def->Opcode == NdOp::INT_SUB) {
      if (SignedDelta == std::numeric_limits<int64_t>::min())
        return std::nullopt;
      SignedDelta = -SignedDelta;
    }
    const llvm::APInt Sum = wideSigned(*BaseDelta) + wideSigned(SignedDelta);
    if (!Sum.isSignedIntN(64))
      return std::nullopt;
    return Sum.getSExtValue();
  };

  auto phiBlock = [&](const PhiNode &Phi) -> const MedBlock * {
    for (const MedBlock &Block : CurMedFunc->Blocks)
      for (const PhiNode &Candidate : Block.Phis)
        if (&Candidate == &Phi)
          return &Block;
    return nullptr;
  };
  auto recurrenceEdgeExcludes =
      [&](int PredId, const MedBlock &Successor,
          const MedVar &RecurrenceValue) -> std::optional<uint64_t> {
    auto PredIt = BlocksById.find(PredId);
    if (PredIt == BlocksById.end() || PredIt->second->Succs.size() != 2 ||
        PredIt->second->Ops.empty())
      return std::nullopt;
    const MedBlock &Pred = *PredIt->second;
    const MedOp &Branch = Pred.Ops.back();
    if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs < 2 ||
        !Branch.Inputs[0].isConst())
      return std::nullopt;

    const uint64_t TargetAddr = Branch.Inputs[0].ConstVal;
    bool SuccessorOnTrue = false;
    bool SawTarget = false;
    bool SawSuccessor = false;
    for (int SuccId : Pred.Succs) {
      auto SuccIt = BlocksById.find(SuccId);
      if (SuccIt == BlocksById.end())
        return std::nullopt;
      SawSuccessor |= SuccId == Successor.Id;
      if (SuccIt->second->StartAddr == TargetAddr) {
        if (SawTarget)
          return std::nullopt;
        SawTarget = true;
        SuccessorOnTrue = SuccId == Successor.Id;
      }
    }
    if (!SawTarget || !SawSuccessor)
      return std::nullopt;

    MedVar Condition = Branch.Inputs[1];
    bool Inverted = false;
    for (unsigned Guard = 0; Guard < 8; ++Guard) {
      const MedOp *Def = lookupDef(Condition);
      if (!Def || Def->NumInputs < 1)
        break;
      if (Def->Opcode == NdOp::COPY &&
          Def->Inputs[0].Size == Def->Output.Size) {
        Condition = Def->Inputs[0];
        continue;
      }
      if (Def->Opcode == NdOp::BOOL_NOT) {
        Inverted = !Inverted;
        Condition = Def->Inputs[0];
        continue;
      }
      break;
    }
    const MedOp *Compare = lookupDef(Condition);
    if (!Compare || Compare->NumInputs < 2 ||
        (Compare->Opcode != NdOp::INT_EQUAL &&
         Compare->Opcode != NdOp::INT_NOTEQUAL))
      return std::nullopt;
    const MedVar *Other = nullptr;
    if (sameValue(Compare->Inputs[0], RecurrenceValue))
      Other = &Compare->Inputs[1];
    else if (sameValue(Compare->Inputs[1], RecurrenceValue))
      Other = &Compare->Inputs[0];
    if (!Other)
      return std::nullopt;
    auto Bound = constantValue(*Other);
    if (!Bound)
      return std::nullopt;

    const bool CompareTrueMeansNotEqual = Compare->Opcode == NdOp::INT_NOTEQUAL;
    const bool EdgeTakesCondition = SuccessorOnTrue != Inverted;
    const bool EdgeMeansNotEqual = EdgeTakesCondition
                                       ? CompareTrueMeansNotEqual
                                       : !CompareTrueMeansNotEqual;
    return EdgeMeansNotEqual ? Bound : std::nullopt;
  };

  std::map<AddressProvenanceVarKey, std::optional<UnsignedRange>> RangeMemo;
  std::set<AddressProvenanceVarKey> RangeActive;
  size_t RemainingRangeNodes = 8192;
  std::function<std::optional<UnsignedRange>(const MedVar &)> unsignedRange =
      [&](const MedVar &Value) -> std::optional<UnsignedRange> {
    auto Mask = maskForSize(Value.Size);
    if (!Mask || RemainingRangeNodes-- == 0)
      return std::nullopt;
    if (auto Constant = constantValue(Value))
      return UnsignedRange{*Constant, *Constant};
    if (Value.isConst())
      return std::nullopt;

    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Value);
    if (auto It = RangeMemo.find(Key); It != RangeMemo.end())
      return It->second;
    if (!RangeActive.insert(Key).second)
      return std::nullopt;
    auto finish = [&](std::optional<UnsignedRange> Result) {
      RangeActive.erase(Key);
      RangeMemo.emplace(Key, Result);
      return Result;
    };
    auto merge =
        [](std::optional<UnsignedRange> Left,
           std::optional<UnsignedRange> Right) -> std::optional<UnsignedRange> {
      if (!Left || !Right)
        return std::nullopt;
      return UnsignedRange{std::min(Left->Min, Right->Min),
                           std::max(Left->Max, Right->Max)};
    };

    if (const PhiNode *Phi = lookupPhi(Value)) {
      const MedBlock *Owner = phiBlock(*Phi);
      if (!Owner)
        return finish(std::nullopt);
      std::optional<UnsignedRange> Initial;
      struct RecurrenceArm {
        int PredId = -1;
        MedVar Value;
        int64_t Step = 0;
      };
      std::vector<RecurrenceArm> Recurrences;
      bool SawFeasible = false;
      for (const auto &[PredId, Arg] : Phi->Args) {
        PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, PredId);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible)
          return finish(std::nullopt);
        SawFeasible = true;
        if (auto Step = affineStepTo(Arg, Value, {})) {
          Recurrences.push_back({PredId, Arg, *Step});
          continue;
        }
        auto Arm = unsignedRange(Arg);
        if (!Arm)
          return finish(std::nullopt);
        Initial = Initial ? merge(Initial, Arm) : Arm;
      }
      if (!SawFeasible || !Initial)
        return finish(std::nullopt);
      if (Recurrences.empty())
        return finish(Initial);

      std::optional<RecurrenceArm> Positive;
      for (const RecurrenceArm &Arm : Recurrences) {
        if (Arm.Step == 0)
          continue;
        if (Arm.Step < 0 || Positive)
          return finish(std::nullopt);
        Positive = Arm;
      }
      if (!Positive)
        return finish(Initial);
      if (Initial->Min != Initial->Max)
        return finish(std::nullopt);
      auto Bound =
          recurrenceEdgeExcludes(Positive->PredId, *Owner, Positive->Value);
      const uint64_t Step = static_cast<uint64_t>(Positive->Step);
      if (!Bound || *Bound > *Mask || Initial->Min >= *Bound || Step == 0 ||
          Step > *Bound - Initial->Min || ((*Bound - Initial->Min) % Step) != 0)
        return finish(std::nullopt);
      return finish(UnsignedRange{Initial->Min, *Bound - Step});
    }

    const MedOp *Def = lookupDef(Value);
    if (!Def || Def->NumInputs < 1)
      return finish(std::nullopt);
    auto unary = [&]() { return unsignedRange(Def->Inputs[0]); };
    auto binary =
        [&]() -> std::optional<std::pair<UnsignedRange, UnsignedRange>> {
      if (Def->NumInputs < 2)
        return std::nullopt;
      auto Left = unsignedRange(Def->Inputs[0]);
      auto Right = unsignedRange(Def->Inputs[1]);
      return Left && Right ? std::optional(std::pair{*Left, *Right})
                           : std::nullopt;
    };

    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
      if (auto Input = unary(); Input && Input->Max <= *Mask)
        return finish(Input);
      break;
    case NdOp::INT_SEXT:
      if (auto Input = unary()) {
        auto InputMask = maskForSize(Def->Inputs[0].Size);
        if (InputMask && Input->Max <= (*InputMask >> 1) && Input->Max <= *Mask)
          return finish(Input);
      }
      break;
    case NdOp::SUBBYTES:
      if (Def->NumInputs >= 2 &&
          constantValue(Def->Inputs[1]) == std::optional<uint64_t>(0))
        if (auto Input = unary(); Input && Input->Max <= *Mask)
          return finish(Input);
      break;
    case NdOp::INT_AND:
      if (Def->NumInputs >= 2) {
        if (auto Constant = constantValue(Def->Inputs[0]))
          return finish(UnsignedRange{0, *Constant & *Mask});
        if (auto Constant = constantValue(Def->Inputs[1]))
          return finish(UnsignedRange{0, *Constant & *Mask});
      }
      break;
    case NdOp::INT_ADD:
      if (auto Inputs = binary();
          Inputs && Inputs->first.Max <= *Mask - Inputs->second.Max)
        return finish(UnsignedRange{Inputs->first.Min + Inputs->second.Min,
                                    Inputs->first.Max + Inputs->second.Max});
      break;
    case NdOp::INT_SUB:
      if (auto Inputs = binary();
          Inputs && Inputs->first.Min >= Inputs->second.Max)
        return finish(UnsignedRange{Inputs->first.Min - Inputs->second.Max,
                                    Inputs->first.Max - Inputs->second.Min});
      break;
    case NdOp::INT_MULT:
      if (auto Inputs = binary();
          Inputs && (Inputs->first.Max == 0 ||
                     Inputs->second.Max <= *Mask / Inputs->first.Max))
        return finish(UnsignedRange{Inputs->first.Min * Inputs->second.Min,
                                    Inputs->first.Max * Inputs->second.Max});
      break;
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
      if (Def->NumInputs >= 2)
        if (auto Shift = constantValue(Def->Inputs[1])) {
          const unsigned Bits = Def->Output.Size * 8;
          if (*Shift >= Bits)
            return finish(UnsignedRange{0, 0});
          if (auto Input = unary()) {
            if (Def->Opcode == NdOp::INT_RIGHT)
              return finish(
                  UnsignedRange{Input->Min >> *Shift, Input->Max >> *Shift});
            if (Input->Max <= (*Mask >> *Shift))
              return finish(
                  UnsignedRange{Input->Min << *Shift, Input->Max << *Shift});
          }
        }
      break;
    case NdOp::SELECT:
      if (selectPreservesPointerValues(*Def))
        return finish(merge(unsignedRange(Def->Inputs[1]),
                            unsignedRange(Def->Inputs[2])));
      break;
    default:
      break;
    }
    return finish(std::nullopt);
  };

  struct FrameOffsetRange {
    std::pair<int, int> Root;
    int64_t Min = 0;
    int64_t Max = 0;
  };
  struct FrameRangeProof {
    bool Valid = false;
    bool SawCycle = false;
    std::optional<FrameOffsetRange> Range;
  };
  size_t RemainingFrameRangeNodes = 8192;
  std::function<FrameRangeProof(const MedVar &,
                                std::set<AddressProvenanceVarKey>)>
      proveFrameOffsetRange =
          [&](const MedVar &Address,
              std::set<AddressProvenanceVarKey> Seen) -> FrameRangeProof {
    if (Address.isConst() || RemainingFrameRangeNodes-- == 0)
      return {};
    if (auto Exact = canonicalFrameSlotKey(Address))
      return {true, false,
              FrameOffsetRange{Exact->first, Exact->second, Exact->second}};
    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Address);
    if (!Seen.insert(Key).second)
      return {true, true, std::nullopt};
    auto extend = [&](const FrameOffsetRange &Base, const UnsignedRange &Delta,
                      bool Subtract) -> std::optional<FrameOffsetRange> {
      const llvm::APInt Min =
          wideSigned(Base.Min) +
          (Subtract ? -wideUnsigned(Delta.Max) : wideUnsigned(Delta.Min));
      const llvm::APInt Max =
          wideSigned(Base.Max) +
          (Subtract ? -wideUnsigned(Delta.Min) : wideUnsigned(Delta.Max));
      if (!Min.isSignedIntN(64) || !Max.isSignedIntN(64))
        return std::nullopt;
      return FrameOffsetRange{Base.Root, Min.getSExtValue(),
                              Max.getSExtValue()};
    };
    auto mergeProofs = [](const FrameRangeProof &Left,
                          const FrameRangeProof &Right) -> FrameRangeProof {
      if (!Left.Valid || !Right.Valid ||
          (Left.Range && Right.Range && Left.Range->Root != Right.Range->Root))
        return {};
      std::optional<FrameOffsetRange> Range =
          Left.Range ? Left.Range : Right.Range;
      if (Left.Range && Right.Range)
        Range = FrameOffsetRange{Left.Range->Root,
                                 std::min(Left.Range->Min, Right.Range->Min),
                                 std::max(Left.Range->Max, Right.Range->Max)};
      return {true, Left.SawCycle || Right.SawCycle, Range};
    };

    if (const PhiNode *Phi = lookupPhi(Address)) {
      FrameRangeProof Result;
      bool SawFeasible = false;
      for (const auto &[PredId, Arg] : Phi->Args) {
        PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, PredId);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible)
          return {};
        FrameRangeProof Arm = proveFrameOffsetRange(Arg, Seen);
        if (!Arm.Valid)
          return {};
        Result = SawFeasible ? mergeProofs(Result, Arm) : Arm;
        if (!Result.Valid)
          return {};
        SawFeasible = true;
      }
      return SawFeasible ? Result : FrameRangeProof{};
    }

    const MedOp *Def = lookupDef(Address);
    if (!Def || Def->NumInputs < 1)
      return {};
    if (Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
        Def->Opcode == NdOp::INT_SEXT || Def->Opcode == NdOp::SUBBYTES) {
      if (auto Forwarded = pointerPreservingInput(*Def))
        return proveFrameOffsetRange(*Forwarded, std::move(Seen));
      return {};
    }
    if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
        Def->NumInputs >= 2) {
      auto extendProof = [&](const MedVar &BaseValue, const MedVar &DeltaValue,
                             bool Subtract) -> FrameRangeProof {
        FrameRangeProof Base = proveFrameOffsetRange(BaseValue, Seen);
        auto Delta = unsignedRange(DeltaValue);
        if (!Base.Valid || !Delta)
          return {};
        if (!Base.Range)
          return Delta->Min == 0 && Delta->Max == 0 ? Base : FrameRangeProof{};
        auto Range = extend(*Base.Range, *Delta, Subtract);
        return Range ? FrameRangeProof{true, Base.SawCycle, Range}
                     : FrameRangeProof{};
      };
      if (FrameRangeProof Left = extendProof(Def->Inputs[0], Def->Inputs[1],
                                             Def->Opcode == NdOp::INT_SUB);
          Left.Valid)
        return Left;
      if (Def->Opcode == NdOp::INT_ADD)
        return extendProof(Def->Inputs[1], Def->Inputs[0], false);
      return {};
    }
    if (selectPreservesPointerValues(*Def)) {
      FrameRangeProof True = proveFrameOffsetRange(Def->Inputs[1], Seen);
      FrameRangeProof False =
          proveFrameOffsetRange(Def->Inputs[2], std::move(Seen));
      return mergeProofs(True, False);
    }
    return {};
  };
  auto frameOffsetRange =
      [&](const MedVar &Address) -> std::optional<FrameOffsetRange> {
    FrameRangeProof Proof = proveFrameOffsetRange(Address, {});
    return Proof.Valid ? Proof.Range : std::nullopt;
  };

  auto ARange = frameOffsetRange(A);
  auto BRange = frameOffsetRange(B);
  if (!ARange || !BRange || ARange->Root != BRange->Root)
    return false;

  // Two byte ranges alias after pointer-width wrapping iff the signed
  // difference interval contains a multiple of 2^N.
  const llvm::APInt Modulus = wideUnsigned(1).shl(PointerBytes * 8);
  const llvm::APInt DifferenceMin =
      wideSigned(ARange->Min) -
      (wideSigned(BRange->Max) + wideUnsigned(BSize - 1));
  const llvm::APInt DifferenceMax =
      (wideSigned(ARange->Max) + wideUnsigned(ASize - 1)) -
      wideSigned(BRange->Min);
  auto floorDiv = [](const llvm::APInt &Value, const llvm::APInt &Divisor) {
    if (!Value.isNegative())
      return Value.sdiv(Divisor);
    const llvm::APInt One(Value.getBitWidth(), 1);
    return -((-Value + Divisor - One).sdiv(Divisor));
  };
  auto ceilDiv = [&](const llvm::APInt &Value, const llvm::APInt &Divisor) {
    return -floorDiv(-Value, Divisor);
  };
  return ceilDiv(DifferenceMin, Modulus).sgt(floorDiv(DifferenceMax, Modulus));
}

bool MedLLVMEmitter::collectFrameReloadSources(
    const MedOp &Load, std::vector<MedVar> &Sources) const {
  Sources.clear();
  const bool Escapes = CurMedFunc && Load.Opcode == NdOp::LOAD &&
                       Load.NumInputs >= 1 &&
                       stackSlotAddressEscapes(Load.Inputs[0]);
  if (!CurMedFunc || Load.Opcode != NdOp::LOAD || Load.NumInputs < 1 ||
      Load.Output.Size == 0 || Escapes)
    return false;

  const auto Target = canonicalFrameSlotKey(Load.Inputs[0]);
  if (!Target)
    return false;

  std::map<int, const MedBlock *> BlocksById;
  const MedBlock *EntryBlock = nullptr;
  const MedBlock *LoadBlock = nullptr;
  size_t LoadIndex = 0;
  for (const MedBlock &Block : CurMedFunc->Blocks) {
    if (!BlocksById.emplace(Block.Id, &Block).second)
      return false;
    if (Block.StartAddr == CurMedFunc->Entry) {
      if (EntryBlock)
        return false;
      EntryBlock = &Block;
    }
    for (size_t I = 0; I < Block.Ops.size(); ++I)
      if (&Block.Ops[I] == &Load) {
        if (LoadBlock)
          return false;
        LoadBlock = &Block;
        LoadIndex = I;
      }
  }
  if (!EntryBlock || !LoadBlock)
    return false;

  // Preds is cached IR metadata, not authority for reachability.  Reconstruct
  // the incoming relation from every ordinary/exceptional successor and
  // require the two views to agree before proving an all-path reaching store.
  // A malformed or stale predecessor list must not hide an uninitialized
  // bypass and turn a later table-looking STORE into pointer provenance.
  std::map<int, std::set<int>> StructuralPreds;
  for (const auto &[Id, Block] : BlocksById) {
    (void)Block;
    StructuralPreds.emplace(Id, std::set<int>{});
  }
  for (const auto &[Id, Block] : BlocksById) {
    for (int SuccId : Block->Succs) {
      auto It = StructuralPreds.find(SuccId);
      if (It == StructuralPreds.end())
        return false;
      It->second.insert(Id);
    }
    for (const ExceptionalEdge &Edge : Block->ExceptionalSuccs) {
      if (Edge.BlockId < 0)
        continue;
      auto It = StructuralPreds.find(Edge.BlockId);
      if (It == StructuralPreds.end())
        return false;
      It->second.insert(Id);
    }
  }
  for (const auto &[Id, Block] : BlocksById) {
    std::set<int> Declared(Block->Preds.begin(), Block->Preds.end());
    for (const ExceptionalEdge &Edge : Block->ExceptionalPreds)
      if (Edge.BlockId >= 0)
        Declared.insert(Edge.BlockId);
    if (Declared != StructuralPreds[Id])
      return false;
  }

  auto isMemoryWrite = [](NdOp Opcode) {
    return Opcode == NdOp::STORE || Opcode == NdOp::ATOMIC_XCHG ||
           Opcode == NdOp::ATOMIC_ADD || Opcode == NdOp::ATOMIC_CMPXCHG;
  };
  auto overlaps = [](int64_t A, uint16_t ASize, int64_t B, uint16_t BSize) {
    if (ASize == 0 || BSize == 0)
      return true;
    auto endsBefore = [](int64_t Start, uint16_t Size, int64_t Other) {
      return Start < Other &&
             static_cast<uint64_t>(Other) - static_cast<uint64_t>(Start) >=
                 Size;
    };
    return !endsBefore(A, ASize, B) && !endsBefore(B, BSize, A);
  };
  struct ReachingState {
    bool Reachable = false;
    bool Uninitialized = false;
    bool Invalid = false;
    std::vector<MedVar> Values;
  };
  auto addUnique = [](std::vector<MedVar> &Values, const MedVar &Value) {
    if (std::find(Values.begin(), Values.end(), Value) == Values.end())
      Values.push_back(Value);
  };
  auto sameState = [](const ReachingState &A, const ReachingState &B) {
    if (A.Reachable != B.Reachable || A.Uninitialized != B.Uninitialized ||
        A.Invalid != B.Invalid || A.Values.size() != B.Values.size())
      return false;
    for (const MedVar &Value : A.Values)
      if (std::find(B.Values.begin(), B.Values.end(), Value) == B.Values.end())
        return false;
    return true;
  };
  auto mergeInto = [&](ReachingState &Dst, const ReachingState &Src) {
    if (!Src.Reachable)
      return false;
    ReachingState Before = Dst;
    Dst.Reachable = true;
    Dst.Uninitialized |= Src.Uninitialized;
    Dst.Invalid |= Src.Invalid;
    for (const MedVar &Value : Src.Values)
      addUnique(Dst.Values, Value);
    return !sameState(Before, Dst);
  };
  auto transfer = [&](const MedBlock &Block, size_t Boundary,
                      ReachingState State) {
    if (!State.Reachable || Boundary > Block.Ops.size()) {
      State.Invalid |= Boundary > Block.Ops.size();
      return State;
    }
    for (size_t I = 0; I < Boundary; ++I) {
      const MedOp &Op = Block.Ops[I];
      if (!isMemoryWrite(Op.Opcode))
        continue;
      if (Op.NumInputs < 1) {
        State.Invalid = true;
        State.Values.clear();
        continue;
      }
      const MedVar &WriteAddr = Op.Inputs[0];
      if (!varMayBeFrameAddress(WriteAddr))
        continue;
      uint16_t WriteSize = Op.Opcode == NdOp::STORE && Op.NumInputs >= 2
                               ? Op.Inputs[1].Size
                               : Op.Output.Size;
      const auto WriteKey = canonicalFrameSlotKey(WriteAddr);
      // A frame-derived write whose slot cannot be canonicalized, or whose
      // root differs from the reload's root, may still alias after an
      // unmodelled stack adjustment. Keep the state poisoned until a later
      // exact full-width STORE definitely overwrites the target slot.
      if (!WriteKey || WriteKey->first != Target->first) {
        const bool ProvenDisjoint =
            !WriteKey &&
            frameAccessesProvenDisjoint(WriteAddr, WriteSize, Load.Inputs[0],
                                        Load.Output.Size);
        if (ProvenDisjoint)
          continue;
        State.Invalid = true;
        State.Values.clear();
        continue;
      }

      if (!overlaps(WriteKey->second, WriteSize, Target->second,
                    Load.Output.Size))
        continue;
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 && WriteSize != 0 &&
          WriteKey->second == Target->second && WriteSize == Load.Output.Size) {
        State.Uninitialized = false;
        State.Invalid = false;
        State.Values.clear();
        addUnique(State.Values, Op.Inputs[1]);
        continue;
      }
      State.Uninitialized = false;
      State.Invalid = true;
      State.Values.clear();
    }
    return State;
  };

  // Forward may-reach dataflow over the exact slot.  Unlike a recursive
  // backwards walk, this reaches a fixed point across loop back-edges and
  // therefore distinguishes a preheader definition from a STORE that occurs
  // only after the LOAD and can affect later iterations.
  std::map<int, ReachingState> InStates;
  std::map<int, ReachingState> OutStates;
  ReachingState Entry;
  Entry.Reachable = true;
  Entry.Uninitialized = true;
  InStates[EntryBlock->Id] = Entry;
  std::vector<int> Work{EntryBlock->Id};
  while (!Work.empty()) {
    int BlockId = Work.back();
    Work.pop_back();
    auto BlockIt = BlocksById.find(BlockId);
    if (BlockIt == BlocksById.end())
      return false;
    const MedBlock &Block = *BlockIt->second;
    ReachingState Next = transfer(Block, Block.Ops.size(), InStates[BlockId]);
    if (sameState(OutStates[BlockId], Next))
      continue;
    OutStates[BlockId] = Next;

    auto propagate = [&](int SuccId) {
      auto Succ = BlocksById.find(SuccId);
      if (Succ == BlocksById.end())
        return false;
      if (mergeInto(InStates[SuccId], Next))
        Work.push_back(SuccId);
      return true;
    };
    for (int SuccId : Block.Succs)
      if (!propagate(SuccId))
        return false;
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs)
      if (Edge.BlockId >= 0 && !propagate(Edge.BlockId))
        return false;
  }

  ReachingState AtLoad =
      transfer(*LoadBlock, LoadIndex, InStates[LoadBlock->Id]);
  if (!AtLoad.Reachable || AtLoad.Uninitialized || AtLoad.Invalid ||
      AtLoad.Values.empty())
    return false;
  Sources = std::move(AtLoad.Values);
  return true;
}

std::optional<uint64_t> MedLLVMEmitter::traceValueVA(const MedVar &V,
                                                     int Depth) const {
  if (!CurMedFunc && !V.isConst())
    return std::nullopt;
  (void)Depth;
  auto mask = [](uint64_t X, uint16_t Size) -> uint64_t {
    if (Size == 0 || Size >= 8)
      return X;
    return X & ((1ULL << (Size * 8)) - 1);
  };
  using Key = std::tuple<int, int, int, uint16_t>;
  std::set<Key> Active;
  std::map<Key, std::optional<uint64_t>> Memo;
  std::function<std::optional<uint64_t>(const MedVar &)> Eval =
      [&](const MedVar &Cur) -> std::optional<uint64_t> {
    if (Cur.isConst())
      return Cur.ConstVal;
    Key K = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                            Cur.Size);
    if (auto It = Memo.find(K); It != Memo.end())
      return It->second;
    if (!Active.insert(K).second)
      return std::nullopt;

    std::optional<uint64_t> Result;
    const MedOp *Def = lookupDef(Cur);
    if (Def) {
      switch (Def->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        if (Def->NumInputs >= 1)
          if (auto B = Eval(Def->Inputs[0]))
            Result = mask(mask(*B, Def->Inputs[0].Size), Def->Output.Size);
        break;
      case NdOp::INT_SEXT:
        if (Def->NumInputs >= 1)
          if (auto B = Eval(Def->Inputs[0])) {
            unsigned InBits = Def->Inputs[0].Size * 8;
            uint64_t Value = mask(*B, Def->Inputs[0].Size);
            if (InBits != 0 && InBits < 64 &&
                (Value & (uint64_t(1) << (InBits - 1))))
              Value |= ~((uint64_t(1) << InBits) - 1);
            Result = mask(Value, Def->Output.Size);
          }
        break;
      case NdOp::SUBBYTES:
        if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
            Def->Inputs[1].ConstVal == 0)
          if (auto B = Eval(Def->Inputs[0]))
            Result = mask(*B, Def->Output.Size);
        break;
      case NdOp::INT_ADD:
        if (Def->NumInputs >= 2)
          if (auto A = Eval(Def->Inputs[0]))
            if (auto B = Eval(Def->Inputs[1]))
              Result = mask(*A + *B, Def->Output.Size);
        break;
      case NdOp::INT_SUB:
        if (Def->NumInputs >= 2)
          if (auto A = Eval(Def->Inputs[0]))
            if (auto B = Eval(Def->Inputs[1]))
              Result = mask(*A - *B, Def->Output.Size);
        break;
      default:
        break;
      }
    }
    Active.erase(K);
    Memo.emplace(K, Result);
    return Result;
  };
  return Eval(V);
}

bool MedLLVMEmitter::frameSlotReloadUsedLocally(const MedVar &StoreAddr) const {
  if (!CurMedFunc)
    return false;
  auto keyOf = [&](const MedVar &V) {
    if (auto Canonical = canonicalFrameSlotKey(V))
      return Canonical;
    return addrSlotKey(V);
  };
  auto SK = keyOf(StoreAddr);
  if (!SK)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotReloadUsedLocallyCache.find(*SK);
      It != SlotReloadUsedLocallyCache.end())
    return It->second;

  auto isFwd = [](NdOp Op) {
    return Op == NdOp::COPY || Op == NdOp::INT_ZEXT || Op == NdOp::INT_SEXT ||
           Op == NdOp::SUBBYTES;
  };
  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };
  auto compute = [&]() -> bool {
    // The reload values (LOAD outputs of slot SK) plus everything they flow
    // into through pure-forwarding ops (COPY/widen/low-slice) — the values that
    // still carry the reloaded pointer.
    std::vector<MedVar> ReloadVals;
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
          if (auto LK = keyOf(Op.Inputs[0]); LK && *LK == *SK)
            ReloadVals.push_back(Op.Output);
    if (ReloadVals.empty())
      return false;
    auto inReloadSet = [&](const MedVar &V) {
      for (const auto &R : ReloadVals)
        if (sameVar(V, R))
          return true;
      return false;
    };
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const auto &Blk : CurMedFunc->Blocks)
        for (const auto &Op : Blk.Ops)
          if (isFwd(Op.Opcode) && Op.NumInputs >= 1 &&
              inReloadSet(Op.Inputs[0]) && !inReloadSet(Op.Output)) {
            if (Op.Opcode == NdOp::SUBBYTES &&
                !(Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                  Op.Inputs[1].ConstVal == 0))
              continue; // a high-slice extract drops the pointer, not
                        // forwarding
            ReloadVals.push_back(Op.Output);
            Changed = true;
          }
    }
    // Any NON-forwarding op consuming a reloaded value uses the pointer locally
    // (dereference address, `p++`, `p - base`, comparison) — so it must keep
    // the original VA.  Pure forwarding to the return register, and a RETURN
    // that takes the reload directly as its value operand (x86-64 lowers
    // `return p` to `RETURN <reload>`), are escapes, not local uses.
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if (isFwd(Op.Opcode) || Op.Opcode == NdOp::RETURN)
          continue;
        for (int I = 0; I < Op.NumInputs; ++I)
          if (inReloadSet(Op.Inputs[I]))
            return true;
      }
    return false;
  };

  bool Result = compute();
  SlotReloadUsedLocallyCache[*SK] = Result;
  return Result;
}

} // namespace neverd
