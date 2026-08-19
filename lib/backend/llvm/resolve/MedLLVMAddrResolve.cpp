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
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/support/Diagnostic.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// Conservative control-flow feasibility
//===----------------------------------------------------------------------===//

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
    if (Cur.isConst() && getVarMayRelocateConstant(Cur.ConstVal, Cur.Size))
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
          !getVarMayRelocateConstant(Def->Inputs[1].ConstVal,
                                     Def->Inputs[1].Size))
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

void MedLLVMEmitter::ensureFeasibleEdgeCache() const {
  if (FeasibleEdgesFor == CurMedFunc)
    return;
  FeasibleEdgesFor = CurMedFunc;
  FeasibleEdges.clear();
  FeasibleBlocks.clear();
  if (!CurMedFunc || CurMedFunc->Blocks.empty())
    return;

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
  FeasibleBlocks.insert(Work.front());
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
      if (Branch && Branch->NumInputs >= 2 && Branch->Inputs[0].isConst())
        if (auto Cond = traceControlConst(Branch->Inputs[1])) {
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

    for (int Succ : OrdinarySuccs) {
      if (!Blocks.count(Succ))
        continue;
      FeasibleEdges.insert({BlockId, Succ});
      if (FeasibleBlocks.insert(Succ).second)
        Work.push_back(Succ);
    }
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
      if (!Blocks.count(Edge.BlockId))
        continue;
      FeasibleEdges.insert({BlockId, Edge.BlockId});
      if (FeasibleBlocks.insert(Edge.BlockId).second)
        Work.push_back(Edge.BlockId);
    }
  }
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
  if (auto It = PhiEdgeClassCache.find(CacheKey); It != PhiEdgeClassCache.end())
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

  ensureFeasibleEdgeCache();
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

  ++AddressProvenanceWork.StableOffsetProofs;
  const bool Result = valueIsStableAddressOffsetImpl(V, Forbidden);
  StableOffsetCache.emplace(Key, Result);
  return Result;
}

