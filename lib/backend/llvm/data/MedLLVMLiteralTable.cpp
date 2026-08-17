//===- MedLLVMLiteralTable.cpp - Literal table resolution -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Literal-pool and select-merged read-only table resolution for
/// MedLLVMEmitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"

#include <functional>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::tryResolveLiteralPoolTable(
    const MedVar &AddrVar, uint16_t SizeHint, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  const MedOp *Def = lookupDef(AddrVar);
  if (!Def)
    return nullptr;

  // An ARM32 predicated address-add (`addeq r,r,#k`) lowers to a SELECT of two
  // table addresses.  Resolve each arm to its rodata litptr and select between
  // the pointers, so the conditional table index is redirected into the rebuilt
  // rodata global instead of left as raw PC+literal arithmetic (which still
  // points at the original, un-relocated table address).
  if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
    llvm::Value *PT =
        tryResolveLiteralPoolTable(Def->Inputs[1], SizeHint, Builder);
    llvm::Value *PF =
        tryResolveLiteralPoolTable(Def->Inputs[2], SizeHint, Builder);
    if (!PT || !PF)
      return nullptr;
    llvm::Value *Cond = getVar(Def->Inputs[0], Builder);
    if (!Cond)
      return nullptr;
    if (!Cond->getType()->isIntegerTy(1))
      Cond = Builder.CreateICmpNE(
          Cond, llvm::ConstantInt::get(Cond->getType(), 0), "litselc");
    return Builder.CreateSelect(Cond, PT, PF, "litselptr");
  }

  if (Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return nullptr;

  // The base must fold through a literal-pool LOAD (the ARM `ldr rN,[pc]; add
  // rN,pc` idiom); this gates the pattern so direct-const x86/AArch64 tables
  // and stack arrays never reach here.  Decompose the (possibly
  // multi-dimensional) address into the literal-pool base plus runtime index
  // addends.  The index must stay runtime.
  uint64_t Base = 0;
  bool HaveBase = false;
  std::vector<MedVar> IdxTerms;
  if (!collectLiteralPoolBase(AddrVar, Base, HaveBase, IdxTerms) || !HaveBase ||
      Base == 0 || IdxTerms.empty())
    return tryResolveSelectBaseLitTable(AddrVar, SizeHint, Builder);

  // A real table index is a data value; a frame-derived addend is stack-pointer
  // arithmetic, not a table access.
  for (const auto &T : IdxTerms)
    if (varIsFrameDerived(T))
      return nullptr;

  // Only redirect into a genuine read-only table at this base, and never when
  // the function indexes-stores to it (a read-write array).
  const auto *Seg = Img->getSegmentFor(Base);
  if (!Seg || Seg->isWritable() || Seg->Data.empty())
    return nullptr;
  if (StoredBasesFor != CurMedFunc) {
    StoredBasesFor = CurMedFunc;
    StoredConstBases.clear();
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 1)
          if (auto SB = indexedConstBase(Op.Inputs[0]))
            StoredConstBases.insert(*SB);
  }
  if (StoredConstBases.count(Base))
    return nullptr;

  auto *G = tryResolveGlobalData(Base, SizeHint);
  if (!G)
    return nullptr;
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
    IdxVal = IdxVal ? Builder.CreateAdd(IdxVal, TV, "litidx") : TV;
  }
  if (!IdxVal)
    return nullptr;
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, IdxVal, "litptr");
}

