//===- MedLLVMSymbolizedPtr.cpp - Symbolized pointer resolution -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resolution and detection of addresses that are already, or must
/// become, relocatable recompiled pointers: reloads of a symbolized
/// writable pointer, addresses carrying a symbolized segment constant,
/// the code-pointer segment mirror access, and the pointer-argument
/// symbolization that mirrors the LOAD/STORE address resolvers.
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

bool MedLLVMEmitter::reloadsSymbolizedWritablePtr(const MedVar &AddrVar) const {
  if (!CurMedFunc || AddrVar.isConst())
    return false;
  auto findDef = [&](const MedVar &X) { return lookupDef(X); };
  // True when a LOAD reloads a stack slot whose matching STORE wrote a
  // writable-global pointer that the store-value path SYMBOLIZED (emitted as a
  // relocatable `@G + i`) rather than kept as the original VA.  This mirrors
  // the emitOp store-value guard exactly: a frame slot reloaded via a
  // matching-key load keeps the original VA — the deref re-bases it — so it is
  // NOT already-symbolized; any other writable-data pointer spill IS, so the
  // deref must use it directly.  (i386 frames are RBP-relative, which
  // varIsFrameDerived does not treat as frame-derived, so the store symbolizes
  // and the deref must match; x64 frames are stack-pointer-relative, so the
  // store keeps the VA and the deref re-bases.)
  unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
  auto reloadIsSymbolizedPtr = [&](const MedOp *Def) -> bool {
    if (Def->Opcode != NdOp::LOAD || Def->NumInputs < 1)
      return false;
    auto LKey = addrSlotKey(Def->Inputs[0]);
    if (!LKey || stackSlotAddressEscapes(Def->Inputs[0]))
      return false;
    for (const auto &B : CurMedFunc->Blocks)
      for (const auto &O : B.Ops) {
        if (O.Opcode != NdOp::STORE || O.NumInputs < 2 || O.Inputs[1].isConst())
          continue;
        auto SKey = addrSlotKey(O.Inputs[0]);
        if (!SKey || *SKey != *LKey || PtrSz == 0 || O.Inputs[1].Size != PtrSz)
          continue;
        // A frame-derived (SP-relative) store normally keeps the original VA
        // and the deref re-bases it — UNLESS getVar SYMBOLIZED the stored
        // value: an i386 PIC `cond ? &A : &B` blend arm is a
        // writable-reloc-target global pointer getVar emits as `@G + off` even
        // on an SP-relative frame, so the slot already holds the recompiled
        // pointer and the deref must use it directly (re-basing it would
        // double-relocate).  traceValueVA folds the `SUBBYTES(ZEXT(base0 +
        // GOTOFF))` materialization to the VA so symbolizesWritableRelocPtr can
        // mirror getVar's decision exactly.
        auto StoreVA = traceValueVA(O.Inputs[1]);
        bool StoreSymbolized =
            StoreVA && symbolizesWritableRelocPtr(*StoreVA, O.Inputs[1].Size);
        if (!StoreSymbolized && varIsFrameDerived(O.Inputs[0]) &&
            frameSlotHasMatchingKeyLoad(O.Inputs[0]))
          continue; // store kept the original VA; the deref re-bases it
        if (StoreSymbolized ||
            writableDataSegOf(O.Inputs[1], /*RequireRelocBase=*/true))
          return true;
      }
    return false;
  };
  // Walk the address-forming chain (COPY/ZEXT/SEXT widen the pointer; ADD/SUB
  // advance it by a constant or runtime index — the `q++` / `&q[k]` update). If
  // ANY leaf is a symbolized-pointer reload, getVar already yields the
  // relocatable `@G + i (+ off)` value, so re-basing it double-references @G. A
  // fresh base constant (`q = &G[0]`, base 0xF4 + GOT(0)) reaches no such
  // reload, so it still re-bases.  INT_MULT/AND/shift are index sub-trees, not
  // pointer advances, so they are not descended.
  std::set<std::tuple<int, int, int>> Seen;
  std::vector<MedVar> Work{AddrVar};
  int Budget = 256;
  while (!Work.empty() && Budget-- > 0) {
    MedVar Cur = Work.back();
    Work.pop_back();
    if (Cur.isConst())
      continue;
    if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
      continue;
    const MedOp *Def = findDef(Cur);
    if (!Def)
      continue;
    if (reloadIsSymbolizedPtr(Def))
      return true;
    if ((Def->Opcode == NdOp::COPY || Def->Opcode == NdOp::INT_ZEXT ||
         Def->Opcode == NdOp::INT_SEXT) &&
        Def->NumInputs >= 1)
      Work.push_back(Def->Inputs[0]);
    else if (Def->Opcode == NdOp::SUBBYTES && Def->NumInputs >= 2 &&
             Def->Inputs[1].isConst() && Def->Inputs[1].ConstVal == 0)
      // Low-slice truncation (the i386 RAX->EAX narrowing of a 32-bit pointer);
      // the pointer value flows through it unchanged.
      Work.push_back(Def->Inputs[0]);
    else if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
             Def->NumInputs >= 2) {
      Work.push_back(Def->Inputs[0]);
      Work.push_back(Def->Inputs[1]);
    } else if ((Def->Opcode == NdOp::INT_OR || Def->Opcode == NdOp::INT_AND ||
                Def->Opcode == NdOp::INT_XOR) &&
               Def->NumInputs >= 2) {
      // A branchless pointer blend `cond ? &A : &B` clang lowers to
      // `(reloadA & mask) | (reloadB & ~mask)`: descend both arms so a
      // symbolized-frame-slot reload inside the blend is reached (the i386
      // structval/structwr family).  The mask sub-trees reach no such reload,
      // so they fall through harmlessly; the symbolized-store gate above is
      // what actually decides, keeping a genuine bitwise integer unaffected.
      Work.push_back(Def->Inputs[0]);
      Work.push_back(Def->Inputs[1]);
    }
  }
  return false;
}