bool MedLLVMEmitter::valueIsStableAddressOffsetImpl(
    const MedVar &V, const MedVar *Forbidden) const {
  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };
  using Key = std::tuple<int, int, int>;
  using FrameSlotKey = std::pair<std::pair<int, int>, int64_t>;
  auto keyOf = [](const MedVar &V) {
    return Key{static_cast<int>(V.Kind), V.Id, V.SSAVer};
  };
  // The non-pointer side of an induction step must remain a numeric offset in
  // rebuilt IR. Reject a second occurrence of the recurrence and any constant
  // or forwarded/arithmetic DAG that carries independently relocatable address
  // provenance. Runtime parameters and opaque scalar results remain valid
  // offsets because getVar does not rewrite them into another global pointer.
  std::function<bool(const MedVar &, int, std::set<Key>,
                     std::set<FrameSlotKey>)>
      prove = [&](const MedVar &Start, int Depth, std::set<Key> Seen,
                  std::set<FrameSlotKey> ActiveFrameSlots) -> bool {
    if (Forbidden && sameVar(Start, *Forbidden))
      return false;
    // Recognize a scalar recurrence before applying the acyclic-depth budget:
    // a long lowered arithmetic chain can return to its PHI only after dozens
    // nodes.  Every non-cyclic initialization arm is still audited below.
    if (!Start.isConst() && Seen.count(keyOf(Start)))
      return true;
    if (Depth > 128)
      return false;
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
    if (Start.isConst())
      return !constantIsMappedAddress(Start.ConstVal, Start.Size);
    Seen.insert(keyOf(Start));

    if (const PhiNode *Nested = lookupPhi(Start)) {
      bool SawPotential = false;
      for (const auto &[NestedPred, NestedArg] : Nested->Args) {
        PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Nested, NestedPred);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible)
          return false;
        SawPotential = true;
        if (!prove(NestedArg, Depth + 1, Seen, ActiveFrameSlots))
          return false;
      }
      return SawPotential;
    }

    const MedOp *Def = lookupDef(Start);
    if (!Def)
      return true;
    bool SawLoad = false;
    bool SawArithmetic = false;
    if (auto Value =
            traceTableBaseConst(Start, 0, &SawLoad, nullptr, &SawArithmetic);
        Value && constantIsMappedAddress(*Value, Start.Size))
      return false;
    if (auto Forwarded = pointerPreservingInput(*Def))
      return prove(*Forwarded, Depth + 1, Seen, ActiveFrameSlots);
    if (Def->Opcode == NdOp::SELECT) {
      // Width changes do not by themselves make a scalar SELECT unstable, but
      // every chosen value must remain numeric in rebuilt IR. In particular a
      // narrowing SELECT whose arm contains a relocated table address is still
      // rejected when that arm is audited here; ordinary lowered flag/index
      // SELECTs remain valid scalar offset computations.
      if (Def->NumInputs < 3)
        return false;
      return prove(Def->Inputs[1], Depth + 1, Seen, ActiveFrameSlots) &&
             prove(Def->Inputs[2], Depth + 1, Seen, ActiveFrameSlots);
    }
    if (Def->Opcode == NdOp::INT_OR) {
      MedVar Cond, ArmT, ArmF;
      if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF))
        return prove(ArmT, Depth + 1, Seen, ActiveFrameSlots) &&
               prove(ArmF, Depth + 1, Seen, ActiveFrameSlots);
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
      if (auto Slot = canonicalFrameSlotKey(Def->Inputs[0])) {
        // A loop-carried scalar can be represented through memory rather than
        // an SSA PHI: load(slot), update, store(slot), backedge. Once the exact
        // slot's complete reaching definitions are being audited, encountering
        // that slot again is the recurrence edge, not an unproved new source.
        // The outer source set still audits every non-cyclic initialization,
        // so a pointer initializer or an uninitialized path remains rejected.
        if (ActiveFrameSlots.count(*Slot))
          return true;
        std::vector<MedVar> Sources;
        if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
          return false;
        ActiveFrameSlots.insert(*Slot);
        for (const MedVar &Source : Sources)
          if (!prove(Source, Depth + 1, Seen, ActiveFrameSlots))
            return false;
        return true;
      }

      // A non-frame load narrower than the target pointer cannot transport a
      // complete independently relocatable address. It is an ordinary numeric
      // input (for example a byte from the selected string feeding an x86-64
      // loop state/index DAG), even when later arithmetic widens it.
      if (PointerSize != 0 && Def->Output.Size < PointerSize)
        return true;

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
      return scalarLiteralLoad();
    }
    for (uint8_t I = 0; I < Def->NumInputs; ++I)
      if (!prove(Def->Inputs[I], Depth + 1, Seen, ActiveFrameSlots))
        return false;
    return true;
  };
  return prove(V, 0, {}, {});
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

  // Prove value recurrence, not generic data/control dependence. COPY-like
  // forwarders and the pointer side of ADD/SUB transport a pointer; SELECT and
  // masked-select transport it only when every selectable value arm does. A PHI
  // appearing solely in a condition, multiplier, shift count, or mask is not a
  // loop-carried pointer (for example SELECT(cmp(phi), scalarA, scalarB)).
  std::function<bool(const MedVar &, const MedVar &, int, std::set<Key>)>
      reachesExact = [&](const MedVar &Start, const MedVar &Target, int Depth,
                         std::set<Key> Seen) -> bool {
    if (Depth > 32 || Start.isConst())
      return false;
    if (sameVar(Start, Target))
      return true;
    if (!Seen.insert(keyOf(Start)).second)
      return false;

    if (const PhiNode *Nested = lookupPhi(Start)) {
      bool SawFeasible = false;
      for (const auto &[NestedPred, NestedArg] : Nested->Args) {
        PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Nested, NestedPred);
        if (Edge == PhiEdgeFeasibility::Infeasible)
          continue;
        if (Edge != PhiEdgeFeasibility::ProvenFeasible)
          return false;
        SawFeasible = true;
        if (!reachesExact(NestedArg, Target, Depth + 1, Seen))
          return false;
      }
      return SawFeasible;
    }

    const MedOp *Def = lookupDef(Start);
    if (!Def)
      return false;
    if (auto Forwarded = pointerPreservingInput(*Def))
      return reachesExact(*Forwarded, Target, Depth + 1, Seen);

    auto canCarry = [&](const MedVar &Input) {
      return Def->Output.Size != 0 && Input.Size != 0 &&
             Def->Output.Size >= Input.Size;
    };
    if (Def->Opcode == NdOp::INT_ADD && Def->NumInputs >= 2) {
      bool LeftRecurrent =
          canCarry(Def->Inputs[0]) &&
          reachesExact(Def->Inputs[0], Target, Depth + 1, Seen);
      bool RightRecurrent =
          canCarry(Def->Inputs[1]) &&
          reachesExact(Def->Inputs[1], Target, Depth + 1, Seen);
      if (LeftRecurrent == RightRecurrent)
        return false;
      const MedVar &Offset = LeftRecurrent ? Def->Inputs[1] : Def->Inputs[0];
      return valueIsStableAddressOffset(Offset, &Target);
    }
    if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2) {
      bool LeftRecurrent =
          canCarry(Def->Inputs[0]) &&
          reachesExact(Def->Inputs[0], Target, Depth + 1, Seen);
      bool RightRecurrent =
          canCarry(Def->Inputs[1]) &&
          reachesExact(Def->Inputs[1], Target, Depth + 1, Seen);
      return LeftRecurrent && !RightRecurrent &&
             valueIsStableAddressOffset(Def->Inputs[1], &Target);
    }
    if (selectPreservesPointerValues(*Def))
      return reachesExact(Def->Inputs[1], Target, Depth + 1, Seen) &&
             reachesExact(Def->Inputs[2], Target, Depth + 1, Seen);
    if (Def->Opcode == NdOp::INT_OR) {
      MedVar Cond, ArmT, ArmF;
      return isMaskedSelectOr(*Def, Cond, ArmT, ArmF) &&
             reachesExact(ArmT, Target, Depth + 1, Seen) &&
             reachesExact(ArmF, Target, Depth + 1, Seen);
    }
    return false;
  };

  if (reachesExact(Arg, Phi.Output, 0, {}))
    return true;

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
    if (!reachesExact(Arg, Alias->Output, 0, {}))
      continue;
    for (const auto &[AliasPred, AliasArg] : Alias->Args)
      if (classifyPhiIncomingEdge(*Alias, AliasPred) ==
              PhiEdgeFeasibility::ProvenFeasible &&
          reachesExact(AliasArg, Alias->Output, 0, {}))
        return true;
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
  if (V.isConst()) {
    if (OriginSize)
      *OriginSize = V.Size;
    return V.ConstVal;
  }
  if (!CurMedFunc || Depth > 8)
    return std::nullopt;

  const MedOp *Def = lookupDef(V);
  if (!Def)
    return std::nullopt;

  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
    if (auto Forwarded = pointerPreservingInput(*Def))
      return traceTableBaseConst(*Forwarded, Depth + 1, SawLoad, OriginSize,
                                 SawArithmetic);
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
      return *A + *B;
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
    // The literal stores a signed PC-relative displacement; sign-extend so the
    // subsequent `+ pc` produces the absolute table address.
    if (Sz < 8 && (Val & (1ull << (Sz * 8 - 1))))
      Val |= ~uint64_t(0) << (Sz * 8);
    if (SawLoad)
      *SawLoad = true;
    return Val;
  }
  default:
    return std::nullopt;
  }
}

