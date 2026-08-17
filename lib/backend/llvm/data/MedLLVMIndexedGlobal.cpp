//===- MedLLVMIndexedGlobal.cpp - Indexed/induction globals ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runtime-indexed and induction-pointer global resolution for
/// MedLLVMEmitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"

#include <set>
#include <tuple>
#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::tryResolveInductionGlobalPtr(
    const MedVar &AddrVar, uint16_t SizeHint, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  auto findPhi = [&](const MedVar &V) { return lookupPhi(V); };
  auto findDef = [&](const MedVar &V) { return lookupDef(V); };
  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };
  // Recognize the branchless-select (cmov-as-mask) idiom clang emits for a
  // pointer wrap-reset on x86: `OR(AND(x, m), AND(y, ~m))` == `m ? x : y`,
  // where one mask is the bitwise complement of the other.  The induction base
  // hides in a value arm (x or y), so report the two value arms — handled like
  // SELECT arms by the caller.  AArch64/ARM emit a real `csel` (a SELECT) and
  // never reach here.
  auto isMaskedSelectOr = [&](const MedOp &Or, MedVar &X, MedVar &Y) -> bool {
    if (Or.Opcode != NdOp::INT_OR || Or.NumInputs < 2)
      return false;
    const MedOp *A = findDef(Or.Inputs[0]);
    const MedOp *B = findDef(Or.Inputs[1]);
    if (!A || !B || A->Opcode != NdOp::INT_AND || B->Opcode != NdOp::INT_AND ||
        A->NumInputs < 2 || B->NumInputs < 2)
      return false;
    auto isNotOf = [&](const MedVar &M1, const MedVar &M2) {
      const MedOp *D = findDef(M1);
      return D && D->Opcode == NdOp::INT_NOT && D->NumInputs >= 1 &&
             sameVar(D->Inputs[0], M2);
    };
    for (int Ai = 0; Ai < 2; ++Ai)
      for (int Bi = 0; Bi < 2; ++Bi)
        if (isNotOf(A->Inputs[Ai], B->Inputs[Bi]) ||
            isNotOf(B->Inputs[Bi], A->Inputs[Ai])) {
          X = A->Inputs[1 - Ai];
          Y = B->Inputs[1 - Bi];
          return true;
        }
    return false;
  };

  // The access address `EA` is the induction pointer plus a displacement
  // (`INT_ADD(p, disp)`); walk INT_ADD/INT_SUB/COPY back to the defining PHI so
  // `tab[i].field` resolves.  The displacement may be a constant
  // (`tab[i].field`) or itself a runtime index (`base_phi + (i%n)*stride`, the
  // rolled-loop value table): in either case getVar(EA) below captures the full
  // address, so the `Cur - Base` offset stays exact — only reaching the PHI
  // matters here.  When both addends are runtime the loop-carried base is the
  // first operand. Collect every induction PHI reachable from the access
  // address through COPY / INT_ADD / INT_SUB chains.  Either operand of an
  // ADD/SUB can carry the pointer: the strength-reduced `tab[(i+k)%n]` modulo
  // walk forms `base+running_index - n*(idx/n)`, and clang may emit the
  // `n*(idx/n)` subtrahend as the first ADD operand (x86) or the pointer first
  // (AArch64), so both sides are explored.  Each candidate is validated below
  // by an incoming rodata base; a non-induction PHI (e.g. a loop counter)
  // simply fails that check, so over-collecting is safe.
  auto constInRodata = [&](uint64_t C) {
    const auto *Seg = Img->getSegmentFor(C);
    return C != 0 && Seg && !Seg->isWritable() && Img->isDataAddress(C) &&
           !Seg->Data.empty();
  };
  std::vector<const PhiNode *> Candidates;
  uint64_t DagRodataBase = 0;
  bool HaveDagRodata = false;
  // SELECT-merged base candidates without a PHI (the unrolled `p = cond ? &W :
  // p+1` reset on ARM32, where the loop-invariant literal-pool base is carried
  // through SELECT, not a PHI).  Their bases are recovered by the literal-pool
  // / indexed detectors below when no PHI candidate yields a base.
  std::vector<MedVar> SelectBaseVars;
  bool SawSelect = false;
  {
    std::vector<MedVar> Work{AddrVar};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 256;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst()) {
        // A rodata-segment constant reached through pure address arithmetic is
        // a table/string base materialized inline (the unrolled string-walk
        // wrap-around `p = cond ? &W : p+1` folds `&W`'s VA into a SELECT arm).
        if (!HaveDagRodata && constInRodata(Cur.ConstVal)) {
          DagRodataBase = Cur.ConstVal;
          HaveDagRodata = true;
        }
        continue;
      }
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *P = findPhi(Cur)) {
        Candidates.push_back(P);
        // Also walk the PHI's incoming values: a pointer PHI's reset arm may
        // fold its rodata base to an inline VA constant (the string-walk
        // wrap-around `p = phi(p+1, &W)`), which the DagRodata fallback below
        // anchors when the per-arg base detectors miss the bare-VA form.
        for (const auto &[Pred, Arg] : P->Args) {
          (void)Pred;
          Work.push_back(Arg);
        }
        continue;
      }
      const MedOp *Def = findDef(Cur);
      if (!Def)
        continue;
      // COPY / ZEXT / SEXT just rename or widen the pointer (an i386 32-bit
      // induction pointer is zero-extended to the 64-bit address temp before
      // the load); a pointer-valued SELECT carries the base in its value arms —
      // so descend through them too.
      if ((Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
           Def->Opcode == NdOp::INT_SEXT) &&
          Def->NumInputs >= 1)
        Work.push_back(Def->Inputs[0]);
      else if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
               Def->NumInputs >= 2) {
        // A loop-invariant literal-pool base (`load(@pool) + PC_const`, the
        // ARM32 `ldr rN,[pc]; add rN,pc` idiom) anchors a forward/backward
        // array walk whose varying offset is a separate induction term — the
        // address has no pointer PHI, only an offset PHI whose arms expose no
        // rodata base.  The bare-VA detectors miss it because the base folds
        // through a LOAD, so recover it here; the @run anchoring below then
        // redirects via the address's own original VA (e.g. revwalk's
        // `&bc[last] - 2*i`).
        if (!HaveDagRodata) {
          bool SawLoad = false;
          if (auto C = traceTableBaseConst(Cur, 0, &SawLoad);
              C && SawLoad && constInRodata(*C)) {
            DagRodataBase = *C;
            HaveDagRodata = true;
          }
        }
        Work.push_back(Def->Inputs[0]);
        Work.push_back(Def->Inputs[1]);
      } else if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
        SawSelect = true;
        SelectBaseVars.push_back(Def->Inputs[1]);
        SelectBaseVars.push_back(Def->Inputs[2]);
        Work.push_back(Def->Inputs[1]);
        Work.push_back(Def->Inputs[2]);
      } else if (Def->Opcode == NdOp::INT_OR) {
        // x86 lowers a pointer wrap-reset to the branchless masked-select idiom
        // `OR(AND(x, m), AND(y, ~m))`; its value arms carry the induction base
        // and the advanced pointer, so descend through them like a SELECT.
        MedVar MX, MY;
        if (isMaskedSelectOr(*Def, MX, MY)) {
          SawSelect = true;
          SelectBaseVars.push_back(MX);
          SelectBaseVars.push_back(MY);
          Work.push_back(MX);
          Work.push_back(MY);
        }
      } else if (Def->Opcode == NdOp::LOAD && Def->NumInputs >= 1) {
        // Stack spill/reload: a register-constrained target (ARM32) spills the
        // loop-invariant literal-pool base (`ldr[pc]; add pc`) to a frame slot
        // and reloads it inside the neighbourhood walk (clang's 3x3 stencil).
        // The walk would otherwise stop at the reload; follow the matching
        // STORE's value so the literal-pool base behind the spill is reached
        // and anchored.  addrSlotKey only keys a `base+const` frame slot, so a
        // real indexed table load never matches a store and is left to the
        // resolvers.
        if (auto LKey = addrSlotKey(Def->Inputs[0]))
          for (const auto &B : CurMedFunc->Blocks)
            for (const auto &O : B.Ops)
              if (O.Opcode == NdOp::STORE && O.NumInputs >= 2)
                if (auto SKey = addrSlotKey(O.Inputs[0]);
                    SKey && *SKey == *LKey)
                  Work.push_back(O.Inputs[1]);
      }
    }
  }
  if (Candidates.empty() && !HaveDagRodata && !SawSelect)
    return nullptr;

  // A PHI incoming value loaded from a rebuilt data-pointer table already
  // carries a resolved `ptrtoint(@global)` pointer, not a raw VA to anchor.
  // This is the 32-bit switch-returning-string shape, where the dispatch merges
  // the default string pointer and the absolute `.data.rel.ro` table loads
  // through one PHI; re-anchoring such a value to the rodata run would corrupt
  // it, so bail and let the access use the resolved pointer directly.
  if (Img && (!Img->DataPtrRelocSlots.empty() || !Img->ImportPtrSlots.empty() ||
              !Img->DyldBindSlots.empty())) {
    auto loadsFromDataPtrTable = [&](const MedVar &Start) {
      MedVar Cur = Start;
      for (int D = 0; D < 8; ++D) {
        const MedOp *Def = findDef(Cur);
        if (!Def)
          return false;
        if ((Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
             Def->Opcode == NdOp::INT_SEXT) &&
            Def->NumInputs >= 1) {
          Cur = Def->Inputs[0];
          continue;
        }
        if (Def->Opcode != NdOp::LOAD || Def->NumInputs < 1)
          return false;
        uint64_t LB = 0;
        bool HaveLB = false;
        std::vector<MedVar> LIdx;
        if (!collectIndexedGlobalBase(Def->Inputs[0], LB, HaveLB, LIdx) ||
            !HaveLB) {
          LB = 0;
          HaveLB = false;
          LIdx.clear();
          collectLiteralPoolBase(Def->Inputs[0], LB, HaveLB, LIdx);
        }
        if (!HaveLB || LB == 0)
          return false;
        const Segment *LSeg = Img->getSegmentFor(LB);
        if (!LSeg)
          return false;
        for (uint64_t S : Img->DataPtrRelocSlots)
          if (S >= LSeg->VA && S < LSeg->VA + LSeg->Data.size())
            return true;
        for (const auto &[S, Name] : Img->ImportPtrSlots) {
          (void)Name;
          if (S >= LSeg->VA && S < LSeg->VA + LSeg->Data.size())
            return true;
        }
        for (const auto &[S, Binding] : Img->DyldBindSlots) {
          (void)Binding;
          if (S >= LSeg->VA && S < LSeg->VA + LSeg->Data.size())
            return true;
        }
        return false;
      }
      return false;
    };
    for (const PhiNode *Phi : Candidates)
      for (const auto &[Pred, Arg] : Phi->Args)
        if (loadsFromDataPtrTable(Arg))
          return nullptr;
  }

  // One incoming value must expose a base inside a read-only segment, reached
  // either through a literal-pool / rip-relative LOAD or as a bare constant
  // address (a `lea rip`/`adrp+add` materialization of a .rodata table base
  // folded to its VA).  This runs only for a LOAD address that walks back to a
  // PHI, so a bare rodata-VA constant here is a genuine table pointer, never a
  // plain integer that merely equals a rodata VA (e.g. a loop bound) — a loop
  // counter PHI is not a load address and never reaches this resolver.  A
  // frame-derived base is skipped so a stack-array walk is left absolute.  The
  // init is either the bare base (`&tab`, unrolled loop) or the base already
  // advanced by a runtime offset (`&tab + i*stride`, when clang pre-scales the
  // first iteration); collectLiteralPoolBase peels the runtime addends off the
  // latter, and the `Cur - Base` offset below still recovers the exact element.
  uint64_t Base = 0;
  bool HaveBase = false;
  auto baseInRodata = [&](uint64_t B) {
    const auto *Seg = Img->getSegmentFor(B);
    return B != 0 && Seg && !Seg->isWritable() && Img->isDataAddress(B) &&
           !Seg->Data.empty();
  };
  for (const PhiNode *Phi : Candidates) {
    for (const auto &[Pred, Arg] : Phi->Args) {
      if (varIsFrameDerived(Arg))
        continue;
      bool SawLoad = false;
      if (auto C = traceTableBaseConst(Arg, 0, &SawLoad);
          C && baseInRodata(*C)) {
        Base = *C;
        HaveBase = true;
        break;
      }
      uint64_t LpBase = 0;
      bool HaveLp = false;
      std::vector<MedVar> LpIdx;
      if (collectLiteralPoolBase(Arg, LpBase, HaveLp, LpIdx) && HaveLp &&
          baseInRodata(LpBase)) {
        Base = LpBase;
        HaveBase = true;
        break;
      }
      // Direct const-base init advanced by a runtime offset: `&tab + index`
      // where clang folds `lea tab(%rip)` / `adrp+add` to the base VA and
      // pre-adds the first iteration's index (the strength-reduced
      // `tab[(i+k)%n]` modulo walk keeps a `base + running_index` pointer).
      // traceTableBaseConst only folds a pure-constant init, so peel the
      // rodata-segment base off the runtime index here — the x86/AArch64 dual
      // of the literal-pool form above.
      uint64_t IgBase = 0;
      bool HaveIg = false;
      std::vector<MedVar> IgIdx;
      if (collectIndexedGlobalBase(Arg, IgBase, HaveIg, IgIdx) && HaveIg &&
          baseInRodata(IgBase)) {
        Base = IgBase;
        HaveBase = true;
        break;
      }
    }
    if (HaveBase)
      break;
  }
  // Fallback for a SELECT-merged base with no induction PHI: the ARM32 unrolled
  // `p = cond ? &W : p+1` reset carries a loop-invariant literal-pool base
  // (`base = PC_const + ldr[pc]`) through SELECT, not a PHI, so the per-PHI
  // scan above never reaches it.  Recover the rodata base from a SELECT arm
  // with the same literal-pool / indexed detectors.  getVar(addr) stays the
  // original absolute VA (the base is computed in code, not getVar-symbolized),
  // so the
  // @run anchoring below is exact — the x86-64-style original-VA model.
  if (!HaveBase && SawSelect) {
    for (const MedVar &Arg : SelectBaseVars) {
      if (varIsFrameDerived(Arg))
        continue;
      if (auto C = traceTableBaseConst(Arg, 0, nullptr);
          C && baseInRodata(*C)) {
        Base = *C;
        HaveBase = true;
        break;
      }
      uint64_t LpBase = 0;
      bool HaveLp = false;
      std::vector<MedVar> LpIdx;
      if (collectLiteralPoolBase(Arg, LpBase, HaveLp, LpIdx) && HaveLp &&
          baseInRodata(LpBase)) {
        Base = LpBase;
        HaveBase = true;
        break;
      }
      uint64_t IgBase = 0;
      bool HaveIg = false;
      std::vector<MedVar> IgIdx;
      if (collectIndexedGlobalBase(Arg, IgBase, HaveIg, IgIdx) && HaveIg &&
          baseInRodata(IgBase)) {
        Base = IgBase;
        HaveBase = true;
        break;
      }
    }
  }
  // Fallback for a pointer with no induction PHI but a rodata-segment base
  // folded inline (the unrolled string-walk wrap-around `p = cond ? &W : p+1`,
  // whose SELECT arms carry `&W`'s VA and `&W + offset`).  The frame-derived
  // guard keeps a stack access whose displacement merely lands in a rodata VA
  // range absolute; the segment anchor below recovers the exact element since
  // getVar(addr) still computes the original absolute VA.
  if (!HaveBase && HaveDagRodata && !varIsFrameDerived(AddrVar)) {
    Base = DagRodataBase;
    HaveBase = true;
  }
  if (!HaveBase)
    return nullptr;

  // When getVar already symbolizes the base constant to a relocatable global,
  // getVar(AddrVar) is ALREADY a valid recompiled pointer
  // (`ptrtoint(@global + off) + index`), so emit a plain load through it rather
  // than `@run + (val - Anchor)` — the latter adds the global a SECOND time
  // (the
  // `- Anchor` only cancels at the lift-time VA, so once the relinked object
  // moves @run the two references no longer cancel and the access reads far out
  // of bounds).  This covers the C-string walk AND the i386/ARM32 PIC GOTOFF /
  // literal-pool table access (`GOT_base(0) + idx + field@GOTOFF`, whose field
  // displacement the loader records in RelocDataAddrs), plus any high-VA
  // pointer base.  x86-64/AArch64 non-PIC keep the base a bare origVA constant
  // getVar leaves numeric (not flagged), so they fall through to the @run
  // anchoring.
  bool BaseGetVarSymbolizes =
      Base > limits::kMinGlobalDataAddr ||
      ((Img->RelocDataAddrs.count(Base) || Img->RodataAnchorSeg.count(Base)) &&
       !constValueUsedAsInteger(Base));
  if (isInductionRodataStringBase(Base) || BaseGetVarSymbolizes) {
    llvm::Value *Cur = getVar(AddrVar, Builder);
    if (!Cur)
      return nullptr;
    if (Cur->getType()->isPointerTy())
      return Cur;
    return Builder.CreateIntToPtr(Cur, llvm::PointerType::get(*Ctx, 0),
                                  "indrawptr");
  }

  // Anchor to the merged contiguous rodata run, not a single string/segment
  // global.  The induction value can range over the WHOLE rodata region — a
  // switch-to-string table yields any of several strings spread across
  // `.rodata.str1.1` — so resolving Base to a lone string global (which the
  // C-string path would return for a Base that lands inside a string) leaves
  // every other reachable target out of bounds.  The run preserves the original
  // relative layout, so `@run + (Cur - run_start)` lands on the correct element
  // for any VA in the region.  Falls back to the single-base global only when
  // the run is too large to embed.
  llvm::Constant *G = nullptr;
  uint64_t Anchor = Base;
  if (const Segment *BaseSeg = Img->getSegmentFor(Base)) {
    if (auto [RunGV, RunStart] = embedRodataRun(BaseSeg->VA); RunGV) {
      G = RunGV;
      Anchor = RunStart;
    }
  }
  if (!G) {
    G = tryResolveGlobalData(Base, SizeHint);
    Anchor = Base;
    if (!G)
      return nullptr;
    if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G->stripPointerCasts()))
      if (!GV->isConstant())
        return nullptr;
  }

  // GEP by (current pointer - anchor): the pointer still carries the original
  // VA, so the difference is the element byte offset, valid against the global
  // the recompiled object places at its own VA.
  unsigned Bits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
  auto *Ty = llvm::IntegerType::get(*Ctx, Bits);
  llvm::Value *Cur = getVar(AddrVar, Builder);
  if (!Cur)
    return nullptr;
  if (Cur->getType()->isPointerTy())
    Cur = Builder.CreatePtrToInt(Cur, Ty);
  else if (Cur->getType() != Ty)
    Cur = Builder.CreateZExtOrTrunc(Cur, Ty);
  llvm::Value *Off =
      Builder.CreateSub(Cur, llvm::ConstantInt::get(Ty, Anchor), "indoff");
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, Off, "indptr");
}

