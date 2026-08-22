//===- MedLLVMAddrResolve.cpp - Shared address tracing helpers --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared SSA constant tracing and address-base decomposition helpers for the
/// literal/select, indexed/induction, and code-pointer resolvers.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/support/Diagnostic.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

namespace {

struct IndexedPointerLaneSummary {
  std::set<uint64_t> Slots;
  std::vector<MedVar> IndexTerms;
  bool Recognized = false;
  bool Complete = false;
};

struct OffsetCongruence {
  bool Valid = false;
  uint64_t Residue = 0;
  uint64_t Modulus = 0;
  std::optional<uint64_t> MinValue;
  std::optional<uint64_t> MaxValue;

  bool isExact() const { return Valid && Modulus == 0; }
  bool hasFiniteRange() const {
    return MinValue.has_value() && MaxValue.has_value();
  }
};

/// Recover the one record lane selected by a conventional `base + index`
/// address without deciding whether the runtime index itself is relocatable.
/// The modular result is purely algebraic: multiplying any bit-pattern by 16
/// reaches only the +0 lane modulo 16, for example.  Consumers separately
/// prove the index value model before using the result, while scalar-load
/// recurrence analysis can use the lane first and then audit the index in the
/// same DFS (so a loop-carried scalar field does not create a false cycle).
template <typename LookupDefFn, typename TraceConstFn, typename CollectBaseFn,
          typename DiscoverRunFn, typename ReadOnlyRunFn>
IndexedPointerLaneSummary analyzeIndexedPointerLane(
    const MedVar &Address, const BinaryImage *Img, unsigned PtrSize,
    LookupDefFn &&LookupDef, TraceConstFn &&TraceConst,
    CollectBaseFn &&CollectBase, DiscoverRunFn &&DiscoverRun,
    ReadOnlyRunFn &&ReadOnlyRun, const std::set<uint64_t> *KnownBases = nullptr,
    const std::vector<MedVar> *KnownTerms = nullptr,
    bool SubtractKnownTerms = false) {
  IndexedPointerLaneSummary Result;
  if (!Img || PtrSize == 0 || PtrSize > 8)
    return Result;

  const unsigned PtrBits = PtrSize * 8;
  const uint64_t PtrMask =
      PtrBits >= 64 ? ~uint64_t(0) : (uint64_t(1) << PtrBits) - 1;
  const uint64_t MaxModulus = uint64_t(1) << (std::min(PtrBits, 64U) - 1);
  auto exact = [&](uint64_t Value) {
    Value &= PtrMask;
    return OffsetCongruence{true, Value, 0, Value, Value};
  };
  auto dynamic = [&](uint64_t Residue, uint64_t Modulus,
                     std::optional<uint64_t> MinValue = std::nullopt,
                     std::optional<uint64_t> MaxValue = std::nullopt) {
    assert(Modulus != 0 && (Modulus & (Modulus - 1)) == 0);
    return OffsetCongruence{true, Residue & (Modulus - 1), Modulus, MinValue,
                            MaxValue};
  };
  auto lowBit = [](uint64_t Value) {
    return Value == 0 ? uint64_t(0) : Value & (~Value + uint64_t(1));
  };
  auto add = [&](OffsetCongruence Left, OffsetCongruence Right, bool Subtract) {
    if (!Left.Valid || !Right.Valid)
      return OffsetCongruence{};
    const uint64_t RightResidue =
        Subtract ? (uint64_t(0) - Right.Residue) & PtrMask : Right.Residue;
    const uint64_t Residue = (Left.Residue + RightResidue) & PtrMask;
    if (Left.isExact() && Right.isExact())
      return exact(Residue);
    const uint64_t Modulus = Left.isExact() ? Right.Modulus
                             : Right.isExact()
                                 ? Left.Modulus
                                 : std::min(Left.Modulus, Right.Modulus);
    OffsetCongruence Result = dynamic(Residue, Modulus);
    if (Left.hasFiniteRange() && Right.hasFiniteRange()) {
      if (!Subtract && *Left.MaxValue <= PtrMask - *Right.MaxValue) {
        Result.MinValue = *Left.MinValue + *Right.MinValue;
        Result.MaxValue = *Left.MaxValue + *Right.MaxValue;
      } else if (Subtract && *Left.MinValue >= *Right.MaxValue) {
        Result.MinValue = *Left.MinValue - *Right.MaxValue;
        Result.MaxValue = *Left.MaxValue - *Right.MinValue;
      }
    }
    return Result;
  };
  auto scale = [&](OffsetCongruence Input, uint64_t Factor) {
    if (!Input.Valid)
      return OffsetCongruence{};
    const uint64_t Residue = (Input.Residue * Factor) & PtrMask;
    if (Input.isExact() || Factor == 0)
      return exact(Residue);
    const uint64_t FactorModulus = lowBit(Factor);
    uint64_t Modulus = Input.Modulus;
    if (FactorModulus > 1) {
      if (Modulus > MaxModulus / FactorModulus)
        Modulus = MaxModulus;
      else
        Modulus *= FactorModulus;
    }
    OffsetCongruence Result = dynamic(Residue, Modulus);
    if (Input.hasFiniteRange() &&
        (Factor == 0 || *Input.MaxValue <= PtrMask / Factor)) {
      Result.MinValue = *Input.MinValue * Factor;
      Result.MaxValue = *Input.MaxValue * Factor;
    }
    return Result;
  };
  auto resize = [&](OffsetCongruence Input, unsigned SourceBits,
                    unsigned ResultBits, bool SignExtend) {
    if (!Input.Valid || SourceBits == 0 || ResultBits == 0)
      return OffsetCongruence{};
    SourceBits = std::min(SourceBits, 64U);
    ResultBits = std::min(ResultBits, 64U);
    const uint64_t SourceMask =
        SourceBits == 64 ? ~uint64_t(0) : (uint64_t(1) << SourceBits) - 1;
    const uint64_t ResultMask =
        ResultBits == 64 ? ~uint64_t(0) : (uint64_t(1) << ResultBits) - 1;
    uint64_t Residue = Input.Residue & SourceMask;
    if (SignExtend && ResultBits > SourceBits &&
        (Residue & (uint64_t(1) << (SourceBits - 1))) != 0)
      Residue |= ~SourceMask;
    Residue &= ResultMask & PtrMask;
    if (Input.isExact())
      return exact(Residue);
    const unsigned PreservedBits =
        std::min({SourceBits, ResultBits, PtrBits, 64U});
    const uint64_t MaxPreserved = uint64_t(1) << (PreservedBits - 1);
    OffsetCongruence Result =
        dynamic(Residue, std::min(Input.Modulus, MaxPreserved));
    if (Input.hasFiniteRange()) {
      if (!SignExtend && *Input.MaxValue <= ResultMask) {
        Result.MinValue = *Input.MinValue;
        Result.MaxValue = *Input.MaxValue;
      } else if (!SignExtend) {
        Result.MinValue = 0;
        Result.MaxValue = ResultMask & PtrMask;
      } else {
        const uint64_t SignBit = uint64_t(1) << (SourceBits - 1);
        if (*Input.MaxValue < SignBit) {
          Result.MinValue = *Input.MinValue;
          Result.MaxValue = *Input.MaxValue;
        }
      }
    }
    return Result;
  };
  auto merge = [&](OffsetCongruence Left, OffsetCongruence Right) {
    if (!Left.Valid || !Right.Valid)
      return OffsetCongruence{};
    if (Left.isExact() && Right.isExact() && Left.Residue == Right.Residue)
      return Left;
    const uint64_t Difference = (Left.Residue - Right.Residue) & PtrMask;
    uint64_t Modulus = 0;
    auto includeModulus = [&](uint64_t Candidate) {
      if (Candidate != 0)
        Modulus = Modulus == 0 ? Candidate : std::min(Modulus, Candidate);
    };
    includeModulus(Left.isExact() ? 0 : Left.Modulus);
    includeModulus(Right.isExact() ? 0 : Right.Modulus);
    includeModulus(lowBit(Difference));
    OffsetCongruence Result =
        Modulus == 0 ? exact(Left.Residue) : dynamic(Left.Residue, Modulus);
    if (Left.hasFiniteRange() && Right.hasFiniteRange()) {
      Result.MinValue = std::min(*Left.MinValue, *Right.MinValue);
      Result.MaxValue = std::max(*Left.MaxValue, *Right.MaxValue);
    }
    return Result;
  };
  auto foldedConstant = [&](const MedVar &Value) -> std::optional<uint64_t> {
    if (Value.isConst())
      return Value.ConstVal & PtrMask;
    if (auto Folded = TraceConst(Value))
      return *Folded & PtrMask;
    return std::nullopt;
  };

  using Key = std::tuple<int, int, int, uint16_t>;
  auto keyOf = [](const MedVar &Value) {
    return Key{static_cast<int>(Value.Kind), Value.Id, Value.SSAVer,
               Value.Size};
  };
  std::function<OffsetCongruence(const MedVar &, int, std::set<Key>)> walk =
      [&](const MedVar &Value, int Depth,
          std::set<Key> Seen) -> OffsetCongruence {
    if (Depth > 64)
      return {};
    if (auto Folded = foldedConstant(Value))
      return exact(*Folded);
    if (!Seen.insert(keyOf(Value)).second)
      return dynamic(0, 1);
    const MedOp *Def = LookupDef(Value);
    if (!Def)
      return dynamic(0, 1);
    if ((Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
         Def->Opcode == NdOp::INT_SEXT || Def->Opcode == NdOp::SUBBYTES) &&
        Def->NumInputs >= 1) {
      if (Def->Opcode == NdOp::SUBBYTES &&
          (Def->NumInputs < 2 || !Def->Inputs[1].isConst() ||
           Def->Inputs[1].ConstVal != 0))
        return dynamic(0, 1);
      return resize(walk(Def->Inputs[0], Depth + 1, Seen),
                    Def->Inputs[0].Size * 8, Def->Output.Size * 8,
                    Def->Opcode == NdOp::INT_SEXT);
    }
    if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
        Def->NumInputs >= 2)
      return add(walk(Def->Inputs[0], Depth + 1, Seen),
                 walk(Def->Inputs[1], Depth + 1, Seen),
                 Def->Opcode == NdOp::INT_SUB);
    if (Def->Opcode == NdOp::INT_MULT && Def->NumInputs >= 2) {
      for (unsigned ConstantIndex : {0U, 1U})
        if (auto Factor = foldedConstant(Def->Inputs[ConstantIndex]))
          return scale(walk(Def->Inputs[1U - ConstantIndex], Depth + 1, Seen),
                       *Factor);
      return dynamic(0, 1);
    }
    if (Def->Opcode == NdOp::INT_LEFT && Def->NumInputs >= 2)
      if (auto Shift = foldedConstant(Def->Inputs[1]);
          Shift && *Shift < PtrBits)
        return scale(walk(Def->Inputs[0], Depth + 1, Seen),
                     uint64_t(1) << static_cast<unsigned>(*Shift));
    if ((Def->Opcode == NdOp::INT_AND || Def->Opcode == NdOp::INT_OR ||
         Def->Opcode == NdOp::INT_XOR) &&
        Def->NumInputs >= 2) {
      for (unsigned ConstantIndex : {0U, 1U}) {
        auto Constant = foldedConstant(Def->Inputs[ConstantIndex]);
        if (!Constant)
          continue;
        const uint64_t Bits = *Constant & PtrMask;
        OffsetCongruence Input =
            walk(Def->Inputs[1U - ConstantIndex], Depth + 1, Seen);
        if (!Input.Valid)
          return {};
        if (Input.isExact()) {
          if (Def->Opcode == NdOp::INT_AND)
            return exact(Input.Residue & Bits);
          if (Def->Opcode == NdOp::INT_OR)
            return exact(Input.Residue | Bits);
          return exact(Input.Residue ^ Bits);
        }
        if (Def->Opcode == NdOp::INT_XOR)
          return dynamic(Input.Residue ^ Bits, Input.Modulus);
        const uint64_t FreeBits =
            Def->Opcode == NdOp::INT_AND ? Bits : (~Bits & PtrMask);
        if (FreeBits == 0)
          return exact(Def->Opcode == NdOp::INT_AND ? 0 : PtrMask);
        const uint64_t ForcedModulus = lowBit(FreeBits);
        const uint64_t Modulus = std::max(Input.Modulus, ForcedModulus);
        const uint64_t Residue = Def->Opcode == NdOp::INT_AND
                                     ? Input.Residue & Bits
                                     : Input.Residue | Bits;
        if (Def->Opcode == NdOp::INT_AND)
          return dynamic(Residue, Modulus, uint64_t(0), Bits);
        return dynamic(Residue, Modulus);
      }
    }
    if ((Def->Opcode == NdOp::INT_RIGHT || Def->Opcode == NdOp::INT_ASHR) &&
        Def->NumInputs >= 2)
      if (auto Shift = foldedConstant(Def->Inputs[1]);
          Shift && Def->Inputs[0].Size != 0 &&
          *Shift < Def->Inputs[0].Size * 8 && *Shift < PtrBits) {
        OffsetCongruence Input = walk(Def->Inputs[0], Depth + 1, Seen);
        if (!Input.Valid)
          return {};
        if (Input.isExact()) {
          uint64_t Shifted = Input.Residue >> static_cast<unsigned>(*Shift);
          const unsigned SourceBits =
              std::min<unsigned>(Def->Inputs[0].Size * 8, 64U);
          if (Def->Opcode == NdOp::INT_ASHR && *Shift != 0 && SourceBits != 0 &&
              (Input.Residue & (uint64_t(1) << (SourceBits - 1))) != 0)
            Shifted |= ~uint64_t(0) << (SourceBits - *Shift);
          return exact(Shifted);
        }
        const uint64_t Divisor = uint64_t(1) << static_cast<unsigned>(*Shift);
        if (Input.Modulus <= Divisor)
          return dynamic(0, 1);
        OffsetCongruence Result =
            dynamic(Input.Residue >> static_cast<unsigned>(*Shift),
                    Input.Modulus / Divisor);
        if (Input.hasFiniteRange()) {
          Result.MinValue = *Input.MinValue >> static_cast<unsigned>(*Shift);
          Result.MaxValue = *Input.MaxValue >> static_cast<unsigned>(*Shift);
        }
        return Result;
      }
    if ((Def->Opcode == NdOp::INT_NEG2 || Def->Opcode == NdOp::INT_NOT) &&
        Def->NumInputs >= 1) {
      OffsetCongruence Input = walk(Def->Inputs[0], Depth + 1, Seen);
      if (!Input.Valid)
        return {};
      const uint64_t Residue = Def->Opcode == NdOp::INT_NEG2
                                   ? uint64_t(0) - Input.Residue
                                   : ~Input.Residue;
      return Input.isExact() ? exact(Residue) : dynamic(Residue, Input.Modulus);
    }
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3)
      return merge(walk(Def->Inputs[1], Depth + 1, Seen),
                   walk(Def->Inputs[2], Depth + 1, Seen));
    return dynamic(0, 1);
  };

  const std::optional<uint64_t> Run = DiscoverRun(Address);
  Result.Recognized = Run.has_value();
  std::set<uint64_t> Bases;
  if (KnownBases && KnownTerms) {
    Bases = *KnownBases;
    Result.IndexTerms = *KnownTerms;
  } else {
    uint64_t Base = 0;
    bool HaveBase = false;
    if (!CollectBase(Address, Base, HaveBase, Result.IndexTerms) || !HaveBase)
      return Result;
    Bases.insert(Base);
  }
  if (Bases.empty() || Result.IndexTerms.empty())
    return Result;

  OffsetCongruence Combined = exact(0);
  for (const MedVar &Term : Result.IndexTerms)
    Combined = add(Combined, walk(Term, 0, {}), SubtractKnownTerms);
  if (!Combined.Valid || Combined.isExact() || Combined.Modulus < PtrSize ||
      Combined.Modulus % PtrSize != 0)
    return Result;

  size_t Remaining = static_cast<size_t>(limits::kMaxSSANodes);
  std::optional<uint64_t> CommonRunStart;
  for (uint64_t Base : Bases) {
    uint64_t Origin = (Base + Combined.Residue) & PtrMask;
    std::optional<uint64_t> LastBoundedSlot;
    if (Combined.hasFiniteRange()) {
      const uint64_t MinValue = *Combined.MinValue;
      const uint64_t MaxValue = *Combined.MaxValue;
      const uint64_t MinResidue = MinValue & (Combined.Modulus - 1);
      const uint64_t Adjustment =
          (Combined.Residue - MinResidue) & (Combined.Modulus - 1);
      if (MinValue > PtrMask - Adjustment)
        return Result;
      const uint64_t FirstOffset = MinValue + Adjustment;
      if (FirstOffset > MaxValue || Base > PtrMask - FirstOffset)
        return Result;
      const uint64_t LastOffset =
          FirstOffset +
          ((MaxValue - FirstOffset) / Combined.Modulus) * Combined.Modulus;
      if (Base > PtrMask - LastOffset)
        return Result;
      Origin = Base + FirstOffset;
      LastBoundedSlot = Base + LastOffset;
    }
    const Segment *OriginSegment = Img->getSegmentFor(Origin);
    const Section *OriginSection =
        OriginSegment && !OriginSegment->isExecutable()
            ? Img->getSectionFor(Origin)
            : nullptr;
    if (!OriginSection || !OriginSection->isReadable() ||
        OriginSection->isExecutable() ||
        OriginSection->Size > InvalidVA - OriginSection->VA ||
        Origin < OriginSection->VA ||
        Origin >= OriginSection->VA + OriginSection->Size)
      return Result;

    uint64_t RunStart = OriginSegment->VA;
    uint64_t RunEnd = OriginSegment->VA + OriginSegment->Data.size();
    ReadOnlyRun(OriginSegment, RunStart, RunEnd);
    if ((Run && *Run != RunStart) ||
        (CommonRunStart && *CommonRunStart != RunStart))
      return Result;
    CommonRunStart = RunStart;

    const uint64_t BoundEnd = OriginSection->VA + OriginSection->Size;
    uint64_t Slot = Origin;
    if (!LastBoundedSlot) {
      const uint64_t Residue = (Origin - OriginSection->VA) % Combined.Modulus;
      Slot = OriginSection->VA + Residue;
    } else if (*LastBoundedSlot < Origin || *LastBoundedSlot >= BoundEnd ||
               PtrSize > BoundEnd - *LastBoundedSlot) {
      return Result;
    }
    while (Slot < BoundEnd && PtrSize <= BoundEnd - Slot &&
           (!LastBoundedSlot || Slot <= *LastBoundedSlot)) {
      if (Remaining-- == 0)
        return Result;
      const Segment *SlotSegment = Img->getSegmentFor(Slot);
      if (!SlotSegment || SlotSegment->isExecutable())
        return Result;
      uint64_t SlotRunStart = SlotSegment->VA;
      uint64_t SlotRunEnd = SlotSegment->VA + SlotSegment->Data.size();
      ReadOnlyRun(SlotSegment, SlotRunStart, SlotRunEnd);
      if (SlotRunStart != RunStart || Slot < SlotRunStart ||
          PtrSize > SlotRunEnd - Slot)
        return Result;
      Result.Slots.insert(Slot);
      if (Slot > InvalidVA - Combined.Modulus)
        break;
      Slot += Combined.Modulus;
    }
  }
  Result.Complete = !Result.Slots.empty();
  return Result;
}

} // namespace

//===----------------------------------------------------------------------===//
// Conservative control-flow feasibility
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::controlConstantMayRelocate(const MedVar &V) const {
  if (!Img)
    return false;
  const uint64_t Val = V.ConstVal;
  const uint16_t Size = V.Size;

  const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
  const bool IsPointerWidth =
      Size == 0 || PointerSize == 0 || Size >= PointerSize;
  const bool CanResolveData = canResolveGlobalDataConstant(Val);
  const bool ExactReadOnlyData = Img->RelocDataAddrs.count(Val) != 0 ||
                                 Img->RodataAnchorSeg.count(Val) != 0;

  if (isExactAddressProvenance(V.Provenance) ||
      V.Provenance == ConstantAddressProvenance::AddressFragment)
    return true;
  if (V.Provenance == ConstantAddressProvenance::Scalar)
    return false;

  // Numeric zero is a control/null value unless this exact occurrence carried
  // address provenance above. A different relocation that legitimately names
  // data or code at VA zero must not keep an unrelated `if (0)` edge alive.
  if (Val == 0)
    return false;

  // Match the high-VA direct getVar domain without consulting use-site
  // analysis. Readable executable bytes are intentionally included: an exact
  // literal-pool/data use inside .text can still be emitted as rebuilt data.
  if (IsPointerWidth && Val > limits::kMinGlobalDataAddr && CanResolveData)
    return true;

  // Every pointer-width data constant getVar can currently rewrite must first
  // have a valid materialization route. Treat that complete route domain as
  // potential, including anchors and one-past-end runs. Narrow constants need
  // stronger loader/string evidence: otherwise a small integer that merely
  // lands in a low-VA segment would make an ordinary constant branch unknown.
  if (CanResolveData &&
      (ExactReadOnlyData || Img->getSegmentFor(Val) == nullptr ||
       isCleanRodataStringAddress(Val)))
    return true;

  if (!IsPointerWidth)
    return false;

  if (Img->WritableRelocDataAddrs.count(Val) != 0)
    return true;

  if (CanResolveData && mayBeWritableSelectPeerCycleFree(Val))
    return true;

  // Function/code-reference constants can become rebuilt function pointers
  // even when they have no data materialization route. A bare low-VA function
  // name is not enough: getVar deliberately requires either an address-taken
  // target or the same high-VA threshold, otherwise an ordinary small integer
  // that equals a function entry would keep a dead control edge alive.
  return FuncNames.count(Val) != 0 && (Val >= limits::kMinGlobalDataAddr ||
                                       Img->CodeRefTargets.count(Val) != 0);
}

std::optional<uint64_t>
MedLLVMEmitter::traceControlConst(const MedVar &V) const {
  if (!CurMedFunc)
    return std::nullopt;

  using Key = std::tuple<int, int, int>;
  std::map<Key, std::optional<uint64_t>> Cache;
  std::set<Key> Active;

  auto width = [](const MedVar &X) -> unsigned {
    if (X.Size == 0)
      return 64;
    return X.Size <= 8 ? X.Size * 8 : 0;
  };
  auto bitMask = [](unsigned Bits) -> uint64_t {
    return Bits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << Bits) - 1);
  };
  auto atWidth = [&](uint64_t X, unsigned Bits) { return X & bitMask(Bits); };
  auto signedAtWidth = [&](uint64_t X, unsigned Bits) -> int64_t {
    X = atWidth(X, Bits);
    if (Bits < 64 && (X & (uint64_t(1) << (Bits - 1))))
      X |= ~bitMask(Bits);
    return static_cast<int64_t>(X);
  };
  std::function<std::optional<uint64_t>(const MedVar &, int)> Eval =
      [&](const MedVar &Cur, int Depth) -> std::optional<uint64_t> {
    unsigned CurWidth = width(Cur);
    if (CurWidth == 0 || Depth > 32)
      return std::nullopt;
    // getVar does not necessarily materialize an address-shaped constant as
    // this original numeric value. Once a leaf becomes ptrtoint(@global) or
    // ptrtoint(@function), equality/order against another original VA is a
    // link-time question and cannot prove either CFG edge dead here.
    if (Cur.isConst() && controlConstantMayRelocate(Cur))
      return std::nullopt;
    if (Cur.isConst())
      return atWidth(Cur.ConstVal, CurWidth);

    Key K{static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer};
    if (auto It = Cache.find(K); It != Cache.end())
      return It->second;
    if (!Active.insert(K).second)
      return std::nullopt;

    std::optional<uint64_t> Result;
    const MedOp *Def = lookupDef(Cur);
    if (!Def) {
      Active.erase(K);
      Cache.emplace(K, Result);
      return Result;
    }

    auto one = [&]() -> std::optional<uint64_t> {
      return Def->NumInputs >= 1 ? Eval(Def->Inputs[0], Depth + 1)
                                 : std::nullopt;
    };
    auto two = [&]() -> std::optional<std::pair<uint64_t, uint64_t>> {
      if (Def->NumInputs < 2)
        return std::nullopt;
      auto A = Eval(Def->Inputs[0], Depth + 1);
      auto B = Eval(Def->Inputs[1], Depth + 1);
      if (!A || !B)
        return std::nullopt;
      unsigned W = std::max(width(Def->Inputs[0]), width(Def->Inputs[1]));
      if (W == 0)
        return std::nullopt;
      return std::pair<uint64_t, uint64_t>{atWidth(*A, W), atWidth(*B, W)};
    };
    unsigned OutWidth = width(Def->Output);

    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
      if (auto A = one())
        Result = atWidth(*A, OutWidth);
      break;
    case NdOp::INT_SEXT:
      if (Def->NumInputs >= 1)
        if (auto A = one()) {
          unsigned InWidth = width(Def->Inputs[0]);
          if (InWidth != 0)
            Result = atWidth(static_cast<uint64_t>(signedAtWidth(*A, InWidth)),
                             OutWidth);
        }
      break;
    case NdOp::SUBBYTES:
      if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
          !controlConstantMayRelocate(Def->Inputs[1]))
        if (auto A = one()) {
          uint64_t Shift = Def->Inputs[1].ConstVal * 8;
          unsigned InWidth = width(Def->Inputs[0]);
          Result = Shift >= InWidth ? 0 : atWidth(*A >> Shift, OutWidth);
        }
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_MULT:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
      if (auto AB = two()) {
        uint64_t R = 0;
        switch (Def->Opcode) {
        case NdOp::INT_ADD:
          R = AB->first + AB->second;
          break;
        case NdOp::INT_SUB:
          R = AB->first - AB->second;
          break;
        case NdOp::INT_AND:
        case NdOp::BOOL_AND:
          R = AB->first & AB->second;
          break;
        case NdOp::INT_OR:
        case NdOp::BOOL_OR:
          R = AB->first | AB->second;
          break;
        case NdOp::INT_XOR:
        case NdOp::BOOL_XOR:
          R = AB->first ^ AB->second;
          break;
        case NdOp::INT_MULT:
          R = AB->first * AB->second;
          break;
        default:
          break;
        }
        Result = atWidth(R, OutWidth);
      }
      break;
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
      if (auto AB = two()) {
        // emitOp coerces both operands to their widest integer type before
        // shifting.  Use that same width for the overshift guard and ASHR
        // sign bit; the left operand's original width is insufficient when a
        // narrow value is shifted by a pointer-width count.
        unsigned OpWidth =
            std::max(width(Def->Inputs[0]), width(Def->Inputs[1]));
        if (OpWidth != 0) {
          uint64_t Shift = AB->second;
          if (Def->Opcode == NdOp::INT_LEFT)
            Result =
                Shift >= OpWidth ? 0 : atWidth(AB->first << Shift, OutWidth);
          else if (Def->Opcode == NdOp::INT_RIGHT)
            Result =
                Shift >= OpWidth ? 0 : atWidth(AB->first >> Shift, OutWidth);
          else {
            unsigned Clamped =
                static_cast<unsigned>(std::min<uint64_t>(Shift, OpWidth - 1));
            Result = atWidth(static_cast<uint64_t>(
                                 signedAtWidth(AB->first, OpWidth) >> Clamped),
                             OutWidth);
          }
        }
      }
      break;
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
      if (auto AB = two()) {
        unsigned W = std::max(width(Def->Inputs[0]), width(Def->Inputs[1]));
        bool R = false;
        switch (Def->Opcode) {
        case NdOp::INT_EQUAL:
          R = AB->first == AB->second;
          break;
        case NdOp::INT_NOTEQUAL:
          R = AB->first != AB->second;
          break;
        case NdOp::INT_LESS:
          R = AB->first < AB->second;
          break;
        case NdOp::INT_SLESS:
          R = signedAtWidth(AB->first, W) < signedAtWidth(AB->second, W);
          break;
        case NdOp::INT_LESSEQUAL:
          R = AB->first <= AB->second;
          break;
        case NdOp::INT_SLESSEQUAL:
          R = signedAtWidth(AB->first, W) <= signedAtWidth(AB->second, W);
          break;
        default:
          break;
        }
        Result = R ? 1 : 0;
      }
      break;
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT:
      if (auto A = one()) {
        unsigned InWidth = width(Def->Inputs[0]);
        if (InWidth != 0)
          Result = atWidth(~*A, InWidth);
      }
      break;
    case NdOp::INT_NEG2:
      if (auto A = one()) {
        unsigned InWidth = width(Def->Inputs[0]);
        if (InWidth != 0)
          Result = atWidth(uint64_t(0) - *A, InWidth);
      }
      break;
    case NdOp::BOOL_NOT:
      if (auto A = one())
        Result = *A == 0 ? 1 : 0;
      break;
    case NdOp::SELECT:
      if (Def->NumInputs >= 3)
        if (auto Cond = Eval(Def->Inputs[0], Depth + 1))
          Result = Eval(Def->Inputs[*Cond != 0 ? 1 : 2], Depth + 1);
      break;
    default:
      break;
    }

    // setVar stores every operation result at the declared output width after
    // emitOp has evaluated it at the operands' coerced width.  SELECT needs
    // this in particular: choosing a wide non-zero arm can still produce zero
    // after truncation to a narrow condition variable.
    if (Result)
      Result = atWidth(*Result, OutWidth);

    Active.erase(K);
    Cache.emplace(K, Result);
    return Result;
  };

  return Eval(V, 0);
}