bool MedLLVMEmitter::recoverAbsoluteDataPointerLoadTargets(
    const MedVar &V, std::set<uint64_t> &Targets) const {
  Targets.clear();
  if (!CurMedFunc || !Img || V.isConst() || Img->DataPtrRelocSlots.empty())
    return false;

  const unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  if (PtrSize == 0 || PtrSize > 8)
    return false;

  auto isStableNumericOffset = [&](const MedVar &Offset) {
    if (!Offset.isConst() ||
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
        Cur = Def->Inputs[0];
        continue;
      }
      if (isStableNumericOffset(Def->Inputs[0]) &&
          canCarryPointer(Def->Inputs[1])) {
        Cur = Def->Inputs[1];
        continue;
      }
    } else if (Def->Opcode == NdOp::INT_SUB && Def->NumInputs >= 2 &&
               isStableNumericOffset(Def->Inputs[1]) &&
               canCarryPointer(Def->Inputs[0])) {
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
    // Absolute pointer tables commonly live in .data.rel.ro, whose segment is
    // writable in the object flags even though the emitter mirrors it as
    // read-only-after-relocation.  The ordinary rodata decomposition rejects
    // that segment by design; use the dedicated pointer-table proof first.
    if (!Addr.isConst())
      if (uint64_t SegVA = ptrTableUniqueSegment(Addr)) {
        Base = SegVA;
        const Segment *Seg = Img->getSegmentFor(SegVA);
        uint64_t RunStart = 0;
        readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
        if (!Seg || RunStart != SegVA || RunEnd <= RunStart)
          return false;
        return true;
      }
    if (Addr.isConst()) {
      Base = Addr.ConstVal;
      return Base != 0;
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
    return Base != 0;
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
    if (!isMaterializableReadOnlyDataAddress(Target))
      return false;
    Targets.insert(Target);
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
    BaseSymbolized = !SawLoad && getVarSymbolizesDataConstant(Base, OriginSize);
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

bool MedLLVMEmitter::collectIndexedGlobalBaseImpl(
    const MedVar &V, uint64_t &Base, bool &HaveBase,
    std::vector<MedVar> &IdxTerms, int Depth, bool *SawAmbiguousPhi,
    const PhiNode **AmbiguousPhi) const {
  if (!CurMedFunc || Depth > 8)
    return false;

  if (V.isConst()) {
    if (!Img || V.ConstVal == 0)
      return false;
    const Segment *Seg = Img->getSegmentFor(V.ConstVal);
    // Ordinary writable bytes are not immutable-table bases. A segment with
    // loader-proven pointer slots is different: the pointer-table mirror owns
    // it even when Mach-O/ELF object flags remain writable while relocations
    // are applied. Admitting that explicit loader-owned domain lets the
    // pointer-table resolver claim `base + scalar_index` before the generic
    // all-arms audit, while the caller still rejects mixed/multiple bases.
    const bool LoaderOwnedPointerTable =
        Seg && !Seg->isExecutable() && segHasPtrRelocSlots(Seg);
    if (!Seg || (Seg->isWritable() && !LoaderOwnedPointerTable) ||
        !hasObjectDataProvenance(V.ConstVal) ||
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
      const Segment *Seg = Candidate && *Candidate != 0
                               ? Img->getSegmentFor(*Candidate)
                               : nullptr;
      bool IsMappedReadOnlyData = Candidate && Seg && !Seg->isWritable() &&
                                  hasObjectDataProvenance(*Candidate);
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
    for (const MedVar &Source : Sources) {
      auto Candidate = traceSSAConst(Source);
      if (!Candidate || *Candidate == 0)
        return false;
      if (!hasObjectDataProvenance(*Candidate))
        return false;
      if (CommonBase && *CommonBase != *Candidate)
        return false;
      CommonBase = *Candidate;
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
  auto isBaseConst = [&](const std::optional<uint64_t> &C) {
    if (!C || *C == 0)
      return false;
    return hasObjectDataProvenance(*C);
  };
  const MedVar &A = Def->Inputs[0];
  const MedVar &B = Def->Inputs[1];
  auto CA = traceSSAConst(A);
  auto CB = traceSSAConst(B);
  bool ABase = isBaseConst(CA);
  bool BBase = isBaseConst(CB);
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
