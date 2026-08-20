//===- MedLLVMCodePtrResolve.cpp - Code-pointer resolution -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Code/data-pointer table mirroring and code-reference resolution for
/// MedLLVMEmitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/WithColor.h"

#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

llvm::Constant *MedLLVMEmitter::resolveLiftedCodeAddress(va_t Address) {
  if (auto NameIt = FuncNames.find(Address); NameIt != FuncNames.end())
    if (llvm::Function *Function = Mod->getFunction(NameIt->second))
      return Function;
  if (auto BlockIt = LiftedCodeBlocks.find(Address);
      BlockIt != LiftedCodeBlocks.end()) {
    llvm::BasicBlock *Block = BlockIt->second;
    if (Block && Block->getParent())
      return llvm::BlockAddress::get(Block->getParent(), Block);
  }
  return nullptr;
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
  enum class PtrSlotKind : uint8_t { Code, Data, Import };
  struct PtrSlot {
    uint64_t VA;
    PtrSlotKind Kind;
    std::string ImportName;
    int64_t ImportAddend = 0;
  };
  std::map<uint64_t, PtrSlot> SlotsByVA;
  auto slotInRun = [&](uint64_t S) {
    return S >= RunStart && S <= RunEnd && PtrSz <= RunEnd - S;
  };
  for (uint64_t S : Img->CodePtrRelocSlots)
    if (slotInRun(S))
      SlotsByVA.emplace(S, PtrSlot{S, PtrSlotKind::Code, {}});
  for (uint64_t S : Img->DataPtrRelocSlots)
    if (slotInRun(S))
      SlotsByVA.emplace(S, PtrSlot{S, PtrSlotKind::Data, {}});
  // The indirect-symbol view supplies slots on older images even when no bind
  // stream is present.  A decoded dyld binding is authoritative when both
  // views name the same address because it also carries the effective addend.
  for (const auto &[S, Name] : Img->ImportPtrSlots)
    if (slotInRun(S))
      SlotsByVA[S] = PtrSlot{S, PtrSlotKind::Import, Name, 0};
  for (const auto &[S, Binding] : Img->DyldBindSlots)
    if (slotInRun(S))
      SlotsByVA[S] =
          PtrSlot{S, PtrSlotKind::Import, Binding.Name, Binding.Addend};
  if (SlotsByVA.empty())
    return nullptr;
  std::vector<PtrSlot> Slots;
  Slots.reserve(SlotsByVA.size());
  for (auto &[VA, Slot] : SlotsByVA) {
    (void)VA;
    Slots.push_back(std::move(Slot));
  }

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
  // front. A code pointer that cannot map to a recompiled function or owned
  // block aborts the entire emission; resolving them before the global exists
  // keeps that failure clean and prevents a stale-address fallback.
  // Data-pointer targets are
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
    PtrSlotKind Kind;
    std::string ImportName;
    int64_t ImportAddend;
  };
  std::vector<KeptSlot> Kept;
  size_t Cur = 0;
  for (const auto &Slot : Slots) {
    size_t Off = static_cast<size_t>(Slot.VA - RunStart);
    if (Off < Cur)
      continue; // overlapping/duplicate slot — skip
    uint64_t TargetVA = 0;
    std::memcpy(&TargetVA, Data + Off, PtrSz);
    if (Slot.Kind == PtrSlotKind::Code) {
      TargetVA = normalizeCodeAddress(TargetVA, Img->Arch, Img->Mode);
      if (!resolveLiftedCodeAddress(TargetVA)) {
        if (!FatalCodePointerResolution)
          llvm::WithColor::error()
              << "med_llvm_emitter: relocation-proven code pointer at 0x"
              << llvm::utohexstr(Slot.VA) << " targets unresolved address 0x"
              << llvm::utohexstr(TargetVA)
              << "; refusing stale-address fallback\n";
        FatalCodePointerResolution = true;
        return nullptr;
      }
    }
    Kept.push_back(
        {Off, TargetVA, Slot.Kind, Slot.ImportName, Slot.ImportAddend});
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
  // legal; read-only-after-relocation and rodata pointer tables stay constant
  // — their slots are never stored to.
  bool SegWritable = Seg->isWritable() && !Seg->isExecutable() &&
                     !section_names::isReadOnlyAfterRelocSectionName(Seg->Name);
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
    if (K.Kind == PtrSlotKind::Code) {
      llvm::Constant *Target = resolveLiftedCodeAddress(K.TargetVA);
      if (!Target) {
        FatalCodePointerResolution = true;
        return nullptr;
      }
      FieldVal = llvm::ConstantExpr::getPtrToInt(Target, PtrIntTy);
    } else if (K.Kind == PtrSlotKind::Data) {
      if (K.TargetVA == 0)
        FieldVal = llvm::ConstantInt::get(PtrIntTy, 0);
      else if (llvm::Constant *G = tryResolveGlobalData(K.TargetVA))
        FieldVal = llvm::ConstantExpr::getPtrToInt(G, PtrIntTy);
      else {
        // This slot is loader-proven pointer state.  Keeping its original VA
        // would make the mirror look complete while leaving a stale pointer in
        // the rebuilt object; reject the entire module just as an unresolved
        // code-pointer relocation does.  A literal null remains valid above.
        if (!FatalCodePointerResolution && !FatalDataPointerResolution)
          llvm::WithColor::error()
              << "med_llvm_emitter: relocation-proven data pointer at 0x"
              << llvm::utohexstr(RunStart + K.Off)
              << " targets unmaterializable address 0x"
              << llvm::utohexstr(K.TargetVA)
              << "; refusing stale-address fallback\n";
        if (!FatalCodePointerResolution)
          FatalDataPointerResolution = true;
        return nullptr;
      }
    } else {
      const std::string Name =
          llvm_name::fromObjectSymbol(K.ImportName, TargetFormat).str();
      llvm::GlobalValue *Symbol = Mod->getNamedValue(Name);
      if (!Symbol) {
        auto *Placeholder = new llvm::GlobalVariable(
            *Mod, llvm::Type::getInt8Ty(*Ctx), /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage, /*Initializer=*/nullptr, Name);
        ImportedSymbolPlaceholders[Name] = Placeholder;
        Symbol = Placeholder;
      }
      FieldVal = llvm::ConstantExpr::getPtrToInt(Symbol, PtrIntTy);
      if (K.ImportAddend != 0)
        FieldVal = llvm::ConstantExpr::getAdd(
            FieldVal, llvm::ConstantInt::getSigned(PtrIntTy, K.ImportAddend));
    }
    Fields.push_back(FieldVal);
    Cursor = K.Off + PtrSz;
  }
  addBytes(Cursor, Size);

  GV->setInitializer(llvm::ConstantStruct::get(StructTy, Fields));
  return GV;
}

