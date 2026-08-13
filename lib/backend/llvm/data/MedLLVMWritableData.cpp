//===- MedLLVMWritableData.cpp - Writable data resolution ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Writable (.data/.bss) segment address resolution for MedLLVMEmitter:
/// locating the writable segment an address indexes and redirecting the
/// access into the cohesive global that embedWritableRun rebuilt.  The
/// already-symbolized-pointer predicates this consults live in
/// MedLLVMSymbolizedPtr.cpp.
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
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

uint64_t MedLLVMEmitter::writableDataSegOf(const MedVar &AddrVar,
                                           bool RequireRelocBase) const {
  if (!Img || !CurMedFunc)
    return 0;
  auto writableSeg = [&](uint64_t C) -> const Segment * {
    if (C == 0)
      return nullptr;
    const Segment *S = Img->getSegmentFor(C);
    if (!S || !isMutableDataSeg(S) || (S->Data.empty() && S->Size == 0))
      return nullptr;
    // A VALUE context (store/return operand) demands the base be a recorded
    // writable relocation target: an ordinary data immediate that merely lands
    // in a wide low-VA run (a 4-byte LCG increment on a 32-bit target) is not a
    // stored-pointer base.  An ADDRESS context trusts the segment-range test
    // (the base is definitionally a pointer there, and an i386 PIC GOTOFF base
    // folds to a VA that never appears as a literal the reloc set or
    // constUsedAsPointer can confirm).
    if (RequireRelocBase && !Img->WritableRelocDataAddrs.count(C))
      return nullptr;
    return S;
  };
  if (AddrVar.isConst()) {
    const Segment *S = writableSeg(AddrVar.ConstVal);
    return S ? S->VA : 0;
  }

  // Memoize the DAG walk per non-constant address value: it is a pure function
  // of the (immutable during emit) function body, and the same address value is
  // queried repeatedly as a load/store address and as a stored value operand.
  ensureAddrPredCache();
  std::tuple<int, int, int, bool> CacheKey{static_cast<int>(AddrVar.Kind),
                                           AddrVar.Id, AddrVar.SSAVer,
                                           RequireRelocBase};
  if (auto It = WritableDataSegCache.find(CacheKey);
      It != WritableDataSegCache.end())
    return It->second;

  auto findDef = [&](const MedVar &X) { return lookupDef(X); };
  auto findPhi = [&](const MedVar &X) { return lookupPhi(X); };

  auto compute = [&]() -> uint64_t {
    uint64_t SegVA = 0;
    bool Found = false;
    std::vector<MedVar> Work{AddrVar};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 4096;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      // A fully-constant subexpression is a base or an offset: record it as a
      // base only when it lands in a writable data segment.
      if (auto C = traceTableBaseConst(Cur)) {
        if (const Segment *S = writableSeg(*C)) {
          if (Found && SegVA != S->VA)
            return 0; // base constants span two distinct data segments
          SegVA = S->VA;
          Found = true;
        }
        continue;
      }
      if (Cur.isConst())
        continue;
      auto K = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer);
      if (!Seen.insert(K).second)
        continue;
      if (const MedOp *Def = findDef(Cur)) {
        switch (Def->Opcode) {
        // Address-forming ops: the base may live in any operand, so descend.
        case NdOp::INT_ADD:
        case NdOp::INT_SUB:
        case NdOp::INT_OR:
        case NdOp::COPY:
        case NdOp::INT_ZEXT:
        case NdOp::INT_SEXT:
        case NdOp::SUBBYTES:
        case NdOp::SELECT:
          for (int I = 0; I < Def->NumInputs; ++I)
            Work.push_back(Def->Inputs[I]);
          break;
        case NdOp::INT_AND:
          // Branchless base blend `(-cond & baseA) | (~cond & baseB)`: each arm
          // is `mask & baseVA`, the constant being a table base rather than a
          // mask when it lands in a writable segment (an ordinary index mask's
          // constant is a small bitmask in no segment).  Record such a constant
          // as a base; the runtime mask operand carries no base, so descending
          // it is inert.
          for (int I = 0; I < Def->NumInputs; ++I) {
            if (Def->Inputs[I].isConst()) {
              uint64_t C = Def->Inputs[I].ConstVal;
              // A contiguous low-bit mask (C == 2^k - 1, e.g. 0xff/0xffff from
              // a byte/half-word extraction) is a bitmask, not a base address —
              // even when it coincidentally lands inside a small relocatable
              // object's writable segment VA range (a .bss at 0xD8 spans 0xFF).
              // A genuine base-blend constant is a full global VA, essentially
              // never of that form; treating the mask as a base mis-symbolizes
              // an RMW value `(x & 0xff) + load(G)` stored back as a G pointer.
              if (C != 0 && (C & (C + 1)) == 0)
                continue;
              if (const Segment *S = writableSeg(C)) {
                if (Found && SegVA != S->VA)
                  return 0;
                SegVA = S->VA;
                Found = true;
              }
            } else {
              Work.push_back(Def->Inputs[I]);
            }
          }
          break;
        case NdOp::LOAD:
          // Stack spill/reload of a writable-data base: a register-constrained
          // target (i386 PIC) spills a `lea`/GOTOFF global address to a frame
          // slot and reloads it to blend into a call-arg / stored pointer.
          // Follow the matching STORE's value to reach the spilled base
          // constant. addrSlotKey only keys on a register-rooted slot, so a
          // load from a global pointer (constant address) yields no key and is
          // left as an index computation. A slot whose ADDRESS escapes is
          // skipped: a callee may have written an already-symbolized pointer
          // there, so the in-function constant base no longer matches the
          // runtime value (double-relocation, the #475 shape).
          if (Def->NumInputs >= 1)
            if (auto LKey = addrSlotKey(Def->Inputs[0]);
                LKey && !stackSlotAddressEscapes(Def->Inputs[0]))
              for (const auto &B : CurMedFunc->Blocks)
                for (const auto &O : B.Ops)
                  if (O.Opcode == NdOp::STORE && O.NumInputs >= 2)
                    if (auto SKey = addrSlotKey(O.Inputs[0]);
                        SKey && *SKey == *LKey)
                      Work.push_back(O.Inputs[1]);
          break;
        default:
          // INT_MULT/XOR/shift: an index computation, not a base — its
          // constants are scales/masks, so do not search inside it for a base.
          break;
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
  WritableDataSegCache[CacheKey] = Result;
  return Result;
}

llvm::Value *MedLLVMEmitter::tryResolveWritableData(const MedVar &AddrVar,
                                                    uint16_t SizeHint,
                                                    llvm::IRBuilder<> &Builder,
                                                    bool IsValueOperand) {
  (void)SizeHint;
  if (!Img || !CurMedFunc)
    return nullptr;
  uint64_t SegVA =
      writableDataSegOf(AddrVar, /*RequireRelocBase=*/IsValueOperand);
  if (!SegVA)
    return nullptr;
  auto [RunGV, RunStart] = embedWritableRun(SegVA);
  if (!RunGV)
    return nullptr;

  // A compiler may merge two forms of the same writable-data pointer at a
  // MedIR PHI: one arm loaded from a relocated data-pointer table (already
  // emitted as ptrtoint(@writable_run + off)), and another arm materialized
  // from an executable literal pool (still the original image VA).  MedIR
  // phis are lowered through edge copies and allocas, so inspect the MedIR
  // provenance here; the pre-optimization LLVM value is only an alloca load.
  std::function<bool(const MedVar &, int)> carriesSymbolizedPointer =
      [&](const MedVar &V, int Depth) -> bool {
    if (V.isConst() || Depth > 16)
      return false;
    if (const PhiNode *Phi = lookupPhi(V)) {
      for (const auto &[PredId, Arg] : Phi->Args) {
        (void)PredId;
        if (carriesSymbolizedPointer(Arg, Depth + 1))
          return true;
      }
      return false;
    }
    const MedOp *Def = lookupDef(V);
    if (!Def)
      return false;
    if (Def->Opcode == NdOp::LOAD)
      return Def->NumInputs >= 1 && ptrTableUniqueSegment(Def->Inputs[0]) != 0;
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_OR:
    case NdOp::INT_AND:
    case NdOp::INT_XOR:
    case NdOp::SELECT:
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (carriesSymbolizedPointer(Def->Inputs[I], Depth + 1))
          return true;
      break;
    default:
      break;
    }
    return false;
  };
  auto resolveMixedPointerPhi = [&]() -> llvm::Value * {
    const PhiNode *BasePhi = lookupPhi(AddrVar);
    if (!BasePhi)
      if (const MedOp *Def = lookupDef(AddrVar))
        if (Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB ||
            Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
            Def->Opcode == NdOp::INT_SEXT || Def->Opcode == NdOp::SUBBYTES)
          for (uint8_t I = 0; I < Def->NumInputs; ++I)
            if (const PhiNode *Candidate = lookupPhi(Def->Inputs[I])) {
              BasePhi = Candidate;
              break;
            }
    if (!BasePhi)
      return nullptr;
    bool AnySymbolized = false;
    bool AnyRaw = false;
    for (const auto &[PredId, Arg] : BasePhi->Args) {
      (void)PredId;
      bool IsSymbolized = carriesSymbolizedPointer(Arg, 0);
      AnySymbolized |= IsSymbolized;
      AnyRaw |= !IsSymbolized;
    }
    if (!AnySymbolized)
      return nullptr;

    llvm::Value *Raw = getVar(AddrVar, Builder);
    if (!Raw)
      return nullptr;
    auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
    if (!AnyRaw)
      return Raw->getType()->isPointerTy()
                 ? Raw
                 : Builder.CreateIntToPtr(Raw, PtrTy, "wrptr.symbolized");

    const Segment *RunSeg = Img->getSegmentFor(RunStart);
    uint64_t RunLen =
        RunSeg ? (RunSeg->Size ? RunSeg->Size : RunSeg->Data.size()) : 0;
    if (RunLen == 0)
      return nullptr;
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    llvm::Value *RawInt = Raw;
    if (RawInt->getType()->isPointerTy())
      RawInt = Builder.CreatePtrToInt(RawInt, I64Ty);
    else if (RawInt->getType() != I64Ty)
      RawInt = Builder.CreateZExtOrTrunc(RawInt, I64Ty);
    llvm::Value *InOldRun = Builder.CreateAnd(
        Builder.CreateICmpUGE(RawInt, llvm::ConstantInt::get(I64Ty, RunStart)),
        Builder.CreateICmpULT(
            RawInt, llvm::ConstantInt::get(I64Ty, RunStart + RunLen)));
    llvm::Value *OldOff =
        Builder.CreateSub(RawInt, llvm::ConstantInt::get(I64Ty, RunStart));
    llvm::Value *Rebased = Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV,
                                             OldOff, "wrptr.rebased");
    llvm::Value *AlreadySymbolized =
        Raw->getType()->isPointerTy()
            ? Raw
            : Builder.CreateIntToPtr(Raw, PtrTy, "wrptr.relocated");
    return Builder.CreateSelect(InOldRun, Rebased, AlreadySymbolized,
                                "wrptr.mixed");
  };
  if (!IsValueOperand)
    if (llvm::Value *P = resolveMixedPointerPhi())
      return P;

  auto i386WritableBlendAddr = [&](const MedVar &V) -> bool {
    if (TargetArch != Arch::X86 || !CurMedFunc || V.isConst())
      return false;
    auto findDef = [&](const MedVar &X) { return lookupDef(X); };
    auto findPhi = [&](const MedVar &X) { return lookupPhi(X); };
    std::set<std::tuple<int, int, int>> Seen;
    std::vector<MedVar> Work{V};
    int Budget = 64;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst())
        continue;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *Ph = findPhi(Cur)) {
        for (const auto &[PredId, Arg] : Ph->Args) {
          (void)PredId;
          Work.push_back(Arg);
        }
        continue;
      }
      const MedOp *D = findDef(Cur);
      if (!D)
        continue;
      if (D->Opcode == NdOp::INT_OR)
        return true;
      switch (D->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::SUBBYTES:
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_AND:
      case NdOp::INT_XOR:
      case NdOp::SELECT:
        for (int I = 0; I < D->NumInputs; ++I)
          Work.push_back(D->Inputs[I]);
        break;
      default:
        break;
      }
    }
    return false;
  };

  // A walked stack-spilled pointer dereferenced as `*q` re-bases `q` against
  // the run (`GEP(@G, q)`) ONLY when `q` was spilled as the RAW original VA.
  // When the spill SYMBOLIZED `q` (the i386 `q = &G[0]` init folds `GOT_base(0)
  // + G@GOTOFF` and getVar emits `ptrtoint(@G)`), `q` already IS the recompiled
  // pointer, so re-basing it would reference @G twice (`@G + @G`).  Defer such
  // a spill to the reloadsSymbolizedWritablePtr branch below, which uses it
  // directly (the gptrrw `unsigned *q=PW; *q; q++` double-base shape).
  if (TargetArch == Arch::X86 && !IsValueOperand &&
      !i386WritableBlendAddr(AddrVar) && i386WalkedPointerDeref(AddrVar) &&
      !reloadsSymbolizedWritablePtr(AddrVar)) {
    llvm::Value *P = getVar(AddrVar, Builder);
    if (!P)
      return nullptr;
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    llvm::Value *Off = P;
    if (Off->getType() != I64Ty)
      Off = Builder.CreateZExtOrTrunc(Off, I64Ty);
    return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV, Off, "wrptr");
  }

  // A reload of a spilled induction pointer (`q = &G[i]; *q; q++` at -O0) was
  // stored already symbolized to `@G + i`, so getVar(AddrVar) IS that
  // relocatable pointer.  Re-basing it (`@G + (val - segVA)`) would reference
  // @G a second time and the `- segVA` only cancels at the lift-time VA, so use
  // it directly (the writable counterpart of the #507 induction double-base
  // fix). The same double-base arises when a SECOND large global shares the
  // segment
  // (`static T A[N], B[N]`): B's base const `segBase + sizeof A` exceeds the
  // pointer threshold and getVar already symbolizes it to `@G + sizeof A`. Both
  // are recognised on the MedIR address (getVar emits temps through a virtual
  // stack alloca/load, so the @G reference is not visible on the emitted
  // value).
  if (reloadsSymbolizedWritablePtr(AddrVar) ||
      addrHasSymbolizedSegConst(AddrVar, SegVA)) {
    llvm::Value *P = getVar(AddrVar, Builder);
    if (!P)
      return nullptr;
    if (P->getType()->isPointerTy())
      return P;
    if (TargetArch == Arch::X86 && !IsValueOperand &&
        !i386WritableBlendAddr(AddrVar) &&
        addrHasSymbolizedSegConst(AddrVar, SegVA) &&
        !i386WalkedPointerDeref(AddrVar)) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(P)) {
        if (CI->getSExtValue() <
            static_cast<int64_t>(limits::kMinGlobalDataAddr)) {
          auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
          return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV,
                                   Builder.CreateZExtOrTrunc(P, I64Ty),
                                   "wrptr");
        }
      } else if (i386PeeledInitStoreAddr(AddrVar, SegVA)) {
        auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
        llvm::Value *Off = P;
        if (Off->getType() != I64Ty)
          Off = Builder.CreateZExtOrTrunc(Off, I64Ty);
        return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV, Off,
                                 "wrptr");
      } else {
        MedVar SegOff;
        uint64_t BaseConstVA = 0;
        if (i386WritableSegBasePlusOff(AddrVar, SegVA, SegOff, BaseConstVA)) {
          llvm::Value *Off = getVar(SegOff, Builder);
          if (!Off)
            return nullptr;
          auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
          if (Off->getType() != I64Ty)
            Off = Builder.CreateZExtOrTrunc(Off, I64Ty);
          // The base const is `runStart + field_off` (e.g. `&st.hist` = `&st +
          // 8`); add that in-segment displacement so an indexed struct-field
          // access `&st.hist[i]` is not collapsed to `&st + i`.
          int64_t FieldDisp = static_cast<int64_t>(BaseConstVA) -
                              static_cast<int64_t>(RunStart);
          if (FieldDisp != 0)
            Off = Builder.CreateAdd(
                Off, llvm::ConstantInt::getSigned(I64Ty, FieldDisp));
          return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV, Off,
                                   "wrptr");
        }
      }
    }
    if (TargetArch == Arch::X86 && !IsValueOperand) {
      auto peelRunBase = [&](llvm::Value *V) -> llvm::Value * {
        llvm::Instruction *Add = nullptr;
        if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V))
          Add = BO->getOpcode() == llvm::Instruction::Add ? BO : nullptr;
        else if (auto *I = llvm::dyn_cast<llvm::Instruction>(V))
          Add = I->getOpcode() == llvm::Instruction::Add ? I : nullptr;
        if (!Add || Add->getNumOperands() != 2)
          return nullptr;
        auto runRelOff = [&](llvm::Value *Off,
                             llvm::Value *BasePart) -> llvm::Value * {
          if (BasePart == RunGV)
            return Off;
          const llvm::Value *Ptr = nullptr;
          if (auto *PTI = llvm::dyn_cast<llvm::PtrToIntInst>(BasePart))
            Ptr = PTI->getPointerOperand();
          else if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(BasePart))
            if (CE->getOpcode() == llvm::Instruction::PtrToInt)
              Ptr = CE->getOperand(0);
          if (!Ptr)
            return nullptr;
          if (Ptr == RunGV)
            return Off;
          if (auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Ptr)) {
            if (GEP->getPointerOperand() == RunGV && GEP->getNumIndices() == 1)
              return Builder.CreateAdd(Off, GEP->getOperand(1));
          }
          return nullptr;
        };
        if (llvm::Value *O = runRelOff(Add->getOperand(0), Add->getOperand(1)))
          return O;
        if (llvm::Value *O = runRelOff(Add->getOperand(1), Add->getOperand(0)))
          return O;
        return nullptr;
      };
      if (llvm::Value *Off = peelRunBase(P)) {
        auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
        if (Off->getType() != I64Ty)
          Off = Builder.CreateZExtOrTrunc(Off, I64Ty);
        return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV, Off,
                                 "wrptr");
      }
      if (funcUsesI386WalkedPointerDeref() && !i386WritableBlendAddr(AddrVar) &&
          !i386WalkedPointerDeref(AddrVar)) {
        llvm::Instruction *Add = nullptr;
        if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(P))
          Add = BO->getOpcode() == llvm::Instruction::Add ? BO : nullptr;
        else if (auto *I = llvm::dyn_cast<llvm::Instruction>(P))
          Add = I->getOpcode() == llvm::Instruction::Add ? I : nullptr;
        if (Add) {
          for (unsigned I = 0; I < 2; ++I) {
            if (auto *CI =
                    llvm::dyn_cast<llvm::ConstantInt>(Add->getOperand(I))) {
              int64_t C = CI->getSExtValue();
              if (C == -12 || C == -8 || C == -4 || C == 0) {
                auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
                llvm::Value *Off = P;
                if (Off->getType() != I64Ty)
                  Off = Builder.CreateZExtOrTrunc(Off, I64Ty);
                return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV,
                                         Off, "wrptr");
              }
            }
          }
        } else if (i386PeeledInitStoreAddr(AddrVar, SegVA)) {
          auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
          llvm::Value *Off = P;
          if (Off->getType() != I64Ty)
            Off = Builder.CreateZExtOrTrunc(Off, I64Ty);
          return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV, Off,
                                   "wrptr");
        }
      }
    }
    return Builder.CreateIntToPtr(P, llvm::PointerType::get(*Ctx, 0),
                                  "wrrawptr");
  }

  auto *I64 = llvm::Type::getInt64Ty(*Ctx);
  llvm::Value *Raw = nullptr;
  if (AddrVar.isConst()) {
    Raw = llvm::ConstantInt::get(I64, AddrVar.ConstVal);
  } else {
    Raw = getVar(AddrVar, Builder);
    if (!Raw)
      return nullptr;
    if (Raw->getType()->isPointerTy())
      Raw = Builder.CreatePtrToInt(Raw, I64);
    else if (Raw->getType() != I64)
      Raw = Builder.CreateZExtOrTrunc(Raw, I64);
  }
  llvm::Value *Off =
      Builder.CreateSub(Raw, llvm::ConstantInt::get(I64, RunStart), "wroff");
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), RunGV, Off, "wrptr");
}

} // namespace neverd