void MedLLVMEmitter::invalidateFeasibleEdgeDependentCaches() const {
  PhiEdgeClassCache.clear();
  FeasibleControlComponentsReady = false;
  FeasibleControlComponents.clear();

  PhiRecurrenceCacheFor = nullptr;
  PhiRecurrenceCache.clear();
  SelfRecurrenceCacheFor = nullptr;
  SelfRecurrenceCache.clear();
  StableOffsetCacheFor = nullptr;
  StableOffsetCache.clear();
  IndexedGlobalBaseCacheFor = nullptr;
  IndexedGlobalBaseCache.clear();
  InductionBasesFor = nullptr;
  InductionBaseVAs.clear();
  PtrTableUniqueSegCache.clear();
  PointerTableLoadRoleCacheFor = nullptr;
  PointerTableLoadRoleCache.clear();
  WritableDataSegCache.clear();
  FrameDerivedCacheFor = nullptr;
  FrameDerivedCache.clear();
  FrameAddressCacheFor = nullptr;
  FrameAddressCache.clear();
}

void MedLLVMEmitter::ensureFeasibleEdgeCache() const {
  if (FeasibleEdgesFor != CurMedFunc) {
    FeasibleEdgesFor = CurMedFunc;
    FeasibleEdgeState = FeasibleEdgeCacheState::Empty;
    FeasibleEdgeBuildSawReentrantQuery = false;
    FeasibleEdges.clear();
    FeasibleBlocks.clear();
  }
  if (FeasibleEdgeState == FeasibleEdgeCacheState::Ready)
    return;
  if (FeasibleEdgeState == FeasibleEdgeCacheState::Building) {
    FeasibleEdgeBuildSawReentrantQuery = true;
    return;
  }

  // No dependent memo may survive into a new generation. A second reset after
  // publication discards any provisional result produced by defensive
  // Building-state re-entry.
  invalidateFeasibleEdgeDependentCaches();
  FeasibleEdgeState = FeasibleEdgeCacheState::Building;
  FeasibleEdgeBuildSawReentrantQuery = false;

  std::set<std::pair<int, int>> NextFeasibleEdges;
  std::set<int> NextFeasibleBlocks;
  if (!CurMedFunc || CurMedFunc->Blocks.empty()) {
    FeasibleEdges = std::move(NextFeasibleEdges);
    FeasibleBlocks = std::move(NextFeasibleBlocks);
    FeasibleEdgeState = FeasibleEdgeCacheState::Ready;
    invalidateFeasibleEdgeDependentCaches();
    return;
  }

  std::map<int, const MedBlock *> Blocks;
  for (const MedBlock &Block : CurMedFunc->Blocks)
    Blocks.emplace(Block.Id, &Block);
  auto blockAddress = [&](int Id) -> std::optional<va_t> {
    auto It = Blocks.find(Id);
    if (It == Blocks.end())
      return std::nullopt;
    const MedBlock &Block = *It->second;
    return Block.StartAddr != 0 || Block.Ops.empty() ? Block.StartAddr
                                                     : Block.Ops.front().Addr;
  };

  std::vector<int> Work{CurMedFunc->Blocks.front().Id};
  NextFeasibleBlocks.insert(Work.front());
  while (!Work.empty()) {
    int BlockId = Work.back();
    Work.pop_back();
    auto BIt = Blocks.find(BlockId);
    if (BIt == Blocks.end())
      continue;
    const MedBlock &Block = *BIt->second;

    std::vector<int> OrdinarySuccs = Block.Succs;
    if (Block.Succs.size() == 2) {
      const MedOp *Branch = nullptr;
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::COND_BR) {
          Branch = &Op;
          break;
        }
      if (Branch && Branch->NumInputs >= 2 && Branch->Inputs[0].isConst() &&
          !FeasibleEdgeBuildSawReentrantQuery) {
#ifndef NDEBUG
        const bool HadFatalCodePointerResolution = FatalCodePointerResolution;
        const bool HadFatalDataPointerResolution = FatalDataPointerResolution;
#endif
        auto Cond = traceControlConst(Branch->Inputs[1]);
#ifndef NDEBUG
        assert(HadFatalCodePointerResolution == FatalCodePointerResolution &&
               HadFatalDataPointerResolution == FatalDataPointerResolution &&
               "control feasibility must not emit pointer diagnostics");
#endif
        // A future dependency accidentally re-entering PHI feasibility while
        // this condition is evaluated makes the whole build conservative. Do
        // not let a provisional negative result prune this or any later edge.
        if (Cond && !FeasibleEdgeBuildSawReentrantQuery) {
          // Match MedLLVMFuncBody's branch lowering exactly: successor 1 is
          // the default taken edge, while a non-zero target address may name
          // successor 0 explicitly. In particular, never confuse an unknown
          // zero block address with positive evidence for successor 0.
          int Taken = Block.Succs[1];
          if (Branch->Inputs[0].ConstVal != 0)
            if (auto A0 = blockAddress(Block.Succs[0]);
                A0 && *A0 == Branch->Inputs[0].ConstVal)
              Taken = Block.Succs[0];
          int Fallthrough =
              Taken == Block.Succs[0] ? Block.Succs[1] : Block.Succs[0];
          OrdinarySuccs = {*Cond != 0 ? Taken : Fallthrough};
        }
      }
    }

    for (int Succ : OrdinarySuccs) {
      if (!Blocks.count(Succ))
        continue;
      NextFeasibleEdges.insert({BlockId, Succ});
      if (NextFeasibleBlocks.insert(Succ).second)
        Work.push_back(Succ);
    }
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
      if (!Blocks.count(Edge.BlockId))
        continue;
      NextFeasibleEdges.insert({BlockId, Edge.BlockId});
      if (NextFeasibleBlocks.insert(Edge.BlockId).second)
        Work.push_back(Edge.BlockId);
    }
    if (FeasibleEdgeBuildTestHook) {
      std::function<void()> Hook = std::move(FeasibleEdgeBuildTestHook);
      FeasibleEdgeBuildTestHook = {};
      Hook();
    }
  }

  if (FeasibleEdgeBuildSawReentrantQuery) {
    // A defensive re-entry may be discovered only after earlier conditions
    // were pruned. Rebuild from structure alone so the sticky fallback really
    // applies to the entire generation, not merely the remaining worklist.
    NextFeasibleEdges.clear();
    NextFeasibleBlocks.clear();
    Work = {CurMedFunc->Blocks.front().Id};
    NextFeasibleBlocks.insert(Work.front());
    while (!Work.empty()) {
      const int BlockId = Work.back();
      Work.pop_back();
      const auto It = Blocks.find(BlockId);
      if (It == Blocks.end())
        continue;
      const MedBlock &Block = *It->second;
      for (int Succ : Block.Succs) {
        if (!Blocks.count(Succ))
          continue;
        NextFeasibleEdges.insert({BlockId, Succ});
        if (NextFeasibleBlocks.insert(Succ).second)
          Work.push_back(Succ);
      }
      for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
        if (!Blocks.count(Edge.BlockId))
          continue;
        NextFeasibleEdges.insert({BlockId, Edge.BlockId});
        if (NextFeasibleBlocks.insert(Edge.BlockId).second)
          Work.push_back(Edge.BlockId);
      }
    }
  }

  FeasibleEdges = std::move(NextFeasibleEdges);
  FeasibleBlocks = std::move(NextFeasibleBlocks);
  FeasibleEdgeState = FeasibleEdgeCacheState::Ready;
  FeasibleEdgeBuildSawReentrantQuery = false;
  invalidateFeasibleEdgeDependentCaches();
}

void MedLLVMEmitter::ensurePhiEdgeIndex() const {
  if (PhiEdgeIndexFor == CurMedFunc)
    return;
  PhiEdgeIndexFor = CurMedFunc;
  PhiOwnerBlocks.clear();
  StructuralEdges.clear();
  PhiEdgeClassCache.clear();
  if (!CurMedFunc)
    return;

  for (const MedBlock &Block : CurMedFunc->Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      PhiOwnerBlocks.emplace(&Phi, Block.Id);
    for (int Succ : Block.Succs)
      StructuralEdges.insert({Block.Id, Succ});
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs)
      StructuralEdges.insert({Block.Id, Edge.BlockId});
  }
}

MedLLVMEmitter::PhiEdgeFeasibility
MedLLVMEmitter::classifyPhiIncomingEdge(const PhiNode &Phi, int PredId) const {
  if (!CurMedFunc)
    return PhiEdgeFeasibility::Unknown;
  ensurePhiEdgeIndex();
  const auto CacheKey = std::make_pair(&Phi, PredId);
  const bool BuildingFeasibleEdges =
      FeasibleEdgesFor == CurMedFunc &&
      FeasibleEdgeState == FeasibleEdgeCacheState::Building;
  if (!BuildingFeasibleEdges)
    if (auto It = PhiEdgeClassCache.find(CacheKey);
        It != PhiEdgeClassCache.end())
      return It->second;

  ++AddressProvenanceWork.EdgeClassifications;
  auto Owner = PhiOwnerBlocks.find(&Phi);
  if (Owner == PhiOwnerBlocks.end()) {
    PhiEdgeClassCache.emplace(CacheKey, PhiEdgeFeasibility::Unknown);
    return PhiEdgeFeasibility::Unknown;
  }
  const int OwnerId = Owner->second;

  // A PHI argument whose predecessor cannot be tied to any CFG edge is
  // malformed or incomplete input, not proof that the arm is dead. Keep it
  // feasible so pointer recovery fails closed instead of silently accepting
  // whichever well-formed arm remains.
  if (!StructuralEdges.count({PredId, OwnerId})) {
    PhiEdgeClassCache.emplace(CacheKey, PhiEdgeFeasibility::Unknown);
    return PhiEdgeFeasibility::Unknown;
  }

  // A structurally valid edge queried during feasible-graph construction has
  // no terminal reachability class yet. Returning Unknown keeps every
  // provenance proof fail-closed; not memoizing it prevents a provisional
  // answer from surviving publication. The sticky marker also makes the
  // current feasible build retain all remaining control edges if a future
  // change accidentally recreates a semantic dependency cycle.
  if (BuildingFeasibleEdges) {
    FeasibleEdgeBuildSawReentrantQuery = true;
    return PhiEdgeFeasibility::Unknown;
  }

  ensureFeasibleEdgeCache();
  if (FeasibleEdgeState != FeasibleEdgeCacheState::Ready)
    return PhiEdgeFeasibility::Unknown;
  const PhiEdgeFeasibility Result = FeasibleEdges.count({PredId, OwnerId}) != 0
                                        ? PhiEdgeFeasibility::ProvenFeasible
                                        : PhiEdgeFeasibility::Infeasible;
  PhiEdgeClassCache.emplace(CacheKey, Result);
  return Result;
}

bool MedLLVMEmitter::phiIncomingEdgeFeasible(const PhiNode &Phi,
                                             int PredId) const {
  return classifyPhiIncomingEdge(Phi, PredId) != PhiEdgeFeasibility::Infeasible;
}

bool MedLLVMEmitter::constantIsStableAddressOffset(const MedVar &V) const {
  if (!V.isConst())
    return false;
  if (V.Provenance == ConstantAddressProvenance::Scalar)
    return true;
  if (isAddressProvenance(V.Provenance))
    return false;
  if (!Img)
    return true;

  // Most strides and masks are small unmapped immediates. Prove that case
  // from occurrence and loader inventories without entering whole-function
  // getVar use analysis: that analysis may ask whether the surrounding PHI is
  // recurrent and create a transient mutual proof cycle. A negative answer
  // from that cycle must not become the PHI's memoized final role.
  const uint64_t Value = V.ConstVal;
  const bool HasLowRelocationEvidence =
      Img->RelocDataAddrs.count(Value) || Img->RodataAnchorSeg.count(Value) ||
      Img->WritableRelocDataAddrs.count(Value) ||
      Img->CodeRefTargets.count(Value) || hasObjectDataProvenance(Value);
  if (Value < limits::kMinGlobalDataAddr && !HasLowRelocationEvidence &&
      !Img->getSegmentFor(Value))
    return true;

  return !getVarMayRelocateConstant(Value, V.Size) &&
         !hasObjectDataProvenance(Value);
}

bool MedLLVMEmitter::valueIsStableAddressOffset(const MedVar &V,
                                                const MedVar *Forbidden) const {
  if (!CurMedFunc) {
    ++AddressProvenanceWork.StableOffsetProofs;
    return valueIsStableAddressOffsetImpl(V, Forbidden);
  }
  if (StableOffsetCacheFor != CurMedFunc) {
    StableOffsetCacheFor = CurMedFunc;
    StableOffsetCache.clear();
  }
  const AddressProvenanceVarKey EmptyForbidden{};
  const auto Key = std::make_tuple(
      addressProvenanceVarKey(V), Forbidden != nullptr,
      Forbidden ? addressProvenanceVarKey(*Forbidden) : EmptyForbidden);
  if (auto It = StableOffsetCache.find(Key); It != StableOffsetCache.end())
    return It->second;

  // A frame-domain proof may need to validate the scalar side of a
  // loop-carried frame address.  That nested query can encounter the original
  // frame reload again through a different SSA path before its cache entry is
  // complete.  Treat such cross-query re-entry as unproved instead of starting
  // a second unbounded walk.  The outer proof still audits every ordinary
  // initializer/source and publishes only its completed result.
  using ActiveStableOffsetKey =
      std::tuple<const MedLLVMEmitter *, const MedFunc *, decltype(Key)>;
  static thread_local std::set<ActiveStableOffsetKey> ActiveProofs;
  const ActiveStableOffsetKey ActiveKey{this, CurMedFunc, Key};
  if (!ActiveProofs.insert(ActiveKey).second)
    return false;

  ++AddressProvenanceWork.StableOffsetProofs;
  const bool Result = valueIsStableAddressOffsetImpl(V, Forbidden);
  ActiveProofs.erase(ActiveKey);
  StableOffsetCache.emplace(Key, Result);
  return Result;
}

