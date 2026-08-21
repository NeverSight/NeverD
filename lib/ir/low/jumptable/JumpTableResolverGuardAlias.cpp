//===- JumpTableResolverGuardAlias.cpp - Non-copy guard matching ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guard strategies that tie a comparison to the table index by something
/// other than a register copy chain: the COND_BR flag-consumption polarity
/// that reveals an inclusive (`ja`/`jbe`) upper bound worth one extra entry,
/// and the same-location reload equivalence that matches a guard on one reload
/// of a spilled switch variable to the separate reload that feeds the index.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// guardUsesInclusiveCompare — COND_BR-polarity-aware off-by-one recovery
//===----------------------------------------------------------------------===//

/// clang lowers `idx > N -> default` as `cmp idx,N; ja default`, so the table
/// covers idx in [0, N] = N+1 entries.  The lifted CF flag is `idx < N`, which
/// the range analysis reports as only N.  The strict `ja`/`jbe` family also
/// consumes the ZF equality `idx == N`; the `jae`/`jb` family consumes only CF.
/// Return true when the guarding COND_BR transitively consumes both, so the
/// inclusive upper bound is Bound+1.
bool CFGBuilder::guardUsesInclusiveCompare(const InsnRecord &Rec,
                                           uint64_t IndexReg,
                                           uint64_t Bound) const {
  std::vector<LowOp> Ops;
  for (auto &[A, IR] : Insns) {
    if (A >= Rec.Addr)
      break;
    for (auto &Op : IR.Ops)
      Ops.push_back(Op);
  }

  // The dispatch may index through a register copy/widening that is defined
  // after the guarding compare (`cmp edi,N; ...; mov eax,edi; jmp *(,rax,8)`).
  // Compare predicates are expressed in terms of the original register, so
  // canonicalize the recovered table-index register through the complete
  // prefix before matching the lifted CF/ZF graph.  Without this, an inclusive
  // `ja default` bound is misread as N rather than N+1 and loses the last case.
  if (!Ops.empty())
    IndexReg = traceRegSource(Ops, static_cast<int>(Ops.size()) - 1, IndexReg);

  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset;
  };
  auto reachingDef = [&](const NdVar &V, int Before) -> int {
    for (int I = Before; I >= 0 && I < static_cast<int>(Ops.size()); --I)
      if (sameVar(Ops[I].Output, V))
        return I;
    return -1;
  };
  // Trace a register/temp back to the index register through value-preserving
  // ops (copy / extend / low-half subpiece / mask).
  auto isIndex = [&](NdVar V, int Before) -> bool {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (V.isReg() && V.Offset == IndexReg)
        return true;
      if (!V.isReg() && !V.isTemp())
        return false;
      int D = reachingDef(V, Before);
      if (D < 0)
        return false;
      const LowOp &Op = Ops[D];
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::INT_AND:
        if (Op.NumInputs < 1)
          return false;
        V = Op.Inputs[0];
        Before = D - 1;
        break;
      case NdOp::SUBBYTES:
        if (Op.NumInputs < 2 || !Op.Inputs[1].isConst() ||
            Op.Inputs[1].Offset != 0)
          return false;
        V = Op.Inputs[0];
        Before = D - 1;
        break;
      default:
        return false;
      }
    }
    return false;
  };
  auto isLessBound = [&](const LowOp &Op, int At) -> bool {
    if ((Op.Opcode != NdOp::INT_LESS && Op.Opcode != NdOp::INT_SLESS) ||
        Op.NumInputs < 2)
      return false;
    uint64_t C;
    if (!resolveConstThroughCopy(Ops, At - 1, Op.Inputs[1], C))
      return false;
    return C == Bound && isIndex(Op.Inputs[0], At - 1);
  };
  auto isEqualBound = [&](const LowOp &Op, int At) -> bool {
    if (Op.Opcode != NdOp::INT_EQUAL || Op.NumInputs < 2 ||
        !Op.Inputs[1].isConst())
      return false;
    if (Op.Inputs[1].Offset == Bound && isIndex(Op.Inputs[0], At - 1))
      return true;
    // ZF of `cmp idx,Bound` is `(idx - Bound) == 0`; the subtraction result may
    // live in a temp or, on x86 where `cmp` overwrites the register, a
    // register.
    if (Op.Inputs[1].Offset == 0 &&
        (Op.Inputs[0].isTemp() || Op.Inputs[0].isReg())) {
      int D = reachingDef(Op.Inputs[0], At - 1);
      if (D >= 0 && Ops[D].Opcode == NdOp::INT_SUB && Ops[D].NumInputs >= 2 &&
          Ops[D].Inputs[1].isConst() && Ops[D].Inputs[1].Offset == Bound &&
          isIndex(Ops[D].Inputs[0], D - 1))
        return true;
    }
    return false;
  };

  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Opcode != NdOp::COND_BR || Ops[I].NumInputs < 2)
      continue;
    bool SawLess = false, SawEqual = false;
    std::vector<std::pair<NdVar, int>> Work{{Ops[I].Inputs[1], I - 1}};
    std::set<int> SeenDefs;
    int Steps = 0;
    while (!Work.empty() && Steps++ < limits::kMaxGuardScanOps) {
      auto [V, Before] = Work.back();
      Work.pop_back();
      if (V.isConst())
        continue;
      int D = reachingDef(V, Before);
      // Dedup by definition site, not nd-var identity: a temp offset may be
      // reused for distinct defs (ARM flag chains), each needing its own walk.
      if (D < 0 || !SeenDefs.insert(D).second)
        continue;
      const LowOp &Def = Ops[D];
      if (isLessBound(Def, D)) {
        SawLess = true;
        continue;
      }
      if (isEqualBound(Def, D)) {
        SawEqual = true;
        continue;
      }
      switch (Def.Opcode) {
      case NdOp::BOOL_AND:
      case NdOp::BOOL_OR:
      case NdOp::BOOL_XOR:
      case NdOp::BOOL_NOT:
      case NdOp::COPY:
        for (int K = 0; K < Def.NumInputs; ++K)
          Work.push_back({Def.Inputs[K], D - 1});
        break;
      default:
        break;
      }
    }
    if (SawLess && SawEqual)
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromLoadAliasGuard — bound via same-location reload equivalence
//===----------------------------------------------------------------------===//

