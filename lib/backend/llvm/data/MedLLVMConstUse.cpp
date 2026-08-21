//===- MedLLVMConstUse.cpp - Constant use classification -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Use-site classification of a bare constant for MedLLVMEmitter: whether
/// it is used as a genuine pointer (a memory address, or compared /
/// differenced against one) or as a genuine integer (a loop counter that
/// merely coincides with a data VA), plus the per-function memo caches
/// that keep both queries cheap.  The run-boundary and symbolization
/// predicates that consult these live in MedLLVMConstClass.cpp.  Every
/// routine here is a MedLLVMEmitter member declared in the shared header,
/// so this is a pure translation-unit split.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

void MedLLVMEmitter::ensureConstClassCache() const {
  if (ConstClassCacheFor == CurMedFunc)
    return;
  ConstClassCacheFor = CurMedFunc;
  ConstUsedAsPointerCache.clear();
  ConstValueUsedAsIntegerCache.clear();
}

bool MedLLVMEmitter::resolveMaterializableDataAddress(const MedVar &V,
                                                      uint64_t &Value,
                                                      uint64_t *OwnerVA) const {
  if (!Img)
    return false;
  const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
  auto IsPointerWidth = [&](const MedVar &Candidate) {
    return Candidate.Size == 0 || PointerSize == 0 ||
           Candidate.Size >= PointerSize;
  };
  if (!IsPointerWidth(V))
    return false;

  MedVar Cur = V;
  std::set<std::tuple<int, int, int, uint16_t>> Seen;
  for (;;) {
    // COPY may preserve an address bit pattern, but widening a narrow scalar
    // never creates address provenance.
    if (!IsPointerWidth(Cur))
      return false;
    if (Cur.isConst())
      break;
    auto Key = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                               Cur.Size);
    if (!Seen.insert(Key).second)
      return false;
    const MedOp *Def = lookupDef(Cur);
    if (!Def || Def->Opcode != NdOp::COPY || Def->NumInputs != 1 ||
        Def->Output.Size != Def->Inputs[0].Size)
      return false;
    Cur = Def->Inputs[0];
  }

  // Loader sets are value-global upper bounds and cannot establish the role
  // of this occurrence: an integer immediate may equal a relocation target.
  // Only provenance retained from the address-forming operand is exact enough
  // to opt this occurrence into address-algebra materialization.
  if (!isExactAddressProvenance(Cur.Provenance) ||
      isCodeAddressProvenance(Cur.Provenance))
    return false;
  // A role-neutral address inside executable bytes is not enough to choose a
  // data representation. Literal-pool producers use DataAddress explicitly;
  // generic ADR/LEA values stay deferred until a use-site owns their role.
  if (Cur.Provenance == ConstantAddressProvenance::Address &&
      Img->isCodeAddress(Cur.ConstVal))
    return false;
  bool HasOwnedRoute = false;
  if (Cur.AddressOwnerVA != InvalidVA) {
    const Segment *OwnerSeg = Img->getSegmentFor(Cur.AddressOwnerVA);
    const Section *OwnerSec = Img->getSectionFor(Cur.AddressOwnerVA);
    if (OwnerSeg && OwnerSeg->isReadable()) {
      const uint64_t Begin = OwnerSec ? OwnerSec->VA : OwnerSeg->VA;
      const uint64_t Size = OwnerSec ? OwnerSec->Size : OwnerSeg->Size;
      HasOwnedRoute = Size <= InvalidVA - Begin && Cur.ConstVal >= Begin &&
                      Cur.ConstVal <= Begin + Size;
    }
  }
  if ((Cur.Provenance != ConstantAddressProvenance::DataAddress &&
       !Img->isPotentiallyRelocatableAddress(Cur.ConstVal)) ||
      (!HasOwnedRoute && !canResolveGlobalDataConstant(Cur.ConstVal)) ||
      (Cur.Provenance != ConstantAddressProvenance::DataAddress &&
       addrInCodePtrMirrorRun(Cur.ConstVal)))
    return false;
  Value = Cur.ConstVal;
  if (OwnerVA)
    *OwnerVA = Cur.AddressOwnerVA;
  return true;
}