uint64_t
MedLLVMEmitter::ptrTableUniqueSegment(const MedVar &V,
                                      bool IncludeSymbolizedEvidence) const {
  if (!CurMedFunc || !Img)
    return 0;
  if (V.isConst())
    return 0;

  // Memoize per non-constant address value: a pure function of the (immutable
  // during emit) function body, queried repeatedly for the same values.
  ensureAddrPredCache();
  std::tuple<int64_t, int, bool> CacheKey{
      static_cast<int64_t>(
          (static_cast<uint64_t>(static_cast<uint32_t>(V.Id)) << 32) |
          static_cast<uint32_t>(V.SSAVer)),
      static_cast<int>(V.Kind), IncludeSymbolizedEvidence};
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
    for (const auto &[S, Name] : Img->ImportPtrSlots) {
      (void)Name;
      if (S >= Lo && S < Hi)
        return true;
    }
    for (const auto &[S, Binding] : Img->DyldBindSlots) {
      (void)Binding;
      if (S >= Lo && S < Hi)
        return true;
    }
    return false;
  };
  auto findDef = [&](const MedVar &X) { return lookupDef(X); };
  auto findPhi = [&](const MedVar &X) { return lookupPhi(X); };

  // traceTableBaseConst may fold several numeric leaves into one table VA, but
  // getVar emits those leaves individually. Determine whether the rebuilt IR
  // already carries a relocatable pointer by classifying the actual leaves,
  // stopping at a literal-pool LOAD because its loaded value remains the raw
  // image VA even though the pool address itself is symbolized.
  enum class ConstLeafModel { Raw, Symbolized, Incomplete };
  auto emittedConstLeafRelocates = [&](const MedVar &Root) {
    std::vector<MedVar> Pending{Root};
    std::set<std::tuple<int, int, int, uint16_t>> LeafSeen;
    int LeafBudget = 128;
    while (!Pending.empty() && LeafBudget-- > 0) {
      MedVar Cur = Pending.back();
      Pending.pop_back();
      if (Cur.isConst()) {
        if (constantOccurrenceMayRelocate(Cur))
          return ConstLeafModel::Symbolized;
        continue;
      }
      auto Key = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                                 Cur.Size);
      if (!LeafSeen.insert(Key).second)
        continue;
      const MedOp *Def = findDef(Cur);
      if (!Def)
        continue;
      switch (Def->Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        if (auto Forwarded = pointerPreservingInput(*Def))
          Pending.push_back(*Forwarded);
        break;
      case NdOp::INT_ADD:
        for (int I = 0; I < Def->NumInputs; ++I)
          Pending.push_back(Def->Inputs[I]);
        break;
      case NdOp::LOAD:
        break;
      default:
        break;
      }
    }
    return Pending.empty() ? ConstLeafModel::Raw : ConstLeafModel::Incomplete;
  };

  auto compute = [&]() -> uint64_t {
    uint64_t SegVA = 0;
    bool Found = false;
    std::vector<std::pair<MedVar, bool>> Work{{V, false}};
    std::set<std::tuple<int, int, int, bool>> Seen;
    // The Seen set bounds the walk to the function's distinct
    // address-arithmetic values; the counter is only a safety cap against a
    // pathological DAG (and is large enough that an index subtree — e.g. a deep
    // PRNG chain feeding the index — never starves the base operands of a `base
    // + index` address).
    int Budget = 4096;
    while (!Work.empty() && Budget-- > 0) {
      auto [Cur, ThroughFrameReload] = Work.back();
      Work.pop_back();

      // A subexpression that folds to a single constant is a base or an offset:
      // a plain constant, `const + const`, or the ARM biased literal-pool base
      // `const_offset + ldr[pc]` (the table VA split as a constant plus a
      // literal the loader applied).  The folded value locates the table; the
      // address-role audit below proves whether that table evidence actually
      // reaches the result. Representation is tracked separately so consumers
      // can choose between the already-relocated value and a raw-VA rebase.
      if (auto C = traceTableBaseConst(Cur)) {
        const Segment *Seg = Img->getSegmentFor(*C);
        if (Seg && !Seg->isExecutable() && !Seg->Data.empty() &&
            segHasPtrSlots(Seg)) {
          // Callers that can only rebase a raw original VA must not claim an
          // already-symbolized table base: doing so applies the mirror twice.
          // The pointer-table LOAD owner instead requests this segment
          // evidence and follows it with the complete structural role/model
          // proof below, which selects direct use for a symbolized induction
          // and raw rebasing otherwise. A proven frame spill/reload already
          // had this exception because the LOAD hides the representation from
          // the address consumer and its caller performs the direct-use audit.
          ConstLeafModel LeafModel = emittedConstLeafRelocates(Cur);
          if (LeafModel == ConstLeafModel::Incomplete ||
              (LeafModel == ConstLeafModel::Symbolized &&
               !IncludeSymbolizedEvidence && !ThroughFrameReload))
            return 0;
          uint64_t RunStart = 0, RunEnd = 0;
          readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
          if (Found && SegVA != RunStart)
            return 0; // bases span distinct pointer-table mirror runs
          SegVA = RunStart;
          Found = true;
        }
        continue; // fully constant: recorded as a base, else an ignorable
                  // offset
      }
      if (Cur.isConst())
        continue;

      auto K = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                               ThroughFrameReload);
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
            Work.emplace_back(Def->Inputs[I], ThroughFrameReload);
          break;
        case NdOp::LOAD:
          // Stack spill/reload: a register-constrained target (i386/ARM32)
          // spills the table base to a stack slot. Follow only exact all-path
          // reaching STOREs; a whole-function same-key scan admits later and
          // path-only writes and disagrees with the other provenance walkers.
          {
            std::vector<MedVar> Sources;
            if (collectFrameReloadSources(*Def, Sources))
              for (const MedVar &Source : Sources)
                Work.emplace_back(Source, true);
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
          Work.emplace_back(Arg, ThroughFrameReload);
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
  // A single recognized pointer-table base does not make every remaining term
  // an index.  Prove each term stays scalar in rebuilt IR before Path 1 may
  // claim the access; otherwise a recurrent rodata pointer (or another mapped
  // base) can be hidden in IdxTerms and added to the mirror address.  Do not
  // fall through to Path 2 after this concrete decomposition failed its
  // uniqueness proof, because ptrTableUniqueSegment intentionally reports only
  // the pointer-table component and would hide the same unsafe term again.
  bool UnsafeIndexedTerms = false;
  if (HaveConst)
    for (const MedVar &Term : IdxTerms)
      if (!valueIsStableAddressOffset(Term)) {
        UnsafeIndexedTerms = true;
        break;
      }
  if (HaveConst && !UnsafeIndexedTerms) {
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
  if (UnsafeIndexedTerms)
    return nullptr;

  // Path 2: the base is a SELECT/PHI or branchless AND/OR mask of several base
  // constants (a `cond ? A[i] : B[j]` pointer select), so path 1 cannot isolate
  // a single constant base.  When every base-like constant in the address lands
  // in ONE pointer-table segment, the whole address provably indexes that
  // segment, so redirect by the address's own value: GEP(@seg, addr - segVA).
  if (uint64_t SelSeg =
          ptrTableUniqueSegment(AddrVar, /*IncludeSymbolizedEvidence=*/true)) {
    auto segmentHasPointerSlots = [&](const Segment *Seg) {
      if (!Seg)
        return false;
      uint64_t Lo = Seg->VA;
      uint64_t Hi = Seg->VA + Seg->Data.size();
      auto contains = [&](uint64_t Slot) { return Slot >= Lo && Slot < Hi; };
      for (uint64_t Slot : Img->CodePtrRelocSlots)
        if (contains(Slot))
          return true;
      for (uint64_t Slot : Img->DataPtrRelocSlots)
        if (contains(Slot))
          return true;
      for (const auto &[Slot, Name] : Img->ImportPtrSlots) {
        (void)Name;
        if (contains(Slot))
          return true;
      }
      for (const auto &[Slot, Binding] : Img->DyldBindSlots) {
        (void)Binding;
        if (contains(Slot))
          return true;
      }
      return false;
    };
    // ptrTableUniqueSegment deliberately discovers segment evidence in a broad
    // arithmetic DAG; uniqueness alone is not proof that the RESULT carries a
    // pointer. Prove value roles structurally before Path 2 claims the access:
    // a table component may flow only through a complete-width forwarder, every
    // value arm of a selection/PHI, or the unique pointer side of ADD/SUB.
    // Merely appearing in a condition, mask, multiplier, shift, or other scalar
    // input is invalid provenance.
    enum class AddressRole { Invalid, Scalar, PointerTable };
    enum class AddressModel { Raw, Symbolized };
    struct AddressProof {
      AddressRole Role = AddressRole::Invalid;
      uint64_t Segment = 0;
      AddressModel Model = AddressModel::Raw;
    };
    auto invalidProof = []() { return AddressProof{}; };
    auto scalarProof = []() {
      return AddressProof{AddressRole::Scalar, 0, AddressModel::Raw};
    };
    auto tableProof = [](uint64_t Segment,
                         AddressModel Model = AddressModel::Raw) {
      return AddressProof{AddressRole::PointerTable, Segment, Model};
    };
    auto pointerTableRunStart = [&](const Segment *Seg) {
      uint64_t RunStart = Seg->VA, RunEnd = Seg->VA + Seg->Data.size();
      readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
      return RunStart;
    };
    using AuditSeen = std::set<std::tuple<int, int, int>>;
    std::function<AddressProof(const MedVar &, int, AuditSeen, bool)>
        proveAddressRole = [&](const MedVar &Value, int Depth, AuditSeen Seen,
                               bool DirectPhiConstant) -> AddressProof {
      if (Depth > 128)
        return invalidProof();
      if (Value.isConst()) {
        const Segment *Seg =
            Value.ConstVal != 0 ? Img->getSegmentFor(Value.ConstVal) : nullptr;
        if (!Seg || Seg->isExecutable() || !segmentHasPointerSlots(Seg))
          return scalarProof();
        // A pointer-table constant is valid provenance in either address
        // model. Operation inputs pass through getVar and may already carry
        // the mirror, while a direct PHI constant bypasses getVar and remains
        // an original numeric VA. Preserve that distinction through the same
        // role proof so the emitter can choose direct use versus raw rebasing.
        AddressModel Model = dataOccurrenceSymbolizes(Value, DirectPhiConstant)
                                 ? AddressModel::Symbolized
                                 : AddressModel::Raw;
        return tableProof(pointerTableRunStart(Seg), Model);
      }
      auto Key =
          std::make_tuple(static_cast<int>(Value.Kind), Value.Id, Value.SSAVer);
      if (!Seen.insert(Key).second)
        return invalidProof();

      if (const PhiNode *Phi = lookupPhi(Value)) {
        std::optional<AddressProof> Merged;
        bool SawInitialization = false;
        for (const auto &[Pred, Arg] : Phi->Args) {
          PhiEdgeFeasibility Edge = classifyPhiIncomingEdge(*Phi, Pred);
          if (Edge == PhiEdgeFeasibility::Infeasible)
            continue;
          if (Edge != PhiEdgeFeasibility::ProvenFeasible)
            return invalidProof();
          if (phiIncomingIsRecurrent(*Phi, Pred, Arg))
            continue;
          SawInitialization = true;
          AddressProof Arm = proveAddressRole(
              Arg, Depth + 1, Seen, /*DirectPhiConstant=*/Arg.isConst());
          if (Arm.Role == AddressRole::Invalid)
            return invalidProof();
          if (!Merged)
            Merged = Arm;
          else if (Merged->Role != Arm.Role ||
                   (Arm.Role == AddressRole::PointerTable &&
                    (Merged->Segment != Arm.Segment ||
                     Merged->Model != Arm.Model)))
            return invalidProof();
        }
        return SawInitialization && Merged ? *Merged : invalidProof();
      }

      const MedOp *Def = lookupDef(Value);
      if (!Def)
        return scalarProof();
      if (auto Forwarded = pointerPreservingInput(*Def))
        return proveAddressRole(*Forwarded, Depth + 1, Seen,
                                /*DirectPhiConstant=*/false);

      auto mergeChosenArms = [&](const MedVar &Left, const MedVar &Right,
                                 bool PreservesPointer) {
        AddressProof L = proveAddressRole(Left, Depth + 1, Seen,
                                          /*DirectPhiConstant=*/false);
        AddressProof R = proveAddressRole(Right, Depth + 1, Seen,
                                          /*DirectPhiConstant=*/false);
        if (L.Role == AddressRole::Invalid || R.Role == AddressRole::Invalid ||
            L.Role != R.Role)
          return invalidProof();
        if (L.Role == AddressRole::PointerTable &&
            (!PreservesPointer || L.Segment != R.Segment || L.Model != R.Model))
          return invalidProof();
        return L;
      };
      if (Def->Opcode == NdOp::SELECT) {
        if (Def->NumInputs < 3)
          return invalidProof();
        return mergeChosenArms(Def->Inputs[1], Def->Inputs[2],
                               selectPreservesPointerValues(*Def));
      }
      if (Def->Opcode == NdOp::INT_OR) {
        MedVar Cond, ArmT, ArmF;
        if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF))
          return mergeChosenArms(ArmT, ArmF, /*PreservesPointer=*/true);
      }
      if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
          Def->NumInputs >= 2) {
        // ARM32 materializes a PIC table base as `literal-load + PC-bias`.
        // Both operands look scalar in isolation, but the complete arithmetic
        // expression is a loader-proven pointer value. Recognize that idiom in
        // the pointer-producing ADD/SUB role before requiring both operands to
        // be stable scalar offsets. Absolute pointer loads are excluded: they
        // already produce symbolized pointers and must not be anchored again.
        bool SawLiteralLoad = false;
        bool SawArithmetic = false;
        auto FoldedLiteralBase = traceTableBaseConst(Value, 0, &SawLiteralLoad,
                                                     nullptr, &SawArithmetic);
        std::set<uint64_t> LoadedTargets;
        if (FoldedLiteralBase && SawLiteralLoad && SawArithmetic &&
            Def->Output.Size == Img->getPointerSize() &&
            !recoverAbsoluteDataPointerLoadTargets(Value, LoadedTargets)) {
          const Segment *Seg = Img->getSegmentFor(*FoldedLiteralBase);
          if (Seg && !Seg->isExecutable() && segmentHasPointerSlots(Seg))
            return tableProof(pointerTableRunStart(Seg));
        }

        AddressProof L = proveAddressRole(Def->Inputs[0], Depth + 1, Seen,
                                          /*DirectPhiConstant=*/false);
        AddressProof R = proveAddressRole(Def->Inputs[1], Depth + 1, Seen,
                                          /*DirectPhiConstant=*/false);
        bool LeftTable = L.Role == AddressRole::PointerTable;
        bool RightTable = R.Role == AddressRole::PointerTable;
        if (RightTable && (Def->Opcode == NdOp::INT_SUB || LeftTable))
          return invalidProof();
        if (LeftTable) {
          if (!valueIsStableAddressOffset(Def->Inputs[1]) ||
              Def->Output.Size == 0 || Def->Inputs[0].Size == 0 ||
              Def->Output.Size < Def->Inputs[0].Size)
            return invalidProof();
          return L;
        }
        if (RightTable) {
          if (Def->Opcode != NdOp::INT_ADD ||
              !valueIsStableAddressOffset(Def->Inputs[0]) ||
              Def->Output.Size == 0 || Def->Inputs[1].Size == 0 ||
              Def->Output.Size < Def->Inputs[1].Size)
            return invalidProof();
          return R;
        }

        // Scalar arithmetic may contain deep PHI/flag-expanded DAGs whose
        // control cycles are not pointer recurrences. Use the shared scalar
        // proof for the non-pointer case instead of demanding that the
        // pointer-role walk classify every implementation detail as acyclic.
        if (!valueIsStableAddressOffset(Def->Inputs[0]) ||
            !valueIsStableAddressOffset(Def->Inputs[1]))
          return invalidProof();

        // Low-VA PIC object code can materialize a fixed data address as the
        // sum of two scalar immediates that individually live in .text (for
        // example, the call/pop PC plus a SECTDIFF displacement). When the
        // complete value lands inside the one relocation-backed mirror run
        // already discovered by ptrTableUniqueSegment, the result is a raw
        // address into that run even though neither operand is pointer-valued.
        if (auto Folded = traceValueVA(Value)) {
          const Segment *Seg = Img->getSegmentFor(*Folded);
          if (Seg && !Seg->isExecutable() && segmentHasPointerSlots(Seg)) {
            uint64_t RunStart = Seg->VA;
            uint64_t RunEnd = Seg->VA + Seg->Data.size();
            readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
            if (RunStart == SelSeg && *Folded >= RunStart && *Folded < RunEnd)
              return tableProof(RunStart);
          }
        }

        return scalarProof();
      }

      if (Def->Opcode == NdOp::LOAD) {
        std::vector<MedVar> Sources;
        if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
          return scalarProof();
        std::optional<AddressProof> Merged;
        for (const MedVar &Source : Sources) {
          AddressProof SourceProof = proveAddressRole(
              Source, Depth + 1, Seen, /*DirectPhiConstant=*/false);
          if (SourceProof.Role == AddressRole::Invalid)
            return invalidProof();
          if (!Merged)
            Merged = SourceProof;
          else if (Merged->Role != SourceProof.Role ||
                   (SourceProof.Role == AddressRole::PointerTable &&
                    (Merged->Segment != SourceProof.Segment ||
                     Merged->Model != SourceProof.Model)))
            return invalidProof();
        }
        return Merged ? *Merged : scalarProof();
      }

      // Every remaining opcode is scalar/destructive for address provenance.
      // Audit all inputs so a table used only as a mask, multiplier, condition,
      // shift operand, or boolean source poisons the proof instead of granting
      // it to the result.
      for (uint8_t I = 0; I < Def->NumInputs; ++I) {
        AddressProof Input = proveAddressRole(Def->Inputs[I], Depth + 1, Seen,
                                              /*DirectPhiConstant=*/false);
        if (Input.Role != AddressRole::Scalar)
          return invalidProof();
      }
      return scalarProof();
    };

    AddressProof Proven =
        proveAddressRole(AddrVar, 0, {}, /*DirectPhiConstant=*/false);
    if (Proven.Role != AddressRole::PointerTable || Proven.Segment != SelSeg) {
      if (!FatalDataPointerResolution)
        syncError() << "med_llvm_emitter: ambiguous pointer-table address "
                    << AddrVar.display() << " in " << CurMedFunc->Name
                    << "; refusing stale-address fallback\n";
      FatalDataPointerResolution = true;
      return nullptr;
    }

    uint64_t OutSeg = 0;
    if (auto *G = buildCodePtrSegmentGlobal(SelSeg, OutSeg)) {
      if (llvm::Value *A = getVar(AddrVar, Builder)) {
        if (Proven.Model == AddressModel::Symbolized) {
          if (A->getType()->isPointerTy())
            return A;
          return Builder.CreateIntToPtr(A, llvm::PointerType::getUnqual(*Ctx),
                                        "cptsel.direct");
        }
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
  llvm::Constant *Target = resolveLiftedCodeAddress(*VA);
  if (!Target)
    return nullptr;
  unsigned PtrSz = Img->getPointerSize() ? Img->getPointerSize() : 8;
  return Builder.CreatePtrToInt(
      Target, sizeToType(V.Size > 0 ? V.Size : static_cast<uint16_t>(PtrSz)),
      "fnptr");
}

bool MedLLVMEmitter::operationUsesRelocatableCodeIdentity(NdOp Opcode) const {
  switch (Opcode) {
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_NEG2:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::CONCAT:
  case NdOp::SUBBYTES:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
    return true;
  default:
    return false;
  }
}

bool MedLLVMEmitter::codeIdentityOccurrenceMayRelocate(const MedVar &V) const {
  if (!Img || !CurMedFunc || !Mod)
    return false;

  if (CodeIdentityOccurrenceCacheFor != CurMedFunc) {
    CodeIdentityOccurrenceCacheFor = CurMedFunc;
    CodeIdentityOccurrenceCache.clear();
  }

  struct ContainsResult {
    bool Contains = false;
    bool Complete = true;
  };
  std::set<AddressProvenanceVarKey> Active;
  std::function<ContainsResult(const MedVar &)> ContainsFunctionIdentity =
      [&](const MedVar &Current) -> ContainsResult {
    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Current);
    if (auto It = CodeIdentityOccurrenceCache.find(Key);
        It != CodeIdentityOccurrenceCache.end())
      return {It->second, true};
    if (!Active.insert(Key).second)
      // A backedge is not negative evidence. Propagate incompleteness so an
      // SCC member is never memoized false before a sibling initializer has
      // exposed its function identity.
      return {false, false};
    auto Finish = [&](ContainsResult Result) {
      Active.erase(Key);
      if (Result.Contains || Result.Complete)
        CodeIdentityOccurrenceCache.insert_or_assign(Key, Result.Contains);
      return Result;
    };

    if (Current.isConst()) {
      const va_t Normalized =
          normalizeCodeAddress(Current.ConstVal, Img->Arch, Img->Mode);
      const bool HasStrongCodeProvenance =
          isCodeAddressProvenance(Current.Provenance) ||
          (Current.Provenance == ConstantAddressProvenance::Unknown &&
           Img->CodeRefTargets.count(Normalized) != 0);
      bool HasLiftedIdentity = false;
      if (auto It = FuncNames.find(Normalized); It != FuncNames.end())
        HasLiftedIdentity = Mod->getFunction(It->second) != nullptr;
      if (auto It = LiftedCodeBlocks.find(Normalized);
          !HasLiftedIdentity && It != LiftedCodeBlocks.end())
        HasLiftedIdentity = It->second && It->second->getParent();
      if (!HasLiftedIdentity && !HasStrongCodeProvenance)
        return Finish({false, true});
      const unsigned PointerSize = Img->getPointerSize();
      if (Current.Size != 0 && PointerSize != 0 && Current.Size < PointerSize)
        return Finish({false, true});
      if (HasStrongCodeProvenance)
        return Finish({true, true});
      if (Current.Provenance == ConstantAddressProvenance::Address)
        return Finish({Img->isCodeAddress(Normalized), true});
      return Finish({Current.Provenance == ConstantAddressProvenance::Unknown &&
                         constantOccurrenceMayRelocate(Current),
                     true});
    }

    if (const PhiNode *Phi = lookupPhi(Current)) {
      bool Complete = true;
      for (const auto &[Pred, Arg] : Phi->Args) {
        (void)Pred;
        ContainsResult Arm = ContainsFunctionIdentity(Arg);
        if (Arm.Contains)
          return Finish({true, true});
        Complete &= Arm.Complete;
      }
      return Finish({false, Complete});
    }

    const MedOp *Def = lookupDef(Current);
    if (!Def)
      return Finish({false, true});
    if (std::optional<MedVar> Forwarded = pointerPreservingInput(*Def))
      return Finish(ContainsFunctionIdentity(*Forwarded));
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
      ContainsResult TrueArm = ContainsFunctionIdentity(Def->Inputs[1]);
      if (TrueArm.Contains)
        return Finish({true, true});
      ContainsResult FalseArm = ContainsFunctionIdentity(Def->Inputs[2]);
      return Finish(
          {FalseArm.Contains,
           FalseArm.Contains || (TrueArm.Complete && FalseArm.Complete)});
    }
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (!collectFrameReloadSources(*Def, Sources))
        return Finish({false, true});
      bool Complete = true;
      for (const MedVar &Source : Sources) {
        ContainsResult SourceResult = ContainsFunctionIdentity(Source);
        if (SourceResult.Contains)
          return Finish({true, true});
        Complete &= SourceResult.Complete;
      }
      return Finish({false, Complete});
    }

    switch (Def->Opcode) {
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::ATOMIC_XCHG:
    case NdOp::ATOMIC_ADD:
    case NdOp::ATOMIC_CMPXCHG:
      return Finish({false, true});
    case NdOp::INTRINSIC:
      if (atomicIntrinsicAddressInput(*Def))
        return Finish({false, true});
      break;
    default:
      break;
    }
    bool Complete = true;
    for (uint8_t I = 0; I < Def->NumInputs; ++I) {
      ContainsResult Input = ContainsFunctionIdentity(Def->Inputs[I]);
      if (Input.Contains)
        return Finish({true, true});
      Complete &= Input.Complete;
    }
    return Finish({false, Complete});
  };

  return ContainsFunctionIdentity(V).Contains;
}