bool CFGBuilder::inferBoundsFromLoadAliasGuard(const InsnRecord &Rec,
                                               JumpTableInfo &Info) {
  if (Info.IndexReg == InvalidVA || !CurrentImg)
    return false;

  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
  int VarSize = TRI.PointerSize;

  // Flatten the function prefix through the dispatch so the guard, its compared
  // reload, and the index's own reload are all visible to the backward walk.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);
  if (Ops.empty())
    return false;

  // A location key for the value feeding a register: either a fixed address
  // (Kind 0, read-only source) or a stack/frame slot (Kind 1).
  struct MemKey {
    int Kind = -1;
    uint64_t Addr = 0;
    uint64_t Base = 0;
    int64_t Off = 0;
    int LoadIdx = -1;
  };

  // Trace a value backward through value-preserving reshapes (copy / extend /
  // low-half subpiece) to the LOAD that produced it, and key that load's
  // address.  Returns nullopt if the value is not a plain reload.
  auto keyOfLoadFeeding = [&](NdVar V, int From) -> std::optional<MemKey> {
    for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
      int D = reachingDefIdx(Ops, From, V);
      if (D < 0)
        return std::nullopt;
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::LOAD && O.NumInputs >= 1) {
        const NdVar &A = (O.NumInputs >= 2) ? O.Inputs[1] : O.Inputs[0];
        MemKey K;
        K.LoadIdx = D;
        if (A.isConst()) {
          K.Kind = 0;
          K.Addr = A.Offset;
          return K;
        }
        uint64_t B = InvalidVA;
        int64_t Off = 0;
        if (frameSlotKey(Ops, D - 1, A, TRI, B, Off)) {
          K.Kind = 1;
          K.Base = B;
          K.Off = Off;
          return K;
        }
        return std::nullopt;
      }
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
           O.Opcode == NdOp::INT_SEXT) &&
          O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset == 0 &&
          (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      return std::nullopt;
    }
    return std::nullopt;
  };

  auto sameKey = [](const MemKey &A, const MemKey &B) {
    if (A.Kind != B.Kind)
      return false;
    return A.Kind == 0 ? (A.Addr == B.Addr)
                       : (A.Base == B.Base && A.Off == B.Off);
  };

  std::optional<MemKey> IdxKey = keyOfLoadFeeding(
      NdVar::reg(Info.IndexReg, VarSize), static_cast<int>(Ops.size()) - 1);
  if (!IdxKey)
    return false;

  // A fixed-address source is only a stable value if it lives in a non-writable
  // segment (a store could otherwise change it between the two reads); a frame
  // slot's stability is checked per-guard by the no-intervening-store test.
  if (IdxKey->Kind == 0) {
    const auto *Seg = CurrentImg->getSegmentFor(IdxKey->Addr);
    if (!Seg || Seg->isWritable() || CurrentImg->isCodeAddress(IdxKey->Addr))
      return false;
  }

  auto sameSlotStoreBetween = [&](int A, int B) -> bool {
    int Lo = std::min(A, B);
    int Hi = std::max(A, B);
    for (int I = Lo + 1; I < Hi; ++I) {
      const LowOp &S = Ops[I];
      if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
        continue;
      uint64_t B2 = InvalidVA;
      int64_t Off2 = 0;
      if (frameSlotKey(Ops, I - 1, S.Inputs[0], TRI, B2, Off2) &&
          B2 == IdxKey->Base && Off2 == IdxKey->Off)
        return true;
    }
    return false;
  };

  // Only comparisons whose boolean reaches a conditional branch are guards; an
  // equality/range test buried in a case body is not a dispatch bound.  Mark
  // each range-compare op index whose result flows (through BOOL_*/COPY) into a
  // COND_BR condition.
  std::set<int> GuardCmp;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Opcode != NdOp::COND_BR || Ops[I].NumInputs < 2)
      continue;
    std::vector<std::pair<NdVar, int>> Work{{Ops[I].Inputs[1], I - 1}};
    std::set<int> Seen;
    int Steps = 0;
    while (!Work.empty() && Steps++ < limits::kMaxGuardScanOps) {
      auto [V, Before] = Work.back();
      Work.pop_back();
      if (V.isConst())
        continue;
      int D = -1;
      for (int K = Before; K >= 0 && K < static_cast<int>(Ops.size()); --K)
        if (Ops[K].Output.Space == V.Space &&
            Ops[K].Output.Offset == V.Offset) {
          D = K;
          break;
        }
      if (D < 0 || !Seen.insert(D).second)
        continue;
      const LowOp &Def = Ops[D];
      switch (Def.Opcode) {
      case NdOp::INT_LESS:
      case NdOp::INT_SLESS:
      case NdOp::INT_LESSEQUAL:
      case NdOp::INT_SLESSEQUAL:
        GuardCmp.insert(D);
        break;
      case NdOp::BOOL_AND:
      case NdOp::BOOL_OR:
      case NdOp::BOOL_XOR:
      case NdOp::BOOL_NOT:
      case NdOp::COPY:
        for (int L = 0; L < Def.NumInputs; ++L)
          Work.push_back({Def.Inputs[L], D - 1});
        break;
      default:
        break;
      }
    }
  }
  if (GuardCmp.empty())
    return false;

  auto boundFromCmp = [&](int GI) -> uint32_t {
    const LowOp &Op = Ops[GI];
    if (Op.NumInputs < 2)
      return 0;
    uint64_t C;
    if (!resolveConstThroughCopy(Ops, GI - 1, Op.Inputs[1], C))
      return 0;
    uint64_t Bound;
    switch (Op.Opcode) {
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
      Bound = C;
      break;
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
      Bound = C + 1;
      break;
    default:
      return 0;
    }
    if (Bound < limits::kMinJumpTableEntries ||
        Bound > limits::kMaxJumpTableEntries)
      return 0;
    return static_cast<uint32_t>(Bound);
  };

  uint32_t Best = 0;
  for (int GI : GuardCmp) {
    const LowOp &Cmp = Ops[GI];
    if (Cmp.NumInputs < 1 ||
        (!Cmp.Inputs[0].isReg() && !Cmp.Inputs[0].isTemp()))
      continue;
    std::optional<MemKey> GKey = keyOfLoadFeeding(Cmp.Inputs[0], GI - 1);
    if (!GKey || !sameKey(*GKey, *IdxKey))
      continue;
    // A store to the shared frame slot between the two reloads breaks the value
    // equivalence, so the guard no longer bounds the index.
    if (IdxKey->Kind == 1 &&
        sameSlotStoreBetween(GKey->LoadIdx, IdxKey->LoadIdx))
      continue;
    uint32_t Bnd = boundFromCmp(GI);
    if (Bnd == 0)
      continue;
    if (Best == 0 || Bnd < Best)
      Best = Bnd;
  }

  if (Best == 0)
    return false;
  if (Info.MaxEntries == 0 || Best < Info.MaxEntries) {
    Info.MaxEntries = Best;
    LLVM_DEBUG(llvm::dbgs() << "  load-alias-guard: bound " << Best
                            << " from same-location reload of the index\n");
    return true;
  }
  return false;
}

} // namespace neverd