bool MedLLVMEmitter::valueIsStableAddressOffsetImpl(
    const MedVar &V, const MedVar *Forbidden) const {
  auto stableOffsetFailure = [](const char *, const MedVar &, int) {
    return false;
  };
  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };
  auto exactSameVar = [](const MedVar &A, const MedVar &B) {
    if (A.isConst() || B.isConst() || A.Kind != B.Kind ||
        A.TheArch != B.TheArch || A.RenameTag != B.RenameTag || A.Id != B.Id ||
        A.SSAVer != B.SSAVer || A.Size != B.Size)
      return false;
    if (A.Kind == MedVar::Reg || A.Kind == MedVar::Param)
      return A.RegOff == B.RegOff;
    if (A.Kind == MedVar::Stack)
      return A.StackOff == B.StackOff;
    return true;
  };
  using Key = std::tuple<int, int, int>;
  using FrameSlotKey = std::pair<std::pair<int, int>, int64_t>;
  using FrameRootKey = std::pair<int, int>;
  auto keyOf = [](const MedVar &V) {
    return Key{static_cast<int>(V.Kind), V.Id, V.SSAVer};
  };
  // Exact frame-slot recovery intentionally rejects runtime indexes.  Scalar
  // provenance needs a weaker, orthogonal identity: which entry SP/FP root an
  // address is derived from, irrespective of its dynamic lane.  This lets a
  // local scalar array be audited as one memory domain without weakening the
  // exact reaching-store proof used to recover spilled pointers.
  struct FrameRootProof {
    bool Valid = false;
    bool SawCycle = false;
    std::optional<FrameRootKey> Root;
  };
  int RemainingFrameRootNodes = 8192;
  std::function<FrameRootProof(const MedVar &, int, std::set<Key>)>
      frameRootProof = [&](const MedVar &Start, int Depth,
                           std::set<Key> Seen) -> FrameRootProof {
    if (Start.isConst() || RemainingFrameRootNodes-- <= 0)
      return {};
    if (auto Exact = canonicalFrameSlotKey(Start))
      return {true, false, Exact->first};
    const MedOp *Def = lookupDef(Start);
    if (!Seen.insert(keyOf(Start)).second)
      return {true, true, std::nullopt};

    auto mergeProofs = [](const FrameRootProof &A,
                          const FrameRootProof &B) -> FrameRootProof {
      if (!A.Valid || !B.Valid || (A.Root && B.Root && *A.Root != *B.Root))
        return {};
      return {true, A.SawCycle || B.SawCycle, A.Root ? A.Root : B.Root};
    };

    if (const PhiNode *Phi = lookupPhi(Start)) {
      FrameRootProof Result;
      bool SawFeasible = false;
      for (const auto &[Pred, Arg] : Phi->Args) {
        PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, Pred);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible)
          return {};
        FrameRootProof Arm = frameRootProof(Arg, Depth + 1, Seen);
        if (!Arm.Valid)
          return {};
        Result = SawFeasible ? mergeProofs(Result, Arm) : Arm;
        if (!Result.Valid)
          return {};
        SawFeasible = true;
      }
      return SawFeasible ? Result : FrameRootProof{};
    }

    if (!Def || Def->NumInputs < 1)
      return {};
    if (auto Forwarded = pointerPreservingInput(*Def))
      return frameRootProof(*Forwarded, Depth + 1, Seen);
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
        return {};
      FrameRootProof Result;
      bool SawSource = false;
      for (const MedVar &Source : Sources) {
        FrameRootProof SourceProof = frameRootProof(Source, Depth + 1, Seen);
        if (!SourceProof.Valid)
          return {};
        Result = SawSource ? mergeProofs(Result, SourceProof) : SourceProof;
        if (!Result.Valid)
          return {};
        SawSource = true;
      }
      return SawSource ? Result : FrameRootProof{};
    }

    auto mergeFrameOperand = [&](const MedVar &A,
                                 const MedVar *B) -> FrameRootProof {
      FrameRootProof Left = frameRootProof(A, Depth + 1, Seen);
      if (!Left.Valid)
        return {};
      if (!B) {
        return Left;
      }
      FrameRootProof Right = frameRootProof(*B, Depth + 1, Seen);
      if (!Right.Valid) {
        // A recurrence is pointer-preserving only when its non-pointer side
        // remains numeric in rebuilt IR.  This is the same invariant used by
        // the general pointer recurrence proof; applying it at the operation
        // that carries the cycle avoids blessing `p + @other_object`.
        if (B && Left.SawCycle && !valueIsStableAddressOffset(*B, &Start))
          return {};
        return Left;
      }
      // Combining two independently frame-derived values is not an affine
      // pointer step.  Preserve the legacy same-root classification for
      // acyclic address formation, but never let it close a recurrence.
      if (Left.SawCycle || Right.SawCycle)
        return {};
      return mergeProofs(Left, Right);
    };
    switch (Def->Opcode) {
    case NdOp::INT_ADD:
      if (Def->NumInputs < 2)
        return {};
      if (FrameRootProof Left =
              mergeFrameOperand(Def->Inputs[0], &Def->Inputs[1]);
          Left.Valid)
        return Left;
      return mergeFrameOperand(Def->Inputs[1], &Def->Inputs[0]);
    case NdOp::INT_SUB:
      if (Def->NumInputs < 2)
        return {};
      return mergeFrameOperand(Def->Inputs[0], &Def->Inputs[1]);
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR: {
      if (Def->NumInputs < 2)
        return {};
      FrameRootProof Carrier =
          mergeFrameOperand(Def->Inputs[0], &Def->Inputs[1]);
      // Bitwise transforms can conservatively retain an already-established
      // frame domain (alignment/tag arithmetic), but are not valid recurrence
      // edges: repeated XOR/OR/AND is not pointer-plus-scalar induction.
      return Carrier.Valid && !Carrier.SawCycle ? Carrier : FrameRootProof{};
    }
    case NdOp::SELECT: {
      if (!selectPreservesPointerValues(*Def))
        return {};
      FrameRootProof TrueRoot = frameRootProof(Def->Inputs[1], Depth + 1, Seen);
      FrameRootProof FalseRoot =
          frameRootProof(Def->Inputs[2], Depth + 1, Seen);
      return mergeProofs(TrueRoot, FalseRoot);
    }
    default:
      return {};
    }
  };
  auto frameRoot = [&](const MedVar &Start, int Depth,
                       std::set<Key> Seen) -> std::optional<FrameRootKey> {
    FrameRootProof Proof = frameRootProof(Start, Depth, std::move(Seen));
    return Proof.Valid ? Proof.Root : std::nullopt;
  };
  std::set<FrameRootKey> ActiveFrameDomains;
  // While one complete frame domain is being audited, a stored scalar update
  // may depend on a LOAD from that same domain.  Detect only that value-flow
  // recurrence: it is a neutral backedge once every potentially aliasing
  // write is in the outer audit, but it is not an initializer by itself.
  struct FrameDomainReachSummary {
    bool ReachesBackedge = false;
    bool HasIndependentAlternative = false;
    bool Unknown = false;
  };
  auto mergeFrameDomainAlternatives =
      [](const std::vector<FrameDomainReachSummary> &Alternatives) {
        FrameDomainReachSummary Result;
        for (const FrameDomainReachSummary &Alternative : Alternatives) {
          Result.ReachesBackedge |= Alternative.ReachesBackedge;
          Result.HasIndependentAlternative |=
              Alternative.HasIndependentAlternative;
          Result.Unknown |= Alternative.Unknown;
        }
        return Result;
      };
  auto mergeFrameDomainDependencies =
      [](const std::vector<FrameDomainReachSummary> &Dependencies) {
        FrameDomainReachSummary Result;
        if (Dependencies.empty()) {
          Result.Unknown = true;
          return Result;
        }
        bool AllHaveIndependentAlternative = true;
        unsigned BackedgeDependencies = 0;
        for (const FrameDomainReachSummary &Dependency : Dependencies) {
          Result.ReachesBackedge |= Dependency.ReachesBackedge;
          Result.Unknown |= Dependency.Unknown;
          AllHaveIndependentAlternative &= Dependency.HasIndependentAlternative;
          BackedgeDependencies += Dependency.ReachesBackedge ? 1u : 0u;
        }
        // A scalar operation preserves an independently initialized value only
        // when at most one operand conditionally carries the recurrence and
        // every other operand is independently available. In particular,
        // `load + 1` is not an initializer, while
        // `select(load, initializer) + 1` retains the SELECT alternative.
        Result.HasIndependentAlternative = !Result.Unknown &&
                                           AllHaveIndependentAlternative &&
                                           BackedgeDependencies <= 1;
        return Result;
      };
  int RemainingFrameDomainReachNodes = 8192;
  std::function<FrameDomainReachSummary(const MedVar &, const FrameRootKey &,
                                        int, std::set<Key>)>
      summarizeFrameDomainReach =
          [&](const MedVar &Start, const FrameRootKey &TargetRoot, int Depth,
              std::set<Key> Seen) -> FrameDomainReachSummary {
    if (Depth > 128 || RemainingFrameDomainReachNodes-- <= 0)
      return {.Unknown = true};
    if (Start.isConst())
      return {.HasIndependentAlternative = true};
    if (!Seen.insert(keyOf(Start)).second)
      return {};
    if (const PhiNode *Phi = lookupPhi(Start)) {
      bool SawFeasible = false;
      std::vector<FrameDomainReachSummary> Alternatives;
      for (const auto &[Pred, Arg] : Phi->Args) {
        const PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, Pred);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible)
          return {.Unknown = true};
        SawFeasible = true;
        Alternatives.push_back(
            summarizeFrameDomainReach(Arg, TargetRoot, Depth + 1, Seen));
      }
      return SawFeasible ? mergeFrameDomainAlternatives(Alternatives)
                         : FrameDomainReachSummary{.Unknown = true};
    }
    const MedOp *Def = lookupDef(Start);
    if (!Def)
      return {.HasIndependentAlternative = true};
    if (Def->Opcode == NdOp::LOAD && Def->NumInputs >= 1) {
      const auto Root = frameRoot(Def->Inputs[0], 0, {});
      if (Root)
        return *Root == TargetRoot
                   ? FrameDomainReachSummary{.ReachesBackedge = true}
                   : FrameDomainReachSummary{.HasIndependentAlternative = true};
      return varMayBeFrameAddress(Def->Inputs[0])
                 ? FrameDomainReachSummary{.Unknown = true}
                 : FrameDomainReachSummary{.HasIndependentAlternative = true};
    }
    if (auto Forwarded = pointerPreservingInput(*Def))
      return summarizeFrameDomainReach(*Forwarded, TargetRoot, Depth + 1, Seen);
    if (Def->Opcode == NdOp::SELECT) {
      if (Def->NumInputs < 3)
        return {.Unknown = true};
      return mergeFrameDomainAlternatives(
          {summarizeFrameDomainReach(Def->Inputs[1], TargetRoot, Depth + 1,
                                     Seen),
           summarizeFrameDomainReach(Def->Inputs[2], TargetRoot, Depth + 1,
                                     Seen)});
    }
    if (Def->Opcode == NdOp::INT_OR) {
      MedVar Cond, ArmT, ArmF;
      if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF))
        return mergeFrameDomainAlternatives(
            {summarizeFrameDomainReach(ArmT, TargetRoot, Depth + 1, Seen),
             summarizeFrameDomainReach(ArmF, TargetRoot, Depth + 1, Seen)});
    }
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_DIV:
    case NdOp::INT_SDIV:
    case NdOp::INT_REM:
    case NdOp::INT_SREM:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_NEG2:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT: {
      std::vector<FrameDomainReachSummary> Dependencies;
      Dependencies.reserve(Def->NumInputs);
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        Dependencies.push_back(summarizeFrameDomainReach(
            Def->Inputs[I], TargetRoot, Depth + 1, Seen));
      return mergeFrameDomainDependencies(Dependencies);
    }
    default:
      break;
    }
    return {.Unknown = true};
  };
  // A relocation-free immutable scalar table can feed the index used by its
  // next lookup (for example, a finite-state transition table in a loop).
  // Record such a LOAD only after every reachable lane slot has been audited
  // as scalar.  Re-entry is then a value recurrence, not a new pointer source;
  // it is accepted only while a PHI or frame recurrence with an independently
  // proven initializer is already active.
  std::set<Key> ActiveImmutableScalarLoads;
  // The non-pointer side of an induction step must remain a numeric offset in
  // rebuilt IR. Reject a second occurrence of the recurrence and any constant
  // or forwarded/arithmetic DAG that carries independently relocatable address
  // provenance. Runtime parameters and opaque scalar results remain valid
  // offsets because getVar does not rewrite them into another global pointer.
  // Keep one budget for the complete proof, rather than one budget per copied
  // DFS branch.  Lowered SELECT/PHI diamonds often share both value arms; a
  // path-local depth limit alone still permits exponential re-traversal.  On
  // exhaustion, false is conservative: callers retain the mixed/raw model
  // instead of granting a scalar-offset proof.
  // The proof is deliberately bounded, but real flag-expanded loop indices
  // can exceed 4K visited occurrences while remaining a finite scalar DAG.
  // Match the complete table-base audit's bound so the two halves of the same
  // provenance decision cannot disagree solely because one has less budget.
  int RemainingProofNodes = 8192;
  // Identify only value-producing recurrence edges.  This is deliberately
  // separate from generic use/def reachability: a PHI used as a SELECT
  // condition or shift/mask control does not anchor the selected scalar.
  // Exact frame reload sources are followed so an SSA PHI whose backedge is
  // carried through a spill slot remains one scalar SCC.
  std::function<bool(const MedVar &, const Key &, int, std::set<Key>)>
      scalarValueReaches = [&](const MedVar &Start, const Key &Target,
                               int Depth, std::set<Key> Seen) -> bool {
    if (Depth > 128 || Start.isConst())
      return false;
    const Key StartKey = keyOf(Start);
    if (StartKey == Target)
      return true;
    if (!Seen.insert(StartKey).second)
      return false;
    if (const PhiNode *Phi = lookupPhi(Start)) {
      for (const auto &[Pred, Arg] : Phi->Args)
        if (classifyPhiIncomingEdge(*Phi, Pred) ==
                PhiEdgeFeasibility::ProvenFeasible &&
            scalarValueReaches(Arg, Target, Depth + 1, Seen))
          return true;
      return false;
    }
    const MedOp *Def = lookupDef(Start);
    if (!Def || Def->NumInputs < 1)
      return false;
    if (auto Forwarded = pointerPreservingInput(*Def))
      return scalarValueReaches(*Forwarded, Target, Depth + 1, std::move(Seen));
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
        return false;
      for (const MedVar &Source : Sources)
        if (scalarValueReaches(Source, Target, Depth + 1, Seen))
          return true;
      return false;
    }
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3)
      return scalarValueReaches(Def->Inputs[1], Target, Depth + 1, Seen) ||
             scalarValueReaches(Def->Inputs[2], Target, Depth + 1,
                                std::move(Seen));
    if (Def->Opcode == NdOp::INT_OR) {
      MedVar Cond, ArmT, ArmF;
      if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF))
        return scalarValueReaches(ArmT, Target, Depth + 1, Seen) ||
               scalarValueReaches(ArmF, Target, Depth + 1, std::move(Seen));
    }
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_DIV:
    case NdOp::INT_SDIV:
    case NdOp::INT_REM:
    case NdOp::INT_SREM:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_NEG2:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT:
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (scalarValueReaches(Def->Inputs[I], Target, Depth + 1, Seen))
          return true;
      return false;
    default:
      return false;
    }
  };
  std::function<bool(const MedVar &, const std::set<Key> &)>
      reachesAnchoredPhi =
          [&](const MedVar &Start, const std::set<Key> &AnchoredPhis) {
            for (const Key &Anchor : AnchoredPhis)
              if (scalarValueReaches(Start, Anchor, 0, {}))
                return true;
            return false;
          };

  // Some lowered loop states are not value-carried back to their PHI.  A
  // later iteration can instead feed the state through a LOAD address or a
  // call argument before producing the incoming value.  scalarValueReaches()
  // deliberately does not follow those non-value operands, so use the
  // already-proven feasible CFG as the second, orthogonal recurrence signal:
  // an incoming edge is loop-carried exactly when the PHI owner can reach its
  // predecessor again.  This marks only edges inside a feasible control SCC;
  // every edge entering that SCC remains an initializer and is audited before
  // any recurrent arm receives PHI authority.
  auto phiIncomingClosesFeasibleControlCycle = [&](const PhiNode &Phi,
                                                   int PredId) {
    ensurePhiEdgeIndex();
    const auto Owner = PhiOwnerBlocks.find(&Phi);
    if (Owner == PhiOwnerBlocks.end())
      return false;
    const int OwnerId = Owner->second;

    ensureFeasibleEdgeCache();
    if (FeasibleEdgeState != FeasibleEdgeCacheState::Ready ||
        !FeasibleEdges.count({PredId, OwnerId}))
      return false;
    if (!FeasibleControlComponentsReady) {
      std::map<int, std::vector<int>> Successors;
      std::map<int, std::vector<int>> Predecessors;
      for (int BlockId : FeasibleBlocks) {
        Successors[BlockId];
        Predecessors[BlockId];
      }
      for (const auto &[From, To] : FeasibleEdges) {
        Successors[From].push_back(To);
        Predecessors[To].push_back(From);
      }

      std::set<int> SeenBlocks;
      std::vector<int> FinishOrder;
      for (int Root : FeasibleBlocks) {
        if (SeenBlocks.count(Root))
          continue;
        std::vector<std::pair<int, bool>> Work{{Root, false}};
        while (!Work.empty()) {
          const auto [BlockId, Expanded] = Work.back();
          Work.pop_back();
          if (Expanded) {
            FinishOrder.push_back(BlockId);
            continue;
          }
          if (!SeenBlocks.insert(BlockId).second)
            continue;
          Work.push_back({BlockId, true});
          for (auto It = Successors[BlockId].rbegin();
               It != Successors[BlockId].rend(); ++It)
            if (!SeenBlocks.count(*It))
              Work.push_back({*It, false});
        }
      }

      int Component = 0;
      for (auto It = FinishOrder.rbegin(); It != FinishOrder.rend(); ++It) {
        if (FeasibleControlComponents.count(*It))
          continue;
        std::vector<int> Work{*It};
        FeasibleControlComponents.emplace(*It, Component);
        while (!Work.empty()) {
          const int BlockId = Work.back();
          Work.pop_back();
          for (int Pred : Predecessors[BlockId])
            if (FeasibleControlComponents.emplace(Pred, Component).second)
              Work.push_back(Pred);
        }
        ++Component;
      }
      FeasibleControlComponentsReady = true;
    }

    const auto OwnerComponent = FeasibleControlComponents.find(OwnerId);
    const auto PredComponent = FeasibleControlComponents.find(PredId);
    return OwnerComponent != FeasibleControlComponents.end() &&
           PredComponent != FeasibleControlComponents.end() &&
           OwnerComponent->second == PredComponent->second;
  };

  // A lowered loop can contain a whole PHI SCC rather than one canonical
  // header PHI with a direct initializer.  In particular, an unrolled switch
  // feeds its case-merge PHIs back into a later loop header while the actual
  // entry value reaches a deeper dispatch PHI.  Classifying each PHI in
  // isolation makes every immediate arm look recurrent and loses that entry
  // proof.  Find an initializer on the value-carrying portion of the complete
  // recurrence component instead.  Arithmetic side inputs are deliberately
  // not initializers: `p = p + 1` remains a source-free cycle, while a PHI,
  // SELECT, or exact frame reload with an external value arm establishes a
  // real entry.  The ordinary prove() walk below still audits that arm and all
  // other operands for address provenance.
  auto scalarRecurrenceHasInitializer = [&](const MedVar &Root) {
    if (Root.isConst())
      return false;
    const Key RootKey = keyOf(Root);
    std::vector<MedVar> Work{Root};
    std::set<Key> Seen;
    int Budget = 8192;
    auto reachesRoot = [&](const MedVar &Value) {
      return scalarValueReaches(Value, RootKey, 0, {});
    };
    auto inspectValueArms = [&](const std::vector<MedVar> &Arms,
                                bool &FoundInitializer) {
      bool SawCarrier = false;
      for (const MedVar &Arm : Arms) {
        if (reachesRoot(Arm)) {
          Work.push_back(Arm);
          SawCarrier = true;
        } else {
          FoundInitializer = true;
        }
      }
      return SawCarrier;
    };

    while (!Work.empty() && Budget-- > 0) {
      MedVar Current = Work.back();
      Work.pop_back();
      if (Current.isConst() || !Seen.insert(keyOf(Current)).second)
        continue;

      if (const PhiNode *Phi = lookupPhi(Current)) {
        bool SawFeasible = false;
        for (const auto &[Pred, Arg] : Phi->Args) {
          PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, Pred);
          if (Edge == PhiEdgeFeasibility::Infeasible)
            continue;
          if (Edge != PhiEdgeFeasibility::ProvenFeasible)
            return false;
          SawFeasible = true;
          if (!reachesRoot(Arg))
            return true;
          Work.push_back(Arg);
        }
        if (!SawFeasible)
          return false;
        continue;
      }

      const MedOp *Def = lookupDef(Current);
      if (!Def || Def->NumInputs < 1)
        continue;
      if (auto Forwarded = pointerPreservingInput(*Def)) {
        if (reachesRoot(*Forwarded))
          Work.push_back(*Forwarded);
        continue;
      }
      if (Def->Opcode == NdOp::LOAD) {
        std::vector<MedVar> Sources;
        if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
          return false;
        bool FoundInitializer = false;
        (void)inspectValueArms(Sources, FoundInitializer);
        if (FoundInitializer)
          return true;
        continue;
      }
      if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
        bool FoundInitializer = false;
        (void)inspectValueArms({Def->Inputs[1], Def->Inputs[2]},
                               FoundInitializer);
        if (FoundInitializer)
          return true;
        continue;
      }
      if (Def->Opcode == NdOp::INT_OR) {
        MedVar Cond, ArmT, ArmF;
        if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF)) {
          bool FoundInitializer = false;
          (void)inspectValueArms({ArmT, ArmF}, FoundInitializer);
          if (FoundInitializer)
            return true;
          continue;
        }
      }

      switch (Def->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::SUBBYTES:
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_MULT:
      case NdOp::INT_DIV:
      case NdOp::INT_SDIV:
      case NdOp::INT_REM:
      case NdOp::INT_SREM:
      case NdOp::INT_LEFT:
      case NdOp::INT_RIGHT:
      case NdOp::INT_ASHR:
      case NdOp::INT_AND:
      case NdOp::INT_OR:
      case NdOp::INT_XOR:
      case NdOp::INT_NEG2:
      case NdOp::INT_NEGATE:
      case NdOp::INT_NOT:
        for (uint8_t I = 0; I < Def->NumInputs; ++I)
          if (reachesRoot(Def->Inputs[I]))
            Work.push_back(Def->Inputs[I]);
        break;
      default:
        break;
      }
    }
    return false;
  };

  std::function<bool(const MedVar &, int, std::set<Key>, std::set<FrameSlotKey>,
                     std::set<Key>)>
      prove = [&](const MedVar &Start, int Depth, std::set<Key> Seen,
                  std::set<FrameSlotKey> ActiveFrameSlots,
                  std::set<Key> AnchoredPhis) -> bool {
    if (Forbidden && sameVar(Start, *Forbidden))
      return stableOffsetFailure("forbidden", Start, Depth);
    // Recognize a scalar recurrence before applying the acyclic-depth budget:
    // a long lowered arithmetic chain can return to its PHI only after dozens
    // nodes.  Every non-cyclic initialization arm is still audited below.
    if (!Start.isConst() && Seen.count(keyOf(Start))) {
      // A real SSA recurrence returns to a PHI whose non-cyclic initializer
      // arms were audited before this edge.  Keep the legacy exact live-in
      // self-COPY as a harmless identity, but reject an arbitrary multi-node
      // forwarding cycle: it has no dominating scalar initializer and could
      // otherwise hide unproved address provenance.
      if (reachesAnchoredPhi(Start, AnchoredPhis))
        return true;
      for (const FrameRootKey &Root : ActiveFrameDomains) {
        const FrameDomainReachSummary Summary =
            summarizeFrameDomainReach(Start, Root, 0, {});
        if (!Summary.Unknown && Summary.ReachesBackedge)
          return true;
      }
      const MedOp *CycleDef = lookupDef(Start);
      if (CycleDef && CycleDef->Opcode == NdOp::LOAD &&
          CycleDef->NumInputs >= 1) {
        if (auto Slot = canonicalFrameSlotKey(CycleDef->Inputs[0]);
            Slot && ActiveFrameSlots.count(*Slot))
          return true;
        if (auto Root = frameRoot(CycleDef->Inputs[0], 0, {});
            Root && ActiveFrameDomains.count(*Root))
          return true;
        if (ActiveImmutableScalarLoads.count(keyOf(Start)) &&
            (!AnchoredPhis.empty() || !ActiveFrameSlots.empty() ||
             !ActiveFrameDomains.empty()))
          return true;
      }
      const bool ExactSelfCopy = CycleDef && CycleDef->Opcode == NdOp::COPY &&
                                 CycleDef->NumInputs == 1 &&
                                 exactSameVar(CycleDef->Output, Start) &&
                                 exactSameVar(CycleDef->Inputs[0], Start);
      return ExactSelfCopy
                 ? true
                 : stableOffsetFailure("unanchored-cycle", Start, Depth);
    }
    if (Depth > 128)
      return stableOffsetFailure("depth", Start, Depth);
    if (RemainingProofNodes-- <= 0)
      return stableOffsetFailure("budget", Start, Depth);
    auto constantIsMappedAddress = [&](uint64_t Value, uint16_t Size) {
      if (!Img || Value == 0)
        return false;
      if (getVarMayRelocateConstant(Value, Size))
        return true;
      // A low object-file text VA can numerically equal an ordinary induction
      // stride (for example AArch64 `p += 4`).  getVar keeps such an immediate
      // numeric, so segment membership alone is not pointer provenance.  The
      // same applies to ELF-header bytes inside a readable low PT_LOAD.  Only
      // exact object-data provenance is an independent base when getVar keeps
      // the low address raw.
      return hasObjectDataProvenance(Value);
    };
    if (Start.isConst() &&
        Start.Provenance == ConstantAddressProvenance::Scalar)
      return true;
    if (Start.isConst() && isAddressProvenance(Start.Provenance)) {
      if (Start.Provenance != ConstantAddressProvenance::Address) {
        return stableOffsetFailure("exact-address", Start, Depth);
      }
      // A role-neutral architectural-PC seed (i386 call/pop) is numeric at a
      // scalar-offset use when neither data nor code arbitration would rewrite
      // this exact occurrence.  This keeps `pc + GOTOFF/SECTDIFF + index`
      // eligible for the pointer-table owner while still rejecting explicit
      // DataAddress/CodeAddress leaves and any Address occurrence that will be
      // materialized as a global, function, or lifted block.
      uint64_t MaterializedDataVA = 0;
      return !resolveMaterializableDataAddress(Start, MaterializedDataVA) &&
             !codeIdentityOccurrenceMayRelocate(
                 Start, /*IncludeLayoutCodeOwners=*/false);
    }
    if (Start.isConst())
      return !constantIsMappedAddress(Start.ConstVal, Start.Size)
                 ? true
                 : stableOffsetFailure("mapped-constant", Start, Depth);
    Seen.insert(keyOf(Start));

    if (const PhiNode *Nested = lookupPhi(Start)) {
      bool SawPotential = false;
      struct PhiArm {
        int Pred = -1;
        MedVar Arg;
        bool Recurrent = false;
      };
      std::vector<PhiArm> Arms;
      for (const auto &[NestedPred, NestedArg] : Nested->Args) {
        PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Nested, NestedPred);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible)
          return stableOffsetFailure("unknown-phi-edge", Start, Depth);
        SawPotential = true;
        Arms.push_back(
            {NestedPred, NestedArg,
             scalarValueReaches(NestedArg, keyOf(Start), 0, {}) ||
                 phiIncomingClosesFeasibleControlCycle(*Nested, NestedPred)});
      }
      if (!SawPotential)
        return stableOffsetFailure("no-feasible-phi-arm", Start, Depth);

      // Establish the PHI's authority only from its non-recurrent incoming
      // values.  Then audit every recurrence arm with that authority.  This
      // admits PHI -> spill -> arithmetic -> PHI scalar loops while rejecting
      // a self-contained or unrelated cycle with no numeric initializer.
      bool SawInitializer = false;
      for (const PhiArm &Arm : Arms) {
        if (Arm.Recurrent)
          continue;
        const bool ArmResult =
            prove(Arm.Arg, Depth + 1, Seen, ActiveFrameSlots, AnchoredPhis);
        if (!ArmResult)
          return stableOffsetFailure("unstable-phi-initializer", Start, Depth);
        SawInitializer = true;
      }
      if (!SawInitializer && !scalarRecurrenceHasInitializer(Start))
        return stableOffsetFailure("unanchored-phi-component", Start, Depth);
      AnchoredPhis.insert(keyOf(Start));
      for (const PhiArm &Arm : Arms) {
        if (!Arm.Recurrent)
          continue;
        const bool ArmResult =
            prove(Arm.Arg, Depth + 1, Seen, ActiveFrameSlots, AnchoredPhis);
        if (!ArmResult)
          return stableOffsetFailure("unstable-phi-recurrence", Start, Depth);
      }
      return true;
    }

    const MedOp *Def = lookupDef(Start);
    if (!Def)
      return true;
    bool SawLoad = false;
    bool SawArithmetic = false;
    if (auto Value =
            traceTableBaseConst(Start, 0, &SawLoad, nullptr, &SawArithmetic);
        Value && constantIsMappedAddress(*Value, Start.Size)) {
      // Numeric coincidence is not address provenance.  traceTableBaseConst
      // intentionally returns only the folded bits, so consult the structural
      // occurrence summary before rejecting a low-VA scalar expression.  A
      // literal-pool LOAD, an untagged construction, or any explicit/mixed
      // address source remains fail-closed; only an all-path Scalar summary
      // can discharge the collision.
      const ConstantProvenanceSummary Summary =
          summarizeConstantProvenance(Start);
      if (SawLoad ||
          Summary.Model != ConstantProvenanceSummary::ValueModel::Scalar)
        return stableOffsetFailure("folded-mapped-address", Start, Depth);
    }
    if (auto Forwarded = pointerPreservingInput(*Def))
      return prove(*Forwarded, Depth + 1, Seen, ActiveFrameSlots, AnchoredPhis);
    if (Def->Opcode == NdOp::SELECT) {
      // Width changes do not by themselves make a scalar SELECT unstable, but
      // every chosen value must remain numeric in rebuilt IR. In particular a
      // narrowing SELECT whose arm contains a relocated table address is still
      // rejected when that arm is audited here; ordinary lowered flag/index
      // SELECTs remain valid scalar offset computations.
      if (Def->NumInputs < 3)
        return stableOffsetFailure("short-select", Start, Depth);
      return prove(Def->Inputs[1], Depth + 1, Seen, ActiveFrameSlots,
                   AnchoredPhis) &&
             prove(Def->Inputs[2], Depth + 1, Seen, ActiveFrameSlots,
                   AnchoredPhis);
    }
    if (Def->Opcode == NdOp::INT_OR) {
      MedVar Cond, ArmT, ArmF;
      if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF))
        return prove(ArmT, Depth + 1, Seen, ActiveFrameSlots, AnchoredPhis) &&
               prove(ArmF, Depth + 1, Seen, ActiveFrameSlots, AnchoredPhis);
    }

    bool CarriesArithmeticValue = false;
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_DIV:
    case NdOp::INT_SDIV:
    case NdOp::INT_REM:
    case NdOp::INT_SREM:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_NEG2:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NOT:
      CarriesArithmeticValue = true;
      break;
    default:
      break;
    }
    if (!CarriesArithmeticValue) {
      if (Def->Opcode != NdOp::LOAD)
        return true;

      // A frame reload transports the exact stored bit pattern even when it is
      // narrower than the target pointer and later widened.  Audit its
      // all-path reaching definitions first: a truncated ptrtoint(@table)
      // remains an independently relocatable component, while an ordinary
      // spilled scalar remains a valid offset.
      const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
      const auto ExactFrameSlot = canonicalFrameSlotKey(Def->Inputs[0]);
      const auto ReloadFrameRoot = frameRoot(Def->Inputs[0], 0, {});
      if (ReloadFrameRoot && ActiveFrameDomains.count(*ReloadFrameRoot))
        return true;
      if (ExactFrameSlot) {
        // A loop-carried scalar can be represented through memory rather than
        // an SSA PHI: load(slot), update, store(slot), backedge. Once the exact
        // slot's complete reaching definitions are being audited, encountering
        // that slot again is the recurrence edge, not an unproved new source.
        // The outer source set still audits every non-cyclic initialization,
        // so a pointer initializer or an uninitialized path remains rejected.
        if (ActiveFrameSlots.count(*ExactFrameSlot))
          return true;
        std::vector<MedVar> Sources;
        const bool CompleteSources = collectFrameReloadSources(*Def, Sources);
        if (CompleteSources && !Sources.empty()) {
          ActiveFrameSlots.insert(*ExactFrameSlot);
          bool SawAnchoredSource = false;
          for (const MedVar &Source : Sources) {
            // A memory-carried recurrence returns the value being computed to
            // the same exact slot, so its later-iteration reaching source can
            // already be on this proof path.  That edge contributes no new
            // provenance.  Skip only that exact path-local backedge, and still
            // require a separately proven initializer/source below; a slot
            // whose reaching definitions are only cyclic remains unproved.
            if (!Source.isConst() && Seen.count(keyOf(Source))) {
              if (reachesAnchoredPhi(Source, AnchoredPhis))
                SawAnchoredSource = true;
              continue;
            }
            if (!prove(Source, Depth + 1, Seen, ActiveFrameSlots, AnchoredPhis))
              return false;
            SawAnchoredSource = true;
          }
          return SawAnchoredSource;
        }
        // A pointer-width exact slot has a stronger contract than frame-domain
        // purity: its reaching-store fixed point must cover the LOAD on every
        // path. A narrow reload cannot carry a complete native pointer, so an
        // incomplete exact-slot proof may continue into the same-root audit
        // below. That audit still checks every possibly aliasing write and
        // rejects address provenance, escapes, atomics, and unknown owners.
        if (PointerSize == 0 || Def->Output.Size == 0 ||
            Def->Output.Size >= PointerSize)
          return stableOffsetFailure("incomplete-frame-reload", Start, Depth);
      }

      // Runtime-indexed local arrays cannot supply an exact SlotKey, and a
      // constant-lane reload can be reached by a dynamic STORE.  Do not relax
      // collectFrameReloadSources: pointer recovery needs its exact all-path
      // definition set.  For the scalar-offset question, instead audit the
      // complete same-root frame memory domain. Every possibly aliasing local
      // write must itself remain numeric, and no same-root address may escape
      // to a call or an untracked memory owner. This proves domain purity, not
      // a particular reaching value.
      if (ReloadFrameRoot) {
        const FrameRootKey Root = *ReloadFrameRoot;
        if (ActiveFrameDomains.count(Root))
          return true;

        auto sameOrUnknownFrameRoot = [&](const MedVar &Address) {
          if (!varMayBeFrameAddress(Address))
            return false;
          auto Candidate = frameRoot(Address, 0, {});
          return !Candidate || *Candidate == Root;
        };

        auto rangesDisjoint = [](int64_t A, uint64_t ASize, int64_t B,
                                 uint64_t BSize) {
          if (ASize == 0 || BSize == 0)
            return true;
          auto endsBefore = [](int64_t Start, uint64_t Size, int64_t Other) {
            return Start < Other && static_cast<uint64_t>(Other) -
                                            static_cast<uint64_t>(Start) >=
                                        Size;
          };
          return endsBefore(A, ASize, B) || endsBefore(B, BSize, A);
        };
        auto boundedCallFrameArgIsSafe = [&](const MedCallInfo &Call,
                                             size_t ArgIndex) {
          llvm::StringRef Name = stripLeadingUnderscores(Call.TargetName);
          int WriteArg = -1;
          int LengthArg = -1;
          if (Name == "memset" || Name == "memset_chk") {
            WriteArg = 0;
            LengthArg = 2;
          } else if (Name == "bzero" || Name == "explicit_bzero") {
            WriteArg = 0;
            LengthArg = 1;
          } else if (Name == "memcpy" || Name == "memmove" ||
                     Name == "mempcpy" || Name == "memcpy_chk" ||
                     Name == "memmove_chk" || Name == "mempcpy_chk" ||
                     Name == "strncpy" || Name == "stpncpy") {
            WriteArg = 0;
            LengthArg = 2;
          } else {
            return false;
          }

          // All other pointer-shaped operands of these exact signatures are
          // read-only sources (or scalar fill/size operands), so they cannot
          // mutate the frame domain being audited.
          if (ArgIndex != static_cast<size_t>(WriteArg))
            return true;
          if (!ExactFrameSlot || WriteArg < 0 || LengthArg < 0 ||
              static_cast<size_t>(std::max(WriteArg, LengthArg)) >=
                  Call.Args.size())
            return false;
          auto Destination =
              canonicalFrameSlotKey(Call.Args[static_cast<size_t>(WriteArg)]);
          auto Length =
              traceControlConst(Call.Args[static_cast<size_t>(LengthArg)]);
          return Destination && Destination->first == Root && Length &&
                 rangesDisjoint(Destination->second, *Length,
                                ExactFrameSlot->second, Def->Output.Size);
        };

        bool DomainInvalid = false;
        for (const auto &Call : CurMedFunc->CallInfos)
          for (size_t ArgIndex = 0; ArgIndex < Call.Args.size(); ++ArgIndex) {
            const MedVar &Arg = Call.Args[ArgIndex];
            if (sameOrUnknownFrameRoot(Arg)) {
              if (!boundedCallFrameArgIsSafe(Call, ArgIndex))
                DomainInvalid = true;
            }
          }

        std::vector<MedVar> DomainSources;
        for (const MedBlock &Block : CurMedFunc->Blocks) {
          for (const MedOp &Write : Block.Ops) {
            if (Write.Opcode == NdOp::STORE && Write.NumInputs >= 2) {
              // Storing the frame address outside its own root lets an alias
              // re-enter through a call or an otherwise untracked load.
              if (sameOrUnknownFrameRoot(Write.Inputs[1])) {
                auto DestinationRoot = frameRoot(Write.Inputs[0], 0, {});
                if (!DestinationRoot || *DestinationRoot != Root)
                  DomainInvalid = true;
              }
              if (!sameOrUnknownFrameRoot(Write.Inputs[0]))
                continue;
              if (frameAccessesProvenDisjoint(Write.Inputs[0],
                                              Write.Inputs[1].Size,
                                              Def->Inputs[0], Def->Output.Size))
                continue;
              if (Write.Inputs[1].Size == 0)
                DomainInvalid = true;
              else
                DomainSources.push_back(Write.Inputs[1]);
              continue;
            }

            const bool IsAtomicWrite = Write.Opcode == NdOp::ATOMIC_XCHG ||
                                       Write.Opcode == NdOp::ATOMIC_ADD ||
                                       Write.Opcode == NdOp::ATOMIC_CMPXCHG;
            if (IsAtomicWrite && Write.NumInputs >= 1 &&
                sameOrUnknownFrameRoot(Write.Inputs[0])) {
              if (frameAccessesProvenDisjoint(Write.Inputs[0],
                                              Write.Output.Size, Def->Inputs[0],
                                              Def->Output.Size))
                continue;
              DomainInvalid = true;
            }
            if (Write.Opcode == NdOp::INTRINSIC)
              for (uint8_t I = 1; I < Write.NumInputs; ++I)
                if (sameOrUnknownFrameRoot(Write.Inputs[I]))
                  DomainInvalid = true;
          }
        }

        if (!DomainInvalid && !DomainSources.empty()) {
          ActiveFrameDomains.insert(Root);
          bool DomainStable = true;
          bool SawInitializer = false;
          for (const MedVar &Source : DomainSources)
            if (!prove(Source, Depth + 1, Seen, ActiveFrameSlots,
                       AnchoredPhis)) {
              DomainStable = false;
              break;
            } else {
              const FrameDomainReachSummary Summary =
                  summarizeFrameDomainReach(Source, Root, 0, {});
              if (!Summary.Unknown && Summary.HasIndependentAlternative)
                SawInitializer = true;
            }
          ActiveFrameDomains.erase(Root);
          if (DomainStable && SawInitializer)
            return true;
        }
      }

      if (ExactFrameSlot || ReloadFrameRoot ||
          varMayBeFrameAddress(Def->Inputs[0]))
        return stableOffsetFailure("incomplete-frame-reload", Start, Depth);

      // A non-frame load narrower than the target pointer cannot transport a
      // complete independently relocatable address. It is an ordinary numeric
      // input (for example a byte from the selected string feeding an x86-64
      // loop state/index DAG), even when later arithmetic widens it.
      if (PointerSize != 0 && Def->Output.Size < PointerSize)
        return true;

      // An exact immutable record field with no overlapping relocation is a
      // scalar load even when its width equals the machine pointer width.  Its
      // numeric payload may itself fall inside a low-VA section; relocation
      // provenance belongs to the field occurrence, not to that coincidental
      // value.  Indexed records take the lane proof below because more than
      // one slot can be reached.
      auto exactImmutableScalarLoad = [&]() {
        if (!Img || PointerSize == 0 || Def->Output.Size == 0)
          return false;
        auto SlotVA = traceValueVA(Def->Inputs[0]);
        if (!SlotVA)
          return false;
        const Segment *SlotSegment = Img->getSegmentFor(*SlotVA);
        if (!SlotSegment || !SlotSegment->isReadable() ||
            (SlotSegment->isExecutable() && Def->Output.Size <= PointerSize) ||
            (SlotSegment->isWritable() && !isReadOnlyAfterReloc(SlotSegment)) ||
            *SlotVA < SlotSegment->VA ||
            *SlotVA - SlotSegment->VA > SlotSegment->Data.size() ||
            Def->Output.Size >
                SlotSegment->Data.size() - (*SlotVA - SlotSegment->VA))
          return false;

        const uint64_t FirstCandidate =
            *SlotVA >= PointerSize - 1 ? *SlotVA - (PointerSize - 1) : 0;
        if (*SlotVA > InvalidVA - Def->Output.Size)
          return false;
        const uint64_t LoadEnd = *SlotVA + Def->Output.Size;
        for (uint64_t Candidate = FirstCandidate; Candidate < LoadEnd;
             ++Candidate) {
          if (!Img->hasRelocationProvenanceAt(Candidate))
            continue;
          if (Candidate <= InvalidVA - PointerSize &&
              Candidate + PointerSize > *SlotVA)
            return false;
        }
        return true;
      };
      if (exactImmutableScalarLoad())
        return true;

      // A writable cell can still be a scalar offset when this function proves
      // its complete local store domain. Keep this narrow: an exact address,
      // full-width scalar stores, no relocation overlap, no unknown aliasing
      // stores, and no call that can execute before the LOAD.
      auto exactScalarStoreBackedLoad = [&]() {
        if (!Img || PointerSize == 0 || Def->Output.Size != PointerSize)
          return false;
        {
          struct AffineAddress {
            bool Valid = false;
            std::map<Key, int64_t> Terms;
            int64_t Constant = 0;
          };
          auto addSignedValue = [](int64_t Left, int64_t Right,
                                    int64_t &Out) {
            if ((Right > 0 && Left > std::numeric_limits<int64_t>::max() - Right) ||
                (Right < 0 && Left < std::numeric_limits<int64_t>::min() - Right))
              return false;
            Out = Left + Right;
            return true;
          };
          auto scaleAffine = [&](AffineAddress Value, int64_t Factor) {
            if (!Value.Valid)
              return AffineAddress{};
            for (auto &[TermKey, Coefficient] : Value.Terms) {
              int64_t Scaled = 0;
              if (!__builtin_mul_overflow(Coefficient, Factor, &Scaled))
                Coefficient = Scaled;
              else
                return AffineAddress{};
            }
            int64_t ScaledConstant = 0;
            if (__builtin_mul_overflow(Value.Constant, Factor,
                                        &ScaledConstant))
              return AffineAddress{};
            Value.Constant = ScaledConstant;
            return Value;
          };
          auto combineAffine = [&](AffineAddress Left, AffineAddress Right,
                                     bool Subtract) {
            if (!Left.Valid || !Right.Valid)
              return AffineAddress{};
            for (const auto &[TermKey, Coefficient] : Right.Terms) {
              int64_t SignedCoefficient =
                  Subtract ? -Coefficient : Coefficient;
              int64_t Combined = 0;
              auto It = Left.Terms.find(TermKey);
              if (It != Left.Terms.end()) {
                if (!addSignedValue(It->second, SignedCoefficient, Combined))
                  return AffineAddress{};
                if (Combined == 0)
                  Left.Terms.erase(It);
                else
                  It->second = Combined;
              } else if (SignedCoefficient != 0) {
                Left.Terms.emplace(TermKey, SignedCoefficient);
              }
            }
            int64_t CombinedConstant = 0;
            if (!addSignedValue(Left.Constant,
                                 Subtract ? -Right.Constant : Right.Constant,
                                 CombinedConstant))
              return AffineAddress{};
            Left.Constant = CombinedConstant;
            return Left;
          };
          std::function<AffineAddress(const MedVar &,
                                      std::set<Key>)> normalizeAddress;
          normalizeAddress = [&](const MedVar &Value, std::set<Key> Seen) {
            if (Value.isConst()) {
              if (Value.ConstVal >
                  static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return AffineAddress{};
              return AffineAddress{true, {},
                                    static_cast<int64_t>(Value.ConstVal)};
            }
            const Key ValueKey = keyOf(Value);
            if (!Seen.insert(ValueKey).second)
              return AffineAddress{true, {{ValueKey, 1}}, 0};
            const MedOp *ValueDef = lookupDef(Value);
            if (!ValueDef || ValueDef->NumInputs == 0)
              return AffineAddress{true, {{ValueKey, 1}}, 0};
            if (ValueDef->Opcode == NdOp::COPY ||
                ValueDef->Opcode == NdOp::INT_ZEXT ||
                ValueDef->Opcode == NdOp::INT_SEXT ||
                (ValueDef->Opcode == NdOp::SUBBYTES &&
                 ValueDef->NumInputs >= 2 && ValueDef->Inputs[1].isConst() &&
                 ValueDef->Inputs[1].ConstVal == 0))
              return normalizeAddress(ValueDef->Inputs[0], Seen);
            if ((ValueDef->Opcode == NdOp::INT_ADD ||
                 ValueDef->Opcode == NdOp::INT_SUB) &&
                ValueDef->NumInputs >= 2)
              return combineAffine(
                  normalizeAddress(ValueDef->Inputs[0], Seen),
                  normalizeAddress(ValueDef->Inputs[1], Seen),
                  ValueDef->Opcode == NdOp::INT_SUB);
            if (ValueDef->Opcode == NdOp::INT_LEFT &&
                ValueDef->NumInputs >= 2 && ValueDef->Inputs[1].isConst() &&
                ValueDef->Inputs[1].ConstVal < 63)
              return scaleAffine(
                  normalizeAddress(ValueDef->Inputs[0], Seen),
                  int64_t(1) << ValueDef->Inputs[1].ConstVal);
            if (ValueDef->Opcode == NdOp::INT_MULT &&
                ValueDef->NumInputs >= 2) {
              const MedVar &Constant = ValueDef->Inputs[0].isConst()
                                             ? ValueDef->Inputs[0]
                                             : ValueDef->Inputs[1];
              const MedVar &Other = ValueDef->Inputs[0].isConst()
                                         ? ValueDef->Inputs[1]
                                         : ValueDef->Inputs[0];
              if (Constant.isConst() &&
                  Constant.ConstVal <
                      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return scaleAffine(normalizeAddress(Other, Seen),
                                   static_cast<int64_t>(Constant.ConstVal));
            }
            if (ValueDef->Opcode == NdOp::LOAD && ValueDef->NumInputs >= 1) {
              for (const MedBlock &Block : CurMedFunc->Blocks) {
                for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
                  if (&Block.Ops[Index] != ValueDef)
                    continue;
                  for (size_t Previous = Index; Previous > 0; --Previous) {
                    const MedOp &Write = Block.Ops[Previous - 1];
                    if (Write.Opcode == NdOp::STORE && Write.NumInputs >= 2 &&
                        addressProvenanceVarKey(Write.Inputs[0]) ==
                            addressProvenanceVarKey(ValueDef->Inputs[0]))
                      return normalizeAddress(Write.Inputs[1], Seen);
                  }
                  break;
                }
              }
            }
            return AffineAddress{true, {{ValueKey, 1}}, 0};
          };
          const MedBlock *LoadBlock = nullptr;
          size_t LoadIndex = 0;
          for (const MedBlock &Block : CurMedFunc->Blocks)
            for (size_t Index = 0; Index < Block.Ops.size(); ++Index)
              if (&Block.Ops[Index] == Def) {
                LoadBlock = &Block;
                LoadIndex = Index;
              }
          if (LoadBlock) {
            const AffineAddress LoadAddress =
                normalizeAddress(Def->Inputs[0], {});
            for (size_t Index = 0; Index < LoadIndex; ++Index) {
              const MedOp &Write = LoadBlock->Ops[Index];
              if (Write.Opcode != NdOp::STORE || Write.NumInputs < 2 ||
                  Write.Inputs[1].Size != Def->Output.Size)
                continue;
              const AffineAddress StoreAddress =
                  normalizeAddress(Write.Inputs[0], {});
              if (!LoadAddress.Valid || !StoreAddress.Valid ||
                  LoadAddress.Terms != StoreAddress.Terms ||
                  LoadAddress.Constant != StoreAddress.Constant)
                continue;
              bool Opaque = false;
              for (size_t Between = Index + 1; Between < LoadIndex; ++Between)
                if (LoadBlock->Ops[Between].Opcode == NdOp::CALL ||
                    LoadBlock->Ops[Between].Opcode == NdOp::INDIR_CALL ||
                    LoadBlock->Ops[Between].Opcode == NdOp::INTRINSIC)
                  Opaque = true;
              if (!Opaque &&
                  valueIsStableAddressOffset(Write.Inputs[1], &Start))
                return true;
            }
          }
        }
        auto SlotVA = traceValueVA(Def->Inputs[0]);
        if (!SlotVA)
          return false;
        const Segment *SlotSegment = Img->getSegmentFor(*SlotVA);
        if (!SlotSegment || !SlotSegment->isReadable() ||
            !SlotSegment->isWritable() ||
            SlotSegment->isExecutable() || *SlotVA < SlotSegment->VA ||
            *SlotVA - SlotSegment->VA > SlotSegment->Data.size() ||
            Def->Output.Size >
                SlotSegment->Data.size() - (*SlotVA - SlotSegment->VA))
          return false;
        const uint64_t FirstCandidate =
            *SlotVA >= PointerSize - 1 ? *SlotVA - (PointerSize - 1) : 0;
        if (*SlotVA > InvalidVA - Def->Output.Size)
          return false;
        const uint64_t LoadEnd = *SlotVA + Def->Output.Size;
        for (uint64_t Candidate = FirstCandidate; Candidate < LoadEnd;
             ++Candidate)
          if (Img->hasRelocationProvenanceAt(Candidate))
            return false;

        int LoadBlockId = -1;
        int LoadOpIndex = -1;
        for (const MedBlock &Block : CurMedFunc->Blocks)
          for (size_t OpIndex = 0; OpIndex < Block.Ops.size(); ++OpIndex)
            if (&Block.Ops[OpIndex] == Def) {
              LoadBlockId = Block.Id;
              LoadOpIndex = static_cast<int>(OpIndex);
            }
        if (LoadBlockId < 0 || LoadOpIndex < 0)
          return false;

        auto canReachBlock = [&](int Start, int Goal) {
          std::vector<int> Work{Start};
          std::set<int> Seen;
          while (!Work.empty()) {
            const int Current = Work.back();
            Work.pop_back();
            if (Current == Goal)
              return true;
            if (!Seen.insert(Current).second)
              continue;
            auto It = std::find_if(
                CurMedFunc->Blocks.begin(), CurMedFunc->Blocks.end(),
                [&](const MedBlock &Block) { return Block.Id == Current; });
            if (It == CurMedFunc->Blocks.end())
              continue;
            Work.insert(Work.end(), It->Succs.begin(), It->Succs.end());
            for (const ExceptionalEdge &Edge : It->ExceptionalSuccs)
              Work.push_back(Edge.BlockId);
          }
          return false;
        };
        for (const MedCallInfo &Call : CurMedFunc->CallInfos) {
          if (Call.BlockId < 0 || Call.OpIdx < 0)
            return false;
          if (Call.BlockId == LoadBlockId) {
            if (Call.OpIdx < LoadOpIndex)
              return false;
          } else if (canReachBlock(Call.BlockId, LoadBlockId)) {
            return false;
          }
        }

        bool SawStore = false;
        for (const MedBlock &Block : CurMedFunc->Blocks) {
          for (const MedOp &Write : Block.Ops) {
            if (Write.Opcode != NdOp::STORE || Write.NumInputs < 2)
              continue;
            auto Destination = traceValueVA(Write.Inputs[0]);
            if (!Destination) {
              if (!varIsFrameDerived(Write.Inputs[0]))
                return false;
              continue;
            }
            if (*Destination != *SlotVA)
              continue;
            if (Write.Inputs[1].Size != Def->Output.Size ||
                !valueIsStableAddressOffset(Write.Inputs[1], &Start))
              return false;
            SawStore = true;
          }
        }
        return SawStore;
      };
      if (exactScalarStoreBackedLoad())
        return true;

      // A pointer-width load can still be an ordinary scalar record field.
      // First recover its algebraic record lane without assuming anything
      // about the loop-carried index, then require every reachable slot in
      // that lane to be immutable and relocation-free.  Finally audit the
      // index terms in this same DFS: if a later iteration feeds this scalar
      // field back into the index, the already-active PHI/frame recurrence is
      // observed here rather than lost through a fresh top-level proof.
      auto sizeMask = [](uint16_t Size) {
        return Size == 0 || Size >= 8 ? ~uint64_t(0)
                                      : (uint64_t(1) << (Size * 8)) - 1;
      };
      std::function<std::optional<uint64_t>(const MedVar &, int, std::set<Key>)>
          traceLaneConstImpl =
              [&](const MedVar &Value, int TraceDepth,
                  std::set<Key> Visited) -> std::optional<uint64_t> {
        if (TraceDepth > 32)
          return std::nullopt;
        if (auto Constant = traceSSAConst(Value))
          return *Constant & sizeMask(Value.Size);
        if (Value.isConst() || !Visited.insert(keyOf(Value)).second)
          return std::nullopt;
        const MedOp *ValueDef = lookupDef(Value);
        if (!ValueDef || ValueDef->NumInputs < 1)
          return std::nullopt;
        if (ValueDef->Opcode == NdOp::LOAD) {
          std::vector<MedVar> Sources;
          const bool CompleteSources =
              collectFrameReloadSources(*ValueDef, Sources);
          if (!CompleteSources || Sources.empty())
            return std::nullopt;
          std::optional<uint64_t> Common;
          for (const MedVar &Source : Sources) {
            auto Constant = traceLaneConstImpl(Source, TraceDepth + 1, Visited);
            if (!Constant || (Common && *Common != *Constant))
              return std::nullopt;
            Common = *Constant;
          }
          return Common;
        }
        if ((ValueDef->Opcode == NdOp::INT_ADD ||
             ValueDef->Opcode == NdOp::INT_SUB) &&
            ValueDef->NumInputs >= 2) {
          auto Left =
              traceLaneConstImpl(ValueDef->Inputs[0], TraceDepth + 1, Visited);
          auto Right =
              traceLaneConstImpl(ValueDef->Inputs[1], TraceDepth + 1, Visited);
          if (!Left || !Right)
            return std::nullopt;
          const uint64_t Result = ValueDef->Opcode == NdOp::INT_ADD
                                      ? *Left + *Right
                                      : *Left - *Right;
          return Result & sizeMask(ValueDef->Output.Size);
        }
        if (ValueDef->Opcode != NdOp::COPY &&
            ValueDef->Opcode != NdOp::INT_ZEXT &&
            ValueDef->Opcode != NdOp::INT_SEXT &&
            ValueDef->Opcode != NdOp::SUBBYTES)
          return std::nullopt;
        auto Constant =
            traceLaneConstImpl(ValueDef->Inputs[0], TraceDepth + 1, Visited);
        if (!Constant)
          return std::nullopt;
        if (ValueDef->Opcode == NdOp::INT_ZEXT)
          return *Constant & sizeMask(ValueDef->Inputs[0].Size);
        if (ValueDef->Opcode == NdOp::INT_SEXT) {
          const unsigned SourceBits = ValueDef->Inputs[0].Size * 8;
          uint64_t Extended = *Constant & sizeMask(ValueDef->Inputs[0].Size);
          if (SourceBits != 0 && SourceBits < 64 &&
              (Extended & (uint64_t(1) << (SourceBits - 1))) != 0)
            Extended |= ~sizeMask(ValueDef->Inputs[0].Size);
          return Extended & sizeMask(ValueDef->Output.Size);
        }
        if (ValueDef->Opcode == NdOp::SUBBYTES) {
          if (ValueDef->NumInputs < 2 || !ValueDef->Inputs[1].isConst() ||
              ValueDef->Inputs[1].ConstVal >= 8)
            return std::nullopt;
          return (*Constant >> (ValueDef->Inputs[1].ConstVal * 8)) &
                 sizeMask(ValueDef->Output.Size);
        }
        return *Constant & sizeMask(ValueDef->Output.Size);
      };
      auto traceLaneConst = [&](const MedVar &Value) {
        return traceLaneConstImpl(Value, 0, {});
      };
      const IndexedPointerLaneSummary ScalarLane = analyzeIndexedPointerLane(
          Def->Inputs[0], Img, PointerSize,
          [&](const MedVar &Value) { return lookupDef(Value); }, traceLaneConst,
          [&](const MedVar &Value, uint64_t &Base, bool &HaveBase,
              std::vector<MedVar> &Terms) {
            if (collectIndexedGlobalBase(Value, Base, HaveBase, Terms) &&
                HaveBase)
              return true;
            Base = 0;
            HaveBase = false;
            Terms.clear();
            return collectLiteralPoolBase(Value, Base, HaveBase, Terms);
          },
          [&](const MedVar &Value) {
            return ptrTableUniqueSegment(Value,
                                         /*IncludeSymbolizedEvidence=*/true);
          },
          [&](const Segment *Segment, uint64_t &RunStart, uint64_t &RunEnd) {
            readOnlyAfterRelocRun(Segment, RunStart, RunEnd);
          });
      if (ScalarLane.Complete) {
        bool ScalarOnly = true;
        for (uint64_t Slot : ScalarLane.Slots) {
          const Segment *SlotSegment = Img->getSegmentFor(Slot);
          if (!SlotSegment || SlotSegment->isExecutable() ||
              (SlotSegment->isWritable() &&
               !isReadOnlyAfterReloc(SlotSegment)) ||
              Img->hasRelocationProvenanceAt(Slot)) {
            ScalarOnly = false;
            break;
          }
        }
        if (ScalarOnly) {
          const Key LoadKey = keyOf(Start);
          const bool Inserted =
              ActiveImmutableScalarLoads.insert(LoadKey).second;
          for (const MedVar &Term : ScalarLane.IndexTerms)
            if (!prove(Term, Depth + 1, Seen, ActiveFrameSlots, AnchoredPhis)) {
              ScalarOnly = false;
              break;
            }
          if (Inserted)
            ActiveImmutableScalarLoads.erase(LoadKey);
          if (ScalarOnly)
            return true;
        }
      }

      // ARM32 materializes large scalar immediates in an executable literal
      // pool. They are pointer-width LOADs, but an exact immutable pool word
      // with no relocation provenance and a non-address payload is still a
      // proven scalar. Keep the rule format-neutral: every loader reports
      // pointer slots/relocations through BinaryImage, and any missing or
      // conflicting evidence fails closed. A relocated PC displacement can
      // still be scalar at the LOAD itself; the enclosing `pc + displacement`
      // is independently folded and rejected above when it forms an address.
      auto scalarLiteralLoad = [&]() {
        if (!Img || PointerSize == 0 || Def->Output.Size != PointerSize)
          return false;
        auto SlotVA = traceValueVA(Def->Inputs[0]);
        if (!SlotVA)
          return false;
        const Segment *SlotSeg = Img->getSegmentFor(*SlotVA);
        if (!SlotSeg || !SlotSeg->isReadable() || !SlotSeg->isExecutable() ||
            SlotSeg->isWritable())
          return false;
        if (Img->hasRelocationProvenanceAt(*SlotVA))
          return false;
        bool SawLiteralLoad = false;
        auto Literal = traceTableBaseConst(Start, 0, &SawLiteralLoad);
        return Literal && SawLiteralLoad &&
               !constantIsMappedAddress(*Literal, Def->Output.Size);
      };
      return scalarLiteralLoad()
                 ? true
                 : stableOffsetFailure("pointer-width-load", Start, Depth);
    }
    for (uint8_t I = 0; I < Def->NumInputs; ++I)
      if (!prove(Def->Inputs[I], Depth + 1, Seen, ActiveFrameSlots,
                 AnchoredPhis))
        return stableOffsetFailure("unstable-input", Start, Depth);
    return true;
  };
  return prove(V, 0, {}, {}, {});
}