llvm::Value *
MedLLVMEmitter::tryResolveCodeIdentityOperand(const MedVar &V,
                                              llvm::IRBuilder<> &Builder) {
  if (!codeIdentityOccurrenceMayRelocate(V))
    return nullptr;
  return tryResolveCodeAddressValue(V, /*RequireCodeRole=*/false, Builder);
}

llvm::Value *MedLLVMEmitter::tryResolveCodeAddressValue(
    const MedVar &V, bool RequireCodeRole, llvm::IRBuilder<> &Builder) {
  if (!Img || !CurMedFunc)
    return nullptr;

  const unsigned PointerSize = Img->getPointerSize();
  if (occurrenceHasAddressFragmentTaint(V)) {
    rejectEscapingAddressFragment(V, "an observable code-address value");
    return nullptr;
  }

  struct CodeProof {
    std::optional<va_t> CommonTarget;
    bool SawCode = false;
    bool SawNonCode = false;
    bool SawNull = false;
    bool SawUnresolved = false;
    bool SawConflict = false;
    bool SawExplicit = false;
    bool SawCodeDependency = false;
    bool SawFunctionIdentity = false;
    bool SawLiftedCodeIdentity = false;
    bool SawStrongCodeProvenance = false;
    bool SawUnsafeCodeDependency = false;
  };
  auto mergeProof = [](CodeProof A, const CodeProof &B) {
    A.SawCode |= B.SawCode;
    A.SawNonCode |= B.SawNonCode;
    A.SawNull |= B.SawNull;
    A.SawUnresolved |= B.SawUnresolved;
    A.SawConflict |= B.SawConflict;
    A.SawExplicit |= B.SawExplicit;
    A.SawCodeDependency |= B.SawCodeDependency;
    A.SawFunctionIdentity |= B.SawFunctionIdentity;
    A.SawLiftedCodeIdentity |= B.SawLiftedCodeIdentity;
    A.SawStrongCodeProvenance |= B.SawStrongCodeProvenance;
    A.SawUnsafeCodeDependency |= B.SawUnsafeCodeDependency;
    if (A.CommonTarget && B.CommonTarget && *A.CommonTarget != *B.CommonTarget)
      A.SawConflict = true;
    else if (!A.CommonTarget)
      A.CommonTarget = B.CommonTarget;
    return A;
  };

  // A loop-carried value that only forwards the PHI output still denotes one
  // exact identity and may collapse to its initializer.  Address arithmetic
  // such as `p = PHI(&f, p + 4)` is a different invariant: it is a dynamic,
  // relocation-aware relation and must retain the current SSA value.
  auto isPureIdentityRecurrence = [&](const PhiNode &Root,
                                      const MedVar &Start) {
    std::set<AddressProvenanceVarKey> Seen;
    std::function<bool(const MedVar &)> ReachesRoot =
        [&](const MedVar &Current) -> bool {
      if (!Current.isConst() && Current.Kind == Root.Output.Kind &&
          Current.Id == Root.Output.Id &&
          Current.SSAVer == Root.Output.SSAVer)
        return true;
      if (Current.isConst())
        return false;
      const AddressProvenanceVarKey Key = addressProvenanceVarKey(Current);
      if (!Seen.insert(Key).second)
        return false;
      const MedOp *Def = lookupDef(Current);
      if (!Def)
        return false;
      std::optional<MedVar> Forwarded = pointerPreservingInput(*Def);
      if (!Forwarded || Forwarded->Size != Current.Size)
        return false;
      return ReachesRoot(*Forwarded);
    };
    return ReachesRoot(Start);
  };

  auto finishValueMerge = [](CodeProof Result) {
    // Every exact arm is materialized by SELECT/PHI emission and every
    // dependency arm was produced by an operation that materializes its code
    // leaves.  The merged value is therefore relocation-aware but no longer a
    // single identity that may be replaced with its initializer.
    if (Result.SawCode &&
        (Result.SawCodeDependency || Result.SawNull) &&
        !Result.SawNonCode && !Result.SawConflict &&
        !Result.SawUnsafeCodeDependency) {
      Result.SawCodeDependency = true;
      Result.SawCode = false;
      Result.SawUnresolved = false;
      Result.CommonTarget.reset();
    }
    return Result;
  };

  // Classify code identity through the SSA value graph without changing the
  // generic getVar policy.  Only pointer-preserving forwarders and value
  // merges can publish a unique identity.  Other operations are scanned for a
  // code-bearing leaf so relocation-dependent arithmetic cannot silently fall
  // back to its old numeric result.
  std::set<AddressProvenanceVarKey> Active;
  std::function<CodeProof(const MedVar &)> proveCodeValue =
      [&](const MedVar &Current) -> CodeProof {
    if (Current.isConst()) {
      if (Current.Provenance == ConstantAddressProvenance::AddressFragment)
        return {.SawUnresolved = true, .SawExplicit = true};
      if (Current.ConstVal == 0 &&
          !isExactAddressProvenance(Current.Provenance))
        return {.SawNull = true, .SawExplicit = true};
      if (Current.Provenance == ConstantAddressProvenance::Scalar ||
          Current.Provenance == ConstantAddressProvenance::DataAddress)
        return {.SawNonCode = true, .SawExplicit = true};
      const va_t Normalized =
          normalizeCodeAddress(Current.ConstVal, Img->Arch, Img->Mode);
      const bool HasLoaderCodeProvenance =
          Current.Provenance == ConstantAddressProvenance::Unknown &&
          Img->CodeRefTargets.count(Normalized) != 0;
      if (Current.Provenance == ConstantAddressProvenance::Unknown &&
          !HasLoaderCodeProvenance)
        return {.SawUnresolved = true};
      const bool IsCode =
          Current.Provenance == ConstantAddressProvenance::CodeAddress ||
          HasLoaderCodeProvenance ||
          (Current.Provenance == ConstantAddressProvenance::Address &&
           (Img->isCodeAddress(Normalized) || FuncNames.count(Normalized)));
      if (!IsCode)
        return {.SawNonCode = true, .SawExplicit = true};
      if (Current.Size != 0 && PointerSize != 0 && Current.Size < PointerSize)
        return {.SawCode = true, .SawUnresolved = true, .SawExplicit = true};
      const auto FunctionIt = FuncNames.find(Normalized);
      const bool HasFunctionIdentity =
          FunctionIt != FuncNames.end() &&
          Mod->getFunction(FunctionIt->second) != nullptr;
      bool HasLiftedCodeIdentity = HasFunctionIdentity;
      if (auto It = LiftedCodeBlocks.find(Normalized);
          !HasLiftedCodeIdentity && It != LiftedCodeBlocks.end())
        HasLiftedCodeIdentity = It->second && It->second->getParent();
      return {.CommonTarget = Normalized,
              .SawCode = true,
              .SawExplicit = true,
              .SawFunctionIdentity = HasFunctionIdentity,
              .SawLiftedCodeIdentity = HasLiftedCodeIdentity,
              .SawStrongCodeProvenance =
                  isCodeAddressProvenance(Current.Provenance) ||
                  HasLoaderCodeProvenance};
    }

    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Current);
    if (!Active.insert(Key).second)
      return {.SawUnresolved = true};
    auto Finish = [&](CodeProof Result) {
      Active.erase(Key);
      return Result;
    };

    if (const PhiNode *Phi = lookupPhi(Current)) {
      CodeProof Combined;
      bool SawArm = false;
      for (const auto &[Pred, Arg] : Phi->Args) {
        if (classifyPhiIncomingEdge(*Phi, Pred) ==
            PhiEdgeFeasibility::Infeasible)
          continue;
        if (phiIncomingIsRecurrent(*Phi, Pred, Arg)) {
          if (!isPureIdentityRecurrence(*Phi, Arg)) {
            Combined.SawCodeDependency = true;
            SawArm = true;
          }
          continue;
        }
        Combined = mergeProof(std::move(Combined), proveCodeValue(Arg));
        SawArm = true;
      }
      if (!SawArm)
        Combined.SawUnresolved = true;
      return Finish(finishValueMerge(std::move(Combined)));
    }

    const MedOp *Def = lookupDef(Current);
    if (!Def)
      return Finish({.SawUnresolved = true});
    if (std::optional<MedVar> Forwarded = pointerPreservingInput(*Def))
      return Finish(proveCodeValue(*Forwarded));
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
      CodeProof Combined = mergeProof(proveCodeValue(Def->Inputs[1]),
                                      proveCodeValue(Def->Inputs[2]));
      return Finish(finishValueMerge(std::move(Combined)));
    }

    // A proven local-frame reload inherits the occurrences stored into that
    // slot. This is how an -O0 `fn_t p = target; p()` remains relocatable while
    // the intermediate STORE itself stays a local transport. An incomplete or
    // ambiguous reaching-store proof is intentionally unresolved.
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
        return Finish({.SawUnresolved = true});
      CodeProof Combined;
      for (const MedVar &Source : Sources)
        Combined = mergeProof(std::move(Combined), proveCodeValue(Source));
      return Finish(std::move(Combined));
    }

    // Calls and atomic old-value results do not inherit provenance from their
    // address/argument/value operands. Their runtime result may be a function
    // pointer, but it has no explicit stale image VA to rewrite.
    switch (Def->Opcode) {
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::ATOMIC_XCHG:
    case NdOp::ATOMIC_ADD:
    case NdOp::ATOMIC_CMPXCHG:
      return Finish({.SawUnresolved = true});
    case NdOp::INTRINSIC:
      if (atomicIntrinsicAddressInput(*Def))
        return Finish({.SawUnresolved = true});
      break;
    default:
      break;
    }

    CodeProof Combined;
    for (uint8_t I = 0; I < Def->NumInputs; ++I)
      Combined =
          mergeProof(std::move(Combined), proveCodeValue(Def->Inputs[I]));
    if (Combined.SawCode || Combined.SawCodeDependency) {
      // Arithmetic/bit operations may use a numeric architectural PC as an
      // offset base (ARM switch lowering). They depend on code layout but no
      // longer carry a code-pointer identity. A forced code use must reject
      // them; an ordinary scalar sink keeps its established owner instead of
      // reintroducing eager BlockAddress materialization.
      const bool RelocationAware =
          operationUsesRelocatableCodeIdentity(Def->Opcode);
      Combined.SawUnsafeCodeDependency |=
          (Combined.SawLiftedCodeIdentity ||
           Combined.SawStrongCodeProvenance) &&
          !RelocationAware;
      Combined.SawCodeDependency = true;
      Combined.SawCode = false;
      Combined.CommonTarget.reset();
      if (RelocationAware && !Combined.SawUnsafeCodeDependency) {
        // The emitter routes every operand of this operation through
        // tryResolveCodeIdentityOperand. Unknown/scalar operands remain
        // ordinary runtime values; only code leaves need rebuilding.
        Combined.SawNonCode = false;
        Combined.SawUnresolved = false;
        Combined.SawConflict = false;
      } else {
        Combined.SawUnresolved = true;
      }
    }
    return Finish(std::move(Combined));
  };

  const CodeProof Proof = proveCodeValue(V);
  const bool UniqueCode = Proof.SawCode && Proof.CommonTarget &&
                          !Proof.SawNonCode && !Proof.SawUnresolved &&
                          !Proof.SawConflict;
  if (UniqueCode) {
    llvm::Constant *Identity = nullptr;
    if (RequireCodeRole) {
      // A CALL target must name a function entry.  An interior BlockAddress is
      // a relocatable code value, but calling it would bypass the lifted
      // function's ABI/prologue and is never a valid fallback.
      if (auto It = FuncNames.find(*Proof.CommonTarget); It != FuncNames.end())
        Identity = Mod->getFunction(It->second);
    } else {
      // Ordinary observable values (return/store/argument/identity relation)
      // may legitimately carry an interior label, for example x86
      // `lea 0(%rip), %rax`.  Preserve that identity as blockaddress while
      // keeping the stricter function-only policy above for indirect calls.
      Identity = resolveLiftedCodeAddress(*Proof.CommonTarget);
    }
    if (Identity)
      return Builder.CreatePtrToInt(
          Identity,
          sizeToType(V.Size > 0 ? V.Size : static_cast<uint16_t>(PointerSize)),
          RequireCodeRole ? "icall.target" : "code.value");
  }

  if (!Proof.SawCode && !Proof.SawUnsafeCodeDependency &&
      !(RequireCodeRole && (Proof.SawExplicit || Proof.SawCodeDependency)))
    return nullptr;

  // An explicit code-address origin exists, but its value graph cannot be
  // emitted as one function identity.  The raw original VA is never a sound
  // fallback, even when the final ABI type is an integer.
  if (!FatalCodePointerResolution && !FatalDataPointerResolution)
    syncError() << "med_llvm_emitter: relocatable code-address value "
                << V.display() << " in " << CurMedFunc->Name
                << " has no unique lifted "
                << (RequireCodeRole ? "function entry" : "code identity")
                << "; refusing stale-address fallback\n";
  FatalCodePointerResolution = true;
  return nullptr;
}