bool MedLLVMEmitter::constUsedAsPointer(uint64_t Val) const {
  ensureConstClassCache();
  auto It = ConstUsedAsPointerCache.find(Val);
  if (It != ConstUsedAsPointerCache.end())
    return It->second;
  bool Result = constUsedAsPointerImpl(Val);
  ConstUsedAsPointerCache.emplace(Val, Result);
  return Result;
}

bool MedLLVMEmitter::constUsedAsPointerImpl(uint64_t Val) const {
  if (!CurMedFunc)
    return false;

  // Backward walk from every memory-access address through address arithmetic;
  // return true when any visited operand satisfies \p Match.
  auto reaches = [&](auto &&Match, bool FollowValueMergeArms) {
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
            Op.NumInputs < 1)
          continue;
        std::vector<MedVar> Work{Op.Inputs[0]};
        std::set<std::tuple<int, int, int>> Seen;
        int Budget = 256;
        while (!Work.empty() && Budget-- > 0) {
          MedVar Cur = Work.back();
          Work.pop_back();
          if (Match(Cur))
            return true;
          if (Cur.isConst())
            continue;
          if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
            continue;
          if (FollowValueMergeArms)
            if (const PhiNode *Phi = lookupPhi(Cur)) {
              // Variable-identity queries may conservatively traverse every
              // structural PHI arm without consulting feasibility. The bare
              // constant query below deliberately disables this: otherwise a
              // low-VA scalar initializer of an index PHI could become a data
              // pointer merely because the derived index reaches memory.
              for (const auto &[Pred, Arg] : Phi->Args) {
                (void)Pred;
                Work.push_back(Arg);
              }
              continue;
            }
          if (const MedOp *D = lookupDef(Cur))
            switch (D->Opcode) {
            // Additive / compositional address forming: a constant operand can
            // be a base (`baseConst + index`, or `alignedBase | scaledIndex`),
            // so follow every operand.
            case NdOp::INT_ADD:
            case NdOp::INT_SUB:
            case NdOp::INT_OR:
            case NdOp::INT_XOR:
            case NdOp::INT_ZEXT:
            case NdOp::INT_SEXT:
            case NdOp::COPY:
            case NdOp::SUBBYTES:
              for (int I = 0; I < D->NumInputs; ++I)
                Work.push_back(D->Inputs[I]);
              break;
            case NdOp::SELECT:
              // Input 0 is the scalar condition; only the selected value arms
              // can carry address provenance into a memory operand. Traverse
              // those arms only for a variable-identity query. A bare
              // constant in a SELECT remains owned by the specialized
              // all-arms resolver; promoting it here would turn every raw
              // table selection into a value-global pointer classification.
              if (FollowValueMergeArms)
                for (int I = 1; I < D->NumInputs; ++I)
                  Work.push_back(D->Inputs[I]);
              break;
            // Index arithmetic: a CONSTANT operand is a mask (AND), scale
            // (MULT), or shift amount (LEFT) — never a base address.  A
            // scaled-index byte mask `(w >> 9) & ((2^11-1)<<2)` carries 0x1FFC,
            // which can land inside a low-VA `.bss` run's range; it must stay
            // an integer, not be taken for a pointer into that run.  Follow
            // only the non-constant operand (the value being masked/scaled,
            // which may trace back to a real base).
            case NdOp::INT_AND:
            case NdOp::INT_MULT:
            case NdOp::INT_LEFT:
              for (int I = 0; I < D->NumInputs; ++I)
                if (!D->Inputs[I].isConst())
                  Work.push_back(D->Inputs[I]);
              break;
            default:
              break;
            }
        }
      }
    return false;
  };

  // (1) The constant (or anything derived from it) is a memory-access address.
  if (reaches([&](const MedVar &C) { return C.isConst() && C.ConstVal == Val; },
              /*FollowValueMergeArms=*/false))
    return true;
  // (2) The constant is compared / offset against a value that is itself a
  //     pointer (`p != end`, `end - begin`): the one-past-the-end idioms.
  auto varIsPointer = [&](const MedVar &Y) {
    if (Y.isConst())
      return false;
    return reaches(
        [&](const MedVar &C) {
          return !C.isConst() && C.Kind == Y.Kind && C.Id == Y.Id &&
                 C.SSAVer == Y.SSAVer;
        },
        /*FollowValueMergeArms=*/true);
  };
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      switch (Op.Opcode) {
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_EQUAL:
      case NdOp::INT_NOTEQUAL:
      case NdOp::INT_LESS:
      case NdOp::INT_SLESS:
      case NdOp::INT_LESSEQUAL:
      case NdOp::INT_SLESSEQUAL:
        break;
      default:
        continue;
      }
      if (Op.NumInputs < 2)
        continue;
      for (int I = 0; I < 2; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == Val &&
            varIsPointer(Op.Inputs[1 - I]))
          return true;
    }
  return false;
}