std::optional<MedLLVMEmitter::PureReadOnlyBaseIdentity>
MedLLVMEmitter::pureReadOnlyBaseIdentity(
    const MedVar &V, bool DirectPhiConstantBypassesGetVar) const {
  if (!CurMedFunc || !Img)
    return std::nullopt;

  const uint16_t PointerSize = getTargetRegInfo(TargetArch).PointerSize;
  if (PointerSize == 0 || V.Size != PointerSize)
    return std::nullopt;

  MedVar Cur = V;
  bool BypassesGetVar = DirectPhiConstantBypassesGetVar && Cur.isConst() &&
                        !isExactAddressProvenance(Cur.Provenance);
  std::set<std::tuple<int, int, int>> Seen;

  // AArch64 ADRP/ADD folding can leave a low linked VA in a 32-bit constant
  // input even though COPY writes the architecturally full X register. The
  // emitter's setVar performs an explicit zero-extension in that case, so the
  // resulting value is an exact raw pointer when (and only when) the narrow
  // leaf itself is a pure constant materialization whose bits already equal
  // the complete mapped VA. Follow same-width COPY/ZEXT forwarders to cover
  // harmless lift temporaries, but never cross SUBBYTES, arithmetic, a PHI, or
  // another width change: those shapes may have discarded pointer bits and a
  // coincidentally equal numeric result is not provenance. A narrow leaf that
  // getVar would symbolize is also invalid because ptrtoint-to-narrow followed
  // by zext cannot preserve the re-linked pointer's high bits.
  auto zeroExtendedNarrowConstantIdentity =
      [&](const MedOp &WideningDef) -> std::optional<PureReadOnlyBaseIdentity> {
    if ((WideningDef.Opcode != NdOp::COPY &&
         WideningDef.Opcode != NdOp::INT_ZEXT) ||
        WideningDef.NumInputs < 1 || WideningDef.Output.Size != PointerSize)
      return std::nullopt;
    MedVar Leaf = WideningDef.Inputs[0];
    const uint16_t LeafSize = Leaf.Size;
    if (LeafSize == 0 || LeafSize >= PointerSize || LeafSize > 8)
      return std::nullopt;

    std::set<std::tuple<int, int, int>> LeafSeen;
    for (int Depth = 0; Depth <= 16; ++Depth) {
      if (Leaf.Size != LeafSize)
        return std::nullopt;
      if (Leaf.isConst()) {
        const unsigned Bits = LeafSize * 8;
        if (Bits < 64 && (Leaf.ConstVal >> Bits) != 0)
          return std::nullopt;
        if (Leaf.Provenance != ConstantAddressProvenance::Unknown ||
            !isMaterializableReadOnlyDataAddress(Leaf.ConstVal) ||
            getVarMayRelocateConstant(Leaf.ConstVal, Leaf.Size))
          return std::nullopt;
        return PureReadOnlyBaseIdentity{Leaf.ConstVal, PointerSize,
                                        /*Symbolized=*/false};
      }
      const auto Key =
          std::make_tuple(static_cast<int>(Leaf.Kind), Leaf.Id, Leaf.SSAVer);
      if (!LeafSeen.insert(Key).second)
        return std::nullopt;
      const MedOp *LeafDef = lookupDef(Leaf);
      if (!LeafDef ||
          (LeafDef->Opcode != NdOp::COPY &&
           LeafDef->Opcode != NdOp::INT_ZEXT) ||
          LeafDef->NumInputs < 1 || LeafDef->Output.Size != LeafSize ||
          LeafDef->Inputs[0].Size != LeafSize)
        return std::nullopt;
      Leaf = LeafDef->Inputs[0];
    }
    return std::nullopt;
  };

  for (int Depth = 0; Depth <= 32; ++Depth) {
    if (Cur.Size != PointerSize)
      return std::nullopt;
    if (Cur.isConst()) {
      if (!isMaterializableReadOnlyDataAddress(Cur.ConstVal))
        return std::nullopt;
      const bool Symbolized = dataOccurrenceSymbolizes(Cur, BypassesGetVar);
      return PureReadOnlyBaseIdentity{Cur.ConstVal, PointerSize, Symbolized};
    }

    const auto Key =
        std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer);
    if (!Seen.insert(Key).second)
      return std::nullopt;
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      return std::nullopt;
    if (auto Identity = zeroExtendedNarrowConstantIdentity(*Def))
      return Identity;
    auto Forwarded = pointerPreservingInput(*Def);
    if (!Forwarded || Forwarded->Size != PointerSize)
      return std::nullopt;
    Cur = *Forwarded;
    // An operation input is materialized through getVar even when the original
    // value supplied to this helper was itself a direct PHI constant.
    BypassesGetVar = false;
  }
  return std::nullopt;
}

bool MedLLVMEmitter::phiIncomingIsRecurrent(const PhiNode &Phi, int PredId,
                                            const MedVar &Arg) const {
  if (!CurMedFunc) {
    ++AddressProvenanceWork.RecurrenceProofs;
    return phiIncomingIsRecurrentImpl(Phi, PredId, Arg);
  }
  if (PhiRecurrenceCacheFor != CurMedFunc) {
    PhiRecurrenceCacheFor = CurMedFunc;
    PhiRecurrenceCache.clear();
  }
  const auto Key = std::make_tuple(&Phi, PredId, addressProvenanceVarKey(Arg));
  if (auto It = PhiRecurrenceCache.find(Key); It != PhiRecurrenceCache.end())
    return It->second;

  ++AddressProvenanceWork.RecurrenceProofs;
  const bool Result = phiIncomingIsRecurrentImpl(Phi, PredId, Arg);
  PhiRecurrenceCache.emplace(Key, Result);
  return Result;
}

bool MedLLVMEmitter::phiIncomingIsRecurrentImpl(const PhiNode &Phi, int PredId,
                                                const MedVar &Arg) const {
  if (classifyPhiIncomingEdge(Phi, PredId) !=
      PhiEdgeFeasibility::ProvenFeasible)
    return false;

  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };
  using Key = std::tuple<int, int, int>;
  auto keyOf = [](const MedVar &V) {
    return Key{static_cast<int>(V.Kind), V.Id, V.SSAVer};
  };
  auto isSubregisterAlias = [&](const MedVar &A, const MedVar &B) {
    if (A.isConst() || B.isConst() || A.Kind != MedVar::Reg ||
        B.Kind != MedVar::Reg || A.TheArch != B.TheArch || A.Size == 0 ||
        B.Size == 0)
      return false;
    const TargetRegInfo &TRI = getTargetRegInfo(A.TheArch);
    return TRI.isSubRegOf(A.RegOff, A.Size, B.RegOff, B.Size) ||
           TRI.isSubRegOf(B.RegOff, B.Size, A.RegOff, A.Size);
  };

  // Pure COPY-like recurrences do not need the bounded general recurrence
  // proof below.  Follow the single identity-preserving edge iteratively so a
  // long lowered chain such as `p = PHI(&f, COPY^N(p))` still collapses to its
  // unique initializer.  The incoming PHI edge was required to be proven
  // feasible above; unknown edges must not acquire recurrence authority from
  // this fast path.
  {
    MedVar Current = Arg;
    std::set<AddressProvenanceVarKey> Seen;
    while (!Current.isConst()) {
      if (addressProvenanceVarKey(Current) ==
          addressProvenanceVarKey(Phi.Output))
        return true;
      if (!Seen.insert(addressProvenanceVarKey(Current)).second)
        break;
      const MedOp *Def = lookupDef(Current);
      if (!Def)
        break;
      std::optional<MedVar> Forwarded = pointerPreservingInput(*Def);
      if (!Forwarded || Forwarded->Size != Current.Size)
        break;
      Current = *Forwarded;
    }
  }

  struct RecurrencePathProof {
    bool PreservesPointer = false;
    bool ReachesExactTarget = false;
  };
  auto sameIdentity = [](const PureReadOnlyBaseIdentity &A,
                         const PureReadOnlyBaseIdentity &B) {
    return A.VA == B.VA && A.Width == B.Width && A.Symbolized == B.Symbolized;
  };

  // Prove value recurrence, not generic data/control dependence. COPY-like
  // forwarders and the pointer side of ADD/SUB transport a pointer; SELECT and
  // masked-select transport it only when every selectable value arm does. A PHI
  // appearing solely in a condition, multiplier, shift count, or mask is not a
  // loop-carried pointer (for example SELECT(cmp(phi), scalarA, scalarB)).
  //
  // PreservesPointer and ReachesExactTarget stay separate so a nested reset can
  // carry the outer PHI's one audited base without pretending that the reset
  // leaf itself is a backedge. The incoming value is recurrent only when both
  // facts hold for the complete feasible expression.
  std::function<RecurrencePathProof(
      const MedVar &, const MedVar &,
      const std::optional<PureReadOnlyBaseIdentity> &, int, std::set<Key>,
      bool)>
      proveRecurrence =
          [&](const MedVar &Start, const MedVar &Target,
              const std::optional<PureReadOnlyBaseIdentity> &ExpectedBase,
              int Depth, std::set<Key> Seen,
              bool DirectPhiConstant) -> RecurrencePathProof {
    if (Depth > 32)
      return {};
    if (!Start.isConst() && sameVar(Start, Target))
      return {true, true};
    if (ExpectedBase)
      if (auto Identity = pureReadOnlyBaseIdentity(Start, DirectPhiConstant);
          Identity && sameIdentity(*Identity, *ExpectedBase))
        return {true, false};
    if (Start.isConst())
      return {};
    // Pointer transport is a coinductive property inside a recurrence SCC.
    // Re-entering a node through COPY/PHI/pointer-side arithmetic preserves
    // the candidate value, but does not by itself prove connection to the
    // outer target.  The caller still requires ReachesExactTarget from another
    // feasible arm, and every selectable arm must preserve the pointer.  This
    // admits nested identity PHIs such as outer <- inner <- outer while still
    // rejecting an unanchored cycle or a scalar/reset arm.
    if (!Seen.insert(keyOf(Start)).second)
      return {true, false};

    if (const PhiNode *Nested = lookupPhi(Start)) {
      bool SawFeasible = false;
      bool AllPreserve = true;
      bool ReachesTarget = false;
      for (const auto &[NestedPred, NestedArg] : Nested->Args) {
        PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Nested, NestedPred);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible) {
          AllPreserve = false;
          continue;
        }
        SawFeasible = true;
        RecurrencePathProof Arm =
            proveRecurrence(NestedArg, Target, ExpectedBase, Depth + 1, Seen,
                            /*DirectPhiConstant=*/NestedArg.isConst());
        AllPreserve &= Arm.PreservesPointer;
        ReachesTarget |= Arm.ReachesExactTarget;
      }
      return {SawFeasible && AllPreserve, ReachesTarget};
    }

    const MedOp *Def = lookupDef(Start);
    if (!Def)
      return {};
    if (auto Forwarded = pointerPreservingInput(*Def))
      return proveRecurrence(*Forwarded, Target, ExpectedBase, Depth + 1, Seen,
                             /*DirectPhiConstant=*/false);
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
        return {};
      bool AllPreserve = true;
      bool ReachesTarget = false;
      for (const MedVar &Source : Sources) {
        RecurrencePathProof SourceProof =
            proveRecurrence(Source, Target, ExpectedBase, Depth + 1, Seen,
                            /*DirectPhiConstant=*/false);
        AllPreserve &= SourceProof.PreservesPointer;
        ReachesTarget |= SourceProof.ReachesExactTarget;
      }
      return {AllPreserve, ReachesTarget};
    }

    auto canCarry = [&](const MedVar &Input) {
      return Def->Output.Size != 0 && Input.Size != 0 &&
             Def->Output.Size >= Input.Size;
    };
    if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
      RecurrencePathProof Left;
      RecurrencePathProof Right;
      if (canCarry(Def->Inputs[0]))
        Left = proveRecurrence(Def->Inputs[0], Target, ExpectedBase, Depth + 1,
                               Seen, /*DirectPhiConstant=*/false);
      if (canCarry(Def->Inputs[1]))
        Right = proveRecurrence(Def->Inputs[1], Target, ExpectedBase, Depth + 1,
                                Seen,
                                /*DirectPhiConstant=*/false);
      const bool ReachesTarget =
          Left.ReachesExactTarget || Right.ReachesExactTarget;
      if (Left.PreservesPointer == Right.PreservesPointer)
        return {false, ReachesTarget};
      const MedVar &Offset =
          Left.PreservesPointer ? Def->Inputs[1] : Def->Inputs[0];
      const bool Stable = Offset.isConst()
                              ? constantIsStableAddressOffset(Offset)
                              : valueIsStableAddressOffset(Offset, &Target);
      return {Stable, ReachesTarget};
    }
    if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2) {
      RecurrencePathProof Left;
      RecurrencePathProof Right;
      if (canCarry(Def->Inputs[0]))
        Left = proveRecurrence(Def->Inputs[0], Target, ExpectedBase, Depth + 1,
                               Seen, /*DirectPhiConstant=*/false);
      if (canCarry(Def->Inputs[1]))
        Right = proveRecurrence(Def->Inputs[1], Target, ExpectedBase, Depth + 1,
                                Seen,
                                /*DirectPhiConstant=*/false);
      const bool ReachesTarget =
          Left.ReachesExactTarget || Right.ReachesExactTarget;
      const bool StableOffset =
          Def->Inputs[1].isConst()
              ? constantIsStableAddressOffset(Def->Inputs[1])
              : valueIsStableAddressOffset(Def->Inputs[1], &Target);
      const bool Preserves =
          Left.PreservesPointer && !Right.PreservesPointer && StableOffset;
      return {Preserves, ReachesTarget};
    }
    if (selectPreservesPointerValues(*Def)) {
      RecurrencePathProof TrueArm =
          proveRecurrence(Def->Inputs[1], Target, ExpectedBase, Depth + 1, Seen,
                          /*DirectPhiConstant=*/false);
      RecurrencePathProof FalseArm =
          proveRecurrence(Def->Inputs[2], Target, ExpectedBase, Depth + 1, Seen,
                          /*DirectPhiConstant=*/false);
      return {TrueArm.PreservesPointer && FalseArm.PreservesPointer,
              TrueArm.ReachesExactTarget || FalseArm.ReachesExactTarget};
    }
    if (Def->Opcode == NdOp::INT_OR) {
      MedVar Cond, ArmT, ArmF;
      if (!isMaskedSelectOr(*Def, Cond, ArmT, ArmF))
        return {};
      RecurrencePathProof TrueArm =
          proveRecurrence(ArmT, Target, ExpectedBase, Depth + 1, Seen,
                          /*DirectPhiConstant=*/false);
      RecurrencePathProof FalseArm =
          proveRecurrence(ArmF, Target, ExpectedBase, Depth + 1, Seen,
                          /*DirectPhiConstant=*/false);
      return {TrueArm.PreservesPointer && FalseArm.PreservesPointer,
              TrueArm.ReachesExactTarget || FalseArm.ReachesExactTarget};
    }
    return {};
  };

  const std::optional<PureReadOnlyBaseIdentity> NoExpectedBase;
  auto reachesExact = [&](const MedVar &Start, const MedVar &Target) {
    RecurrencePathProof Proof =
        proveRecurrence(Start, Target, NoExpectedBase, /*Depth=*/0, {},
                        /*DirectPhiConstant=*/Start.isConst());
    return Proof.PreservesPointer && Proof.ReachesExactTarget;
  };

  RecurrencePathProof ExactProof =
      proveRecurrence(Arg, Phi.Output, NoExpectedBase, /*Depth=*/0, {},
                      /*DirectPhiConstant=*/Arg.isConst());
  if (ExactProof.PreservesPointer && ExactProof.ReachesExactTarget)
    return true;

  // A reset arm cannot nominate the identity that excuses itself. Derive one
  // expected identity solely from every feasible outer-PHI initializer: an
  // incoming value whose exact-only pointer walk does not reach the root. Any
  // unknown edge, complex/unproved initializer, differing base, width, or
  // raw/symbolized model disables re-materialization equivalence and leaves the
  // complete table audit fail-closed.
  if (ExactProof.ReachesExactTarget) {
    std::optional<PureReadOnlyBaseIdentity> ExpectedBase;
    bool InitializersValid = true;
    for (const auto &[InitPred, InitArg] : Phi.Args) {
      PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(Phi, InitPred);
      if (Edge == PhiEdgeFeasibility::Infeasible)
        continue;
      if (Edge != PhiEdgeFeasibility::ProvenFeasible) {
        InitializersValid = false;
        break;
      }
      RecurrencePathProof Shape =
          proveRecurrence(InitArg, Phi.Output, NoExpectedBase, /*Depth=*/0, {},
                          /*DirectPhiConstant=*/InitArg.isConst());
      if (Shape.ReachesExactTarget)
        continue;
      auto Identity = pureReadOnlyBaseIdentity(InitArg, InitArg.isConst());
      if (!Identity ||
          (ExpectedBase && !sameIdentity(*ExpectedBase, *Identity))) {
        InitializersValid = false;
        break;
      }
      ExpectedBase = *Identity;
    }
    if (InitializersValid && ExpectedBase) {
      RecurrencePathProof Rematerialized =
          proveRecurrence(Arg, Phi.Output, ExpectedBase, /*Depth=*/0, {},
                          /*DirectPhiConstant=*/Arg.isConst());
      if (Rematerialized.PreservesPointer && Rematerialized.ReachesExactTarget)
        return true;
    }
  }

  // Wide/narrow register views can form one mutual recurrence. Discover only
  // aliases in pointer-value roles, then require the incoming expression to
  // depend on that alias on every selectable arm before accepting its own exact
  // backedge as recurrence evidence.
  std::set<const PhiNode *> AliasCandidates;
  std::function<void(const MedVar &, int, std::set<Key>)> collectAliases =
      [&](const MedVar &Start, int Depth, std::set<Key> Seen) {
        if (Depth > 32 || Start.isConst() || !Seen.insert(keyOf(Start)).second)
          return;
        if (const PhiNode *Nested = lookupPhi(Start)) {
          if (Nested != &Phi && isSubregisterAlias(Nested->Output, Phi.Output))
            AliasCandidates.insert(Nested);
          for (const auto &[NestedPred, NestedArg] : Nested->Args)
            if (classifyPhiIncomingEdge(*Nested, NestedPred) ==
                PhiEdgeFeasibility::ProvenFeasible)
              collectAliases(NestedArg, Depth + 1, Seen);
          return;
        }
        const MedOp *Def = lookupDef(Start);
        if (!Def)
          return;
        if (auto Forwarded = pointerPreservingInput(*Def)) {
          collectAliases(*Forwarded, Depth + 1, Seen);
          return;
        }
        if (Def->Opcode == NdOp::LOAD) {
          std::vector<MedVar> Sources;
          if (collectFrameReloadSources(*Def, Sources))
            for (const MedVar &Source : Sources)
              collectAliases(Source, Depth + 1, Seen);
          return;
        }
        auto carry = [&](const MedVar &Input) {
          if (Def->Output.Size != 0 && Input.Size != 0 &&
              Def->Output.Size >= Input.Size)
            collectAliases(Input, Depth + 1, Seen);
        };
        if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
          carry(Def->Inputs[0]);
          carry(Def->Inputs[1]);
        } else if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2) {
          carry(Def->Inputs[0]);
        } else if (selectPreservesPointerValues(*Def)) {
          collectAliases(Def->Inputs[1], Depth + 1, Seen);
          collectAliases(Def->Inputs[2], Depth + 1, Seen);
        } else if (Def->Opcode == NdOp::INT_OR) {
          MedVar Cond, ArmT, ArmF;
          if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF)) {
            collectAliases(ArmT, Depth + 1, Seen);
            collectAliases(ArmF, Depth + 1, Seen);
          }
        }
      };
  collectAliases(Arg, 0, {});

  for (const PhiNode *Alias : AliasCandidates) {
    if (!reachesExact(Arg, Alias->Output))
      continue;
    for (const auto &[AliasPred, AliasArg] : Alias->Args)
      if (classifyPhiIncomingEdge(*Alias, AliasPred) ==
              PhiEdgeFeasibility::ProvenFeasible &&
          reachesExact(AliasArg, Alias->Output))
        return true;
  }
  return false;
}

