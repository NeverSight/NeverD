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
#include "neverd/Limits.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/object/SectionNames.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

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
      auto NameIt = FuncNames.find(TargetVA);
      if (NameIt == FuncNames.end() || !Mod->getFunction(NameIt->second))
        return nullptr; // code pointer does not resolve — abort the mirror
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
    if (K.Kind == PtrSlotKind::Code) {
      llvm::Function *F = Mod->getFunction(FuncNames.find(K.TargetVA)->second);
      FieldVal = llvm::ConstantExpr::getPtrToInt(F, PtrIntTy);
    } else if (K.Kind == PtrSlotKind::Data) {
      if (llvm::Constant *G = tryResolveGlobalData(K.TargetVA))
        FieldVal = llvm::ConstantExpr::getPtrToInt(G, PtrIntTy);
      else
        // Unresolvable data pointer: keep the original VA (byte-identical to
        // the verbatim fallback) so the field count still matches the layout.
        FieldVal = llvm::ConstantInt::get(PtrIntTy, K.TargetVA);
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

  auto compute = [&]() -> uint64_t {
    uint64_t SegVA = 0;
    bool Found = false;
    std::vector<MedVar> Work{V};
    std::set<std::tuple<int, int, int>> Seen;
    // The Seen set bounds the walk to the function's distinct
    // address-arithmetic values; the counter is only a safety cap against a
    // pathological DAG (and is large enough that an index subtree — e.g. a deep
    // PRNG chain feeding the index — never starves the base operands of a `base
    // + index` address).
    int Budget = 4096;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();

      // A subexpression that folds to a single constant is a base or an offset:
      // a plain constant, `const + const`, or the ARM biased literal-pool base
      // `const_offset + ldr[pc]` (the table VA split as a constant plus a
      // literal the loader applied).  SawLoad distinguishes a
      // literal-pool-derived base (getVar reloads the raw VA, so no redirect
      // guard) from a plain constant operand (which getVar may rewrite to a
      // relocated global).
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
        continue; // fully constant: recorded as a base, else an ignorable
                  // offset
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
          // Stack spill/reload: a register-constrained target (i386/ARM32)
          // spills the table base to a stack slot; follow the matching STORE's
          // value.
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