bool MedLLVMEmitter::constValueUsedAsInteger(uint64_t Val) const {
  ensureConstClassCache();
  auto It = ConstValueUsedAsIntegerCache.find(Val);
  if (It != ConstValueUsedAsIntegerCache.end())
    return It->second;
  bool Result = constValueUsedAsIntegerImpl(Val);
  ConstValueUsedAsIntegerCache.emplace(Val, Result);
  return Result;
}

bool MedLLVMEmitter::constValueUsedAsIntegerImpl(uint64_t Val) const {
  if (!CurMedFunc)
    return false;

  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };

  // Does var X (or anything derived from it through address arithmetic) serve
  // as a LOAD/STORE address operand?  A backward walk from every memory access
  // address; if it reaches X, X is a pointer, not an integer.
  auto reachesMemAddr = [&](const MedVar &X) {
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
            Op.NumInputs < 1)
          continue;
        std::vector<MedVar> Work{Op.Inputs[0]};
        std::set<std::tuple<int, int, int>> Seen;
        int Budget = 256;
        while (!Work.empty() && Budget-- > 0) {
          MedVar Cur = Work.back();
          Work.pop_back();
          if (Cur.isConst())
            continue;
          if (sameVar(Cur, X))
            return true;
          if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
            continue;
          if (const PhiNode *P = lookupPhi(Cur)) {
            // This walk answers only whether X is protected from the
            // integer-collision heuristic. Following every structurally
            // possible PHI arm is therefore conservative and cycle-free; it
            // must not consult feasible-edge or recurrence analysis here.
            for (const auto &[Pred, Arg] : P->Args) {
              (void)Pred;
              Work.push_back(Arg);
            }
            continue;
          }
          if (const MedOp *D = lookupDef(Cur))
            switch (D->Opcode) {
            case NdOp::INT_ADD:
            case NdOp::INT_SUB:
            case NdOp::INT_AND:
            case NdOp::INT_OR:
            case NdOp::INT_XOR:
            case NdOp::INT_LEFT:
            case NdOp::INT_MULT:
            case NdOp::INT_ZEXT:
            case NdOp::INT_SEXT:
            case NdOp::COPY:
            case NdOp::SUBBYTES:
              for (int I = 0; I < D->NumInputs; ++I)
                Work.push_back(D->Inputs[I]);
              break;
            case NdOp::SELECT:
              // The condition remains an integer; only the selected arms
              // inherit the downstream memory-address role.
              for (int I = 1; I < D->NumInputs; ++I)
                Work.push_back(D->Inputs[I]);
              break;
            default:
              break;
            }
        }
      }
    return false;
  };

  // A merge that exists only because register SSA carries every live-out into
  // an exit block is not evidence that its lone constant is an integer. Walk
  // forward through PHIs and require an actual operation consumer before the
  // single-constant-PHI heuristic below may classify anything. This matters
  // for address registers whose unused exit PHI otherwise globally poisons a
  // genuine pointer relation involving the same numeric value.
  auto reachesOperation = [&](const MedVar &X) {
    std::vector<MedVar> Work{X};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 256;
    while (!Work.empty() && Budget-- > 0) {
      const MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst() ||
          !Seen.insert({static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer}).second)
        continue;
      for (const MedBlock &Block : CurMedFunc->Blocks) {
        for (const MedOp &Op : Block.Ops)
          for (int I = 0; I < Op.NumInputs; ++I)
            if (sameVar(Op.Inputs[I], Cur))
              return true;
        for (const PhiNode &Phi : Block.Phis)
          for (const auto &[Pred, Arg] : Phi.Args) {
            (void)Pred;
            if (sameVar(Arg, Cur))
              Work.push_back(Phi.Output);
          }
      }
    }
    return false;
  };

  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &P : Blk.Phis) {
      bool HasConst = false;
      std::set<uint64_t> DistinctConsts;
      for (const auto &[Pred, Arg] : P.Args) {
        (void)Pred;
        // The init value may be an inline const or a COPY of one (`COPY ESI,
        // 0xA0` feeding the counter PHI), so fold through copies.
        auto C = Arg.isConst() ? std::optional<uint64_t>(Arg.ConstVal)
                               : traceSSAConst(Arg);
        if (C) {
          DistinctConsts.insert(*C);
          if (*C == Val)
            HasConst = true;
        }
      }
      // A loop counter PHI has exactly ONE constant arg (the init) plus a
      // computed back-edge increment, so its const is a genuine integer.  A
      // multi-way SELECTION PHI (a `switch` returning string literals merges
      // the several `&"..."` arms, `cond ? &A : &B`) has SEVERAL constant args
      // that are pointers — the returned address is dereferenced in the CALLER,
      // so reachesMemAddr is locally false and must NOT mark these reloc-target
      // pointer constants as integers.  Only the single-const counter form
      // does.
      if (HasConst && DistinctConsts.size() == 1 &&
          reachesOperation(P.Output) && !reachesMemAddr(P.Output))
        return true;
    }

  // A constant consumed as an INT_MULT factor is a multiplier/scale, never a
  // pointer (pointers are not multiplied).  When the product never flows into a
  // memory address it is a pure integer computation (e.g. the hash multiplier
  // `h*131`), so a value that merely equals a rodata reloc-target VA (the i386
  // switch-string table places a 5-char string at VA 0x83==131) must stay an
  // integer rather than be redirected to that string global.  Gated on the
  // product not reaching a load/store address so an `index*scale` that forms an
  // address is unaffected.
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.Opcode != NdOp::INT_MULT)
        continue;
      bool HasConst = false;
      for (int I = 0; I < Op.NumInputs; ++I) {
        auto C = Op.Inputs[I].isConst()
                     ? std::optional<uint64_t>(Op.Inputs[I].ConstVal)
                     : traceSSAConst(Op.Inputs[I]);
        if (C && *C == Val) {
          HasConst = true;
          break;
        }
      }
      if (HasConst && !reachesMemAddr(Op.Output))
        return true;
    }

  // A constant compared (==/!=/</<=, signed or unsigned) against a value that
  // is itself a pure integer — a loop-trip-count bound tested against the
  // induction counter (`i+1 == N`) — is an integer, not a pointer.  Without
  // this a bound whose value merely equals a low rodata/string reloc-target VA
  // (the i386
  // `.o` places "hotel" at VA 0xC8 == loop bound 200) is redirected to that
  // string global and the loop count is destroyed.  Guarded so a genuine
  // pointer comparison `p == &g` (where p reaches a memory address, i.e. is a
  // pointer) keeps &g redirected.  Only reachable for a low-VA reloc-target
  // constant: the `> kMinGlobalDataAddr` redirect path short-circuits earlier.
  for (const auto &Blk : CurMedFunc->Blocks)
    for (const auto &Op : Blk.Ops) {
      switch (Op.Opcode) {
      case NdOp::INT_EQUAL:
      case NdOp::INT_NOTEQUAL:
      case NdOp::INT_LESS:
      case NdOp::INT_SLESS:
      case NdOp::INT_LESSEQUAL:
      case NdOp::INT_SLESSEQUAL:
        break;
      default:
        continue;
      }
      if (Op.NumInputs < 2)
        continue;
      for (int I = 0; I < 2; ++I) {
        auto C = Op.Inputs[I].isConst()
                     ? std::optional<uint64_t>(Op.Inputs[I].ConstVal)
                     : traceSSAConst(Op.Inputs[I]);
        if (!C || *C != Val)
          continue;
        const MedVar &Other = Op.Inputs[1 - I];
        // The compared-against value is a pure integer (not a dereferenced
        // pointer): no constant pointer comparison, and it never feeds a memory
        // address.  That makes Val an integer bound.
        if (!Other.isConst() && !reachesMemAddr(Other))
          return true;
      }
    }
  return false;
}

} // namespace neverd