bool MedLLVMEmitter::isReadOnlyDataSymbol(uint64_t VA) {
  if (!Img || VA == 0)
    return false;
  if (RodataSymbolsFor != CurMedFunc) {
    RodataSymbolsFor = CurMedFunc;
    RodataSymbolVAs.clear();
    for (const auto &Sym : Img->Symbols) {
      if (Sym.Addr == 0 || Sym.IsFunc)
        continue;
      const auto *Seg = Img->getSegmentFor(Sym.Addr);
      if (Seg && !Seg->isWritable() && !Seg->Data.empty())
        RodataSymbolVAs.insert(Sym.Addr);
    }
  }
  return RodataSymbolVAs.count(VA) > 0;
}

llvm::Value *MedLLVMEmitter::tryResolveIndexedGlobalPtr(
    const MedVar &AddrVar, uint16_t SizeHint, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  // Locate the defining INT_ADD/INT_SUB and the index operand.
  const MedOp *Def = lookupDef(AddrVar);
  if (!Def || Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return nullptr;

  // Decompose the address into one global base constant plus the runtime index
  // addends.  Handles both the one-level `INT_ADD(base,index)` form and a base
  // nested under multi-dimensional indexing (`base + row*stride + col`).
  uint64_t Base = 0;
  bool HaveBase = false;
  std::vector<MedVar> IdxTerms;
  if (!collectIndexedGlobalBase(AddrVar, Base, HaveBase, IdxTerms) ||
      !HaveBase || Base == 0 || IdxTerms.empty())
    return nullptr;

  // A base at a real read-only data symbol is a genuine lookup table (the .o's
  // rodata reference went through a relocation to that symbol).  This is an
  // exact signal, so it bypasses the heuristic guards below that protect
  // against frame-synthesized absolute addresses misread as table bases.
  bool BaseIsRodataSymbol = isReadOnlyDataSymbol(Base);

  unsigned BaseBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
  if (!BaseIsRodataSymbol && isFrameRelativeDisplacement(Base, BaseBits))
    return nullptr;

  // `pop`/epilogue pointer arithmetic is `INT_ADD(stack_ptr, k)` where the
  // small increment `k` looks like a read-only segment VA and the stack pointer
  // looks like the index.  A real table index is a data value, never a stack
  // pointer, so reject when any runtime addend is frame-derived (it stays a
  // stack load).
  for (const auto &T : IdxTerms)
    if (varIsFrameDerived(T))
      return nullptr;

  // A genuine lookup table is read-only.  If this function performs ANY indexed
  // store to a constant base, it has a read-write array that the frame analysis
  // may have modelled with an absolute address colliding with .rodata (e.g.
  // delta's `int v[64]` stored at 0x40, then reloaded by index).  Redirecting
  // those reloads into a .rodata global breaks the store/load pair, so be
  // conservative: only convert indexed loads in functions with no such stores.
  // crc8's CRC table (no stores) still converts; arrays keep absolute access.
  // A proven rodata symbol base is exempt: it is a real table, not a spilled
  // array, even when the function also indexes-stores to its own stack frame
  // (whose negative frame displacements would otherwise poison StoredConstBases
  // and disable all redirection — the base64 table-hoist case).
  if (!BaseIsRodataSymbol) {
    if (StoredBasesFor != CurMedFunc) {
      StoredBasesFor = CurMedFunc;
      StoredConstBases.clear();
      for (const auto &Blk : CurMedFunc->Blocks)
        for (const auto &Op : Blk.Ops)
          if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 1)
            if (auto SB = indexedConstBase(Op.Inputs[0]))
              StoredConstBases.insert(*SB);
    }
    if (!StoredConstBases.empty())
      return nullptr;
  }

  auto *G = tryResolveGlobalData(Base, SizeHint);
  if (!G)
    return nullptr;
  // Only redirect into genuinely read-only globals; writable/BSS resolutions
  // are data the program mutates and must keep absolute addressing.
  if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G->stripPointerCasts()))
    if (!GV->isConstant())
      return nullptr;

  // Sum the index addends at address width; GEP by the resulting byte offset.
  unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
  auto *IdxTy = llvm::IntegerType::get(*Ctx, AddrBits);
  llvm::Value *IdxVal = nullptr;
  for (const auto &T : IdxTerms) {
    llvm::Value *TV = getVar(T, Builder);
    if (!TV)
      return nullptr;
    if (TV->getType()->isPointerTy())
      TV = Builder.CreatePtrToInt(TV, IdxTy);
    else if (TV->getType() != IdxTy)
      TV = Builder.CreateZExtOrTrunc(TV, IdxTy);
    IdxVal = IdxVal ? Builder.CreateAdd(IdxVal, TV, "tblidx") : TV;
  }
  if (!IdxVal)
    return nullptr;
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, IdxVal, "tblptr");
}

} // namespace neverd