bool MedLLVMEmitter::phiHasPureForwardingCycle(const PhiNode &Phi) const {
  if (!CurMedFunc)
    return false;

  auto forwardedPhi = [&](const MedVar &Value) {
    MedVar Current = Value;
    std::set<AddressProvenanceVarKey> Seen;
    for (int Depth = 0; Depth <= 64 && !Current.isConst(); ++Depth) {
      if (const PhiNode *Nested = lookupPhi(Current))
        return Nested;
      if (!Seen.insert(addressProvenanceVarKey(Current)).second)
        break;
      const MedOp *Def = lookupDef(Current);
      if (!Def)
        break;
      auto Forwarded = pointerPreservingInput(*Def);
      if (!Forwarded || Forwarded->Size != Current.Size)
        break;
      Current = *Forwarded;
    }
    return static_cast<const PhiNode *>(nullptr);
  };

  std::vector<const PhiNode *> Work;
  auto appendSources = [&](const PhiNode &Owner) {
    for (const auto &[Pred, Arg] : Owner.Args) {
      if (classifyPhiIncomingEdge(Owner, Pred) !=
          PhiEdgeFeasibility::ProvenFeasible)
        continue;
      if (const PhiNode *Source = forwardedPhi(Arg))
        Work.push_back(Source);
    }
  };
  appendSources(Phi);
  std::set<const PhiNode *> Seen;
  size_t Budget = 4096;
  while (!Work.empty() && Budget-- > 0) {
    const PhiNode *Current = Work.back();
    Work.pop_back();
    if (Current == &Phi)
      return true;
    if (Seen.insert(Current).second)
      appendSources(*Current);
  }
  return false;
}

bool MedLLVMEmitter::phiIsSelfRecurrent(const PhiNode &Phi) const {
  if (CurMedFunc) {
    if (SelfRecurrenceCacheFor != CurMedFunc) {
      SelfRecurrenceCacheFor = CurMedFunc;
      SelfRecurrenceCache.clear();
    }
    if (auto It = SelfRecurrenceCache.find(&Phi);
        It != SelfRecurrenceCache.end())
      return It->second;
  }
  for (const auto &[PredId, Arg] : Phi.Args)
    if (phiIncomingIsRecurrent(Phi, PredId, Arg)) {
      if (CurMedFunc)
        SelfRecurrenceCache.emplace(&Phi, true);
      return true;
    }
  if (CurMedFunc)
    SelfRecurrenceCache.emplace(&Phi, false);
  return false;
}

std::optional<MedVar>
MedLLVMEmitter::pointerPreservingInput(const MedOp &Op) const {
  if (Op.NumInputs < 1 || Op.Output.Size == 0 || Op.Inputs[0].Size == 0)
    return std::nullopt;

  const MedVar &Input = Op.Inputs[0];
  unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  auto PreservesCompletePointer = [&]() {
    // MedIR can temporarily widen an address beyond the machine pointer width
    // and later extract/copy its low subregister (notably i386's i64 address
    // temporary -> SUBBYTES(..., 0) -> i32).  That is a truncation of the
    // temporary, but not of the target pointer payload.  On a 64-bit target the
    // same i64 -> i32 shape remains correctly rejected.
    return Op.Output.Size >= Input.Size ||
           (PtrSize != 0 && Op.Output.Size >= PtrSize);
  };
  if (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT)
    return PreservesCompletePointer() ? std::optional<MedVar>(Input)
                                      : std::nullopt;

  if (Op.Opcode == NdOp::SUBBYTES) {
    if (Op.NumInputs < 2 || !Op.Inputs[1].isConst() ||
        Op.Inputs[1].ConstVal != 0 || !PreservesCompletePointer())
      return std::nullopt;
    return Input;
  }

  if (Op.Opcode != NdOp::INT_SEXT || Op.Output.Size < Input.Size)
    return std::nullopt;
  if (Op.Output.Size == Input.Size)
    return Input;

  // A widening SEXT preserves an unsigned address only when the source sign
  // bit is known clear. Prove that on the constant leaf at its original width;
  // a relocatable leaf is rejected because its link-time sign bit is not
  // represented by the old image VA.
  bool SawLoad = false;
  bool SawArithmetic = false;
  uint16_t OriginSize = Input.Size;
  auto Value =
      traceTableBaseConst(Input, 0, &SawLoad, &OriginSize, &SawArithmetic);
  if (!Value || SawLoad || SawArithmetic || OriginSize == 0 || OriginSize > 8)
    return std::nullopt;
  if (Img) {
    bool IsPointerWidth = PtrSize == 0 || OriginSize >= PtrSize;
    bool LoaderReloc = Img->RelocDataAddrs.count(*Value) ||
                       Img->RodataAnchorSeg.count(*Value) ||
                       Img->WritableRelocDataAddrs.count(*Value) ||
                       Img->CodeRefTargets.count(*Value);
    // A mapped full-width address can be replaced by a rebuilt global/function
    // even when a secondary use classifier supplies the final getVar gate. Be
    // conservative here without calling those classifiers recursively.
    if (LoaderReloc ||
        (IsPointerWidth && Img->getSegmentFor(*Value) != nullptr))
      return std::nullopt;
  }
  unsigned Bits = OriginSize * 8;
  return ((*Value >> (Bits - 1)) & 1) == 0 ? std::optional<MedVar>(Input)
                                           : std::nullopt;
}

bool MedLLVMEmitter::selectPreservesPointerValues(const MedOp &Op) const {
  return Op.Opcode == NdOp::SELECT && Op.NumInputs >= 3 &&
         Op.Output.Size != 0 && Op.Inputs[1].Size == Op.Output.Size &&
         Op.Inputs[2].Size == Op.Output.Size;
}

bool MedLLVMEmitter::isMaskedSelectOr(const MedOp &Or, MedVar &Cond,
                                      MedVar &ArmT, MedVar &ArmF) const {
  if (Or.Opcode != NdOp::INT_OR || Or.NumInputs < 2)
    return false;
  const MedOp *A = lookupDef(Or.Inputs[0]);
  const MedOp *B = lookupDef(Or.Inputs[1]);
  if (!A || !B || A->Opcode != NdOp::INT_AND || B->Opcode != NdOp::INT_AND ||
      A->NumInputs < 2 || B->NumInputs < 2)
    return false;
  auto sameVar = [](const MedVar &Left, const MedVar &Right) {
    return !Left.isConst() && !Right.isConst() && Left.Kind == Right.Kind &&
           Left.Id == Right.Id && Left.SSAVer == Right.SSAVer;
  };
  std::function<bool(const MedVar &, int)> isBooleanValue = [&](const MedVar &V,
                                                                int Depth) {
    if (Depth > 8)
      return false;
    if (V.isConst())
      return V.ConstVal <= 1;
    const MedOp *Def = lookupDef(V);
    if (!Def)
      return false;
    switch (Def->Opcode) {
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::BOOL_NOT:
      return true;
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
      return Def->NumInputs >= 2 && isBooleanValue(Def->Inputs[0], Depth + 1) &&
             isBooleanValue(Def->Inputs[1], Depth + 1);
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
      return Def->NumInputs >= 1 && isBooleanValue(Def->Inputs[0], Depth + 1);
    default:
      return false;
    }
  };
  // Every operation in the blend must execute at one exact width. The emitter
  // zero-extends narrower AND operands before applying the mask; an i32
  // `-cond` therefore is not an all-ones mask for an i64 pointer.
  uint16_t BlendSize = Or.Output.Size;
  if (BlendSize == 0 || Or.Inputs[0].Size != BlendSize ||
      Or.Inputs[1].Size != BlendSize || A->Output.Size != BlendSize ||
      B->Output.Size != BlendSize)
    return false;

  auto maskCond = [&](const MedVar &Mask,
                      bool &Positive) -> std::optional<MedVar> {
    if (Mask.Size != BlendSize)
      return std::nullopt;
    const MedOp *Def = lookupDef(Mask);
    Positive = true;
    if (Def && Def->Opcode == NdOp::INT_NOT && Def->NumInputs >= 1) {
      if (Def->Output.Size != BlendSize || Def->Inputs[0].Size != BlendSize)
        return std::nullopt;
      Positive = false;
      Def = lookupDef(Def->Inputs[0]);
    }
    if (!Def || Def->Opcode != NdOp::INT_NEG2 || Def->NumInputs < 1 ||
        Def->Output.Size != BlendSize || Def->Inputs[0].Size != BlendSize)
      return std::nullopt;
    Def = lookupDef(Def->Inputs[0]);
    if (!Def || Def->Opcode != NdOp::INT_ZEXT || Def->NumInputs < 1 ||
        Def->Output.Size != BlendSize || !isBooleanValue(Def->Inputs[0], 0))
      return std::nullopt;
    return Def->Inputs[0];
  };
  for (int Ai = 0; Ai < 2; ++Ai)
    for (int Bi = 0; Bi < 2; ++Bi) {
      bool APositive = false;
      bool BPositive = false;
      auto ACond = maskCond(A->Inputs[Ai], APositive);
      auto BCond = maskCond(B->Inputs[Bi], BPositive);
      const MedVar &AValue = A->Inputs[1 - Ai];
      const MedVar &BValue = B->Inputs[1 - Bi];
      if (AValue.Size != BlendSize || BValue.Size != BlendSize)
        continue;
      if (ACond && BCond && sameVar(*ACond, *BCond) && APositive != BPositive) {
        Cond = *ACond;
        ArmT = APositive ? AValue : BValue;
        ArmF = APositive ? BValue : AValue;
        return true;
      }
    }
  return false;
}

void MedLLVMEmitter::failAmbiguousDataPointerPhi(const PhiNode &Phi) const {
  if (!FatalDataPointerResolution)
    syncError() << "med_llvm_emitter: ambiguous reachable read-only table-base "
                   "PHI "
                << Phi.Output.display() << " in " << CurMedFunc->Name
                << "; refusing stale-address fallback\n";
  FatalDataPointerResolution = true;
}

//===----------------------------------------------------------------------===//
// SSA constant tracing
//===----------------------------------------------------------------------===//

std::optional<uint64_t> MedLLVMEmitter::traceSSAConst(const MedVar &V) const {
  if (!CurMedFunc)
    return std::nullopt;

  MedVar Cur = V;
  std::set<std::tuple<int, int, int, uint16_t>> Seen;
  for (;;) {
    if (Cur.isConst())
      return Cur.ConstVal;
    auto Key = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                               Cur.Size);
    if (!Seen.insert(Key).second)
      return std::nullopt;
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      return std::nullopt;
    if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1) {
      if (Def->Output.Size == 0 || Def->Inputs[0].Size == 0 ||
          Def->Output.Size < Def->Inputs[0].Size)
        return std::nullopt;
      Cur = Def->Inputs[0];
      continue;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<uint64_t>
MedLLVMEmitter::traceTableBaseConst(const MedVar &V, int Depth, bool *SawLoad,
                                    uint16_t *OriginSize,
                                    bool *SawArithmetic) const {
  auto atSize = [](uint64_t Value, uint16_t Size) {
    if (Size == 0 || Size >= 8)
      return Value;
    return Value & ((uint64_t(1) << (Size * 8)) - 1);
  };
  if (V.isConst()) {
    if (OriginSize)
      *OriginSize = V.Size;
    return atSize(V.ConstVal, V.Size);
  }
  if (!CurMedFunc || Depth > 8)
    return std::nullopt;

  const MedOp *Def = lookupDef(V);
  if (!Def)
    return std::nullopt;

  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::SUBBYTES:
    if (auto Forwarded = pointerPreservingInput(*Def))
      if (auto Value = traceTableBaseConst(*Forwarded, Depth + 1, SawLoad,
                                           OriginSize, SawArithmetic))
        return atSize(*Value, Def->Output.Size);
    return std::nullopt;
  case NdOp::INT_ADD: {
    if (Def->NumInputs < 2)
      return std::nullopt;
    if (SawArithmetic)
      *SawArithmetic = true;
    auto A = traceTableBaseConst(Def->Inputs[0], Depth + 1, SawLoad, OriginSize,
                                 SawArithmetic);
    auto B = traceTableBaseConst(Def->Inputs[1], Depth + 1, SawLoad, OriginSize,
                                 SawArithmetic);
    if (A && B)
      return atSize(*A + *B, Def->Output.Size);
    return std::nullopt;
  }
  case NdOp::LOAD: {
    // Literal-pool load: the table base word lives in a read-only segment and
    // the loader has already applied its relocation, so read it directly.
    if (Def->NumInputs < 1 || !Img)
      return std::nullopt;
    auto Addr = traceTableBaseConst(Def->Inputs[0], Depth + 1, SawLoad,
                                    OriginSize, SawArithmetic);
    if (!Addr)
      return std::nullopt;
    const auto *Seg = Img->getSegmentFor(*Addr);
    if (!Seg || Seg->isWritable() || Seg->Data.empty())
      return std::nullopt;
    size_t Off = static_cast<size_t>(*Addr - Seg->VA);
    uint16_t Sz = Def->Output.Size ? Def->Output.Size : 4;
    if (Sz > 8 || !rangeInBounds(Off, Sz, Seg->Data.size()))
      return std::nullopt;
    uint64_t Val = 0;
    std::memcpy(&Val, Seg->Data.data() + Off, Sz);
    // A native-width absolute data-pointer relocation is an address-sized bit
    // pattern and must be zero/canonical-extended on a 32-bit target.  Other
    // literal loads may be signed PC-relative displacements; preserve their
    // historical sign extension so a subsequent `+ pc` reconstructs the VA.
    const bool IsAbsoluteDataPointer = Img->DataPtrRelocSlots.count(*Addr) != 0;
    if (!IsAbsoluteDataPointer && Sz < 8 && (Val & (1ull << (Sz * 8 - 1))))
      Val |= ~uint64_t(0) << (Sz * 8);
    if (SawLoad)
      *SawLoad = true;
    return Val;
  }
  default:
    return std::nullopt;
  }
}