llvm::Value *
MedLLVMEmitter::tryResolveIndirectCallTarget(const MedVar &V,
                                             llvm::IRBuilder<> &Builder) {
  if (!Img || !CurMedFunc)
    return nullptr;

  // A literal-pool-derived target is a CALL only when it names an emitted
  // function entry.  resolveLiftedCodeAddress also accepts BlockAddress for
  // computed-goto/jump-table ownership; using that broader result here would
  // emit an invalid call to an interior basic-block label.
  if (!V.isConst()) {
    bool SawLoad = false;
    if (auto VA = traceTableBaseConst(V, 0, &SawLoad); VA && SawLoad) {
      const va_t Normalized = normalizeCodeAddress(*VA, Img->Arch, Img->Mode);
      if (auto It = FuncNames.find(Normalized); It != FuncNames.end())
        if (llvm::Function *Function = Mod->getFunction(It->second)) {
          const unsigned PtrSize =
              Img->getPointerSize()
                  ? Img->getPointerSize()
                  : static_cast<unsigned>(V.Size > 0 ? V.Size : 8);
          return Builder.CreatePtrToInt(
              Function,
              sizeToType(V.Size > 0 ? V.Size : static_cast<uint16_t>(PtrSize)),
              "icall.literal.target");
        }

      if (Img->isCodeAddress(Normalized) ||
          LiftedCodeBlocks.count(Normalized) || FuncNames.count(Normalized)) {
        if (!FatalCodePointerResolution && !FatalDataPointerResolution)
          syncError() << "med_llvm_emitter: literal indirect-call target 0x"
                      << llvm::utohexstr(Normalized) << " in "
                      << CurMedFunc->Name
                      << " is not a unique lifted function entry; refusing "
                         "BlockAddress/raw-address fallback\n";
        FatalCodePointerResolution = true;
        return nullptr;
      }
    }
  }
  return tryResolveCodeAddressValue(V, /*RequireCodeRole=*/true, Builder);
}

} // namespace neverd