llvm::Value *MedLLVMEmitter::tryResolveSelectBaseLitTable(
    const MedVar &AddrVar, uint16_t SizeHint, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img)
    return nullptr;

  auto findDef = [&](const MedVar &V) { return lookupDef(V); };

  // A two-way pointer select of distinct rodata table bases, in either form:
  //   * a clean SELECT(cond, A, B) — ARM/i386 predicated table-base load; or
  //   * a branchless bitwise blend `(A & m) | (B & ~m)` with m = -zext(cond)
  //     (all-ones iff cond), the x86-64 `cmov` lowering of `cond ? A : B`.
  // Recognize either and report the condition and the two arms (the arm
  // selected when cond is true, then the other).
  struct SelArms {
    MedVar Cond, ArmT, ArmF;
    bool Ok = false;
  };
  // m = INT_NEG2(INT_ZEXT(cond)); ~m = INT_NOT(m).  Return cond and whether
  // the mask is the positive form (selects its AND operand when cond is true).
  auto maskCond = [&](const MedVar &M,
                      bool &Positive) -> std::optional<MedVar> {
    const MedOp *D = findDef(M);
    Positive = true;
    if (D && D->Opcode == NdOp::INT_NOT && D->NumInputs >= 1) {
      Positive = false;
      D = findDef(D->Inputs[0]);
    }
    if (!D || D->Opcode != NdOp::INT_NEG2 || D->NumInputs < 1)
      return std::nullopt;
    D = findDef(D->Inputs[0]);
    if (!D || D->Opcode != NdOp::INT_ZEXT || D->NumInputs < 1)
      return std::nullopt;
    return D->Inputs[0];
  };
  auto matchSel = [&](const MedVar &V) -> SelArms {
    const MedOp *Def = findDef(V);
    if (!Def)
      return {};
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3)
      return {Def->Inputs[0], Def->Inputs[1], Def->Inputs[2], true};
    if (Def->Opcode == NdOp::INT_OR && Def->NumInputs >= 2) {
      const MedOp *L = findDef(Def->Inputs[0]);
      const MedOp *R = findDef(Def->Inputs[1]);
      if (L && R && L->Opcode == NdOp::INT_AND && R->Opcode == NdOp::INT_AND &&
          L->NumInputs >= 2 && R->NumInputs >= 2) {
        for (int Li = 0; Li < 2; ++Li) {
          bool LPos;
          auto LC = maskCond(L->Inputs[Li], LPos);
          if (!LC)
            continue;
          MedVar LArm = L->Inputs[1 - Li];
          for (int Ri = 0; Ri < 2; ++Ri) {
            bool RPos;
            auto RC = maskCond(R->Inputs[Ri], RPos);
            if (!RC)
              continue;
            if (LC->Kind == RC->Kind && LC->Id == RC->Id &&
                LC->SSAVer == RC->SSAVer && LPos != RPos) {
              MedVar RArm = R->Inputs[1 - Ri];
              return LPos ? SelArms{*LC, LArm, RArm, true}
                          : SelArms{*LC, RArm, LArm, true};
            }
          }
        }
      }
    }
    return {};
  };

  // Peel the runtime index addends off the add chain down to that select/blend.
  SelArms Sel;
  std::vector<MedVar> IdxTerms;
  std::function<bool(const MedVar &, int)> peel = [&](const MedVar &V,
                                                      int Depth) -> bool {
    if (Depth > 8)
      return false;
    if (SelArms S = matchSel(V); S.Ok) {
      Sel = S;
      return true;
    }
    const MedOp *Def = findDef(V);
    if (!Def)
      return false;
    if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
        Def->NumInputs >= 2) {
      if (peel(Def->Inputs[0], Depth + 1)) {
        IdxTerms.push_back(Def->Inputs[1]);
        return true;
      }
      if (Def->Opcode == NdOp::INT_ADD && peel(Def->Inputs[1], Depth + 1)) {
        IdxTerms.push_back(Def->Inputs[0]);
        return true;
      }
    }
    return false;
  };
  if (!peel(AddrVar, 0) || !Sel.Ok || IdxTerms.empty())
    return nullptr;

  for (const auto &T : IdxTerms)
    if (varIsFrameDerived(T))
      return nullptr;

  if (StoredBasesFor != CurMedFunc) {
    StoredBasesFor = CurMedFunc;
    StoredConstBases.clear();
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 1)
          if (auto SB = indexedConstBase(Op.Inputs[0]))
            StoredConstBases.insert(*SB);
  }

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
    IdxVal = IdxVal ? Builder.CreateAdd(IdxVal, TV, "litidx") : TV;
  }
  if (!IdxVal)
    return nullptr;

  // Resolve one select/blend arm to an indexed pointer.  An arm is a base in a
  // read-only data segment reached either through a literal-pool LOAD (ARM/i386
  // `ldr[pc]+pc`) or as a bare rip-relative constant (x86-64 `lea`); or itself
  // a nested select/blend of two such arms — `(c0?(c1?A:B):(c2?C:D))[i]` —
  // resolved recursively into a select of the two indexed pointers.
  std::function<llvm::Value *(const MedVar &, int)> armPtr =
      [&](const MedVar &Arm, int Depth) -> llvm::Value * {
    if (Depth > 8)
      return nullptr;
    if (SelArms S = matchSel(Arm); S.Ok) {
      llvm::Value *AT = armPtr(S.ArmT, Depth + 1);
      llvm::Value *AF = armPtr(S.ArmF, Depth + 1);
      if (!AT || !AF)
        return nullptr;
      llvm::Value *C = getVar(S.Cond, Builder);
      if (!C)
        return nullptr;
      if (!C->getType()->isIntegerTy(1))
        C = Builder.CreateICmpNE(C, llvm::ConstantInt::get(C->getType(), 0),
                                 "litselc");
      return Builder.CreateSelect(C, AT, AF, "litselptr");
    }
    bool SawLoad = false;
    auto VA = traceTableBaseConst(Arm, 0, &SawLoad);
    // The arm must fold to a constant VA in exact data/rodata. A literal-pool
    // LOAD is genuine even when its target lives inline in an instruction
    // section; a bare constant (x86-64 `lea rip` base) is accepted only for an
    // exact data address because it feeds a runtime-indexed pointer blend.
    if (!VA || *VA == 0)
      return nullptr;
    const auto *Seg = Img->getSegmentFor(*VA);
    if (!Seg || Seg->isWritable() || (!SawLoad && Img->isCodeAddress(*VA)) ||
        Seg->Data.empty() || StoredConstBases.count(*VA))
      return nullptr;
    auto *G = tryResolveGlobalData(*VA, SizeHint);
    if (!G)
      return nullptr;
    if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G->stripPointerCasts()))
      if (!GV->isConstant())
        return nullptr;
    return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, IdxVal, "litptr");
  };

  llvm::Value *PT = armPtr(Sel.ArmT, 0);
  llvm::Value *PF = armPtr(Sel.ArmF, 0);
  if (!PT || !PF)
    return nullptr;
  llvm::Value *Cond = getVar(Sel.Cond, Builder);
  if (!Cond)
    return nullptr;
  if (!Cond->getType()->isIntegerTy(1))
    Cond = Builder.CreateICmpNE(
        Cond, llvm::ConstantInt::get(Cond->getType(), 0), "litselc");
  return Builder.CreateSelect(Cond, PT, PF, "litselptr");
}

