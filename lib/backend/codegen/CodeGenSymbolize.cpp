//===- CodeGenSymbolize.cpp - Patch-image pointer symbolization -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Symbolizes image-relative pointer constants before patch-mode codegen.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

void symbolizeImageAbsolutePointers(llvm::Module &Mod, const BinaryImage &Img) {
  llvm::Type *I8 = llvm::Type::getInt8Ty(Mod.getContext());
  llvm::Type *I64 = llvm::Type::getInt64Ty(Mod.getContext());

  // Patch-only: externalize code-pointer-table mirrors (`@__nd_codeptr_*`) so
  // the recompiled code references the ORIGINAL preserved read-only-after-
  // relocation run (at the encoded run-start VA) instead of the freshly emitted
  // mirror copy.  The mirror holds ABSOLUTE recompiled target VAs but lands in
  // an injected read-only segment with NO relocation records, so under ASLR its
  // function-pointer slots are never slid -> wild pointer -> SIGSEGV (e.g. a
  // local function-pointer array `int (*t[])(int) = {f0,..}; t[i](v)`).  The
  // original run survives at its VA with its loader relocations intact (Mach-O
  // chained fixups / ELF RELATIVE / COFF .reloc); dyld slides its slots, and
  // the (slid) original function VAs carry trampolines into the rewritten code.
  // This is exactly how `@__nd_data_*` data-pointer tables already stay ASLR-
  // correct.  Valid only in the patch path (original layout + trampolines
  // preserved); the lift/round-trip path never runs this pass, so its mirror
  // (fixed VA, no slide) is untouched.
  for (auto &GV : llvm::make_early_inc_range(Mod.globals())) {
    if (!GV.hasInitializer())
      continue;
    auto RunStart = parseNdCodePtrSymbol(GV.getName());
    if (!RunStart)
      continue;
    if (!Img.getSegmentFor(*RunStart))
      continue; // original run not in image -> keep the self-contained mirror
    GV.setInitializer(nullptr);
    GV.setLinkage(llvm::GlobalValue::ExternalLinkage);
    GV.setDSOLocal(true);
  }

  auto getOrCreateDataGlobal = [&](va_t VA) -> llvm::GlobalVariable * {
    std::string Name = makeNdDataSymbol(VA);
    if (auto *Existing = Mod.getGlobalVariable(Name, /*AllowInternal=*/true))
      return Existing;
    auto *GV = new llvm::GlobalVariable(Mod, I8, /*isConstant=*/false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        /*Initializer=*/nullptr, Name);
    GV->setDSOLocal(true);
    return GV;
  };

  // Patch-only: canonicalize a function's address TAKEN AS A VALUE to the
  // function's ORIGINAL VA (where the patcher installs a trampoline / keeps the
  // in-place body), so function-pointer IDENTITY holds.  A recompiled function
  // @F lands at a fresh VA in the injected segment, so `ptrtoint(@F)` (address-
  // of as a value -- stored, compared, returned) otherwise resolves to that
  // fresh VA; meanwhile a stored function pointer in a relocated table resolves
  // to the ORIGINAL VA (loader-slid to the trampoline -- see the @__nd_codeptr
  // externalization above and the @__nd_data data-pointer tables).  The
  // mismatch breaks `g == &F` callback-identity checks (`if (cb ==
  // default_cb)`). Rewriting `ptrtoint(@F)` to reference @__nd_data_<origVA>
  // makes the address- of resolve to the original VA too, matching every stored
  // pointer.  CALLS
  // (`call @F`) reference @F directly (not via ptrtoint) and are untouched --
  // they still bind to the fresh VA as a direct branch.  Gated to DEFINED
  // functions with a known original VA (an external/import @puts keeps its stub
  // address).  Runs before the obfuscation passes, so e.g. IndirectBranchPass's
  // own `ptrtoint(@F)` PIC base (added later) keeps the fresh VA it needs.
  {
    std::vector<std::pair<llvm::ConstantExpr *, llvm::Constant *>> Rewrites;
    for (llvm::Function &F : Mod) {
      if (F.isDeclaration())
        continue;
      const auto *Sym = Img.findSymbol(F.getName());
      if (!Sym && F.getName().starts_with("_"))
        Sym = Img.findSymbol(F.getName().drop_front());
      if (!Sym)
        Sym = Img.findSymbol(("_" + F.getName()).str());
      if (!Sym || !Sym->IsFunc || !Img.containsVA(Sym->Addr))
        continue;
      llvm::Constant *DataG = getOrCreateDataGlobal(Sym->Addr);
      for (llvm::User *U : F.users()) {
        auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(U);
        if (!CE || CE->getOpcode() != llvm::Instruction::PtrToInt)
          continue;
        Rewrites.emplace_back(
            CE, llvm::ConstantExpr::getPtrToInt(DataG, CE->getType()));
      }
    }
    for (auto &[Old, New] : Rewrites)
      Old->replaceAllUsesWith(New);
  }

  auto isReadableImageVA = [&](va_t VA) {
    const Segment *Seg = Img.getSegmentFor(VA);
    return Seg && Seg->isReadable();
  };

  auto inImageInttoptrVA = [&](llvm::Value *V) -> std::optional<va_t> {
    auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(V);
    if (!CE || CE->getOpcode() != llvm::Instruction::IntToPtr)
      return std::nullopt;
    auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0));
    if (!CI || !isReadableImageVA(CI->getZExtValue()))
      return std::nullopt;
    return CI->getZExtValue();
  };

  // A bare i64 ConstantInt (no inttoptr wrapper) is treated as an in-image
  // pointer only on 64-bit Mach-O, where __PAGEZERO reserves the low 4 GiB as
  // non-readable: every readable segment VA is therefore >= 0x1'0000'0000, so
  // an ordinary integer literal (a count, flag, or mask) can never collide with
  // a readable segment.  The lifter leaves a string/data address as a bare i64
  // when it cannot type the use -- a variadic call argument (puts / printf %s),
  // a stored pointer, or a returned pointer -- and that absolute VA does not
  // slide under ASLR, producing a wild pointer.  ELF/COFF have no __PAGEZERO
  // guard (low or zero base), so a VA-range guess would corrupt genuine
  // integers there; bare-constant symbolization stays gated off until those
  // formats grow a relocation-evidence-driven path.
  const bool BareConstPtrSymbolizable = Img.isMachO() && Img.is64Bit();
  auto inImageBareConstPtrVA = [&](llvm::Value *V) -> std::optional<va_t> {
    if (!BareConstPtrSymbolizable)
      return std::nullopt;
    auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V);
    if (!CI || CI->getBitWidth() != 64)
      return std::nullopt;
    uint64_t VA = CI->getZExtValue();
    const Segment *Seg = Img.getSegmentFor(VA);
    if (!Seg || !Seg->isReadable())
      return std::nullopt;
    return VA;
  };

  // Structural address equality for stack-slot load/store matching: the lifter
  // addresses a slot as inttoptr(add(frame, K)), and a load/store pair to the
  // same slot are distinct SSA values that are nonetheless structurally equal.
  std::function<bool(llvm::Value *, llvm::Value *)> sameAddr =
      [&](llvm::Value *A, llvm::Value *B) -> bool {
    if (A == B)
      return true;
    if (auto *AI = llvm::dyn_cast<llvm::IntToPtrInst>(A))
      if (auto *BI = llvm::dyn_cast<llvm::IntToPtrInst>(B))
        return sameAddr(AI->getOperand(0), BI->getOperand(0));
    if (auto *AC = llvm::dyn_cast<llvm::ConstantInt>(A))
      if (auto *BC = llvm::dyn_cast<llvm::ConstantInt>(B))
        return AC->getValue() == BC->getValue();
    if (auto *AB = llvm::dyn_cast<llvm::BinaryOperator>(A))
      if (auto *BB2 = llvm::dyn_cast<llvm::BinaryOperator>(B))
        return AB->getOpcode() == BB2->getOpcode() &&
               sameAddr(AB->getOperand(0), BB2->getOperand(0)) &&
               sameAddr(AB->getOperand(1), BB2->getOperand(1));
    return false;
  };

  // True if \p V transitively derives from a symbolized `__nd_data_*` global —
  // i.e. it is (part of) a relocatable address that slides under ASLR.  Walks
  // ptrtoint/binop chains and, for a stack reload, the matching store value in
  // the same function (the lifter's SSA-via-memory scaffolding blocks mem2reg
  // through inttoptr, so the value survives as an explicit load).
  std::function<bool(llvm::Value *, int)> derivesFromNdData =
      [&](llvm::Value *V, int Depth) -> bool {
    if (Depth <= 0)
      return false;
    auto isNdDataGlobal = [&](llvm::Value *P) {
      if (auto *GEPCE = llvm::dyn_cast<llvm::ConstantExpr>(P))
        if (GEPCE->getOpcode() == llvm::Instruction::GetElementPtr)
          P = GEPCE->getOperand(0);
      auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(P);
      return GV && GV->getName().starts_with(kNdDataPrefix);
    };
    if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(V)) {
      if (CE->getOpcode() == llvm::Instruction::PtrToInt &&
          isNdDataGlobal(CE->getOperand(0)))
        return true;
      for (auto &Op : CE->operands())
        if (derivesFromNdData(Op, Depth - 1))
          return true;
      return false;
    }
    if (auto *PTI = llvm::dyn_cast<llvm::PtrToIntInst>(V)) {
      if (isNdDataGlobal(PTI->getOperand(0)))
        return true;
      return derivesFromNdData(PTI->getOperand(0), Depth - 1);
    }
    if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V))
      return derivesFromNdData(BO->getOperand(0), Depth - 1) ||
             derivesFromNdData(BO->getOperand(1), Depth - 1);
    // Width casts: SROA reconstructs a reloaded pointer from byte/word pieces
    // through zext/trunc/sext (`or(shl(zext(piece)), ...)`).  Trace through
    // them so a symbolized pointer is still recognized after the reconstruction
    // -- otherwise the sub/add base-cancel below sees an opaque value, leaves
    // the cancel VA raw, and the result slides twice (one uncancelled slide ->
    // wild pointer, e.g. `static char buf[N]; ((struct F*)buf)->field` under
    // ASLR).
    if (auto *CI2 = llvm::dyn_cast<llvm::CastInst>(V))
      if (llvm::isa<llvm::ZExtInst>(CI2) || llvm::isa<llvm::SExtInst>(CI2) ||
          llvm::isa<llvm::TruncInst>(CI2))
        return derivesFromNdData(CI2->getOperand(0), Depth - 1);
    if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      llvm::Function *Fn = LI->getFunction();
      for (auto &BB2 : *Fn)
        for (auto &I2 : BB2)
          if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I2))
            if (sameAddr(SI->getPointerOperand(), LI->getPointerOperand()) &&
                derivesFromNdData(SI->getValueOperand(), Depth - 1))
              return true;
      return false;
    }
    return false;
  };

  for (auto &F : Mod) {
    if (F.isDeclaration())
      continue;
    for (auto &BB : F) {
      for (auto &I : llvm::make_early_inc_range(BB)) {
        if (auto *ITP = llvm::dyn_cast<llvm::IntToPtrInst>(&I)) {
          if (auto *CI =
                  llvm::dyn_cast<llvm::ConstantInt>(ITP->getOperand(0))) {
            // Mach-O's __PAGEZERO is represented as a segment, but it is an
            // inaccessible guard range rather than relocatable image memory.
            // A small scalar coerced to a pointer (for example, an imprecisely
            // recovered call argument `inttoptr(3)`) must stay numeric instead
            // of becoming an unresolvable `__nd_data_3` reference.
            if (isReadableImageVA(CI->getZExtValue())) {
              ITP->replaceAllUsesWith(
                  getOrCreateDataGlobal(CI->getZExtValue()));
              ITP->eraseFromParent();
              continue;
            }
          }
        }

        // A bare pointer constant only appears where a value is passed, stored,
        // returned, selected, or merged across control flow -- a variadic call
        // argument, a store value, a return value, a `select` arm (a ternary
        // string `cond ? "a" : "b"` lowers both addresses into a `select i64
        // VA1, VA2`), or a `phi` incoming value (the same two strings merged by
        // control flow rather than a ternary, e.g. the two `printf` format
        // strings on the setjmp==0 vs longjmp-return arms that SimplifyCFG
        // sinks into one call with a `phi i64 [VA1], [VA2]` selector). Limiting
        // bare-constant symbolization to those contexts keeps GEP indices and
        // arithmetic immediates untouched.  `phi` is safe here because the
        // bare-constant mechanism fires ONLY on a literal `ConstantInt` whose
        // value lands in a readable segment: a loop induction PHI's operands
        // are an initial constant (0 / a small bound, never an in-image VA) and
        // a non-constant `%i.next` instruction, so neither is touched -- unlike
        // the value-range-driven select-peer mechanism the P8 note excludes
        // from PHIs (whose induction range could fall inside a large array's VA
        // span).
        const bool BareConstCtx =
            llvm::isa<llvm::CallBase>(&I) || llvm::isa<llvm::StoreInst>(&I) ||
            llvm::isa<llvm::ReturnInst>(&I) ||
            llvm::isa<llvm::SelectInst>(&I) || llvm::isa<llvm::PHINode>(&I);
        for (unsigned Op = 0, E = I.getNumOperands(); Op < E; ++Op) {
          llvm::Value *OpV = I.getOperand(Op);
          if (auto VA = inImageInttoptrVA(OpV)) {
            I.setOperand(Op, getOrCreateDataGlobal(*VA));
            continue;
          }
          if (BareConstCtx) {
            if (auto VA = inImageBareConstPtrVA(OpV)) {
              auto *GV = getOrCreateDataGlobal(*VA);
              I.setOperand(Op,
                           llvm::ConstantExpr::getPtrToInt(GV, OpV->getType()));
              continue;
            }
          }
        }

        // GEP with a negated data-segment VA as index: the lifter pattern
        //   getelementptr i8, ptr %base, i64 -DATA_BASE_VA
        // computes "ptr - base" to get in-segment offset. Under ASLR the
        // hardcoded -VA doesn't slide. Replace with -ptrtoint(@data_global).
        if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
          for (unsigned Idx = 1, NumIdx = GEP->getNumOperands(); Idx < NumIdx;
               ++Idx) {
            auto *CI = llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(Idx));
            if (!CI || CI->getBitWidth() != 64)
              continue;
            int64_t SVal = CI->getSExtValue();
            if (SVal >= 0)
              continue;
            uint64_t PosVA = static_cast<uint64_t>(-SVal);
            if (!Img.containsVA(PosVA))
              continue;
            auto *GV = getOrCreateDataGlobal(PosVA);
            auto *PtrToInt =
                new llvm::PtrToIntInst(GV, I64, "", GEP->getIterator());
            auto *Neg = llvm::BinaryOperator::CreateNeg(PtrToInt, "",
                                                        GEP->getIterator());
            GEP->setOperand(Idx, Neg);
          }
        }

        // Sub-form of the base-cancel idiom.  PatchMode optimization rewrites
        // the lifter's `getelementptr %p, -DATA_BASE_VA` into
        // `sub(%x, DATA_BASE_VA)` feeding a `getelementptr @__nd_data_V, ...`.
        // When the minuend %x is itself a *symbolized* address (a relocatable
        // `__nd_data_*` reference that slides under ASLR), the raw DATA_BASE_VA
        // cannot cancel that slide -> the result slides twice -> wild pointer.
        // Symbolize the subtracted VA to ptrtoint(@__nd_data_VA) so it slides
        // and cancels exactly one base.  Gated to a symbolized minuend: a raw
        // absolute %x already yields the correct (slide-invariant) offset, so
        // symbolizing there would wrongly strip the slide.
        if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
          if (BO->getOpcode() == llvm::Instruction::Sub) {
            if (auto VA = inImageBareConstPtrVA(BO->getOperand(1)))
              if (derivesFromNdData(BO->getOperand(0), /*Depth=*/8)) {
                auto *GV = getOrCreateDataGlobal(*VA);
                BO->setOperand(1, llvm::ConstantExpr::getPtrToInt(
                                      GV, BO->getOperand(1)->getType()));
              }
          } else if (BO->getOpcode() == llvm::Instruction::Add ||
                     BO->getOpcode() == llvm::Instruction::Or) {
            // Address arithmetic into a global: `base_VA + index` (or the
            // `base_VA | offset` form clang emits as `or disjoint` when the
            // index bits cannot overlap the base, e.g. `&pool[i & 63]` returned
            // from an accessor).  The base is a bare ConstantInt that the
            // call/store/ret/select bare-constant contexts above miss, because
            // it is the *result* of the add/or (not the constant itself) that
            // reaches the return / store / call.  Left absolute, the base does
            // not slide under ASLR and the derived pointer is wild.  Symbolize
            // the base operand to ptrtoint(@__nd_data_VA) so it slides.  Gated
            // to a DYNAMIC (non-constant) sibling operand, so a constant fold
            // and a mask whose other operand is constant are left untouched;
            // the readable-segment VA test in inImageBareConstPtrVA keeps
            // ordinary integer immediates (which cannot reach a mapped segment
            // under
            // __PAGEZERO) from being mistaken for pointers.
            for (unsigned K = 0; K < 2; ++K) {
              if (llvm::isa<llvm::ConstantInt>(BO->getOperand(1 - K)))
                continue;
              if (auto VA = inImageBareConstPtrVA(BO->getOperand(K))) {
                auto *GV = getOrCreateDataGlobal(*VA);
                BO->setOperand(K, llvm::ConstantExpr::getPtrToInt(
                                      GV, BO->getOperand(K)->getType()));
                break;
              }
            }
          }
        }
      }
    }
  }

  // Debug aid (env-gated, no-op unless NEVERD_DUMP_PATCH_IR is set): emit the
  // post-symbolization module so the patch-mode IR can be inspected for
  // mixed-addressing diagnosis.  Never on a normal run.
  if (std::getenv("NEVERD_DUMP_PATCH_IR")) {
    llvm::errs() << "; ==== NEVERD patch-mode IR (post-symbolize) ====\n";
    Mod.print(llvm::errs(), nullptr);
  }
}

} // namespace neverd