MedLLVMEmitter::PointerTableLoadRoleSummary
MedLLVMEmitter::classifyPointerTableLoadRoles(const MedVar &V,
                                              bool RequirePointerWidth) const {
  PointerTableLoadRoleSummary Result;
  if (!CurMedFunc || !Img || V.isConst())
    return Result;

  const unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  if (PtrSize == 0 || PtrSize > 8)
    return Result;
  const unsigned PtrBits = PtrSize * 8;
  const uint64_t PtrMask =
      PtrBits >= 64 ? ~uint64_t(0) : (uint64_t(1) << PtrBits) - 1;

  if (PointerTableLoadRoleCacheFor != CurMedFunc) {
    PointerTableLoadRoleCacheFor = CurMedFunc;
    PointerTableLoadRoleCache.clear();
  }
  const auto CacheKey =
      std::make_tuple(addressProvenanceVarKey(V), RequirePointerWidth);
  if (auto It = PointerTableLoadRoleCache.find(CacheKey);
      It != PointerTableLoadRoleCache.end())
    return It->second;
  auto finish = [&](PointerTableLoadRoleSummary Summary) {
    if (PointerTableLoadRoleCacheFor != CurMedFunc) {
      PointerTableLoadRoleCacheFor = CurMedFunc;
      PointerTableLoadRoleCache.clear();
    }
    PointerTableLoadRoleCache.insert_or_assign(CacheKey, Summary);
    return Summary;
  };

  auto isStableNumericOffset = [&](const MedVar &Offset) {
    if (!Offset.isConst())
      return false;
    if (Offset.Provenance == ConstantAddressProvenance::Scalar)
      return true;
    if (isAddressProvenance(Offset.Provenance) ||
        getVarMayRelocateConstant(Offset.ConstVal, Offset.Size))
      return false;
    return !hasObjectDataProvenance(Offset.ConstVal);
  };
  auto signedOffset = [&](const MedVar &Offset,
                          unsigned ResultBits) -> std::optional<int64_t> {
    std::optional<uint64_t> Folded;
    if (isStableNumericOffset(Offset))
      Folded = Offset.ConstVal;
    else if (valueIsStableAddressOffset(Offset))
      Folded = traceSSAConst(Offset);
    if (!Folded)
      return std::nullopt;
    const unsigned SourceBits =
        Offset.Size == 0 ? ResultBits : static_cast<unsigned>(Offset.Size) * 8;
    if (SourceBits == 0 || SourceBits > 64 || ResultBits == 0 ||
        ResultBits > 64)
      return std::nullopt;
    uint64_t Value = *Folded;
    if (SourceBits < 64)
      Value &= (uint64_t(1) << SourceBits) - 1;
    if (ResultBits < 64) {
      const uint64_t ResultMask = (uint64_t(1) << ResultBits) - 1;
      Value &= ResultMask;
      if ((Value & (uint64_t(1) << (ResultBits - 1))) != 0)
        Value |= ~ResultMask;
    }
    return static_cast<int64_t>(Value);
  };
  auto addSigned = [](int64_t Left, int64_t Right, int64_t &Out) -> bool {
    if ((Right > 0 && Left > std::numeric_limits<int64_t>::max() - Right) ||
        (Right < 0 && Left < std::numeric_limits<int64_t>::min() - Right))
      return false;
    Out = Left + Right;
    return true;
  };
  auto offsetAddress = [&](uint64_t Base, int64_t Delta,
                           uint64_t &Out) -> bool {
    Base &= PtrMask;
    if (Delta >= 0) {
      const uint64_t Magnitude = static_cast<uint64_t>(Delta);
      if (Magnitude > PtrMask || Base > PtrMask - Magnitude)
        return false;
      Out = Base + Magnitude;
      return true;
    }
    const uint64_t Magnitude =
        static_cast<uint64_t>(-(Delta + 1)) + uint64_t(1);
    if (Magnitude > PtrMask || Base < Magnitude)
      return false;
    Out = Base - Magnitude;
    return true;
  };

  // First isolate the LOAD whose value reaches this consumer. Callable mode
  // requires pointer width; memory-representation mode accepts a narrow LOAD
  // only to decide which rebuilt run owns its bytes. Width-preserving pointer
  // +/- scalar operations retain the slot role, but their adjustment is
  // recorded so a callable consumer can reject an interior target rather than
  // treating it as an exact function entry.
  MedVar Current = V;
  for (int Depth = 0; Depth < 16; ++Depth) {
    const MedOp *Def = lookupDef(Current);
    if (!Def)
      return finish(Result);
    if (Def->Opcode == NdOp::LOAD) {
      Result.Load = Def;
      break;
    }
    if (auto Forwarded = pointerPreservingInput(*Def)) {
      Current = *Forwarded;
      continue;
    }
    auto carriesPointerWidth = [&](const MedVar &Input) {
      return Def->Output.Size == PtrSize && Input.Size == PtrSize;
    };
    std::optional<int64_t> Delta;
    MedVar PointerInput;
    if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
      if (auto D = signedOffset(Def->Inputs[1], Def->Output.Size * 8);
          D && carriesPointerWidth(Def->Inputs[0])) {
        Delta = *D;
        PointerInput = Def->Inputs[0];
      } else if (auto D = signedOffset(Def->Inputs[0], Def->Output.Size * 8);
                 D && carriesPointerWidth(Def->Inputs[1])) {
        Delta = *D;
        PointerInput = Def->Inputs[1];
      }
    } else if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2) {
      if (auto D = signedOffset(Def->Inputs[1], Def->Output.Size * 8);
          D && carriesPointerWidth(Def->Inputs[0]) &&
          *D != std::numeric_limits<int64_t>::min()) {
        Delta = -*D;
        PointerInput = Def->Inputs[0];
      }
    }
    if (!Delta)
      return finish(Result);
    Result.ValueAdjustment =
        (Result.ValueAdjustment + static_cast<uint64_t>(*Delta)) & PtrMask;
    Current = PointerInput;
  }
  if (!Result.Load || Result.Load->NumInputs < 1 ||
      Result.Load->Output.Size == 0 ||
      (RequirePointerWidth && Result.Load->Output.Size != PtrSize))
    return finish(Result);

  const MedVar &LoadAddress = Result.Load->Inputs[0];
  struct AddressDomain {
    std::set<uint64_t> Seeds;
    std::optional<int64_t> Step;
    bool Complete = false;
  };
  using SeenSet = std::set<AddressProvenanceVarKey>;
  auto exactAddress = [&](const MedVar &Address) -> std::optional<uint64_t> {
    if (Address.isConst())
      return Address.ConstVal & PtrMask;
    if (auto Folded = traceTableBaseConst(Address))
      return *Folded & PtrMask;
    return std::nullopt;
  };
  auto sameValue = [&](const MedVar &Left, const MedVar &Right) {
    return addressProvenanceVarKey(Left) == addressProvenanceVarKey(Right);
  };

  std::function<std::optional<int64_t>(const MedVar &, const MedVar &, int,
                                       SeenSet)>
      recurrenceDelta = [&](const MedVar &Value, const MedVar &Root, int Depth,
                            SeenSet Seen) -> std::optional<int64_t> {
    if (Depth > 32 || Value.isConst())
      return std::nullopt;
    if (sameValue(Value, Root))
      return int64_t(0);
    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Value);
    if (!Seen.insert(Key).second)
      return std::nullopt;
    const MedOp *Def = lookupDef(Value);
    if (!Def)
      return std::nullopt;
    if (auto Forwarded = pointerPreservingInput(*Def))
      return recurrenceDelta(*Forwarded, Root, Depth + 1, Seen);

    auto addToInput = [&](const MedVar &Input,
                          int64_t Delta) -> std::optional<int64_t> {
      std::optional<int64_t> Prefix =
          recurrenceDelta(Input, Root, Depth + 1, Seen);
      int64_t Combined = 0;
      return Prefix && addSigned(*Prefix, Delta, Combined)
                 ? std::optional<int64_t>(Combined)
                 : std::nullopt;
    };
    if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
      if (auto Delta = signedOffset(Def->Inputs[1], Def->Output.Size * 8))
        if (auto Combined = addToInput(Def->Inputs[0], *Delta))
          return Combined;
      if (auto Delta = signedOffset(Def->Inputs[0], Def->Output.Size * 8))
        return addToInput(Def->Inputs[1], *Delta);
    }
    if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2)
      if (auto Delta = signedOffset(Def->Inputs[1], Def->Output.Size * 8);
          Delta && *Delta != std::numeric_limits<int64_t>::min())
        return addToInput(Def->Inputs[0], -*Delta);
    return std::nullopt;
  };

  std::function<AddressDomain(const MedVar &, int, SeenSet)> analyzeAddress =
      [&](const MedVar &Address, int Depth, SeenSet Seen) -> AddressDomain {
    if (Depth > 32)
      return {};
    if (auto Exact = exactAddress(Address))
      return AddressDomain{{*Exact}, std::nullopt, true};
    if (Address.isConst())
      return {};
    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Address);
    if (!Seen.insert(Key).second)
      return {};

    if (const PhiNode *Phi = lookupPhi(Address)) {
      AddressDomain Domain;
      Domain.Complete = true;
      bool SawReachableArm = false;
      bool SawRecurrence = false;
      for (const auto &[Pred, Arg] : Phi->Args) {
        const PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, Pred);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible) {
          Domain.Complete = false;
          break;
        }
        SawReachableArm = true;
        if (phiIncomingIsRecurrent(*Phi, Pred, Arg)) {
          std::optional<int64_t> Delta =
              recurrenceDelta(Arg, Phi->Output, 0, {});
          if (!Delta || (Domain.Step && *Domain.Step != *Delta)) {
            Domain.Complete = false;
            break;
          }
          Domain.Step = *Delta;
          SawRecurrence = true;
          continue;
        }
        AddressDomain Initial = analyzeAddress(Arg, Depth + 1, Seen);
        if (!Initial.Complete || Initial.Step) {
          Domain.Complete = false;
          break;
        }
        Domain.Seeds.insert(Initial.Seeds.begin(), Initial.Seeds.end());
      }
      if (!SawReachableArm || Domain.Seeds.empty())
        Domain.Complete = false;
      if (SawRecurrence && Domain.Step && *Domain.Step == 0)
        Domain.Step.reset();
      return Domain;
    }

    const MedOp *Def = lookupDef(Address);
    if (!Def)
      return {};
    if (auto Forwarded = pointerPreservingInput(*Def))
      return analyzeAddress(*Forwarded, Depth + 1, Seen);
    if (Def->Opcode == NdOp::SELECT && selectPreservesPointerValues(*Def)) {
      AddressDomain True = analyzeAddress(Def->Inputs[1], Depth + 1, Seen);
      AddressDomain False = analyzeAddress(Def->Inputs[2], Depth + 1, Seen);
      if (!True.Complete || !False.Complete || True.Step != False.Step)
        return {};
      True.Seeds.insert(False.Seeds.begin(), False.Seeds.end());
      return True;
    }
    if (Def->Opcode == NdOp::INT_OR) {
      MedVar Cond, TrueValue, FalseValue;
      if (isMaskedSelectOr(*Def, Cond, TrueValue, FalseValue)) {
        AddressDomain True = analyzeAddress(TrueValue, Depth + 1, Seen);
        AddressDomain False = analyzeAddress(FalseValue, Depth + 1, Seen);
        if (!True.Complete || !False.Complete || True.Step != False.Step)
          return {};
        True.Seeds.insert(False.Seeds.begin(), False.Seeds.end());
        return True;
      }
    }
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
        return {};
      AddressDomain Combined;
      Combined.Complete = true;
      for (const MedVar &Source : Sources) {
        AddressDomain Arm = analyzeAddress(Source, Depth + 1, Seen);
        if (!Arm.Complete ||
            (Combined.Step && Arm.Step && Combined.Step != Arm.Step) ||
            ((Combined.Step || Arm.Step) && Combined.Seeds.size() != 0 &&
             Combined.Step != Arm.Step))
          return {};
        if (!Combined.Step)
          Combined.Step = Arm.Step;
        Combined.Seeds.insert(Arm.Seeds.begin(), Arm.Seeds.end());
      }
      return Combined;
    }

    const MedVar *BaseInput = nullptr;
    std::optional<int64_t> Delta;
    if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
      if (auto D = signedOffset(Def->Inputs[1], Def->Output.Size * 8)) {
        BaseInput = &Def->Inputs[0];
        Delta = *D;
      } else if (auto D = signedOffset(Def->Inputs[0], Def->Output.Size * 8)) {
        BaseInput = &Def->Inputs[1];
        Delta = *D;
      }
    } else if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2) {
      if (auto D = signedOffset(Def->Inputs[1], Def->Output.Size * 8);
          D && *D != std::numeric_limits<int64_t>::min()) {
        BaseInput = &Def->Inputs[0];
        Delta = -*D;
      }
    }
    if (!BaseInput || !Delta)
      return {};
    AddressDomain Base = analyzeAddress(*BaseInput, Depth + 1, Seen);
    if (!Base.Complete)
      return {};
    std::set<uint64_t> Shifted;
    for (uint64_t Seed : Base.Seeds) {
      uint64_t Value = 0;
      if (!offsetAddress(Seed, *Delta, Value))
        return {};
      Shifted.insert(Value);
    }
    Base.Seeds = std::move(Shifted);
    return Base;
  };

  // A section boundary is only a conservative layout bound, not a runtime
  // trip bound.  Admit a tighter bound for the narrow loop shape produced by
  // a pre-tested unsigned counter: the address and counter PHIs must share the
  // same initial/recurrent edges, and the guarded successor must execute the
  // classified load before that recurrent edge can be taken.  Any missing CFG
  // fact leaves the caller on the existing section-span fallback.
  auto guardedIterationCount =
      [&](const MedVar &Address,
          std::optional<int64_t> AddressStep) -> std::optional<size_t> {
    if (!AddressStep || *AddressStep == 0)
      return std::nullopt;
    const PhiNode *AddressPhi = lookupPhi(Address);
    if (!AddressPhi || AddressPhi->Args.size() != 2)
      return std::nullopt;

    std::map<int, const MedBlock *> Blocks;
    const MedBlock *Header = nullptr;
    const MedBlock *LoadBlock = nullptr;
    for (const MedBlock &Block : CurMedFunc->Blocks) {
      if (!Blocks.emplace(Block.Id, &Block).second)
        return std::nullopt;
      for (const PhiNode &Phi : Block.Phis)
        if (&Phi == AddressPhi)
          Header = &Block;
      for (const MedOp &Op : Block.Ops)
        if (&Op == Result.Load)
          LoadBlock = &Block;
    }
    if (!Header || !LoadBlock || CurMedFunc->Blocks.empty() ||
        Header == LoadBlock || Header->Id == CurMedFunc->Blocks.front().Id ||
        LoadBlock->Id == CurMedFunc->Blocks.front().Id ||
        Header->Preds.size() != 2 || Header->Succs.size() != 2 ||
        !Header->ExceptionalSuccs.empty())
      return std::nullopt;
    if (Header->Succs[0] == Header->Succs[1] ||
        Blocks.count(Header->Succs[0]) == 0 ||
        Blocks.count(Header->Succs[1]) == 0)
      return std::nullopt;

    std::set<int> StructuralHeaderPreds;
    std::set<int> StructuralLoadPreds;
    for (const auto &[BlockId, Block] : Blocks) {
      auto recordIncoming = [&](int Succ) {
        if (Succ == Header->Id)
          StructuralHeaderPreds.insert(BlockId);
        if (Succ == LoadBlock->Id)
          StructuralLoadPreds.insert(BlockId);
      };
      for (int Succ : Block->Succs)
        recordIncoming(Succ);
      for (const ExceptionalEdge &Edge : Block->ExceptionalSuccs)
        recordIncoming(Edge.BlockId);
    }
    const std::set<int> RecordedHeaderPreds(Header->Preds.begin(),
                                            Header->Preds.end());
    const std::set<int> LoadPreds(LoadBlock->Preds.begin(),
                                  LoadBlock->Preds.end());
    if (StructuralHeaderPreds != RecordedHeaderPreds ||
        StructuralLoadPreds != LoadPreds)
      return std::nullopt;


    struct PhiEdges {
      int InitialPred = -1;
      const MedVar *InitialValue = nullptr;
      int RecurrentPred = -1;
      const MedVar *RecurrentValue = nullptr;
    };
    auto splitPhiEdges = [&](const PhiNode &Phi,
                             int64_t ExpectedStep) -> std::optional<PhiEdges> {
      if (Phi.Args.size() != 2)
        return std::nullopt;
      PhiEdges Edges;
      for (const auto &[Pred, Arg] : Phi.Args) {
        if (classifyPhiIncomingEdge(Phi, Pred) !=
            PhiEdgeFeasibility::ProvenFeasible)
          return std::nullopt;
        if (phiIncomingIsRecurrent(Phi, Pred, Arg)) {
          if (Edges.RecurrentValue)
            return std::nullopt;
          const std::optional<int64_t> Delta =
              recurrenceDelta(Arg, Phi.Output, 0, {});
          if (!Delta || *Delta != ExpectedStep)
            return std::nullopt;
          Edges.RecurrentPred = Pred;
          Edges.RecurrentValue = &Arg;
        } else {
          if (Edges.InitialValue)
            return std::nullopt;
          Edges.InitialPred = Pred;
          Edges.InitialValue = &Arg;
        }
      }
      if (!Edges.InitialValue || !Edges.RecurrentValue ||
          Edges.InitialPred == Edges.RecurrentPred)
        return std::nullopt;
      return Edges;
    };
    const std::optional<PhiEdges> AddressEdges =
        splitPhiEdges(*AddressPhi, *AddressStep);
    if (!AddressEdges || Address.Size != PtrSize ||
        AddressPhi->Output.Size != PtrSize ||
        AddressEdges->InitialValue->Size != PtrSize ||
        AddressEdges->RecurrentValue->Size != PtrSize)
      return std::nullopt;
    const std::set<int> HeaderPreds(Header->Preds.begin(), Header->Preds.end());
    if (HeaderPreds !=
        std::set<int>{AddressEdges->InitialPred, AddressEdges->RecurrentPred})
      return std::nullopt;
    auto PostLoopBound = [&]() -> std::optional<size_t> {
    auto reachesBlock = [&](int Start, int Goal, int Forbidden) {
      std::vector<int> Work{Start};
      std::set<int> Seen;
      while (!Work.empty()) {
        const int Current = Work.back();
        Work.pop_back();
        if (Current == Goal)
          return true;
        if (Current == Forbidden || !Seen.insert(Current).second)
          continue;
        const auto It = Blocks.find(Current);
        if (It == Blocks.end())
          continue;
        Work.insert(Work.end(), It->second->Succs.begin(), It->second->Succs.end());
        for (const ExceptionalEdge &Edge : It->second->ExceptionalSuccs)
          Work.push_back(Edge.BlockId);
      }
      return false;
    };
    std::function<bool(const MedVar &, const MedVar &, int,
                       std::set<AddressProvenanceVarKey>)>
        dependsOn = [&](const MedVar &Value, const MedVar &Target, int Depth,
                        std::set<AddressProvenanceVarKey> Seen) {
          if (sameValue(Value, Target))
            return true;
          if (Depth > 32 || Value.isConst() ||
              !Seen.insert(addressProvenanceVarKey(Value)).second)
            return false;
          const MedOp *Def = lookupDef(Value);
          if (!Def)
            return false;
          for (uint8_t I = 0; I < Def->NumInputs; ++I)
            if (dependsOn(Def->Inputs[I], Target, Depth + 1, Seen))
              return true;
          return false;
        };
    const int RecurrentBlockId = AddressEdges->RecurrentPred;
    if (Header->Succs[0] != RecurrentBlockId &&
        Header->Succs[1] != RecurrentBlockId)
      return std::nullopt;
    const int MatchBlockId =
        Header->Succs[0] == RecurrentBlockId ? Header->Succs[1] : Header->Succs[0];
    if (LoadBlock->Id == RecurrentBlockId ||
        !reachesBlock(MatchBlockId, LoadBlock->Id, RecurrentBlockId))
      return std::nullopt;
    bool PostReachedLoad = false;
    for (const MedOp &Op : LoadBlock->Ops) {
      if (&Op == Result.Load) {
        PostReachedLoad = true;
        break;
      }
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN)
        return std::nullopt;
    }
    if (!PostReachedLoad)
      return std::nullopt;
    const auto RecurrentIt = Blocks.find(RecurrentBlockId);
    if (RecurrentIt == Blocks.end() || RecurrentIt->second->Succs.size() != 2 ||
        RecurrentIt->second->ExceptionalSuccs.size() != 0 ||
        RecurrentIt->second->Ops.empty() ||
        RecurrentIt->second->Ops.back().Opcode != NdOp::COND_BR)
      return std::nullopt;
    const MedOp &RecurrentBranch = RecurrentIt->second->Ops.back();
    if (RecurrentBranch.NumInputs < 2)
      return std::nullopt;
    std::function<std::optional<int64_t>(const MedVar &, const MedVar &, int,
                                         std::set<AddressProvenanceVarKey>)>
        scalarDelta = [&](const MedVar &Value, const MedVar &Root, int Depth,
                          std::set<AddressProvenanceVarKey> Seen)
        -> std::optional<int64_t> {
      if (Depth > 32 || Value.isConst())
        return std::nullopt;
      if (sameValue(Value, Root))
        return int64_t(0);
      if (!Seen.insert(addressProvenanceVarKey(Value)).second)
        return std::nullopt;
      const MedOp *Def = lookupDef(Value);
      if (!Def || Def->NumInputs < 1)
        return std::nullopt;
      if (Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
          Def->Opcode == NdOp::INT_SEXT ||
          (Def->Opcode == NdOp::SUBBYTES && Def->NumInputs >= 2 &&
           Def->Inputs[1].isConst() && Def->Inputs[1].ConstVal == 0))
        return scalarDelta(Def->Inputs[0], Root, Depth + 1, Seen);
      if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2) {
        auto Delta = signedOffset(Def->Inputs[1], Def->Output.Size * 8);
        if (Delta && *Delta != std::numeric_limits<int64_t>::min()) {
          auto Prefix = scalarDelta(Def->Inputs[0], Root, Depth + 1, Seen);
          int64_t Combined = 0;
          if (Prefix && addSigned(*Prefix, -*Delta, Combined))
            return Combined;
        }
      }
      return std::nullopt;
    };
    for (const PhiNode &Candidate : Header->Phis) {
      if (&Candidate == AddressPhi || Candidate.Args.size() != 2)
        continue;
      const MedVar *InitialValue = nullptr;
      const MedVar *RecurrentValue = nullptr;
      int InitialPred = -1;
      int RecurrentPred = -1;
      bool Valid = true;
      for (const auto &[Pred, Arg] : Candidate.Args) {
        const bool Recurrent = phiIncomingIsRecurrent(Candidate, Pred, Arg);
        const std::optional<int64_t> Delta =
            Recurrent ? std::optional<int64_t>(0)
                      : scalarDelta(Arg, Candidate.Output, 0, {});
        if (Recurrent || (Delta && *Delta == -1)) {
          if (RecurrentValue) {
            Valid = false;
            break;
          }
          RecurrentValue = &Arg;
          RecurrentPred = Pred;
        } else {
          if (InitialValue) {
            Valid = false;
            break;
          }
          InitialValue = &Arg;
          InitialPred = Pred;
        }
      }
      if (!Valid || !InitialValue || !RecurrentValue ||
          InitialPred != AddressEdges->InitialPred ||
          RecurrentPred != AddressEdges->RecurrentPred)
        continue;
      bool DefinedAddressUpdate = false;
      bool DefinedCounterUpdate = false;
      for (const MedOp &Op : RecurrentIt->second->Ops) {
        DefinedAddressUpdate |= sameValue(Op.Output, *AddressEdges->RecurrentValue);
        DefinedCounterUpdate |= sameValue(Op.Output, *RecurrentValue);
      }
      if (!DefinedAddressUpdate || !DefinedCounterUpdate)
        continue;
      const std::optional<uint64_t> Initial = traceControlConst(*InitialValue);
      if (!Initial || *Initial == 0 ||
          *Initial > static_cast<uint64_t>(limits::kMaxSSANodes))
        continue;
      if (!dependsOn(RecurrentBranch.Inputs[1], Candidate.Output, 0, {}))
        continue;
      return static_cast<size_t>(*Initial);
    }
    return std::nullopt;
  }();
  if (PostLoopBound)
    return PostLoopBound;
  const MedOp *Branch = nullptr;
    for (const MedOp &Op : Header->Ops) {
      if (Op.Opcode == NdOp::COND_BR) {
        if (Branch)
          return std::nullopt;
        Branch = &Op;
      } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::INDIR_BR ||
                 Op.Opcode == NdOp::RETURN) {
        return std::nullopt;
      }
    }
    if (!Branch || Branch->NumInputs < 2 || !Branch->Inputs[0].isConst() ||
        Branch->Inputs[0].ConstVal == 0 || Header->Ops.empty() ||
        &Header->Ops.back() != Branch)
      return std::nullopt;

    bool ReachedLoad = false;
    for (const MedOp &Op : LoadBlock->Ops) {
      if (&Op == Result.Load) {
        ReachedLoad = true;
        break;
      }
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN)
        return std::nullopt;
    }
    if (!ReachedLoad)
      return std::nullopt;

    auto blockAddress = [&](int Id) -> std::optional<va_t> {
      auto It = Blocks.find(Id);
      if (It == Blocks.end())
        return std::nullopt;
      const MedBlock &Block = *It->second;
      return Block.StartAddr != 0 || Block.Ops.empty()
                 ? std::optional<va_t>(Block.StartAddr)
                 : std::optional<va_t>(Block.Ops.front().Addr);
    };
    std::optional<int> Taken;
    for (int Succ : Header->Succs) {
      const std::optional<va_t> Address = blockAddress(Succ);
      if (Address && *Address == Branch->Inputs[0].ConstVal) {
        if (Taken)
          return std::nullopt;
        Taken = Succ;
      }
    }
    if (!Taken)
      return std::nullopt;
    const int Fallthrough =
        *Taken == Header->Succs[0] ? Header->Succs[1] : Header->Succs[0];
    if (LoadBlock->Id != *Taken && LoadBlock->Id != Fallthrough)
      return std::nullopt;
    const bool LoadOnBranchTrue = LoadBlock->Id == *Taken;
    const int ExitSucc = LoadOnBranchTrue ? Fallthrough : *Taken;

    MedVar Condition = Branch->Inputs[1];
    bool Negated = false;
    const MedOp *Compare = nullptr;
    for (int Depth = 0; Depth < 16 && !Condition.isConst(); ++Depth) {
      const MedOp *Def = lookupDef(Condition);
      if (!Def)
        return std::nullopt;
      if (Def->Opcode == NdOp::BOOL_NOT && Def->NumInputs >= 1) {
        Negated = !Negated;
        Condition = Def->Inputs[0];
        continue;
      }
      if (Def->Opcode == NdOp::INT_LESS && Def->NumInputs >= 2) {
        Compare = Def;
        break;
      }
      const std::optional<MedVar> Forwarded = pointerPreservingInput(*Def);
      if (!Forwarded)
        return std::nullopt;
      Condition = *Forwarded;
    }
    if (!Compare || LoadOnBranchTrue != !Negated)
      return std::nullopt;

    const MedVar &Counter = Compare->Inputs[0];
    const MedVar &BoundValue = Compare->Inputs[1];
    const PhiNode *CounterPhi = lookupPhi(Counter);
    if (!CounterPhi || CounterPhi == AddressPhi || Counter.Size == 0 ||
        Counter.Size > 8 || BoundValue.Size != Counter.Size ||
        !sameValue(CounterPhi->Output, Counter))
      return std::nullopt;
    bool CounterOwnedByHeader = false;
    for (const PhiNode &Phi : Header->Phis)
      CounterOwnedByHeader |= &Phi == CounterPhi;
    if (!CounterOwnedByHeader)
      return std::nullopt;
    const std::optional<PhiEdges> CounterEdges = splitPhiEdges(*CounterPhi, 1);
    if (!CounterEdges ||
        CounterEdges->InitialPred != AddressEdges->InitialPred ||
        CounterEdges->RecurrentPred != AddressEdges->RecurrentPred ||
        CounterEdges->InitialValue->Size != Counter.Size ||
        CounterEdges->RecurrentValue->Size != Counter.Size)
      return std::nullopt;

    std::function<std::optional<uint64_t>(const MedVar &, int, SeenSet)>
        scalarConstant = [&](const MedVar &Value, int Depth,
                             SeenSet Seen) -> std::optional<uint64_t> {
      if (Depth > 16 || Value.Size != Counter.Size)
        return std::nullopt;
      if (Value.isConst()) {
        if (!isStableNumericOffset(Value))
          return std::nullopt;
        const unsigned Bits =
            Value.Size == 0 ? PtrBits : static_cast<unsigned>(Value.Size) * 8;
        if (Bits == 0 || Bits > 64)
          return std::nullopt;
        const uint64_t Mask =
            Bits == 64 ? ~uint64_t(0) : (uint64_t(1) << Bits) - uint64_t(1);
        return Value.ConstVal & Mask;
      }
      const AddressProvenanceVarKey Key = addressProvenanceVarKey(Value);
      if (!Seen.insert(Key).second)
        return std::nullopt;
      const MedOp *Def = lookupDef(Value);
      if (!Def)
        return std::nullopt;
      const std::optional<MedVar> Forwarded = pointerPreservingInput(*Def);
      return Forwarded ? scalarConstant(*Forwarded, Depth + 1, Seen)
                       : std::nullopt;
    };
    const std::optional<uint64_t> InitialCounter =
        scalarConstant(*CounterEdges->InitialValue, 0, {});
    const std::optional<uint64_t> Bound = scalarConstant(BoundValue, 0, {});
    if (!InitialCounter || *InitialCounter != 0 || !Bound || *Bound == 0 ||
        *Bound > static_cast<uint64_t>(limits::kMaxSSANodes))
      return std::nullopt;

    auto canReachWithoutHeader = [&](int Start, int Goal) {
      std::vector<int> Work{Start};
      std::set<int> Seen;
      while (!Work.empty()) {
        const int Current = Work.back();
        Work.pop_back();
        if (Current == Goal)
          return true;
        if (!Seen.insert(Current).second)
          continue;
        auto It = Blocks.find(Current);
        if (It == Blocks.end())
          continue;
        auto enqueue = [&](int Succ) {
          if (Succ == Goal || Succ != Header->Id)
            Work.push_back(Succ);
        };
        for (int Succ : It->second->Succs)
          enqueue(Succ);
        for (const ExceptionalEdge &Edge : It->second->ExceptionalSuccs)
          enqueue(Edge.BlockId);
      }
      return false;
    };
    if (!canReachWithoutHeader(LoadBlock->Id, AddressEdges->RecurrentPred) ||
        canReachWithoutHeader(LoadBlock->Id, AddressEdges->InitialPred) ||
        canReachWithoutHeader(ExitSucc, LoadBlock->Id) ||
        canReachWithoutHeader(ExitSucc, AddressEdges->RecurrentPred) ||
        canReachWithoutHeader(ExitSucc, Header->Id))
      return std::nullopt;

    return static_cast<size_t>(*Bound);
  };
  auto boundedScalarTermCount = [&](const MedVar &Term) -> std::optional<size_t> {
    auto sameValue = [](const MedVar &Left, const MedVar &Right) {
      return addressProvenanceVarKey(Left) == addressProvenanceVarKey(Right);
    };
    std::map<int, const MedBlock *> Blocks;
    for (const MedBlock &Block : CurMedFunc->Blocks)
      Blocks.emplace(Block.Id, &Block);
    std::function<std::optional<int64_t>(const MedVar &, const MedVar &, int,
                                         std::set<AddressProvenanceVarKey>)>
        scalarDelta = [&](const MedVar &Value, const MedVar &Root, int Depth,
                          std::set<AddressProvenanceVarKey> Seen)
        -> std::optional<int64_t> {
      if (Depth > 32 || Value.isConst())
        return std::nullopt;
      if (sameValue(Value, Root))
        return int64_t(0);
      if (!Seen.insert(addressProvenanceVarKey(Value)).second)
        return std::nullopt;
      const MedOp *Def = lookupDef(Value);
      if (!Def || Def->NumInputs < 1)
        return std::nullopt;
      if (Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
          Def->Opcode == NdOp::INT_SEXT ||
          (Def->Opcode == NdOp::SUBBYTES && Def->NumInputs >= 2 &&
           Def->Inputs[1].isConst() && Def->Inputs[1].ConstVal == 0))
        return scalarDelta(Def->Inputs[0], Root, Depth + 1, Seen);
      if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
        if (auto Delta = signedOffset(Def->Inputs[1], Def->Output.Size * 8))
          if (auto Prefix = scalarDelta(Def->Inputs[0], Root, Depth + 1, Seen)) {
            int64_t Combined = 0;
            if (addSigned(*Prefix, *Delta, Combined))
              return Combined;
          }
        if (auto Delta = signedOffset(Def->Inputs[0], Def->Output.Size * 8))
          if (auto Prefix = scalarDelta(Def->Inputs[1], Root, Depth + 1, Seen)) {
            int64_t Combined = 0;
            if (addSigned(*Prefix, *Delta, Combined))
              return Combined;
          }
      }
      if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2)
        if (auto Delta = signedOffset(Def->Inputs[1], Def->Output.Size * 8);
            Delta && *Delta != std::numeric_limits<int64_t>::min())
          if (auto Prefix = scalarDelta(Def->Inputs[0], Root, Depth + 1, Seen)) {
            int64_t Combined = 0;
            if (addSigned(*Prefix, -*Delta, Combined))
              return Combined;
          }
      return std::nullopt;
    };
    auto blockAddress = [&](const MedBlock *Block) -> std::optional<va_t> {
      if (!Block)
        return std::nullopt;
      return Block->StartAddr != 0 || Block->Ops.empty()
                 ? std::optional<va_t>(Block->StartAddr)
                 : std::optional<va_t>(Block->Ops.front().Addr);
    };
    std::function<bool(const MedVar &, const MedVar &, int,
                       std::set<AddressProvenanceVarKey>)>
        dependsOn = [&](const MedVar &Value, const MedVar &Target, int Depth,
                        std::set<AddressProvenanceVarKey> Seen) {
      if (sameValue(Value, Target))
        return true;
      if (Depth > 32 || Value.isConst() ||
          !Seen.insert(addressProvenanceVarKey(Value)).second)
        return false;
      const MedOp *Def = lookupDef(Value);
      if (!Def)
        return false;
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (dependsOn(Def->Inputs[I], Target, Depth + 1, Seen))
          return true;
      return false;
    };
    auto reaches = [&](int Start, int Goal, int Forbidden, int HeaderId) {
      std::vector<int> Work{Start};
      std::set<int> Seen;
      while (!Work.empty()) {
        const int Current = Work.back();
        Work.pop_back();
        if (Current == Goal)
          return true;
        if (Current == Forbidden || Current == HeaderId ||
            !Seen.insert(Current).second)
          continue;
        const auto It = Blocks.find(Current);
        if (It == Blocks.end())
          continue;
        Work.insert(Work.end(), It->second->Succs.begin(), It->second->Succs.end());
        for (const ExceptionalEdge &Edge : It->second->ExceptionalSuccs)
          Work.push_back(Edge.BlockId);
      }
      return false;
    };
    std::function<std::optional<size_t>(const PhiNode *)> countForPhi =
        [&](const PhiNode *Phi) -> std::optional<size_t> {
      if (!Phi || Phi->Args.size() != 2 || Phi->Output.Size == 0 ||
          Phi->Output.Size > 8)
        return std::nullopt;
      const MedBlock *Header = nullptr;
      for (const MedBlock &Block : CurMedFunc->Blocks)
        for (const PhiNode &Candidate : Block.Phis)
          if (&Candidate == Phi)
            Header = &Block;
      const MedBlock *LoadBlock = nullptr;
      for (const MedBlock &Block : CurMedFunc->Blocks)
        for (const MedOp &Op : Block.Ops)
          if (&Op == Result.Load)
            LoadBlock = &Block;
      if (!LoadBlock)
        return std::nullopt;
      if (!Header || Header->Preds.size() != 2 || Header->Succs.size() != 2 ||
          Header->ExceptionalSuccs.size() != 0)
        return std::nullopt;
      const MedVar *InitialValue = nullptr;
      const MedVar *RecurrentValue = nullptr;
      int InitialPred = -1;
      int RecurrentPred = -1;
      for (const auto &[Pred, Arg] : Phi->Args) {
        if (classifyPhiIncomingEdge(*Phi, Pred) !=
            PhiEdgeFeasibility::ProvenFeasible)
          return std::nullopt;
        std::set<AddressProvenanceVarKey> Seen;
        const auto Delta = scalarDelta(Arg, Phi->Output, 0, Seen);
        if (Delta && *Delta == 1) {
          if (RecurrentValue)
            return std::nullopt;
          RecurrentValue = &Arg;
          RecurrentPred = Pred;
        } else {
          if (InitialValue)
            return std::nullopt;
          InitialValue = &Arg;
          InitialPred = Pred;
        }
      }
      if (!InitialValue || !RecurrentValue || InitialPred == RecurrentPred)
        return std::nullopt;
      const auto Initial = traceControlConst(*InitialValue);
      if (!Initial || *Initial > static_cast<uint64_t>(limits::kMaxSSANodes))
        return std::nullopt;
      const auto RecurrentIt = Blocks.find(RecurrentPred);
      if (RecurrentIt == Blocks.end() ||
          RecurrentIt->second->Succs.size() != 2 ||
          RecurrentIt->second->ExceptionalSuccs.size() != 0 ||
          RecurrentIt->second->Ops.empty() ||
          RecurrentIt->second->Ops.back().Opcode != NdOp::COND_BR)
        return std::nullopt;
      const MedOp &Branch = RecurrentIt->second->Ops.back();
      if (Branch.NumInputs < 2 || !Branch.Inputs[0].isConst())
        return std::nullopt;
      if (std::find(Header->Succs.begin(), Header->Succs.end(), RecurrentPred) ==
          Header->Succs.end())
        return std::nullopt;
      const int MatchSucc = Header->Succs[0] == RecurrentPred
                                ? Header->Succs[1]
                                : Header->Succs[0];
      if (LoadBlock->Id == RecurrentPred ||
          !reaches(MatchSucc, LoadBlock->Id, RecurrentPred, Header->Id))
        return std::nullopt;
      bool ReachedLoad = false;
      for (const MedOp &Op : LoadBlock->Ops) {
        if (&Op == Result.Load) {
          ReachedLoad = true;
          break;
        }
        if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
            Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN)
          return std::nullopt;
      }
      if (!ReachedLoad)
        return std::nullopt;
      const auto HeaderAddress = blockAddress(Header);
      bool BackedgeOnTrue = false;
      bool SawBranchTarget = false;
      for (int Succ : RecurrentIt->second->Succs) {
        const auto SuccAddress = blockAddress(Blocks[Succ]);
        if (!SuccAddress || *SuccAddress == Branch.Inputs[0].ConstVal) {
          if (SuccAddress)
            SawBranchTarget = true;
          continue;
        }
      }
      if (HeaderAddress && *HeaderAddress == Branch.Inputs[0].ConstVal) {
        BackedgeOnTrue = true;
        SawBranchTarget = true;
      } else {
        for (int Succ : RecurrentIt->second->Succs)
          if (auto SuccAddress = blockAddress(Blocks[Succ]);
              SuccAddress && *SuccAddress == Branch.Inputs[0].ConstVal)
            SawBranchTarget = true;
      }
      if (!SawBranchTarget)
        return std::nullopt;
      MedVar Condition = Branch.Inputs[1];
      bool Negated = false;
      const MedOp *Compare = nullptr;
      for (int Depth = 0; Depth < 16 && !Condition.isConst(); ++Depth) {
        const MedOp *Def = lookupDef(Condition);
        if (!Def)
          return std::nullopt;
        if (Def->Opcode == NdOp::BOOL_NOT && Def->NumInputs >= 1) {
          Negated = !Negated;
          Condition = Def->Inputs[0];
          continue;
        }
        if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1) {
          Condition = Def->Inputs[0];
          continue;
        }
        if ((Def->Opcode == NdOp::INT_NOTEQUAL ||
             Def->Opcode == NdOp::INT_EQUAL ||
             Def->Opcode == NdOp::INT_LESS ||
             Def->Opcode == NdOp::INT_LESSEQUAL) &&
            Def->NumInputs >= 2) {
          Compare = Def;
          break;
        }
        return std::nullopt;
      }
      if (!Compare)
        return std::nullopt;
      int Related = -1;
      int Other = -1;
      for (int I = 0; I < 2; ++I)
        if (dependsOn(Compare->Inputs[I], *RecurrentValue, 0, {}))
          if (Related != -1)
            return std::nullopt;
          else
            Related = I;
      if (Related == -1)
        return std::nullopt;
      Other = 1 - Related;
      const auto Bound = traceControlConst(Compare->Inputs[Other]);
      if (!Bound || *Bound > static_cast<uint64_t>(limits::kMaxSSANodes))
        return std::nullopt;
      const bool CompareTrueBackedge = BackedgeOnTrue != Negated;
      const bool LoopCondition =
          Compare->Opcode == NdOp::INT_EQUAL ? !CompareTrueBackedge
          : Compare->Opcode == NdOp::INT_LESSEQUAL ? CompareTrueBackedge
          : CompareTrueBackedge;
      if (!LoopCondition || *Bound < *Initial)
        return std::nullopt;
      uint64_t Count = *Bound - *Initial;
      if (Compare->Opcode == NdOp::INT_LESSEQUAL) {
        if (Count == std::numeric_limits<uint64_t>::max())
          return std::nullopt;
        ++Count;
      }
      if (Count == 0 || Count > static_cast<uint64_t>(limits::kMaxSSANodes))
        return std::nullopt;
      return static_cast<size_t>(Count);
    };
    std::function<std::optional<size_t>(const MedVar &,
                                         std::set<AddressProvenanceVarKey>)>
        findBound = [&](const MedVar &Value,
                        std::set<AddressProvenanceVarKey> Seen)
        -> std::optional<size_t> {
      if (Value.isConst() || !Seen.insert(addressProvenanceVarKey(Value)).second)
        return std::nullopt;
      if (const PhiNode *Phi = lookupPhi(Value))
        if (auto Count = countForPhi(Phi))
          return Count;
      const MedOp *Def = lookupDef(Value);
      if (!Def || Def->NumInputs == 0 || Def->Opcode == NdOp::LOAD)
        return std::nullopt;
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (auto Count = findBound(Def->Inputs[I], Seen))
          return Count;
      return std::nullopt;
    };
    return findBound(Term, {});
  };

  AddressDomain Domain = analyzeAddress(LoadAddress, 0, {});
  const std::optional<size_t> GuardedIterations =
      Domain.Complete ? guardedIterationCount(LoadAddress, Domain.Step)
      : std::nullopt;
  const std::optional<uint64_t> DiscoveredRun =
      ptrTableUniqueSegment(LoadAddress,
                            /*IncludeSymbolizedEvidence=*/true);
  auto pointerLanes = [&](const std::set<uint64_t> *KnownBases,
                          const std::vector<MedVar> *KnownTerms,
                          bool SubtractKnownTerms) {
    return analyzeIndexedPointerLane(
        LoadAddress, Img, PtrSize,
        [&](const MedVar &Value) { return lookupDef(Value); },
        [&](const MedVar &Value) { return traceSSAConst(Value); },
        [&](const MedVar &Value, uint64_t &Base, bool &HaveBase,
            std::vector<MedVar> &Terms) {
          if (collectIndexedGlobalBase(Value, Base, HaveBase, Terms) &&
              HaveBase)
            return true;
          Base = 0;
          HaveBase = false;
          Terms.clear();
          return collectLiteralPoolBase(Value, Base, HaveBase, Terms);
        },
        [&](const MedVar &Value) {
          return ptrTableUniqueSegment(Value,
                                       /*IncludeSymbolizedEvidence=*/true);
        },
        [&](const Segment *Segment, uint64_t &RunStart, uint64_t &RunEnd) {
          readOnlyAfterRelocRun(Segment, RunStart, RunEnd);
        },
        KnownBases, KnownTerms, SubtractKnownTerms);
  };
  if (!Domain.Complete || Domain.Seeds.empty()) {
    IndexedPointerLaneSummary Lane;
    const MedOp *AddressDef = lookupDef(LoadAddress);
    if (AddressDef && AddressDef->NumInputs >= 2 &&
        (AddressDef->Opcode == NdOp::INT_ADD ||
         AddressDef->Opcode == NdOp::INT_SUB)) {
      auto analyzeBaseAndTerm = [&](const MedVar &BaseValue, const MedVar &Term,
                                    bool SubtractTerm) {
        if (!valueIsStableAddressOffset(Term))
          return IndexedPointerLaneSummary{};
        AddressDomain Bases = analyzeAddress(BaseValue, 0, {});
        if (!Bases.Complete || Bases.Step || Bases.Seeds.empty())
          return IndexedPointerLaneSummary{};
        std::vector<MedVar> Terms{Term};
        return pointerLanes(&Bases.Seeds, &Terms, SubtractTerm);
      };
      Lane = analyzeBaseAndTerm(AddressDef->Inputs[0], AddressDef->Inputs[1],
                                AddressDef->Opcode == NdOp::INT_SUB);
      if (!Lane.Complete && AddressDef->Opcode == NdOp::INT_ADD)
        Lane = analyzeBaseAndTerm(AddressDef->Inputs[1], AddressDef->Inputs[0],
                                  false);
    }
    if (!Lane.Complete)
      Lane = pointerLanes(nullptr, nullptr, false);
    std::optional<size_t> LaneBound;
    if (Lane.Complete && Lane.IndexTerms.size() == 1)
      LaneBound = boundedScalarTermCount(Lane.IndexTerms.front());
    if (LaneBound) {
      if (*LaneBound == 0 || Lane.Slots.size() < *LaneBound)
        Lane = {};
      else {
        std::set<uint64_t> BoundedSlots;
        auto Slot = Lane.Slots.begin();
        for (size_t I = 0; I < *LaneBound; ++I, ++Slot)
          BoundedSlots.insert(*Slot);
        Lane.Slots = std::move(BoundedSlots);
      }
    }
    const bool StableTerms =
        Lane.Complete &&
        std::all_of(Lane.IndexTerms.begin(), Lane.IndexTerms.end(),
                    [&](const MedVar &Term) {
                      return valueIsStableAddressOffset(Term);
                    });
    if (StableTerms) {
      Domain.Seeds = Lane.Slots;
      Domain.Complete = true;
    }
  }
  if (!Domain.Complete || Domain.Seeds.empty()) {
    Result.Recognized = DiscoveredRun.has_value();
    return finish(Result);
  }

  const Segment *OwnerSegment = nullptr;
  const Section *OwnerSection = nullptr;
  uint64_t RunStart = 0;
  uint64_t RunEnd = 0;
  for (uint64_t Seed : Domain.Seeds) {
    const Segment *Segment = Img->getSegmentFor(Seed);
    if (!Segment || Segment->isExecutable() || !segHasPtrRelocSlots(Segment)) {
      Result.Recognized |= DiscoveredRun.has_value();
      return finish(Result);
    }
    uint64_t SeedRunStart = 0;
    uint64_t SeedRunEnd = 0;
    readOnlyAfterRelocRun(Segment, SeedRunStart, SeedRunEnd);
    if (Seed < SeedRunStart || Seed >= SeedRunEnd ||
        PtrSize > SeedRunEnd - Seed) {
      Result.Recognized = true;
      return finish(Result);
    }
    if (!OwnerSegment) {
      OwnerSegment = Segment;
      RunStart = SeedRunStart;
      RunEnd = SeedRunEnd;
      OwnerSection = Img->getSectionFor(Seed);
    } else if (RunStart != SeedRunStart || RunEnd != SeedRunEnd) {
      Result.Recognized = true;
      return finish(Result);
    }
    if (Domain.Step && Img->getSectionFor(Seed) != OwnerSection) {
      Result.Recognized = true;
      return finish(Result);
    }
  }
  Result.Recognized = true;

  if (!Domain.Step) {
    Result.Slots = Domain.Seeds;
  } else {
    uint64_t BoundStart = RunStart;
    uint64_t BoundEnd = RunEnd;
    if (OwnerSection) {
      if (!OwnerSection->isReadable() || OwnerSection->isExecutable() ||
          OwnerSection->Size > InvalidVA - OwnerSection->VA) {
        return finish(Result);
      }
      BoundStart = OwnerSection->VA;
      BoundEnd = OwnerSection->VA + OwnerSection->Size;
    }
    const int64_t Step = *Domain.Step;
    if (Step == 0 || Step == std::numeric_limits<int64_t>::min())
      return finish(Result);
    size_t Remaining = static_cast<size_t>(limits::kMaxSSANodes);
    for (uint64_t Seed : Domain.Seeds) {
      uint64_t Slot = Seed;
      std::optional<size_t> Iterations = GuardedIterations;
      while (Slot >= BoundStart && Slot < BoundEnd &&
             PtrSize <= BoundEnd - Slot && (!Iterations || *Iterations != 0)) {
        if (Remaining == 0)
          return finish(Result);
        --Remaining;
        if (Iterations)
          --*Iterations;
        Result.Slots.insert(Slot);
        if (Iterations && *Iterations == 0)
          break;
        uint64_t Next = 0;
        if (!offsetAddress(Slot, Step, Next))
          return finish(Result);
        if ((Step > 0 && Next <= Slot) || (Step < 0 && Next >= Slot))
          return finish(Result);
        Slot = Next;
      }
      if (Iterations && *Iterations != 0)
        return finish(Result);
    }
  }
  if (Result.Slots.empty())
    return finish(Result);

  {
    const MedBlock *LoadBlock = nullptr;
    for (const MedBlock &Block : CurMedFunc->Blocks)
      for (const MedOp &Op : Block.Ops)
        if (&Op == Result.Load)
          LoadBlock = &Block;
    if (LoadBlock) {
      const MedOp *GuardBranch = nullptr;
      bool AfterLoad = false;
      for (const MedOp &Op : LoadBlock->Ops) {
        if (&Op == Result.Load) {
          AfterLoad = true;
          continue;
        }
        if (AfterLoad && Op.Opcode == NdOp::COND_BR) {
          GuardBranch = &Op;
          break;
        }
      }
      auto sameOccurrence = [](const MedVar &Left, const MedVar &Right) {
        return addressProvenanceVarKey(Left) ==
               addressProvenanceVarKey(Right);
      };
      std::function<bool(const MedVar &, const MedVar &, int,
                         std::set<AddressProvenanceVarKey>)>
          dependsOn = [&](const MedVar &Value, const MedVar &Target, int Depth,
                          std::set<AddressProvenanceVarKey> Seen) {
        if (sameOccurrence(Value, Target))
          return true;
        if (Depth > 32 || Value.isConst() ||
            !Seen.insert(addressProvenanceVarKey(Value)).second)
          return false;
        if (const PhiNode *Phi = lookupPhi(Value)) {
          bool SawFeasible = false;
          for (const auto &[Pred, Arg] : Phi->Args) {
            if (classifyPhiIncomingEdge(*Phi, Pred) !=
                PhiEdgeFeasibility::ProvenFeasible)
              continue;
            SawFeasible = true;
            if (dependsOn(Arg, Target, Depth + 1, Seen))
              return true;
          }
          return false;
        }
        const MedOp *Def = lookupDef(Value);
        if (!Def)
          return false;
        for (uint8_t I = 0; I < Def->NumInputs; ++I)
          if (dependsOn(Def->Inputs[I], Target, Depth + 1, Seen))
            return true;
        return false;
      };
      auto blockAddress = [&](const MedBlock *Block) -> std::optional<va_t> {
        if (!Block)
          return std::nullopt;
        return Block->StartAddr != 0 || Block->Ops.empty()
                   ? std::optional<va_t>(Block->StartAddr)
                   : std::optional<va_t>(Block->Ops.front().Addr);
      };
      auto containsIndirectCall = [&](int Start) {
        std::vector<int> Work{Start};
        std::set<int> Seen;
        while (!Work.empty()) {
          const int BlockId = Work.back();
          Work.pop_back();
          if (!Seen.insert(BlockId).second)
            continue;
          const auto It = std::find_if(
              CurMedFunc->Blocks.begin(), CurMedFunc->Blocks.end(),
              [&](const MedBlock &Block) { return Block.Id == BlockId; });
          if (It == CurMedFunc->Blocks.end())
            continue;
          for (const MedOp &Op : It->Ops) {
            if (Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs >= 1)
              for (uint8_t I = 0; I < Op.NumInputs; ++I)
                if (dependsOn(Op.Inputs[I], V, 0, {}))
                  return true;
            if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::RETURN)
              return false;
          }
          Work.insert(Work.end(), It->Succs.begin(), It->Succs.end());
        }
        return false;
      };
      if (GuardBranch && GuardBranch->NumInputs >= 2 &&
          GuardBranch->Inputs[0].isConst() &&
          LoadBlock->Succs.size() == 2) {
        int TrueSucc = -1;
        for (int Succ : LoadBlock->Succs)
          if (auto Address = blockAddress(
                  CurMedFunc->Blocks.empty() ? nullptr :
                  &*std::find_if(CurMedFunc->Blocks.begin(),
                                 CurMedFunc->Blocks.end(),
                                 [&](const MedBlock &Block) {
                                   return Block.Id == Succ;
                                 }));
              Address && *Address == GuardBranch->Inputs[0].ConstVal)
            TrueSucc = Succ;
        const int FalseSucc =
            TrueSucc == LoadBlock->Succs[0] ? LoadBlock->Succs[1]
                                            : LoadBlock->Succs[0];
        const bool CallOnTrue = TrueSucc >= 0 && containsIndirectCall(TrueSucc);
        const bool CallOnFalse = TrueSucc >= 0 && containsIndirectCall(FalseSucc);
        if (TrueSucc >= 0 && CallOnTrue != CallOnFalse) {
          std::function<std::optional<bool>(const MedVar &, const MedVar &,
                                             int,
                                             std::set<AddressProvenanceVarKey>)>
              highBitPredicate = [&](const MedVar &Condition,
                                     const MedVar &GuardValue, int Depth,
                                     std::set<AddressProvenanceVarKey> Seen)
              -> std::optional<bool> {
            if (Depth > 16 || Condition.isConst() ||
                !Seen.insert(addressProvenanceVarKey(Condition)).second)
              return std::nullopt;
            const MedOp *Def = lookupDef(Condition);
            if (!Def)
              return std::nullopt;
            if (Def->Opcode == NdOp::BOOL_NOT && Def->NumInputs >= 1)
              if (auto Predicate = highBitPredicate(
                      Def->Inputs[0], GuardValue, Depth + 1, Seen))
                return !*Predicate;
            if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1)
              return highBitPredicate(Def->Inputs[0], GuardValue, Depth + 1,
                                       Seen);
            if ((Def->Opcode == NdOp::INT_NOTEQUAL ||
                 Def->Opcode == NdOp::INT_EQUAL) &&
                Def->NumInputs >= 2) {
              int ValueInput = -1;
              for (int I = 0; I < 2; ++I)
                if (Def->Inputs[I].isConst() && Def->Inputs[I].ConstVal == 0)
                  ValueInput = 1 - I;
              if (ValueInput >= 0) {
                const MedOp *MaskDef = lookupDef(Def->Inputs[ValueInput]);
                if (MaskDef && MaskDef->Opcode == NdOp::INT_AND &&
                    MaskDef->NumInputs >= 2) {
                  int ShiftInput = -1;
                  for (int I = 0; I < 2; ++I)
                    if (MaskDef->Inputs[I].isConst() &&
                        MaskDef->Inputs[I].ConstVal == 1)
                      ShiftInput = 1 - I;
                  if (ShiftInput >= 0) {
                    const MedOp *ShiftDef =
                        lookupDef(MaskDef->Inputs[ShiftInput]);
                    if (ShiftDef && ShiftDef->Opcode == NdOp::INT_RIGHT &&
                        ShiftDef->NumInputs >= 2 &&
                        ShiftDef->Inputs[1].ConstVal ==
                            (ShiftDef->Inputs[0].Size * 8 - 1) &&
                        ShiftDef->Inputs[0].Size > 0 &&
                        dependsOn(ShiftDef->Inputs[0], GuardValue, 0, {}))
                      return Def->Opcode == NdOp::INT_NOTEQUAL;
                  }
                }
              }
            }
            return std::nullopt;
          };
          std::vector<const MedOp *> GuardLoads;
          std::function<void(const MedVar &,
                             std::set<AddressProvenanceVarKey>)> collectLoads =
              [&](const MedVar &Value,
                  std::set<AddressProvenanceVarKey> Seen) {
                if (Value.isConst() ||
                    !Seen.insert(addressProvenanceVarKey(Value)).second)
                  return;
                const MedOp *Def = lookupDef(Value);
                if (!Def)
                  return;
                if (Def->Opcode == NdOp::LOAD) {
                  GuardLoads.push_back(Def);
                  return;
                }
                for (uint8_t I = 0; I < Def->NumInputs; ++I)
                  collectLoads(Def->Inputs[I], Seen);
              };
          collectLoads(GuardBranch->Inputs[1], {});
          const MedOp *GuardLoad = nullptr;
          std::optional<int64_t> GuardOffset;
          for (const MedOp *Candidate : GuardLoads) {
            if (Candidate->NumInputs < 1)
              continue;
            if (sameOccurrence(Candidate->Inputs[0], LoadAddress)) {
              GuardLoad = Candidate;
              GuardOffset = 0;
              break;
            }
            const MedOp *AddressDef = lookupDef(Candidate->Inputs[0]);
            if (!AddressDef || AddressDef->NumInputs < 2)
              continue;
            if (AddressDef->Opcode == NdOp::INT_ADD) {
              if (sameOccurrence(AddressDef->Inputs[0], LoadAddress))
                GuardOffset = signedOffset(AddressDef->Inputs[1], PtrBits);
              else if (sameOccurrence(AddressDef->Inputs[1], LoadAddress))
                GuardOffset = signedOffset(AddressDef->Inputs[0], PtrBits);
            } else if (AddressDef->Opcode == NdOp::INT_SUB &&
                       sameOccurrence(AddressDef->Inputs[0], LoadAddress)) {
              if (auto Offset = signedOffset(AddressDef->Inputs[1], PtrBits);
                  Offset && *Offset != std::numeric_limits<int64_t>::min())
                GuardOffset = -*Offset;
            }
            if (GuardOffset) {
              GuardLoad = Candidate;
              break;
            }
          }
          if (GuardLoad && GuardOffset && GuardLoad->Output.Size > 0 &&
              GuardLoad->Output.Size <= 8) {
            const auto HighBit = highBitPredicate(GuardBranch->Inputs[1],
                                                  GuardLoad->Output, 0, {});
            if (HighBit) {
              std::set<uint64_t> CallableSlots;
              std::set<uint64_t> GuardSlots = Result.Slots;
              uint64_t TableBase = 0;
              bool HaveTableBase = false;
              std::vector<MedVar> TableTerms;
              if (collectIndexedGlobalBase(LoadAddress, TableBase,
                                            HaveTableBase, TableTerms) &&
                  HaveTableBase && GuardSlots.size() > 1) {
                auto First = GuardSlots.begin();
                const uint64_t Step = *std::next(First) - *First;
                if (Step != 0 && TableBase <=
                        InvalidVA - Step * (GuardSlots.size() - 1)) {
                  GuardSlots.clear();
                  for (size_t I = 0; I < Result.Slots.size(); ++I)
                    GuardSlots.insert(TableBase + Step * I);
                }
              }
              bool CompleteGuard = true;
              const unsigned GuardBits = GuardLoad->Output.Size * 8;
              for (uint64_t Slot : GuardSlots) {
                if (*GuardOffset < 0 ||
                    Slot > InvalidVA - static_cast<uint64_t>(*GuardOffset)) {
                  CompleteGuard = false;
                  break;
                }
                const uint64_t GuardVA =
                    Slot + static_cast<uint64_t>(*GuardOffset);
                const Segment *Segment = Img->getSegmentFor(GuardVA);
                if (!Segment || GuardVA < Segment->VA ||
                    GuardVA - Segment->VA > Segment->Data.size() ||
                    GuardLoad->Output.Size >
                        Segment->Data.size() - (GuardVA - Segment->VA)) {
                  CompleteGuard = false;
                  break;
                }
                uint64_t Value = 0;
                std::memcpy(&Value,
                            Segment->Data.data() + (GuardVA - Segment->VA),
                            GuardLoad->Output.Size);
                const bool IsHigh =
                    (Value & (uint64_t(1) << (GuardBits - 1))) != 0;
                const bool BranchTaken = CallOnTrue ? IsHigh : !IsHigh;
                if (BranchTaken)
                  CallableSlots.insert(Slot);
              }
              if (CompleteGuard && !CallableSlots.empty())
                Result.Slots = std::move(CallableSlots);
            }
              }
            }
      }
    }
  }
  for (uint64_t Slot : Result.Slots) {
    const bool IsCode = Img->CodePtrRelocSlots.count(Slot) != 0;
    const bool IsData = Img->DataPtrRelocSlots.count(Slot) != 0;
    const bool IsImport = Img->ImportPtrSlots.count(Slot) != 0 ||
                          Img->DyldBindSlots.count(Slot) != 0;
    const unsigned Kinds = static_cast<unsigned>(IsCode) +
                           static_cast<unsigned>(IsData) +
                           static_cast<unsigned>(IsImport);
    Result.SawConflict |= Kinds > 1;
    Result.SawCode |= IsCode;
    Result.SawData |= IsData;
    Result.SawImport |= IsImport;
    Result.SawUnknown |= Kinds == 0;
  }
  Result.Complete = true;
  return finish(Result);
}

