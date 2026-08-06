//===- MedLLVMAddrResolve.cpp - Address & table resolution -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSA constant tracing and address resolution for MedLLVMEmitter: folding an
/// address expression to a constant VA, the read-only/literal-pool/indexed
/// table resolvers, and code-pointer table mirroring.  These feed getVar/setVar
/// in MedLLVMVarAccess.cpp; writable-data resolution lives in
/// MedLLVMGlobalData.cpp, and frame-derived / stack-pointer classification plus
/// dynamic (VLA) stack-allocation recovery live in MedLLVMFrameResolve.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/Object/SectionNames.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// SSA constant tracing
//===----------------------------------------------------------------------===//

std::optional<uint64_t> MedLLVMEmitter::traceSSAConst(const MedVar &V) const {
  if (V.isConst())
    return V.ConstVal;

  if (!CurMedFunc)
    return std::nullopt;

  MedVar Cur = V;
  for (int Depth = 0; Depth < 8; ++Depth) {
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      return std::nullopt;
    if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1) {
      if (Def->Inputs[0].isConst())
        return Def->Inputs[0].ConstVal;
      Cur = Def->Inputs[0];
      continue;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<uint64_t>
MedLLVMEmitter::traceTableBaseConst(const MedVar &V, int Depth,
                                    bool *SawLoad) const {
  if (V.isConst())
    return V.ConstVal;
  if (!CurMedFunc || Depth > 8)
    return std::nullopt;

  const MedOp *Def = lookupDef(V);
  if (!Def)
    return std::nullopt;

  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
    return Def->NumInputs >= 1
               ? traceTableBaseConst(Def->Inputs[0], Depth + 1, SawLoad)
               : std::nullopt;
  case NdOp::INT_ADD: {
    if (Def->NumInputs < 2)
      return std::nullopt;
    auto A = traceTableBaseConst(Def->Inputs[0], Depth + 1, SawLoad);
    auto B = traceTableBaseConst(Def->Inputs[1], Depth + 1, SawLoad);
    if (A && B)
      return *A + *B;
    return std::nullopt;
  }
  case NdOp::LOAD: {
    // Literal-pool load: the table base word lives in a read-only segment and
    // the loader has already applied its relocation, so read it directly.
    if (Def->NumInputs < 1 || !Img)
      return std::nullopt;
    auto Addr = traceTableBaseConst(Def->Inputs[0], Depth + 1, SawLoad);
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
    // The arm must fold to a constant VA inside a read-only, non-executable
    // data segment.  A literal-pool LOAD (SawLoad) is always genuine; a bare
    // constant (x86-64 `lea rip` base) is accepted only because it sits inside
    // such a segment and feeds a two-way pointer blend that is itself
    // runtime-indexed — a plain integer that merely equals a low VA is not a
    // blend of two pointers.
    if (!VA || *VA == 0)
      return nullptr;
    const auto *Seg = Img->getSegmentFor(*VA);
    if (!Seg || Seg->isWritable() || (!SawLoad && Seg->isExecutable()) ||
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
      if (*C != 0 && Seg && !Seg->isWritable() && !Seg->isExecutable() &&
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
  if (!Seg || Seg->isWritable() || Seg->isExecutable() || Seg->Data.empty())
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
    return C != 0 && Seg && !Seg->isWritable() && !Seg->isExecutable() &&
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
  if (Img && !Img->DataPtrRelocSlots.empty()) {
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
    return B != 0 && Seg && !Seg->isWritable() && !Seg->isExecutable() &&
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
                                              int Depth) const {
  if (!CurMedFunc || Depth > 8)
    return false;

  const MedOp *Def = lookupDef(V);
  // A COPY renames and a ZEXT/SEXT widens its source; descend so a `base +
  // index` computation reached through one (e.g. an induction PHI whose init is
  // a COPY of `lea base(%rip), reg; lea (reg,idx), ptr`, or an i386 32-bit
  // `base+idx` zero-extended to the 64-bit address temp) is still decomposed.
  if (Def &&
      (Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
       Def->Opcode == NdOp::INT_SEXT) &&
      Def->NumInputs >= 1)
    return collectIndexedGlobalBase(Def->Inputs[0], Base, HaveBase, IdxTerms,
                                    Depth + 1);
  if (!Def || Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return false;

  // INT_SUB(minuend, k): base/index live in the minuend; a constant subtrahend
  // is a negative index addend (reverse-order vectorized gather `base+i*s-k`).
  // A non-constant subtrahend is not a foldable offset, so keep it absolute.
  if (Def->Opcode == NdOp::INT_SUB) {
    auto KC = traceSSAConst(Def->Inputs[1]);
    if (!KC || !collectIndexedGlobalBase(Def->Inputs[0], Base, HaveBase,
                                         IdxTerms, Depth + 1))
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
    const auto *Seg = Img->getSegmentFor(*C);
    return Seg && !Seg->isExecutable() && !Seg->Data.empty();
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
    Base = *CA;
    HaveBase = true;
    IdxTerms.push_back(B);
    return true;
  }
  if (BBase) {
    Base = *CB;
    HaveBase = true;
    IdxTerms.push_back(A);
    return true;
  }
  // Neither operand is the base.  Recurse into a non-constant side to find the
  // base nested under multi-dimensional indexing (`base + row*stride + col`) or
  // past a constant field offset (`base + i*stride + off`); each non-base side
  // (including a constant offset) becomes an index addend.
  if (!CA && collectIndexedGlobalBase(A, Base, HaveBase, IdxTerms, Depth + 1)) {
    IdxTerms.push_back(B);
    return true;
  }
  if (!CB && collectIndexedGlobalBase(B, Base, HaveBase, IdxTerms, Depth + 1)) {
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
    if (!KC || !collectLiteralPoolBase(Def->Inputs[0], Base, HaveBase, IdxTerms,
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
    Base = *CA;
    HaveBase = true;
    IdxTerms.push_back(B);
    return true;
  }
  if (CB && SawB && !CA) {
    Base = *CB;
    HaveBase = true;
    IdxTerms.push_back(A);
    return true;
  }
  // Neither side is itself the literal-pool base: descend the side that exposes
  // one (`base + row*stride + col`); the other whole side is an index term.
  if (!CA && collectLiteralPoolBase(A, Base, HaveBase, IdxTerms, Depth + 1)) {
    IdxTerms.push_back(B);
    return true;
  }
  if (!CB && collectLiteralPoolBase(B, Base, HaveBase, IdxTerms, Depth + 1)) {
    IdxTerms.push_back(A);
    return true;
  }
  return false;
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

//===----------------------------------------------------------------------===//
// Frame-derived / stack-pointer classification and dynamic (VLA) stack-
// allocation recovery (varIsFrameDerived, varIsStackPtrDerived, addrSlotKey,
// tryEmitDynamicStackAlloc, tryResolveDynVlaAddr, isStackProbeCall, ...) are
// defined in MedLLVMFrameResolve.cpp so this file stays focused on read-only /
// table / code-pointer resolution.
//===----------------------------------------------------------------------===//

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

llvm::Constant *MedLLVMEmitter::buildCodePtrSegmentGlobal(uint64_t SlotVA,
                                                          uint64_t &OutSegVA) {
  if (!Img || SlotVA == 0)
    return nullptr;
  const unsigned PtrSz = Img->getPointerSize();
  if (PtrSz == 0)
    return nullptr;

  const Segment *Seg = Img->getSegmentFor(SlotVA);
  if (!Seg || Seg->Data.empty() || Seg->isExecutable())
    return nullptr;

  // A relocated pointer table (`.data.rel.ro`, or a `.rodata` dispatch/string-
  // pointer table) is read-only after relocation.  clang can lay such a table
  // out CONTIGUOUSLY with an adjacent read-only segment and then fold a
  // pointer- chain walk into a single base+offset access that crosses the
  // segment boundary — e.g. a `static const` `next`-chain whose tail node (its
  // pointer null, so no relocation) is emitted into `.rodata` right after the
  // `.data.rel.ro` head nodes, and a `q=q->next` loop the optimizer proves
  // contiguous becomes `&head + clamp(steps)*stride`.  Mirror the whole
  // contiguous read-only-after-relocation RUN — not just `Seg` — so that cross-
  // segment offset stays in bounds, exactly as embedRodataRun preserves the
  // relative layout of adjacent pure-rodata segments.  A genuinely mutable
  // `.data` function-pointer table (reassigned at runtime) is NOT extended
  // (readOnlyAfterRelocRun returns just `Seg`): it stays a single writable
  // mirror.
  uint64_t RunStart = 0, RunEnd = 0;
  readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
  OutSegVA = RunStart;

  // Mirror a segment/run that holds code- OR data-pointer slots (a
  // `.data.rel.ro` dispatch table or a `const char*[]` string-pointer table);
  // pure data segments with no relocated pointers keep their existing handling.
  // Slots from both sets, anywhere in the run, are merged in address order
  // below.
  struct PtrSlot {
    uint64_t VA;
    bool IsData;
  };
  std::vector<PtrSlot> Slots;
  for (uint64_t S : Img->CodePtrRelocSlots)
    if (S >= RunStart && S + PtrSz <= RunEnd)
      Slots.push_back({S, false});
  for (uint64_t S : Img->DataPtrRelocSlots)
    if (S >= RunStart && S + PtrSz <= RunEnd)
      Slots.push_back({S, true});
  if (Slots.empty())
    return nullptr;
  std::sort(Slots.begin(), Slots.end(),
            [](const PtrSlot &A, const PtrSlot &B) { return A.VA < B.VA; });

  if (auto It = CodePtrTableGlobals.find(RunStart);
      It != CodePtrTableGlobals.end())
    return It->second;

  // Mirror the segment/run as a packed struct: each pointer slot becomes a
  // `ptrtoint @target` field (a relocatable reference that survives relinking),
  // and the bytes between slots stay verbatim.  Any access — a compact pointer
  // table, a strided struct-of-pointers vtable, or a scalar data field beside a
  // pointer — then resolves by GEPing into this mirror at its byte offset.
  // Verbatim bytes come from a combined run buffer so a contiguous neighbour
  // (the `.rodata` chain tail above) is preserved at its run-relative offset.
  std::vector<uint8_t> RunBuf(static_cast<size_t>(RunEnd - RunStart), 0);
  if (RunStart == Seg->VA && RunEnd == Seg->VA + Seg->Data.size()) {
    std::memcpy(RunBuf.data(), Seg->Data.data(), Seg->Data.size());
  } else {
    for (const auto &S : Img->Segments)
      if (isReadOnlyAfterReloc(&S) && S.VA >= RunStart &&
          S.VA + S.Data.size() <= RunEnd)
        std::memcpy(RunBuf.data() + static_cast<size_t>(S.VA - RunStart),
                    S.Data.data(), S.Data.size());
  }
  const uint8_t *Data = RunBuf.data();
  size_t Size = RunBuf.size();

  // Drop overlapping slots and verify every CODE-pointer slot resolves up
  // front. A code pointer that cannot map to a recompiled function aborts the
  // mirror (the segment falls back to a verbatim embed); resolving them before
  // the global exists keeps that abort clean.  Data-pointer targets are
  // resolved only AFTER the global is created and memoized below: a relocated
  // data pointer can point back INTO this same segment (a
  // statically-initialized `next`-style chain) or form a cross-segment cycle,
  // and resolving it before the memo is set would re-enter
  // buildCodePtrSegmentGlobal for the same segment and recurse without bound
  // (stack overflow).  Every reloc slot becomes a pointer field regardless, so
  // the layout is target-independent.
  struct KeptSlot {
    size_t Off;
    uint64_t TargetVA;
    bool IsData;
  };
  std::vector<KeptSlot> Kept;
  size_t Cur = 0;
  for (const auto &Slot : Slots) {
    size_t Off = static_cast<size_t>(Slot.VA - RunStart);
    if (Off < Cur)
      continue; // overlapping/duplicate slot — skip
    uint64_t TargetVA = 0;
    std::memcpy(&TargetVA, Data + Off, PtrSz);
    if (!Slot.IsData) {
      auto NameIt = FuncNames.find(TargetVA);
      if (NameIt == FuncNames.end() || !Mod->getFunction(NameIt->second))
        return nullptr; // code pointer does not resolve — abort the mirror
    }
    Kept.push_back({Off, TargetVA, Slot.IsData});
    Cur = Off + PtrSz;
  }

  auto *PtrIntTy = llvm::IntegerType::get(*Ctx, PtrSz * 8);
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);

  // Layout pass: a byte run between slots + one pointer-int field per kept
  // slot.
  std::vector<llvm::Type *> FieldTys;
  size_t Cursor = 0;
  for (const auto &K : Kept) {
    if (K.Off > Cursor)
      FieldTys.push_back(llvm::ArrayType::get(I8Ty, K.Off - Cursor));
    FieldTys.push_back(PtrIntTy);
    Cursor = K.Off + PtrSz;
  }
  if (Size > Cursor)
    FieldTys.push_back(llvm::ArrayType::get(I8Ty, Size - Cursor));

  auto *StructTy = llvm::StructType::get(*Ctx, FieldTys, /*isPacked=*/true);
  // A plain writable .data segment holding a function-pointer global (mutable,
  // reassigned at runtime) must be a writable global so stores into a slot are
  // legal; RELRO and rodata pointer tables stay constant (read-only after
  // relocation) — their slots are never stored to.
  bool SegWritable = Seg->isWritable() && !Seg->isExecutable() &&
                     !section_names::isDataRelRoSectionName(Seg->Name);
  auto *GV = new llvm::GlobalVariable(
      *Mod, StructTy, /*isConstant=*/!SegWritable, dataLinkage(),
      llvm::ConstantAggregateZero::get(StructTy),
      (kNdCodePtrPrefix + llvm::utohexstr(RunStart)).str());
  GV->setAlignment(llvm::Align(16));
  markSharedLocal(GV);
  // Memoize BEFORE resolving data-pointer targets so a self-referential or
  // cyclic pointer that resolves back into this run returns this global
  // (tryResolveGlobalData GEPs into it) instead of recursing without bound.
  CodePtrTableGlobals[RunStart] = GV;

  // Value pass: fill byte runs verbatim and each slot with its resolved target.
  std::vector<llvm::Constant *> Fields;
  auto addBytes = [&](size_t From, size_t To) {
    if (To > From)
      Fields.push_back(llvm::ConstantDataArray::get(
          *Ctx, llvm::ArrayRef<uint8_t>(Data + From, To - From)));
  };
  Cursor = 0;
  for (const auto &K : Kept) {
    addBytes(Cursor, K.Off);
    llvm::Constant *FieldVal = nullptr;
    if (!K.IsData) {
      llvm::Function *F = Mod->getFunction(FuncNames.find(K.TargetVA)->second);
      FieldVal = llvm::ConstantExpr::getPtrToInt(F, PtrIntTy);
    } else if (llvm::Constant *G = tryResolveGlobalData(K.TargetVA)) {
      FieldVal = llvm::ConstantExpr::getPtrToInt(G, PtrIntTy);
    } else {
      // Unresolvable data pointer: keep the original VA (byte-identical to the
      // verbatim fallback) so the field count still matches the layout.
      FieldVal = llvm::ConstantInt::get(PtrIntTy, K.TargetVA);
    }
    Fields.push_back(FieldVal);
    Cursor = K.Off + PtrSz;
  }
  addBytes(Cursor, Size);

  GV->setInitializer(llvm::ConstantStruct::get(StructTy, Fields));
  return GV;
}

uint64_t MedLLVMEmitter::ptrTableUniqueSegment(const MedVar &V) const {
  if (!CurMedFunc || !Img)
    return 0;
  if (V.isConst())
    return 0;

  // Memoize per non-constant address value: a pure function of the (immutable
  // during emit) function body, queried repeatedly for the same values.
  ensureAddrPredCache();
  std::pair<int64_t, int> CacheKey{
      static_cast<int64_t>(
          (static_cast<uint64_t>(static_cast<uint32_t>(V.Id)) << 32) |
          static_cast<uint32_t>(V.SSAVer)),
      static_cast<int>(V.Kind)};
  if (auto It = PtrTableUniqueSegCache.find(CacheKey);
      It != PtrTableUniqueSegCache.end())
    return It->second;

  // A segment that carries relocated code/data pointer slots (a dispatch or
  // string-pointer table) is the only kind buildCodePtrSegmentGlobal mirrors.
  auto segHasPtrSlots = [&](const Segment *Seg) {
    if (!Seg)
      return false;
    uint64_t Lo = Seg->VA, Hi = Seg->VA + Seg->Data.size();
    for (uint64_t S : Img->CodePtrRelocSlots)
      if (S >= Lo && S < Hi)
        return true;
    for (uint64_t S : Img->DataPtrRelocSlots)
      if (S >= Lo && S < Hi)
        return true;
    return false;
  };
  auto findDef = [&](const MedVar &X) { return lookupDef(X); };
  auto findPhi = [&](const MedVar &X) { return lookupPhi(X); };

  auto compute = [&]() -> uint64_t {
  uint64_t SegVA = 0;
  bool Found = false;
  std::vector<MedVar> Work{V};
  std::set<std::tuple<int, int, int>> Seen;
  // The Seen set bounds the walk to the function's distinct address-arithmetic
  // values; the counter is only a safety cap against a pathological DAG (and is
  // large enough that an index subtree — e.g. a deep PRNG chain feeding the
  // index — never starves the base operands of a `base + index` address).
  int Budget = 4096;
  while (!Work.empty() && Budget-- > 0) {
    MedVar Cur = Work.back();
    Work.pop_back();

    // A subexpression that folds to a single constant is a base or an offset:
    // a plain constant, `const + const`, or the ARM biased literal-pool base
    // `const_offset + ldr[pc]` (the table VA split as a constant plus a literal
    // the loader applied).  SawLoad distinguishes a literal-pool-derived base
    // (getVar reloads the raw VA, so no redirect guard) from a plain constant
    // operand (which getVar may rewrite to a relocated global).
    bool SawLoad = false;
    if (auto C = traceTableBaseConst(Cur, 0, &SawLoad)) {
      const Segment *Seg = Img->getSegmentFor(*C);
      if (Seg && !Seg->isExecutable() && !Seg->Data.empty() &&
          segHasPtrSlots(Seg)) {
        if (!SawLoad &&
            (*C >= limits::kMinGlobalDataAddr ||
             Img->RelocDataAddrs.count(*C) || Img->RodataAnchorSeg.count(*C)))
          return 0; // a plain-constant base getVar would redirect
        if (Found && SegVA != Seg->VA)
          return 0; // base constants span multiple pointer-table segments
        SegVA = Seg->VA;
        Found = true;
      }
      continue; // fully constant: recorded as a base, else an ignorable offset
    }
    if (Cur.isConst())
      continue;

    auto K = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer);
    if (!Seen.insert(K).second)
      continue;
    if (const MedOp *Def = findDef(Cur)) {
      switch (Def->Opcode) {
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_AND:
      case NdOp::INT_OR:
      case NdOp::INT_XOR:
      case NdOp::INT_LEFT:
      case NdOp::INT_RIGHT:
      case NdOp::INT_ASHR:
      case NdOp::INT_MULT:
      case NdOp::INT_NEGATE:
      case NdOp::INT_NEG2:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::COPY:
      case NdOp::SUBBYTES:
      case NdOp::SELECT:
        for (int I = 0; I < Def->NumInputs; ++I)
          Work.push_back(Def->Inputs[I]);
        break;
      case NdOp::LOAD:
        // Stack spill/reload: a register-constrained target (i386/ARM32) spills
        // the table base to a stack slot; follow the matching STORE's value.
        if (Def->NumInputs >= 1)
          if (auto LKey = addrSlotKey(Def->Inputs[0]))
            for (const auto &B : CurMedFunc->Blocks)
              for (const auto &O : B.Ops)
                if (O.Opcode == NdOp::STORE && O.NumInputs >= 2) {
                  auto SKey = addrSlotKey(O.Inputs[0]);
                  if (SKey && *SKey == *LKey)
                    Work.push_back(O.Inputs[1]);
                }
        break;
      default:
        break; // stop at any non-address-arithmetic producer
      }
      continue;
    }
    if (const PhiNode *Phi = findPhi(Cur))
      for (const auto &[PredId, Arg] : Phi->Args) {
        (void)PredId;
        Work.push_back(Arg);
      }
  }
  return Found ? SegVA : 0;
  };

  uint64_t Result = compute();
  PtrTableUniqueSegCache[CacheKey] = Result;
  return Result;
}

llvm::Value *
MedLLVMEmitter::tryResolveCodePtrTablePtr(const MedVar &AddrVar,
                                          llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  // Path 1: one constant base + runtime index addends, the common
  // `lea table; mov (table,idx)` shape.  The base is a direct constant (x86-64
  // rip-relative / AArch64 ADRP / i386 GOTOFF) or folds through an ARM literal-
  // pool load (`ldr rN,[pc]; add rN,pc`).
  uint64_t Base = 0;
  bool HaveBase = false;
  std::vector<MedVar> IdxTerms;
  bool HaveConst =
      collectIndexedGlobalBase(AddrVar, Base, HaveBase, IdxTerms) && HaveBase &&
      Base != 0 && !IdxTerms.empty();
  if (!HaveConst) {
    Base = 0;
    HaveBase = false;
    IdxTerms.clear();
    HaveConst = collectLiteralPoolBase(AddrVar, Base, HaveBase, IdxTerms) &&
                HaveBase && Base != 0 && !IdxTerms.empty();
  }
  if (HaveConst) {
    // Redirect only into a segment that holds pointer slots; also gate out
    // frame-derived "indices" that would be stack-pointer arithmetic.
    uint64_t SegVA = 0;
    if (auto *G = buildCodePtrSegmentGlobal(Base, SegVA)) {
      bool FrameIdx = false;
      for (const auto &T : IdxTerms)
        if (varIsFrameDerived(T)) {
          FrameIdx = true;
          break;
        }
      if (!FrameIdx) {
        // Byte offset into the mirror = (base - segment base) + runtime index.
        unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
        auto *IdxTy = llvm::IntegerType::get(*Ctx, AddrBits);
        llvm::Value *IdxVal = llvm::ConstantInt::get(IdxTy, Base - SegVA);
        bool Ok = true;
        for (const auto &T : IdxTerms) {
          llvm::Value *TV = getVar(T, Builder);
          if (!TV) {
            Ok = false;
            break;
          }
          if (TV->getType()->isPointerTy())
            TV = Builder.CreatePtrToInt(TV, IdxTy);
          else if (TV->getType() != IdxTy)
            TV = Builder.CreateZExtOrTrunc(TV, IdxTy);
          IdxVal = Builder.CreateAdd(IdxVal, TV, "cptidx");
        }
        if (Ok)
          return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, IdxVal,
                                   "cptptr");
      }
    }
  }

  // Path 2: the base is a SELECT/PHI or branchless AND/OR mask of several base
  // constants (a `cond ? A[i] : B[j]` pointer select), so path 1 cannot isolate
  // a single constant base.  When every base-like constant in the address lands
  // in ONE pointer-table segment, the whole address provably indexes that
  // segment, so redirect by the address's own value: GEP(@seg, addr - segVA).
  if (uint64_t SelSeg = ptrTableUniqueSegment(AddrVar)) {
    uint64_t OutSeg = 0;
    if (auto *G = buildCodePtrSegmentGlobal(SelSeg, OutSeg)) {
      if (llvm::Value *A = getVar(AddrVar, Builder)) {
        unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
        auto *IdxTy = llvm::IntegerType::get(*Ctx, AddrBits);
        if (A->getType()->isPointerTy())
          A = Builder.CreatePtrToInt(A, IdxTy);
        else if (A->getType() != IdxTy)
          A = Builder.CreateZExtOrTrunc(A, IdxTy);
        llvm::Value *Off =
            Builder.CreateSub(A, llvm::ConstantInt::get(IdxTy, OutSeg));
        return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, Off, "cptsel");
      }
    }
  }
  return nullptr;
}

llvm::Value *
MedLLVMEmitter::tryResolveCodeRefValue(const MedVar &V,
                                       llvm::IRBuilder<> &Builder) {
  if (!Img || !CurMedFunc || V.isConst())
    return nullptr;
  bool SawLoad = false;
  auto VA = traceTableBaseConst(V, 0, &SawLoad);
  // Only a literal-pool-derived value (a genuine PC-relative address-of) is a
  // function pointer; a plain computed value must not be reinterpreted.
  if (!VA || !SawLoad)
    return nullptr;
  auto FIt = FuncNames.find(*VA);
  if (FIt == FuncNames.end())
    return nullptr;
  llvm::Function *F = Mod->getFunction(FIt->second);
  if (!F)
    return nullptr;
  unsigned PtrSz = Img->getPointerSize() ? Img->getPointerSize() : 8;
  return Builder.CreatePtrToInt(
      F, sizeToType(V.Size > 0 ? V.Size : static_cast<uint16_t>(PtrSz)),
      "fnptr");
}

} // namespace neverd