llvm::Value *MedLLVMEmitter::tryResolveSelectMergeTable(
    const MedVar &AddrVar, uint16_t /*SizeHint*/, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  auto findDef = [&](const MedVar &V) { return lookupDef(V); };
  auto findPhi = [&](const MedVar &V) { return lookupPhi(V); };

  // Peel the runtime index addends off the add chain down to the base term.
  MedVar BaseVar = AddrVar;
  bool PeeledIndex = false;
  for (int Depth = 0; Depth < 8; ++Depth) {
    const MedOp *Def = findDef(BaseVar);
    if (!Def || Def->NumInputs < 2 ||
        (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
      break;
    // The base side is the operand that is not a pure runtime index; prefer the
    // operand that still reaches a PHI/select of rodata bases.  Try operand 0
    // first (the usual pointer position), then operand 1 for INT_ADD.
    bool A0Frame = varIsFrameDerived(Def->Inputs[0]);
    if (!A0Frame) {
      BaseVar = Def->Inputs[0];
    } else if (Def->Opcode == NdOp::INT_ADD &&
               !varIsFrameDerived(Def->Inputs[1])) {
      BaseVar = Def->Inputs[1];
    } else {
      return nullptr; // both addends frame-derived: a stack access
    }
    PeeledIndex = true;
  }
  if (!PeeledIndex)
    return nullptr;

  // Walk the base DAG collecting every rodata-segment base constant, requiring
  // a cross-block PHI to appear (the signature distinguishing this branchy
  // multi-way table select from the flat SELECT/blend
  // tryResolveSelectBaseLitTable already handles).  Traverse the constructs
  // clang emits to merge table bases: PHI, SELECT, the bitwise blend
  // (INT_OR/INT_AND), width casts, COPY, and the literal-pool LOAD (folded by
  // traceTableBaseConst).
  std::set<uint64_t> Bases;
  bool SawPhi = false;
  std::set<std::tuple<int, int, int>> Seen;
  std::function<bool(const MedVar &, int)> walk = [&](const MedVar &V,
                                                      int Depth) -> bool {
    if (Depth > 16 || varIsFrameDerived(V))
      return false;
    if (V.isConst())
      return true; // a non-rodata constant addend (offset); harmless
    auto Key = std::make_tuple(static_cast<int>(V.Kind), V.Id, V.SSAVer);
    if (!Seen.insert(Key).second)
      return true;
    if (bool SawLoad = false; auto C = traceTableBaseConst(V, 0, &SawLoad)) {
      const auto *Seg = Img->getSegmentFor(*C);
      if (*C != 0 && Seg && !Seg->isWritable() && Img->isDataAddress(*C) &&
          !Seg->Data.empty()) {
        Bases.insert(*C);
        return true;
      }
    }
    if (const PhiNode *Phi = findPhi(V)) {
      SawPhi = true;
      for (const auto &[Pred, Arg] : Phi->Args)
        if (!walk(Arg, Depth + 1))
          return false;
      return true;
    }
    const MedOp *Def = findDef(V);
    if (!Def)
      return false;
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
      return Def->NumInputs >= 1 && walk(Def->Inputs[0], Depth + 1);
    case NdOp::SELECT:
      return Def->NumInputs >= 3 && walk(Def->Inputs[1], Depth + 1) &&
             walk(Def->Inputs[2], Depth + 1);
    case NdOp::INT_OR:
    case NdOp::INT_AND:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      if (Def->NumInputs < 2)
        return false;
      // Each operand is a masked base, a base, or a mask/offset; a non-base
      // operand walks to no rodata constant, which is fine.
      walk(Def->Inputs[0], Depth + 1);
      walk(Def->Inputs[1], Depth + 1);
      return true;
    case NdOp::LOAD:
      // Stack spill/reload: a register-constrained target (i386 PIC) spills
      // each table base (GOT-relative `lea`) to a frame slot, then cmov-selects
      // the reloads; follow the matching STORE's value to reach the base.
      if (Def->NumInputs >= 1)
        if (auto LKey = addrSlotKey(Def->Inputs[0]))
          for (const auto &B : CurMedFunc->Blocks)
            for (const auto &O : B.Ops)
              if (O.Opcode == NdOp::STORE && O.NumInputs >= 2) {
                auto SKey = addrSlotKey(O.Inputs[0]);
                if (SKey && *SKey == *LKey)
                  walk(O.Inputs[1], Depth + 1);
              }
      return true;
    default:
      return true;
    }
  };
  if (!walk(BaseVar, 0) || !SawPhi || Bases.size() < 2)
    return nullptr;

  // All collected bases must share one read-only segment so a single embedded
  // global covers every table the select can reach.
  const Segment *Seg = Img->getSegmentFor(*Bases.begin());
  if (!Seg)
    return nullptr;
  for (uint64_t B : Bases)
    if (Img->getSegmentFor(B) != Seg)
      return nullptr;

  if (StoredBasesFor != CurMedFunc) {
    StoredBasesFor = CurMedFunc;
    StoredConstBases.clear();
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 1)
          if (auto SB = indexedConstBase(Op.Inputs[0]))
            StoredConstBases.insert(*SB);
  }
  for (uint64_t B : Bases)
    if (StoredConstBases.count(B))
      return nullptr;

  // Anchor the whole access uniformly: the PHI base still carries the original
  // VA of whichever table was selected, so `@run + (addr - run_start)` lands on
  // the correct element of the rebuilt rodata run for any reachable table +
  // index.
  llvm::Constant *G = nullptr;
  uint64_t Anchor = 0;
  if (auto [RunGV, RunStart] = embedRodataRun(Seg->VA); RunGV) {
    G = RunGV;
    Anchor = RunStart;
  }
  if (!G)
    return nullptr;

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
      Builder.CreateSub(Cur, llvm::ConstantInt::get(Ty, Anchor), "selmrgoff");
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, Off, "selmrgptr");
}