bool MedLLVMEmitter::recoverAbsoluteDataPointerLoadIdentities(
    const MedVar &V, std::set<DataAddressIdentity> &Targets) const {
  Targets.clear();
  if (!CurMedFunc || !Img || V.isConst() || Img->DataPtrRelocSlots.empty())
    return false;

  const unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  if (PtrSize == 0 || PtrSize > 8)
    return false;

  // Segment discovery is only an ownership hint.  When the LOAD address can
  // be classified at occurrence granularity, data recovery must agree with
  // that role proof instead of collecting neighbouring data relocations from
  // the same mixed record run.  Incomplete/mixed/code/import/scalar domains
  // deliberately fail closed here; all consumers of this helper therefore
  // share the same slot-role decision as indirect-call validation.
  const PointerTableLoadRoleSummary SlotRoles =
      classifyPointerTableLoadRoles(V);
  if (SlotRoles.Recognized && !SlotRoles.isDataOnly())
    return false;

  auto isStableNumericOffset = [&](const MedVar &Offset) {
    if (!Offset.isConst())
      return false;
    if (Offset.Provenance == ConstantAddressProvenance::Scalar)
      return true;
    if (isAddressProvenance(Offset.Provenance) ||
        getVarMayRelocateConstant(Offset.ConstVal, Offset.Size))
      return false;
    // A low executable VA can also be an ordinary stride immediate and stays
    // numeric when getVar does not relocate it.  Exact object-data provenance
    // is independent even below the normal symbolization threshold, whereas a
    // coincident ELF-header VA is not.
    return !hasObjectDataProvenance(Offset.ConstVal);
  };

  MedVar Cur = V;
  const MedOp *Load = nullptr;
  uint64_t Adjustment = 0;
  for (int Depth = 0; Depth < 8; ++Depth) {
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      return false;
    if (Def->Opcode == NdOp::LOAD) {
      Load = Def;
      break;
    }
    auto Forwarded = pointerPreservingInput(*Def);
    if (Forwarded) {
      Cur = *Forwarded;
      continue;
    }

    // Arithmetic around a rebuilt pointer-table load does not change its
    // address model.  Peel only a width-preserving pointer +/- numeric offset;
    // a second mapped/relocatable operand is a distinct base and must remain
    // visible to the full provenance audit.
    auto canCarryPointer = [&](const MedVar &Input) {
      return Def->Output.Size != 0 && Input.Size != 0 &&
             Def->Output.Size >= Input.Size;
    };
    if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
      if (isStableNumericOffset(Def->Inputs[1]) &&
          canCarryPointer(Def->Inputs[0])) {
        Adjustment += Def->Inputs[1].ConstVal;
        Cur = Def->Inputs[0];
        continue;
      }
      if (isStableNumericOffset(Def->Inputs[0]) &&
          canCarryPointer(Def->Inputs[1])) {
        Adjustment += Def->Inputs[0].ConstVal;
        Cur = Def->Inputs[1];
        continue;
      }
    } else if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2 &&
               isStableNumericOffset(Def->Inputs[1]) &&
               canCarryPointer(Def->Inputs[0])) {
      Adjustment -= Def->Inputs[1].ConstVal;
      Cur = Def->Inputs[0];
      continue;
    }
    return false;
  }
  if (!Load || Load->NumInputs < 1 || PtrSize == 0 || PtrSize > 8 ||
      Load->Output.Size != PtrSize)
    return false;

  auto recoverAddressBase = [&](const MedVar &Addr, uint64_t &Base,
                                uint64_t &RunEnd) -> bool {
    // Prefer an exactly folded slot over segment-wide pointer-table evidence.
    // A single read-only-after-relocation run may contain both data-pointer
    // and code-pointer slots (Mach-O i386 __data does).  Scanning every data
    // slot in that run for an exact LOAD from a code slot would misclassify the
    // loaded function pointer as data and reject a valid indirect call.
    if (!Addr.isConst()) {
      if (auto Exact = traceTableBaseConst(Addr)) {
        uint64_t Slot = *Exact;
        if (PtrSize < 8)
          Slot &= (uint64_t(1) << (PtrSize * 8)) - 1;
        if (Img->CodePtrRelocSlots.count(Slot) != 0)
          return false;
        if (Img->DataPtrRelocSlots.count(Slot) != 0) {
          Base = Slot;
          RunEnd = 0;
          return true;
        }
      }
    }
    // Absolute pointer tables commonly live in .data.rel.ro, whose segment is
    // writable in the object flags even though the emitter mirrors it as
    // read-only-after-relocation.  The ordinary rodata decomposition rejects
    // that segment by design; use the dedicated pointer-table proof first.
    if (!Addr.isConst())
      if (auto SegVA = ptrTableUniqueSegment(Addr)) {
        Base = *SegVA;
        const Segment *Seg = Img->getSegmentFor(*SegVA);
        uint64_t RunStart = 0;
        readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
        if (!Seg || RunStart != *SegVA || RunEnd <= RunStart)
          return false;
        return true;
      }
    if (Addr.isConst()) {
      Base = Addr.ConstVal;
      return Base != 0 || Img->DataPtrRelocSlots.count(0) != 0;
    }
    bool HaveBase = false;
    std::vector<MedVar> Terms;
    if (collectIndexedGlobalBase(Addr, Base, HaveBase, Terms, /*Depth=*/0,
                                 /*FailClosed=*/false) &&
        HaveBase)
      return true;
    Base = 0;
    HaveBase = false;
    Terms.clear();
    if (collectLiteralPoolBase(Addr, Base, HaveBase, Terms) && HaveBase)
      return true;
    bool SawLoad = false;
    bool SawArithmetic = false;
    auto Folded =
        traceTableBaseConst(Addr, 0, &SawLoad, nullptr, &SawArithmetic);
    if (!Folded || (SawArithmetic && !SawLoad))
      return false;
    Base = *Folded;
    return Base != 0 || Img->DataPtrRelocSlots.count(0) != 0;
  };

  uint64_t Base = 0;
  uint64_t RunEnd = 0;
  bool RecoveredAddress = recoverAddressBase(Load->Inputs[0], Base, RunEnd);
  if (!RecoveredAddress)
    return false;

  auto recoverSlot = [&](uint64_t Slot) {
    const uint8_t *Bytes = Img->readVA(Slot, PtrSize);
    if (!Bytes)
      return false;
    uint64_t Target = 0;
    std::memcpy(&Target, Bytes, PtrSize);
    Target += Adjustment;
    if (PtrSize < 8)
      Target &= (uint64_t(1) << (PtrSize * 8)) - 1;
    uint64_t OwnerVA = InvalidVA;
    if (auto It = Img->DataPtrRelocTargetOwners.find(Slot);
        It != Img->DataPtrRelocTargetOwners.end())
      OwnerVA = It->second;
    Targets.insert({Target, OwnerVA});
    return true;
  };

  if (RunEnd != 0) {
    auto It = Img->DataPtrRelocSlots.lower_bound(Base);
    for (; It != Img->DataPtrRelocSlots.end() && *It < RunEnd; ++It)
      if (!recoverSlot(*It))
        return false;
  } else {
    if (!Img->DataPtrRelocSlots.count(Base))
      return false;
    for (uint64_t Slot = Base; Img->DataPtrRelocSlots.count(Slot);) {
      if (!recoverSlot(Slot))
        return false;
      if (Slot > InvalidVA - PtrSize)
        break;
      Slot += PtrSize;
    }
  }
  return !Targets.empty();
}