// The i386 (32-bit x86) PIC address recognizers -- i386WalkedPointerDeref,
// funcUsesI386WalkedPointerDeref, i386PeeledInitStoreAddr, and
// i386WritableSegBasePlusOff -- are the only architecture-gated members of this
// resolver, so they live in X86/MedLLVMX86GlobalData.cpp following the
// target-dispatch split; addrHasSymbolizedSegConst (below) stays here because
// it is architecture-neutral and shared with the arch-neutral resolvers.

bool MedLLVMEmitter::addrHasSymbolizedSegConst(const MedVar &AddrVar,
                                               uint64_t SegVA) const {
  if (!Img || !CurMedFunc)
    return false;
  auto findDef = [&](const MedVar &X) { return lookupDef(X); };
  auto findPhi = [&](const MedVar &X) { return lookupPhi(X); };
  std::set<std::tuple<int, int, int>> Seen;
  std::vector<MedVar> Work{AddrVar};
  int Budget = 256;
  while (!Work.empty() && Budget-- > 0) {
    MedVar Cur = Work.back();
    Work.pop_back();
    if (Cur.isConst()) {
      // getVar symbolizes a constant above the threshold that lands in this run
      // to `@G + (C - RunStart)`.  The constUsedAsPointer gate mirrors getVar's
      // mutable-segment guard EXACTLY: a base const reached only through a PHI
      // induction (the i386 PIC B-pointer init, which reaches no direct address
      // scan) is NOT symbolized there and still re-bases — using getVar
      // directly on its raw value would leave the bare original VA unmapped.  A
      // low-VA writable reloc target getVar also symbolizes
      // (symbolizesWritableRelocPtr, the SIMD-lane `&G` form) must likewise be
      // used directly, not re-based.
      if ((Cur.ConstVal > limits::kMinGlobalDataAddr &&
           constUsedAsPointer(Cur.ConstVal)) ||
          symbolizesWritableRelocPtr(Cur.ConstVal, Cur.Size)) {
        const Segment *S = Img->getSegmentFor(Cur.ConstVal);
        if (S && S->VA == SegVA)
          return true;
      }
      continue;
    }
    if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
      continue;
    if (const MedOp *D = findDef(Cur)) {
      switch (D->Opcode) {
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_OR:
      // A branchless pointer select (`cond?A:B`) lowers to `(mask & &A) |
      // (~mask & &B)` — the taken-address constants live behind INT_AND /
      // INT_XOR (the ANDN arm), which getVar still symbolizes, so the result
      // already carries an
      // @G reference and must not be re-based.  Follow them too.
      case NdOp::INT_AND:
      case NdOp::INT_XOR:
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::SUBBYTES:
      case NdOp::SELECT:
        for (int I = 0; I < D->NumInputs; ++I)
          Work.push_back(D->Inputs[I]);
        break;
      default:
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
  return false;
}

llvm::Value *
MedLLVMEmitter::tryResolveCodePtrSegPtr(const MedVar &AddrVar,
                                        llvm::IRBuilder<> &Builder) {
  if (!Img)
    return nullptr;
  // Find the pointer-slot segment the address indexes by walking its arithmetic
  // for an in-segment base constant (this does not require the address to fold
  // to a constant, so it handles the i386 PIC `GOT_base + slot@GOTOFF` form the
  // constant-address path misses — the GOT base folds to 0 only at emit time).
  uint64_t SegVA = ptrTableUniqueSegment(AddrVar);
  if (!SegVA)
    return nullptr;
  uint64_t SegOut = 0;
  llvm::Constant *Tbl = buildCodePtrSegmentGlobal(SegVA, SegOut);
  if (!Tbl)
    return nullptr;
  // getVar may already have rewritten an in-segment constant to the relocated
  // code-pointer mirror.  The emitted value is initially hidden behind a
  // virtual-register alloca/load, so inspect the MedIR rather than the current
  // LLVM value.  Re-basing an already-symbolized value would apply the mirror
  // base twice: `@codeptr + (ptrtoint(@codeptr + off) - segVA)`.
  if (addrHasSymbolizedSegConst(AddrVar, SegVA)) {
    llvm::Value *Raw = getVar(AddrVar, Builder);
    if (!Raw)
      return nullptr;
    if (Raw->getType()->isPointerTy())
      return Raw;
    return Builder.CreateIntToPtr(Raw, llvm::PointerType::getUnqual(*Ctx),
                                  "cptr.direct");
  }
  // GEP by the original address minus the segment base, mirroring
  // tryResolveWritableData: getVar reproduces the original absolute slot VA
  // (the GOT base folds to 0), so `addr - segStart` is the in-segment byte
  // offset.
  auto *I64 = llvm::Type::getInt64Ty(*Ctx);
  llvm::Value *Raw = getVar(AddrVar, Builder);
  if (!Raw)
    return nullptr;
  if (Raw->getType()->isPointerTy())
    Raw = Builder.CreatePtrToInt(Raw, I64);
  else if (Raw->getType() != I64)
    Raw = Builder.CreateZExtOrTrunc(Raw, I64);
  llvm::Value *Off =
      Builder.CreateSub(Raw, llvm::ConstantInt::get(I64, SegOut), "cpoff");
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), Tbl, Off, "cptr");
}

llvm::Value *MedLLVMEmitter::tryResolvePointerArg(const MedVar &AddrVar,
                                                  bool FailClosed,
                                                  llvm::IRBuilder<> &Builder) {
  if (!Img)
    return nullptr;
  // A flat pointer-valued SELECT must be validated as one unit before creating
  // any globals: every non-null leaf has to be a mapped data address.  This
  // two-pass shape prevents a rejected code/scalar arm from leaving behind a
  // partially-symbolized sibling and, more importantly, prevents numeric
  // coincidence from turning an ordinary integer SELECT into a pointer.
  if (!AddrVar.isConst())
    if (const MedOp *Top = lookupDef(AddrVar);
        Top && Top->Opcode == NdOp::SELECT && Top->NumInputs >= 3) {
      const unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
      // ARM materializes a global address as `ldr reg, [pc, #literal]` plus
      // the current PC.  Such a SELECT leaf is fully constant, but only the
      // literal-pool-aware folder can see it; traceSSAConst deliberately stops
      // at the LOAD.  Admit that form only when the fold actually crossed a
      // literal-pool load, preserving the all-arms pointer proof below for
      // ordinary computed integer SELECTs.
      auto tracePointerLeafVA = [&](const MedVar &V) {
        if (V.isConst())
          return std::optional<uint64_t>(V.ConstVal);
        if (auto VA = traceSSAConst(V))
          return VA;
        bool SawLiteralLoad = false;
        auto VA = traceTableBaseConst(V, 0, &SawLiteralLoad);
        return SawLiteralLoad ? VA : std::optional<uint64_t>();
      };
      std::function<bool(const MedVar &, int)> isDataTree =
          [&](const MedVar &V, int Depth) -> bool {
        if (Depth > 16)
          return false;
        if (!V.isConst())
          if (const MedOp *Def = lookupDef(V);
              Def && Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3)
            return isDataTree(Def->Inputs[1], Depth + 1) &&
                   isDataTree(Def->Inputs[2], Depth + 1);
        std::optional<uint64_t> VA = tracePointerLeafVA(V);
        if (!VA)
          return false;
        if (*VA == 0)
          return true;
        const Segment *Seg = Img->getSegmentFor(*VA);
        return Seg && !Seg->Data.empty() && Img->isDataAddress(*VA) &&
               !isFrameRelativeDisplacement(*VA, AddrBits);
      };

      if (isDataTree(AddrVar, 0)) {
        std::function<llvm::Value *(const MedVar &, int)> buildDataTree =
            [&](const MedVar &V, int Depth) -> llvm::Value * {
          if (Depth > 16)
            return nullptr;
          if (!V.isConst())
            if (const MedOp *Def = lookupDef(V);
                Def && Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
              llvm::Value *True = buildDataTree(Def->Inputs[1], Depth + 1);
              llvm::Value *False = buildDataTree(Def->Inputs[2], Depth + 1);
              llvm::Value *Cond = getVar(Def->Inputs[0], Builder);
              if (!True || !False || !Cond)
                return nullptr;
              if (Cond->getType()->isPointerTy())
                Cond = Builder.CreateIsNotNull(Cond, "ptrselc");
              else if (!Cond->getType()->isIntegerTy())
                return nullptr;
              else if (!Cond->getType()->isIntegerTy(1))
                Cond = Builder.CreateICmpNE(
                    Cond, llvm::ConstantInt::get(Cond->getType(), 0),
                    "ptrselc");
              return Builder.CreateSelect(Cond, True, False, "ptrsel");
            }
          std::optional<uint64_t> VA = tracePointerLeafVA(V);
          if (!VA)
            return nullptr;
          if (*VA == 0)
            return llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(*Ctx));
          return tryResolveGlobalData(*VA, /*DataSizeHint=*/0);
        };
        if (llvm::Value *Selected = buildDataTree(AddrVar, 0))
          return Selected;
      }
      // An explicit SELECT that failed the simple all-data proof may still be a
      // structured table selection. Give only the all-arms merge resolver a
      // chance to prove it; do not fall into recognizers that could accept a
      // single data-looking leaf. In a known pointer context that resolver also
      // records the fail-closed diagnostic, while speculative integer arguments
      // simply retain their original ABI value.
      return tryResolveSelectMergeTable(AddrVar, /*SizeHint=*/0, FailClosed,
                                        Builder);
    }

  // A direct &global (writable or read-only) folds to a constant address.
  uint64_t ConstAddr = 0;
  if (AddrVar.isConst())
    ConstAddr = AddrVar.ConstVal;
  else if (auto Traced = traceSSAConst(AddrVar))
    ConstAddr = *Traced;
  if (ConstAddr != 0) {
    unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
    // A data-pointer argument never points into an instruction section, so a
    // small integer argument that merely equals a low .text offset (e.g. an
    // unrolled loop counter 3..9 passed by value) must NOT be mistaken for a
    // global. Exact data/rodata classification also admits Mach-O data sections
    // such as __TEXT,__cstring inside an executable segment.
    if (Img->isDataAddress(ConstAddr) &&
        !isFrameRelativeDisplacement(ConstAddr, AddrBits))
      if (auto *G = tryResolveGlobalData(ConstAddr, /*DataSizeHint=*/0))
        return G;
  }
  // A computed `&global[index]`: same resolver sequence as a LOAD address, with
  // the writable-data path first since the callee may store through the
  // pointer.
  if (auto *P = tryResolveWritableData(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  // Keep multi-base PHIs on their all-arms resolver before the single-base
  // indexed path.  This order must match LOAD emission: pointer arithmetic is
  // symbolized while its defining op is emitted, before the later LOAD gets a
  // chance to inspect the same address.
  bool SawAmbiguous = false;
  if (auto *P = tryResolveSelectMergeTable(AddrVar, /*SizeHint=*/0, FailClosed,
                                           Builder, &SawAmbiguous))
    return P;
  // The complete provenance audit distinguishes an ordinary non-match from an
  // expression it explicitly rejected. Once rejected, no narrower resolver may
  // reinterpret one table-looking subexpression and silently discard the mixed
  // sibling, whether that sibling becomes an index or an induction term.
  if (SawAmbiguous)
    return nullptr;
  if (auto *P = tryResolveIndexedGlobalPtr(AddrVar, /*SizeHint=*/0, FailClosed,
                                           Builder))
    return P;
  if (auto *P = tryResolveLiteralPoolTable(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  if (auto *P = tryResolveInductionGlobalPtr(AddrVar, /*SizeHint=*/0,
                                             FailClosed, Builder))
    return P;
  if (auto *P = tryResolveLiteralPoolBase(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  return nullptr;
}

} // namespace neverd