llvm::Value *MedLLVMEmitter::tryResolveLiteralPoolBase(
    const MedVar &AddrVar, uint16_t SizeHint, llvm::IRBuilder<> & /*Builder*/) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  // The whole address must fold to a constant VA *through* a literal-pool LOAD
  // (the ARM `ldr rN,[pc]; add rN,pc` address-of).  SawLoad distinguishes a
  // genuine PC-relative address-of from a plain computed constant that merely
  // equals a data VA, exactly as the function-pointer resolver does.
  bool SawLoad = false;
  auto VA = traceTableBaseConst(AddrVar, 0, &SawLoad);
  if (!VA || !SawLoad || *VA == 0)
    return nullptr;

  // Redirect only into a genuine read-only data constant (a `.rodata` aggregate
  // initializer); a code VA is a function pointer and an executable literal
  // pool is left to the code-pointer path.
  const auto *Seg = Img->getSegmentFor(*VA);
  if (!Seg || Seg->isWritable() || Img->isCodeAddress(*VA) || Seg->Data.empty())
    return nullptr;

  // Never redirect a load aliasing an indexed store into the same base (a
  // read-write table the function mutates); the read-only gate above already
  // excludes it, but keep the symmetry with tryResolveLiteralPoolTable.
  if (StoredBasesFor != CurMedFunc) {
    StoredBasesFor = CurMedFunc;
    StoredConstBases.clear();
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 1)
          if (auto SB = indexedConstBase(Op.Inputs[0]))
            StoredConstBases.insert(*SB);
  }
  if (StoredConstBases.count(*VA))
    return nullptr;

  auto *G = tryResolveGlobalData(*VA, SizeHint);
  if (!G)
    return nullptr;
  if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G->stripPointerCasts()))
    if (!GV->isConstant())
      return nullptr;
  return G;
}

} // namespace neverd