bool MedLLVMEmitter::recoverAbsoluteDataPointerLoadTargets(
    const MedVar &V, std::set<uint64_t> &Targets) const {
  Targets.clear();
  std::set<DataAddressIdentity> Identities;
  if (!recoverAbsoluteDataPointerLoadIdentities(V, Identities))
    return false;
  for (const DataAddressIdentity &Identity : Identities) {
    if (!isMaterializableReadOnlyDataAddress(Identity.VA, Identity.OwnerVA))
      return false;
    Targets.insert(Identity.VA);
  }
  return !Targets.empty();
}

bool MedLLVMEmitter::recoverRelativeDataPointerTargets(
    const MedVar &V, std::set<uint64_t> &Targets, bool &Symbolized) const {
  Targets.clear();
  Symbolized = false;
  if (!CurMedFunc || !Img || V.isConst() || Img->RelDataPtrRelocSlots.empty())
    return false;

  MedVar Cur = V;
  const MedOp *Add = nullptr;
  for (int Depth = 0; Depth < 8; ++Depth) {
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      return false;
    if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
      Add = Def;
      break;
    }
    auto Forwarded = pointerPreservingInput(*Def);
    if (!Forwarded)
      return false;
    Cur = *Forwarded;
  }
  if (!Add)
    return false;

  auto recoverLoadBase = [&](const MedOp &Load, uint64_t &Base) -> bool {
    if (Load.NumInputs < 1 || Load.Output.Size != 4)
      return false;
    bool HaveBase = false;
    std::vector<MedVar> Terms;
    if (collectIndexedGlobalBase(Load.Inputs[0], Base, HaveBase, Terms,
                                 /*Depth=*/0, /*FailClosed=*/false) &&
        HaveBase)
      return true;
    Base = 0;
    HaveBase = false;
    Terms.clear();
    if (collectLiteralPoolBase(Load.Inputs[0], Base, HaveBase, Terms) &&
        HaveBase)
      return true;
    if (Load.Inputs[0].isConst()) {
      Base = Load.Inputs[0].ConstVal;
      return Base != 0;
    }
    return false;
  };

  auto recoverOffsetLoad = [&](const MedVar &Start,
                               uint64_t &LoadBase) -> bool {
    MedVar Offset = Start;
    bool SawSignedExtension = false;
    for (int Depth = 0; Depth < 8; ++Depth) {
      const MedOp *Def = lookupDef(Offset);
      if (!Def)
        return false;
      if (Def->Opcode == NdOp::LOAD) {
        unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
        if (PointerSize > 4 && !SawSignedExtension)
          return false;
        return recoverLoadBase(*Def, LoadBase);
      }
      if (Def->Opcode == NdOp::INT_SEXT && Def->NumInputs >= 1 &&
          Def->Inputs[0].Size == 4 && Def->Output.Size >= Def->Inputs[0].Size) {
        SawSignedExtension = true;
        Offset = Def->Inputs[0];
        continue;
      }
      auto Forwarded = pointerPreservingInput(*Def);
      if (!Forwarded)
        return false;
      Offset = *Forwarded;
    }
    return false;
  };

  auto recoverBaseOperand = [&](const MedVar &BaseVar, uint64_t &Base,
                                bool &BaseSymbolized) -> bool {
    bool SawLoad = false;
    bool SawArithmetic = false;
    uint16_t OriginSize = BaseVar.Size;
    auto Folded =
        traceTableBaseConst(BaseVar, 0, &SawLoad, &OriginSize, &SawArithmetic);
    if (!Folded || *Folded == 0 || (SawArithmetic && !SawLoad))
      return false;
    Base = *Folded;
    BaseSymbolized = !SawLoad && dataOccurrenceSymbolizes(BaseVar);
    return true;
  };

  uint64_t Base = 0;
  bool BaseSymbolized = false;
  bool Matched = false;
  for (unsigned BaseIndex = 0; BaseIndex < 2; ++BaseIndex) {
    uint64_t CandidateBase = 0;
    bool CandidateSymbolized = false;
    uint64_t LoadBase = 0;
    if (!recoverBaseOperand(Add->Inputs[BaseIndex], CandidateBase,
                            CandidateSymbolized) ||
        !recoverOffsetLoad(Add->Inputs[1 - BaseIndex], LoadBase) ||
        CandidateBase != LoadBase ||
        !Img->RelDataPtrRelocSlots.count(CandidateBase))
      continue;
    if (Add->Output.Size == 0 || Add->Inputs[BaseIndex].Size == 0 ||
        Add->Output.Size < Add->Inputs[BaseIndex].Size || Matched)
      return false;
    Base = CandidateBase;
    BaseSymbolized = CandidateSymbolized;
    Matched = true;
  }
  if (!Matched)
    return false;

  for (uint64_t Slot = Base; Img->RelDataPtrRelocSlots.count(Slot);) {
    const uint8_t *Bytes = Img->readVA(Slot, sizeof(int32_t));
    if (!Bytes)
      return false;
    int32_t Displacement = 0;
    std::memcpy(&Displacement, Bytes, sizeof(Displacement));
    uint64_t Target =
        Base + static_cast<uint64_t>(static_cast<int64_t>(Displacement));
    if (!isMaterializableReadOnlyDataAddress(Target))
      return false;
    Targets.insert(Target);
    if (Slot > InvalidVA - sizeof(int32_t))
      break;
    Slot += sizeof(int32_t);
  }
  if (Targets.empty())
    return false;

  if (BaseSymbolized) {
    const Segment *SourceSeg = Img->getSegmentFor(Base);
    if (!SourceSeg)
      return false;
    uint64_t RunStart = 0, RunEnd = 0;
    readOnlyAfterRelocRun(SourceSeg, RunStart, RunEnd);
    for (uint64_t Target : Targets)
      if (Target < RunStart || Target >= RunEnd)
        return false;
  }
  Symbolized = BaseSymbolized;
  return true;
}

std::optional<uint64_t>
MedLLVMEmitter::indexedConstBase(const MedVar &AddrVar) const {
  if (!CurMedFunc || AddrVar.isConst())
    return std::nullopt;

  const MedOp *Def = lookupDef(AddrVar);
  if (!Def || Def->Opcode != NdOp::INT_ADD || Def->NumInputs < 2)
    return std::nullopt;

  const MedVar &A = Def->Inputs[0];
  const MedVar &B = Def->Inputs[1];
  // Exactly one operand must be a compile-time constant (the base); the other
  // is the runtime index.  A frame store `[SP + disp]` is NOT a const-based
  // array: its constant operand is a small stack displacement, not a global
  // base. Reporting it here poisoned StoredConstBases (any function with a
  // stack array store) and disabled all anonymous-table redirection — clang's
  // loop-idiom CRC table (no named symbol) then read its original VA, unmapped
  // at runtime.
  if (auto CA = traceSSAConst(A);
      CA && !traceSSAConst(B) && !varIsFrameDerived(B))
    return *CA;
  if (auto CB = traceSSAConst(B);
      CB && !traceSSAConst(A) && !varIsFrameDerived(A))
    return *CB;
  return std::nullopt;
}

bool MedLLVMEmitter::collectIndexedGlobalBase(const MedVar &V, uint64_t &Base,
                                              bool &HaveBase,
                                              std::vector<MedVar> &IdxTerms,
                                              int Depth, bool FailClosed,
                                              bool *SawAmbiguousPhi) const {
  const bool CleanTopLevel =
      Depth == 0 && Base == 0 && !HaveBase && IdxTerms.empty();
  auto replayDiagnostic = [&](bool SawAmbiguous, const PhiNode *AmbiguousPhi) {
    if (SawAmbiguousPhi && SawAmbiguous)
      *SawAmbiguousPhi = true;
    if (FailClosed && AmbiguousPhi)
      failAmbiguousDataPointerPhi(*AmbiguousPhi);
  };

  if (CleanTopLevel && CurMedFunc) {
    if (IndexedGlobalBaseCacheFor != CurMedFunc) {
      IndexedGlobalBaseCacheFor = CurMedFunc;
      IndexedGlobalBaseCache.clear();
    }
    const AddressProvenanceVarKey Key = addressProvenanceVarKey(V);
    if (auto It = IndexedGlobalBaseCache.find(Key);
        It != IndexedGlobalBaseCache.end()) {
      const IndexedGlobalBaseProof &Proof = It->second;
      Base = Proof.Base;
      HaveBase = Proof.HaveBase;
      IdxTerms = Proof.IdxTerms;
      replayDiagnostic(Proof.SawAmbiguousPhi, Proof.AmbiguousPhi);
      return Proof.Proven;
    }

    ++AddressProvenanceWork.IndexedBaseProofs;
    bool LocalSawAmbiguous = false;
    const PhiNode *LocalAmbiguousPhi = nullptr;
    const bool Proven =
        collectIndexedGlobalBaseImpl(V, Base, HaveBase, IdxTerms, Depth,
                                     &LocalSawAmbiguous, &LocalAmbiguousPhi);
    IndexedGlobalBaseProof Proof;
    Proof.Proven = Proven;
    Proof.Base = Base;
    Proof.HaveBase = HaveBase;
    Proof.IdxTerms = IdxTerms;
    Proof.SawAmbiguousPhi = LocalSawAmbiguous;
    Proof.AmbiguousPhi = LocalAmbiguousPhi;
    IndexedGlobalBaseCache.emplace(Key, std::move(Proof));
    replayDiagnostic(LocalSawAmbiguous, LocalAmbiguousPhi);
    return Proven;
  }

  if (CleanTopLevel)
    ++AddressProvenanceWork.IndexedBaseProofs;
  bool LocalSawAmbiguous = false;
  const PhiNode *LocalAmbiguousPhi = nullptr;
  const bool Proven =
      collectIndexedGlobalBaseImpl(V, Base, HaveBase, IdxTerms, Depth,
                                   &LocalSawAmbiguous, &LocalAmbiguousPhi);
  replayDiagnostic(LocalSawAmbiguous, LocalAmbiguousPhi);
  return Proven;
}

std::optional<uint64_t>
MedLLVMEmitter::uniqueDataAddressOwner(const MedVar &V, uint64_t Value,
                                       bool &Conflict) const {
  Conflict = false;
  std::optional<uint64_t> Owner;
  std::vector<MedVar> Work{V};
  std::set<AddressProvenanceVarKey> Seen;
  int Budget = 4096;
  auto mergeOwner = [&](uint64_t Candidate) {
    if (Owner && *Owner != Candidate)
      Conflict = true;
    else
      Owner = Candidate;
  };

  while (!Work.empty() && Budget-- > 0 && !Conflict) {
    MedVar Cur = Work.back();
    Work.pop_back();
    if (Cur.isConst()) {
      if (Cur.ConstVal == Value && isDataAddressProvenance(Cur.Provenance) &&
          Cur.AddressOwnerVA != InvalidVA)
        mergeOwner(Cur.AddressOwnerVA);
      continue;
    }
    if (!Seen.insert(addressProvenanceVarKey(Cur)).second)
      continue;
    // Preserve the owner on the complete pointer expression, not just on its
    // underlying LOAD. The recovery routine applies any width-preserving
    // pointer +/- scalar adjustment, so `load(slot_to_A) + sizeof(A)` remains
    // A's one-past occurrence even when its numeric value equals B's start.
    std::set<DataAddressIdentity> Identities;
    if (recoverAbsoluteDataPointerLoadIdentities(Cur, Identities)) {
      const unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
      const uint64_t Mask = PtrSize == 0 || PtrSize >= 8
                                ? ~uint64_t(0)
                                : (uint64_t(1) << (PtrSize * 8)) - 1;
      for (const DataAddressIdentity &Identity : Identities) {
        if ((Identity.VA & Mask) != (Value & Mask))
          continue;
        // The absolute relocation slot is authoritative for this occurrence.
        // If its section owner was not preserved, do not silently fall back to
        // the numerically adjacent object at the same VA.
        if (Identity.OwnerVA == InvalidVA) {
          Conflict = true;
          break;
        }
        mergeOwner(Identity.OwnerVA);
      }
    }
    if (const PhiNode *Phi = lookupPhi(Cur)) {
      for (const auto &[PredId, Arg] : Phi->Args)
        if (phiIncomingEdgeFeasible(*Phi, PredId))
          Work.push_back(Arg);
      continue;
    }
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      continue;
    if (auto Forwarded = pointerPreservingInput(*Def)) {
      Work.push_back(*Forwarded);
      continue;
    }
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (collectFrameReloadSources(*Def, Sources))
        Work.insert(Work.end(), Sources.begin(), Sources.end());
      continue;
    }
    if (Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) {
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        Work.push_back(Def->Inputs[I]);
      continue;
    }
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
      Work.push_back(Def->Inputs[1]);
      Work.push_back(Def->Inputs[2]);
    }
  }
  if (Budget <= 0)
    Conflict = true;
  return Conflict ? std::nullopt : Owner;
}

bool MedLLVMEmitter::collectIndexedGlobalBaseImpl(
    const MedVar &V, uint64_t &Base, bool &HaveBase,
    std::vector<MedVar> &IdxTerms, int Depth, bool *SawAmbiguousPhi,
    const PhiNode **AmbiguousPhi) const {
  if (!CurMedFunc || Depth > 8)
    return false;

  if (V.isConst()) {
    if (!Img)
      return false;
    bool OwnerConflict = false;
    std::optional<uint64_t> Owner =
        uniqueDataAddressOwner(V, V.ConstVal, OwnerConflict);
    if (OwnerConflict || (V.ConstVal == 0 && !Owner))
      return false;
    const Segment *Seg = Img->getSegmentFor(Owner.value_or(V.ConstVal));
    // Ordinary writable bytes are not immutable-table bases. A segment with
    // loader-proven pointer slots is different: the pointer-table mirror owns
    // it even when Mach-O/ELF object flags remain writable while relocations
    // are applied. Admitting that explicit loader-owned domain lets the
    // pointer-table resolver claim `base + scalar_index` before the generic
    // all-arms audit, while the caller still rejects mixed/multiple bases.
    const bool LoaderOwnedPointerTable =
        Seg && !Seg->isExecutable() && segHasPtrRelocSlots(Seg);
    bool HasOwnedBytes = false;
    if (Owner && Seg) {
      const Section *OwnerSec = Img->getSectionFor(*Owner);
      const uint64_t Begin = OwnerSec ? OwnerSec->VA : Seg->VA;
      const uint64_t Size = OwnerSec ? OwnerSec->Size : Seg->Size;
      HasOwnedBytes = Size <= InvalidVA - Begin && V.ConstVal >= Begin &&
                      V.ConstVal <= Begin + Size;
    }
    if (!Seg || Img->hasExecutableCodeOwnerAt(Owner.value_or(V.ConstVal)) ||
        (Seg->isWritable() && !LoaderOwnedPointerTable) ||
        !(Owner ? HasOwnedBytes : hasObjectDataProvenance(V.ConstVal)) ||
        (HaveBase && Base != V.ConstVal))
      return false;
    Base = V.ConstVal;
    HaveBase = true;
    return true;
  }

  const MedOp *Def = lookupDef(V);
  // Descend only through operations that preserve the complete unsigned
  // pointer value. In particular a widening SEXT of 0x80001000 and a non-zero
  // SUBBYTES do not carry the original table base.
  if (Def)
    if (auto Forwarded = pointerPreservingInput(*Def))
      return collectIndexedGlobalBaseImpl(*Forwarded, Base, HaveBase, IdxTerms,
                                          Depth + 1, SawAmbiguousPhi,
                                          AmbiguousPhi);
  // A non-recursive control-flow PHI can transport one table base just like a
  // COPY, but only after proving every feasible incoming edge.  Direct PHI
  // constants are emitted as raw original-image integers (the edge-copy path
  // intentionally bypasses getVar), so allowing the induction fallback to
  // guess from one rodata-looking arm leaves a stale Mach-O VA under ASLR.
  if (const PhiNode *Phi = lookupPhi(V)) {
    if (!Img || phiIsSelfRecurrent(*Phi))
      return false;

    std::optional<uint64_t> CommonBase;
    uint64_t CommonOwner = InvalidVA;
    bool HaveCommonOwner = false;
    bool SawFeasible = false;
    bool SawMappedData = false;
    bool SawUnproved = false;
    bool SawDifferent = false;
    bool SawTableShapedInvalid = false;
    for (const auto &[PredId, Arg] : Phi->Args) {
      if (!phiIncomingEdgeFeasible(*Phi, PredId))
        continue;
      SawFeasible = true;
      if (Phi->Output.Size == 0 || Arg.Size == 0 ||
          Phi->Output.Size < Arg.Size) {
        auto Value = traceValueVA(Arg);
        SawTableShapedInvalid |= Value && hasObjectDataProvenance(*Value);
        SawUnproved = true;
        continue;
      }

      std::optional<uint64_t> Candidate;
      if (Arg.isConst()) {
        Candidate = Arg.ConstVal;
      } else {
        uint64_t ArmBase = 0;
        bool HaveArmBase = false;
        std::vector<MedVar> ArmTerms;
        if (collectIndexedGlobalBaseImpl(Arg, ArmBase, HaveArmBase, ArmTerms,
                                         Depth + 1, SawAmbiguousPhi,
                                         AmbiguousPhi) &&
            HaveArmBase)
          Candidate = traceValueVA(Arg);
      }
      bool OwnerConflict = false;
      std::optional<uint64_t> Owner =
          Candidate ? uniqueDataAddressOwner(Arg, *Candidate, OwnerConflict)
                    : std::nullopt;
      const Segment *Seg = Candidate && (*Candidate != 0 || Owner)
                               ? Img->getSegmentFor(Owner.value_or(*Candidate))
                               : nullptr;
      bool HasOwnedRange = false;
      if (Candidate && Owner && Seg) {
        const Section *OwnerSec = Img->getSectionFor(*Owner);
        const uint64_t Begin = OwnerSec ? OwnerSec->VA : Seg->VA;
        const uint64_t Size = OwnerSec ? OwnerSec->Size : Seg->Size;
        HasOwnedRange = Size <= InvalidVA - Begin && *Candidate >= Begin &&
                        *Candidate <= Begin + Size;
      }
      bool IsMappedReadOnlyData =
          Candidate && !OwnerConflict && Seg && !Seg->isWritable() &&
          !Img->hasExecutableCodeOwnerAt(Owner.value_or(*Candidate)) &&
          (Owner ? HasOwnedRange : hasObjectDataProvenance(*Candidate));
      if (!IsMappedReadOnlyData) {
        auto Value = traceValueVA(Arg);
        SawTableShapedInvalid |= Value && hasObjectDataProvenance(*Value);
        SawUnproved = true;
        continue;
      }
      SawMappedData = true;
      if (CommonBase && *CommonBase != *Candidate)
        SawDifferent = true;
      else
        CommonBase = *Candidate;
      const uint64_t OwnerKey = Owner.value_or(InvalidVA);
      if (HaveCommonOwner && CommonOwner != OwnerKey)
        SawDifferent = true;
      else {
        CommonOwner = OwnerKey;
        HaveCommonOwner = true;
      }
    }

    if (!SawFeasible || !SawMappedData) {
      if (SawTableShapedInvalid) {
        if (SawAmbiguousPhi)
          *SawAmbiguousPhi = true;
        if (AmbiguousPhi && !*AmbiguousPhi)
          *AmbiguousPhi = Phi;
      }
      return false;
    }
    if (SawUnproved || SawDifferent) {
      if (SawAmbiguousPhi)
        *SawAmbiguousPhi = true;
      if (AmbiguousPhi && !*AmbiguousPhi)
        *AmbiguousPhi = Phi;
      return false;
    }
    if (!CommonBase || (HaveBase && Base != *CommonBase))
      return false;
    Base = *CommonBase;
    HaveBase = true;
    return true;
  }
  // A compiler may spill the materialized table base to a local frame slot at
  // -O0 and later form `reloaded_base + runtime_index`.  Cross that memory
  // boundary only when the CFG-aware reaching-store proof covers every path
  // and every exact-width source folds to the same mapped data address.  The
  // slot itself is not an index term: it transports the base provenance.
  if (Def && Def->Opcode == NdOp::LOAD && Def->NumInputs >= 1) {
    if (!Img)
      return false;
    std::vector<MedVar> Sources;
    if (!collectFrameReloadSources(*Def, Sources))
      return false;
    std::optional<uint64_t> CommonBase;
    uint64_t CommonOwner = InvalidVA;
    bool HaveCommonOwner = false;
    for (const MedVar &Source : Sources) {
      auto Candidate = traceSSAConst(Source);
      bool OwnerConflict = false;
      std::optional<uint64_t> Owner =
          Candidate ? uniqueDataAddressOwner(Source, *Candidate, OwnerConflict)
                    : std::nullopt;
      if (!Candidate || OwnerConflict || (*Candidate == 0 && !Owner))
        return false;
      const Segment *Seg = Img->getSegmentFor(Owner.value_or(*Candidate));
      bool HasOwnedRange = false;
      if (Owner && Seg) {
        const Section *OwnerSec = Img->getSectionFor(*Owner);
        const uint64_t Begin = OwnerSec ? OwnerSec->VA : Seg->VA;
        const uint64_t Size = OwnerSec ? OwnerSec->Size : Seg->Size;
        HasOwnedRange = Size <= InvalidVA - Begin && *Candidate >= Begin &&
                        *Candidate <= Begin + Size;
      }
      if (!Seg || Img->hasExecutableCodeOwnerAt(Owner.value_or(*Candidate)) ||
          !(Owner ? HasOwnedRange : hasObjectDataProvenance(*Candidate)))
        return false;
      if (CommonBase && *CommonBase != *Candidate)
        return false;
      CommonBase = *Candidate;
      const uint64_t OwnerKey = Owner.value_or(InvalidVA);
      if (HaveCommonOwner && CommonOwner != OwnerKey)
        return false;
      CommonOwner = OwnerKey;
      HaveCommonOwner = true;
    }
    if (!CommonBase || (HaveBase && Base != *CommonBase))
      return false;
    Base = *CommonBase;
    HaveBase = true;
    return true;
  }
  if (!Def || Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return false;

  // INT_SUB(minuend, k): base/index live in the minuend; a constant subtrahend
  // is a negative index addend (reverse-order vectorized gather `base+i*s-k`).
  // A non-constant subtrahend is not a foldable offset, so keep it absolute.
  if (Def->Opcode == NdOp::INT_SUB) {
    auto KC = traceSSAConst(Def->Inputs[1]);
    if (!KC || Def->Output.Size == 0 || Def->Inputs[0].Size == 0 ||
        Def->Output.Size < Def->Inputs[0].Size ||
        !collectIndexedGlobalBaseImpl(Def->Inputs[0], Base, HaveBase, IdxTerms,
                                      Depth + 1, SawAmbiguousPhi, AmbiguousPhi))
      return false;
    uint16_t KSz = Def->Inputs[1].Size ? Def->Inputs[1].Size : 8;
    IdxTerms.push_back(MedVar::makeConst(uint64_t(0) - *KC, KSz));
    return true;
  }

  // Descend only along the branch that exposes the base; each non-base operand
  // is kept whole as one index term (so a constant *inside* the index — e.g.
  // `base + (i+1)` — stays part of that term, never mistaken for the base). The
  // base is identified as a lone constant operand (its value is validated as a
  // resolvable global by the caller), matching the one-level form's leniency.
  // The base is a constant pointing into a non-executable data segment (.rodata
  // /.data).  A small struct-field offset (`tab[i].y` = base+i*s+4) lands in
  // the executable .text range (a .o places .text at VA 0) — treating it as the
  // base would lose the real table base nested deeper, so it is kept as an
  // index addend instead.
  auto isBaseConst = [&](const MedVar &Occurrence,
                         const std::optional<uint64_t> &C) {
    if (!C)
      return false;
    bool OwnerConflict = false;
    std::optional<uint64_t> Owner =
        uniqueDataAddressOwner(Occurrence, *C, OwnerConflict);
    if (OwnerConflict || (*C == 0 && !Owner))
      return false;
    if (!Owner)
      return hasObjectDataProvenance(*C);
    const Segment *Seg = Img ? Img->getSegmentFor(*Owner) : nullptr;
    if (!Seg || !Seg->isReadable() || Img->hasExecutableCodeOwnerAt(*Owner))
      return false;
    const Section *OwnerSec = Img->getSectionFor(*Owner);
    const uint64_t Begin = OwnerSec ? OwnerSec->VA : Seg->VA;
    const uint64_t Size = OwnerSec ? OwnerSec->Size : Seg->Size;
    return Size <= InvalidVA - Begin && *C >= Begin && *C <= Begin + Size;
  };
  const MedVar &A = Def->Inputs[0];
  const MedVar &B = Def->Inputs[1];
  auto CA = traceSSAConst(A);
  auto CB = traceSSAConst(B);
  bool ABase = isBaseConst(A, CA);
  bool BBase = isBaseConst(B, CB);
  if (ABase && BBase)
    return false; // two segment-resident constants — ambiguous
  if (ABase) {
    if (Def->Output.Size == 0 || A.Size == 0 || Def->Output.Size < A.Size)
      return false;
    Base = *CA;
    HaveBase = true;
    IdxTerms.push_back(B);
    return true;
  }
  if (BBase) {
    if (Def->Output.Size == 0 || B.Size == 0 || Def->Output.Size < B.Size)
      return false;
    Base = *CB;
    HaveBase = true;
    IdxTerms.push_back(A);
    return true;
  }
  // Neither operand is the base.  Recurse into a non-constant side to find the
  // base nested under multi-dimensional indexing (`base + row*stride + col`) or
  // past a constant field offset (`base + i*stride + off`); each non-base side
  // (including a constant offset) becomes an index addend.
  if (!CA &&
      collectIndexedGlobalBaseImpl(A, Base, HaveBase, IdxTerms, Depth + 1,
                                   SawAmbiguousPhi, AmbiguousPhi)) {
    if (Def->Output.Size == 0 || A.Size == 0 || Def->Output.Size < A.Size)
      return false;
    IdxTerms.push_back(B);
    return true;
  }
  if (!CB &&
      collectIndexedGlobalBaseImpl(B, Base, HaveBase, IdxTerms, Depth + 1,
                                   SawAmbiguousPhi, AmbiguousPhi)) {
    if (Def->Output.Size == 0 || B.Size == 0 || Def->Output.Size < B.Size)
      return false;
    IdxTerms.push_back(A);
    return true;
  }
  return false;
}

bool MedLLVMEmitter::collectLiteralPoolBase(const MedVar &V, uint64_t &Base,
                                            bool &HaveBase,
                                            std::vector<MedVar> &IdxTerms,
                                            int Depth) const {
  if (!CurMedFunc || Depth > 8)
    return false;

  const MedOp *Def = lookupDef(V);
  if (!Def || Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return false;

  // INT_SUB(minuend, k): the base/index live in the minuend; a constant
  // subtrahend is a negative index addend (clang's reverse-order vectorized
  // gather emits `base + i*stride - k`).  A non-constant subtrahend is not a
  // foldable table offset, so leave such an access absolute.
  if (Def->Opcode == NdOp::INT_SUB) {
    auto KC = traceSSAConst(Def->Inputs[1]);
    if (!KC || Def->Output.Size == 0 || Def->Inputs[0].Size == 0 ||
        Def->Output.Size < Def->Inputs[0].Size ||
        !collectLiteralPoolBase(Def->Inputs[0], Base, HaveBase, IdxTerms,
                                Depth + 1))
      return false;
    uint16_t KSz = Def->Inputs[1].Size ? Def->Inputs[1].Size : 8;
    IdxTerms.push_back(MedVar::makeConst(uint64_t(0) - *KC, KSz));
    return true;
  }

  const MedVar &A = Def->Inputs[0];
  const MedVar &B = Def->Inputs[1];
  bool SawA = false, SawB = false;
  auto CA = traceTableBaseConst(A, 0, &SawA);
  auto CB = traceTableBaseConst(B, 0, &SawB);
  if (CA && SawA && !CB) {
    if (Def->Output.Size == 0 || A.Size == 0 || Def->Output.Size < A.Size)
      return false;
    Base = *CA;
    HaveBase = true;
    IdxTerms.push_back(B);
    return true;
  }
  if (CB && SawB && !CA) {
    if (Def->Output.Size == 0 || B.Size == 0 || Def->Output.Size < B.Size)
      return false;
    Base = *CB;
    HaveBase = true;
    IdxTerms.push_back(A);
    return true;
  }
  // Neither side is itself the literal-pool base: descend the side that exposes
  // one (`base + row*stride + col`); the other whole side is an index term.
  if (!CA && collectLiteralPoolBase(A, Base, HaveBase, IdxTerms, Depth + 1)) {
    if (Def->Output.Size == 0 || A.Size == 0 || Def->Output.Size < A.Size)
      return false;
    IdxTerms.push_back(B);
    return true;
  }
  if (!CB && collectLiteralPoolBase(B, Base, HaveBase, IdxTerms, Depth + 1)) {
    if (Def->Output.Size == 0 || B.Size == 0 || Def->Output.Size < B.Size)
      return false;
    IdxTerms.push_back(A);
    return true;
  }
  return false;
}

} // namespace neverd
