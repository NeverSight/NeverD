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
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/WithColor.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

bool MedLLVMEmitter::hasAuthenticatedFunctionEntryVA(va_t Address) const {
  return EmittedFuncNames.count(Address) != 0 ||
         (Img && Img->isImportStubAt(Address));
}

llvm::Function *MedLLVMEmitter::resolveLiftedFunctionEntry(va_t Address) const {
  if (!Mod)
    return nullptr;

  if (auto NameIt = EmittedFuncNames.find(Address);
      NameIt != EmittedFuncNames.end())
    if (llvm::Function *Function = Mod->getFunction(NameIt->second))
      return Function;

  // FuncNames intentionally mixes code entries with import bind/IAT slots.
  // Only an executable veneer authenticated by the loader may use an import
  // name as a function-entry identity; a data slot stays data even if a
  // same-named declaration already exists in the module.
  if (!hasAuthenticatedFunctionEntryVA(Address))
    return nullptr;
  if (auto NameIt = FuncNames.find(Address); NameIt != FuncNames.end())
    return Mod->getFunction(NameIt->second);
  return nullptr;
}

llvm::Constant *MedLLVMEmitter::resolveLiftedCodeAddress(va_t Address) {
  if (llvm::Function *Function = resolveLiftedFunctionEntry(Address))
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
  if (!Img)
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
  enum class PtrSlotKind : uint8_t { Code, SuppressedCode, Data, Import };
  struct PtrSlot {
    uint64_t VA;
    PtrSlotKind Kind;
    std::string ImportName;
    int64_t ImportAddend = 0;
    uint64_t TargetOwnerVA = InvalidVA;
  };
  std::map<uint64_t, PtrSlot> SlotsByVA;
  auto slotInRun = [&](uint64_t S) {
    return S >= RunStart && S <= RunEnd && PtrSz <= RunEnd - S;
  };
  for (uint64_t S : Img->CodePtrRelocSlots)
    if (slotInRun(S))
      SlotsByVA.emplace(S, PtrSlot{S,
                                   currentJumpTableSuppressesRelocationSlot(S)
                                       ? PtrSlotKind::SuppressedCode
                                       : PtrSlotKind::Code,
                                   {}});
  for (uint64_t S : Img->DataPtrRelocSlots)
    if (slotInRun(S)) {
      uint64_t OwnerVA = InvalidVA;
      if (auto It = Img->DataPtrRelocTargetOwners.find(S);
          It != Img->DataPtrRelocTargetOwners.end())
        OwnerVA = It->second;
      SlotsByVA.emplace(S, PtrSlot{S, PtrSlotKind::Data, {}, 0, OwnerVA});
    }
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
    uint64_t TargetOwnerVA;
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
    Kept.push_back({Off, TargetVA, Slot.Kind, Slot.ImportName,
                    Slot.ImportAddend, Slot.TargetOwnerVA});
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
    } else if (K.Kind == PtrSlotKind::SuppressedCode) {
      // A post-SSA terminal-use proof replaced every observable load of this
      // slot with the recovered switch.  Keep an addressable, layout-preserving
      // field so base arithmetic (including an ET_REL table at VA zero) still
      // has a relocatable global identity, but never re-embed the stale guest
      // code pointer that suppression was designed to remove.
      FieldVal = llvm::ConstantInt::get(PtrIntTy, 0);
    } else if (K.Kind == PtrSlotKind::Data) {
      if (K.TargetVA == 0 && K.TargetOwnerVA == InvalidVA &&
          !Img->hasObjectDataProvenance(0))
        FieldVal = llvm::ConstantInt::get(PtrIntTy, 0);
      else if (llvm::Constant *G =
                   K.TargetOwnerVA != InvalidVA
                       ? tryResolveOwnedGlobalData(K.TargetVA, K.TargetOwnerVA)
                       : tryResolveGlobalData(K.TargetVA))
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

std::optional<uint64_t>
MedLLVMEmitter::ptrTableUniqueSegment(const MedVar &V,
                                      bool IncludeSymbolizedEvidence) const {
  if (!CurMedFunc || !Img)
    return std::nullopt;
  if (V.isConst())
    return std::nullopt;

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

  auto compute = [&]() -> std::optional<uint64_t> {
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
            return std::nullopt;
          uint64_t RunStart = 0, RunEnd = 0;
          readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
          if (Found && SegVA != RunStart)
            return std::nullopt; // bases span distinct mirror runs
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
          for (int I = 0; I < Def->NumInputs; ++I)
            Work.emplace_back(Def->Inputs[I], ThroughFrameReload);
          break;
        case NdOp::SELECT:
          if (!selectPreservesPointerValues(*Def))
            return std::nullopt;
          // The condition chooses a pointer value; it is never part of that
          // value's address provenance.
          Work.emplace_back(Def->Inputs[1], ThroughFrameReload);
          Work.emplace_back(Def->Inputs[2], ThroughFrameReload);
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
          if (!phiIncomingEdgeFeasible(*Phi, PredId))
            continue;
          Work.emplace_back(Arg, ThroughFrameReload);
        }
    }
    return Found ? std::optional<uint64_t>(SegVA) : std::nullopt;
  };

  std::optional<uint64_t> Result = compute();
  PtrTableUniqueSegCache[CacheKey] = Result;
  return Result;
}

const JumpTable *
MedLLVMEmitter::recoveredJumpTableForLoad(const MedOp &Load) const {
  return jumpTableForLoad(Load, /*RequireTerminalExclusive=*/true);
}

const JumpTable *
MedLLVMEmitter::authenticatedJumpTableForLoad(const MedOp &Load) const {
  return jumpTableForLoad(Load, /*RequireTerminalExclusive=*/false);
}

const JumpTable *
MedLLVMEmitter::jumpTableForLoad(const MedOp &Load,
                                 bool RequireTerminalExclusive) const {
  if (!CurMedFunc || !Img || Load.Opcode != NdOp::LOAD || Load.NumInputs < 1 ||
      Load.Output.isConst() || Load.Output.Size == 0 ||
      Load.MemoryOrdering != NdMemoryOrdering::None || Load.OriginSeq < 0)
    return nullptr;

  const AddressProvenanceVarKey LoadKey = addressProvenanceVarKey(Load.Output);

  for (const JumpTable &JT : CurMedFunc->JumpTables) {
    if (JT.EntrySize != Load.Output.Size || JT.Targets.empty() ||
        JT.MutatedUnsafe || JT.SuppressibleRelocationSlots.empty())
      continue;

    // The resolver authenticated a concrete Low LOAD occurrence.  Addr alone
    // is not an identity: one machine instruction can expand to several
    // memory operations, and later Med rewriting can retain the address while
    // changing the value role.  Require the complete stable witness here.
    const bool Authenticated = std::any_of(
        JT.AuthenticatedTableLoads.begin(), JT.AuthenticatedTableLoads.end(),
        [&](const JumpTableOpOccurrence &Occurrence) {
          return Occurrence.Addr == Load.Addr &&
                 Occurrence.Seq == Load.OriginSeq &&
                 Occurrence.Size == Load.Output.Size;
        });
    if (!Authenticated)
      continue;

    // Bind the occurrence to the advertised target-storage role as a second
    // certificate.  Ordinary tables can re-prove one concrete Med base.  A
    // TwoTable address has a runtime SELECT/mask-blend base and deliberately
    // has no single constant Med base; accept that shape only through the
    // exact Low certificate exported as a bound composite selector plan plus
    // complete ownership of both physical pointer runs.
    if (JT.TwoTableSelect) {
      const auto PlanIt = CurMedFunc->SwitchSelectorPlans.find(JT.InsnAddr);
      if (!JT.CompositeSelectorUseRef ||
          PlanIt == CurMedFunc->SwitchSelectorPlans.end())
        continue;
      const MedSwitchSelectorPlan &Plan = PlanIt->second;
      const JumpTableCompositeSelectorUseRef &Recipe =
          *JT.CompositeSelectorUseRef;
      if (Plan.PlanKind != MedSwitchSelectorPlan::Kind::SelectOffset ||
          Recipe.RecipeKind !=
              JumpTableCompositeSelectorUseRef::Kind::SelectOffset ||
          Plan.Selector.isConst() || Plan.Condition.isConst() ||
          Plan.Selector.Size == 0 || Plan.Condition.Size == 0 ||
          Plan.Selector.Size != Recipe.ByteIndex.ExpectedSize ||
          Plan.Condition.Size != Recipe.Condition.ExpectedSize ||
          Plan.ResultSize == 0 || Plan.ResultSize != Recipe.ResultSize ||
          Plan.TrueOffset != Recipe.TrueOffset ||
          Plan.FalseOffset != Recipe.FalseOffset ||
          !((Plan.TrueOffset == 0 && Plan.FalseOffset == JT.TwoTableOffset) ||
            (Plan.FalseOffset == 0 && Plan.TrueOffset == JT.TwoTableOffset)) ||
          JT.StorageRanges.size() != 2)
        continue;

      const JumpTableStorageRange &Lo = JT.StorageRanges[0];
      const JumpTableStorageRange &Hi = JT.StorageRanges[1];
      if (Lo.BaseAddr >= Hi.BaseAddr || Lo.EntrySize != Load.Output.Size ||
          Hi.EntrySize != Load.Output.Size ||
          Lo.EntryStride != Load.Output.Size ||
          Hi.EntryStride != Load.Output.Size || Lo.PhysicalSlotCount == 0 ||
          Lo.PhysicalSlotCount != Hi.PhysicalSlotCount ||
          Lo.PhysicalSlotCount > limits::kMaxJumpTableEntries / 2 ||
          JT.Targets.size() != 2 * Lo.PhysicalSlotCount ||
          Lo.PhysicalSlotCount >
              std::numeric_limits<uint64_t>::max() / Load.Output.Size ||
          uint64_t(Lo.PhysicalSlotCount) * Load.Output.Size !=
              JT.TwoTableOffset)
        continue;

      bool CompleteStorage = true;
      size_t CertifiedSlots = 0;
      for (const JumpTableStorageRange *Range : {&Lo, &Hi}) {
        for (uint64_t I = 0; I < Range->PhysicalSlotCount; ++I) {
          if (I != 0 &&
              Range->EntryStride > (InvalidVA - Range->BaseAddr) / I) {
            CompleteStorage = false;
            break;
          }
          const va_t Slot = Range->BaseAddr + I * Range->EntryStride;
          if (!Img->CodePtrRelocSlots.count(Slot) ||
              !JT.suppressesRelocationSlot(Slot)) {
            CompleteStorage = false;
            break;
          }
          ++CertifiedSlots;
        }
        if (!CompleteStorage)
          break;
      }
      if (!CompleteStorage ||
          CertifiedSlots != JT.SuppressibleRelocationSlots.size())
        continue;
    } else {
      uint64_t Base = 0;
      bool HaveBase = false;
      std::vector<MedVar> IndexTerms;
      bool Decomposed = collectIndexedGlobalBase(Load.Inputs[0], Base, HaveBase,
                                                 IndexTerms) &&
                        HaveBase;
      if (!Decomposed) {
        Base = 0;
        HaveBase = false;
        IndexTerms.clear();
        Decomposed = collectLiteralPoolBase(Load.Inputs[0], Base, HaveBase,
                                            IndexTerms) &&
                     HaveBase;
      }
      if (!Decomposed && JT.HasBaseAddr && JT.BaseAddr == 0) {
        Base = 0;
        HaveBase = true;
        Decomposed = true;
      }
      if (!Decomposed || !HaveBase)
        continue;
      const bool OwnsBase =
          std::any_of(JT.StorageRanges.begin(), JT.StorageRanges.end(),
                      [&](const JumpTableStorageRange &Range) {
                        return Range.EntrySize == Load.Output.Size &&
                               Range.ownsStorageAddress(Base);
                      });
      if (!OwnsBase)
        continue;
    }

    if (!RequireTerminalExclusive)
      return &JT;

    // Suppressed relocation slots need no independent mirror when this exact
    // target LOAD is semantically consumed only by the recovered switch.  Walk
    // the post-SSA use graph in the forward direction: pure target transforms
    // and PHIs are allowed, but any memory/control/observable use other than
    // this JT's terminal INDIR_BR keeps the normal mirror path mandatory.
    std::vector<MedVar> Work{Load.Output};
    std::set<AddressProvenanceVarKey> Seen;
    bool ReachesTerminalBranch = false;
    bool EscapesSwitch = false;
    while (!Work.empty() && !EscapesSwitch) {
      const MedVar Current = Work.back();
      Work.pop_back();
      const AddressProvenanceVarKey CurrentKey =
          addressProvenanceVarKey(Current);
      if (!Seen.insert(CurrentKey).second)
        continue;

      for (const MedBlock &Block : CurMedFunc->Blocks) {
        for (const PhiNode &Phi : Block.Phis) {
          bool Used = false;
          for (const auto &[Pred, Arg] : Phi.Args) {
            (void)Pred;
            Used |= addressProvenanceVarKey(Arg) == CurrentKey;
          }
          if (Used) {
            // A PHI is an observable escape only through its consumers.  Keep
            // walking the exact SSA output so a loop-carried target register
            // that is used solely by this recovered INDIR_BR remains safe,
            // while any STORE/CALL/RETURN/other branch reached from the PHI is
            // still rejected below.
            Work.push_back(Phi.Output);
          }
        }
        for (const MedOp &Op : Block.Ops) {
          bool Used = false;
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            Used |= addressProvenanceVarKey(Op.Inputs[I]) == CurrentKey;
          if (!Used)
            continue;

          if (Op.Opcode == NdOp::INDIR_BR && Op.Addr == JT.InsnAddr) {
            ReachesTerminalBranch = true;
            continue;
          }

          switch (Op.Opcode) {
          case NdOp::COPY:
          case NdOp::INT_ZEXT:
          case NdOp::INT_SEXT:
            if (Op.Output.Size == 0 || Op.Output.isConst())
              EscapesSwitch = true;
            else
              Work.push_back(Op.Output);
            break;
          case NdOp::SUBBYTES:
            if (Op.NumInputs < 2 || !Op.Inputs[1].isConst() ||
                Op.Inputs[1].ConstVal != 0 || Op.Output.Size == 0 ||
                Op.Output.isConst())
              EscapesSwitch = true;
            else
              Work.push_back(Op.Output);
            break;
          case NdOp::INT_ADD:
          case NdOp::INT_SUB: {
            // i386 GOTOFF tables load a target delta and add the GOT base
            // before the terminal branch.  A final-CFG GOTPC certificate
            // proves that exact base SSA is the linker model zero, so this is
            // value transport just like COPY.  Do not generalize to an
            // arbitrary stable/numeric zero: only the bound scalar-model
            // occurrence may discharge the extra operand.
            if (Op.NumInputs != 2 || Op.Output.Size != Current.Size ||
                Op.Output.isConst()) {
              EscapesSwitch = true;
              break;
            }
            const bool CurrentOnLeft =
                addressProvenanceVarKey(Op.Inputs[0]) == CurrentKey;
            const bool CurrentOnRight =
                addressProvenanceVarKey(Op.Inputs[1]) == CurrentKey;
            const MedVar *ZeroInput = nullptr;
            if (CurrentOnLeft != CurrentOnRight) {
              if (CurrentOnLeft)
                ZeroInput = &Op.Inputs[1];
              else if (Op.Opcode == NdOp::INT_ADD)
                ZeroInput = &Op.Inputs[0];
            }
            if (!ZeroInput || !valueIsAuthenticatedModelZero(*ZeroInput)) {
              EscapesSwitch = true;
              break;
            }
            Work.push_back(Op.Output);
            break;
          }
          default:
            EscapesSwitch = true;
            break;
          }
          if (EscapesSwitch)
            break;
        }
        if (EscapesSwitch)
          break;
      }
    }
    if (ReachesTerminalBranch && !EscapesSwitch)
      return &JT;
  }
  return nullptr;
}

llvm::Value *MedLLVMEmitter::tryResolveCodePtrTablePtr(
    const MedVar &AddrVar, llvm::IRBuilder<> &Builder,
    bool AllowImplicitZeroBase, const MedVar *LoadedValue) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  const MedOp *LoadedOp = LoadedValue ? lookupDef(*LoadedValue) : nullptr;
  const JumpTable *RecoveredJumpTable =
      LoadedOp ? authenticatedJumpTableForLoad(*LoadedOp) : nullptr;

  // The relocation mirror owns the memory representation of its complete
  // read-only run, including adjacent narrow scalar fields.  Do not confuse
  // that container role with the loaded value's callable role: indirect-call
  // and indirect-branch consumers validate the exact LOAD domain separately.
  // RecoveredJumpTable is used below only to discharge an otherwise unsafe
  // indexed term for the exact switch table that feeds its terminal branch.
  if (LoadedValue && LoadedValue->Size != 0 && Img->getPointerSize() != 0 &&
      LoadedValue->Size != Img->getPointerSize() && !RecoveredJumpTable) {
    const PointerTableLoadRoleSummary MemoryRoles =
        classifyPointerTableLoadRoles(*LoadedValue,
                                      /*RequirePointerWidth=*/false);
    const bool RelocationOwnedAccess =
        MemoryRoles.Complete && !MemoryRoles.SawConflict &&
        (MemoryRoles.SawCode || MemoryRoles.SawData || MemoryRoles.SawImport);
    if (!RelocationOwnedAccess)
      return nullptr;
  }

  // Path 1: one constant base + runtime index addends, the common
  // `lea table; mov (table,idx)` shape.  The base is a direct constant (x86-64
  // rip-relative / AArch64 ADRP / i386 GOTOFF) or folds through an ARM literal-
  // pool load (`ldr rN,[pc]; add rN,pc`).
  uint64_t Base = 0;
  bool HaveBase = false;
  std::vector<MedVar> IdxTerms;
  bool HaveConst =
      collectIndexedGlobalBase(AddrVar, Base, HaveBase, IdxTerms) && HaveBase &&
      !IdxTerms.empty();
  if (!HaveConst) {
    Base = 0;
    HaveBase = false;
    IdxTerms.clear();
    HaveConst = collectLiteralPoolBase(AddrVar, Base, HaveBase, IdxTerms) &&
                HaveBase && !IdxTerms.empty();
  }

  // In an ET_REL/MH_OBJECT image the first pointer table may start at VA 0.
  // The linked expression `table + index*stride` then contains no observable
  // base term at all: it is just `index*stride`.  Recover that algebraic zero
  // only from a relocation-proven pointer slot at zero and a fully numeric,
  // non-frame address expression.  This is occurrence proof, not a general
  // invitation to reinterpret integer zero as an address.
  if (!HaveConst && AllowImplicitZeroBase &&
      Img->hasRelocationProvenanceAt(0) && !varIsFrameDerived(AddrVar) &&
      valueIsStableAddressOffset(AddrVar)) {
    const Segment *ZeroSeg = Img->getSegmentFor(0);
    const bool PointerSlotAtZero =
        Img->CodePtrRelocSlots.count(0) || Img->DataPtrRelocSlots.count(0) ||
        Img->ImportPtrSlots.count(0) || Img->DyldBindSlots.count(0);
    if (ZeroSeg && !ZeroSeg->isExecutable() && PointerSlotAtZero) {
      Base = 0;
      HaveBase = true;
      IdxTerms = {AddrVar};
      HaveConst = true;
    }
  }
  const bool RecoveredIndexedJumpTable =
      RecoveredJumpTable && RecoveredJumpTable->HasBaseAddr && HaveConst &&
      Base == RecoveredJumpTable->BaseAddr;
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
  if (HaveConst && (!UnsafeIndexedTerms || RecoveredIndexedJumpTable)) {
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
  if (UnsafeIndexedTerms && !RecoveredIndexedJumpTable)
    return nullptr;

  // Path 2: the base is a SELECT/PHI or branchless AND/OR mask of several base
  // constants (a `cond ? A[i] : B[j]` pointer select), so path 1 cannot isolate
  // a single constant base.  When every base-like constant in the address lands
  // in ONE pointer-table segment, the whole address provably indexes that
  // segment, so redirect by the address's own value: GEP(@seg, addr - segVA).
  auto SelSegOpt =
      ptrTableUniqueSegment(AddrVar, /*IncludeSymbolizedEvidence=*/true);
  if (SelSegOpt) {
    const uint64_t SelSeg = *SelSegOpt;
    // Recovered switch metadata deliberately keeps ordinary, dead residual
    // jump-table loads out of generic constant symbolization.  A residual
    // LOAD whose complete address domain reaches only code/import relocation
    // slots is different: it is the live computed-goto/function-pointer
    // access itself and must use the relocation mirror.  Let the shared
    // occurrence-level lane classifier grant that narrow ownership instead of
    // making this address-role walk rediscover slot semantics independently.
    bool HasCompleteCallableLoadDomain = false;
    if (LoadedValue) {
      const PointerTableLoadRoleSummary Roles =
          classifyPointerTableLoadRoles(*LoadedValue);
      HasCompleteCallableLoadDomain =
          Roles.Load && Roles.Load->NumInputs >= 1 && Roles.isCallableOnly() &&
          addressProvenanceVarKey(Roles.Load->Inputs[0]) ==
              addressProvenanceVarKey(AddrVar);
      if (HasCompleteCallableLoadDomain)
        for (uint64_t Slot : Roles.Slots) {
          const Segment *SlotSeg = Img->getSegmentFor(Slot);
          uint64_t RunStart = 0, RunEnd = 0;
          if (!SlotSeg || SlotSeg->isExecutable()) {
            HasCompleteCallableLoadDomain = false;
            break;
          }
          readOnlyAfterRelocRun(SlotSeg, RunStart, RunEnd);
          if (RunStart != SelSeg || Slot < RunStart || Slot >= RunEnd) {
            HasCompleteCallableLoadDomain = false;
            break;
          }
        }
    }
    auto belongsToClaimedCodeTableRun = [&](uint64_t Address) {
      if (addrInCodePtrMirrorRun(Address))
        return true;
      if (!HasCompleteCallableLoadDomain)
        return false;
      const Segment *Seg = Img->getSegmentFor(Address);
      if (!Seg || Seg->isExecutable() || !segHasPtrRelocSlots(Seg))
        return false;
      uint64_t RunStart = 0, RunEnd = 0;
      readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
      return RunStart == SelSeg && Address >= RunStart && Address < RunEnd;
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
        // An exact relocation occurrence may name a real pointer-table slot at
        // VA zero in ET_REL/MH_OBJECT.  Numeric/scalar zero remains the null
        // value; only the occurrence tag grants zero an address owner.
        const Segment *Seg =
            (Value.ConstVal != 0 || isExactAddressProvenance(Value.Provenance))
                ? Img->getSegmentFor(Value.ConstVal)
                : nullptr;
        if (!Seg || Seg->isExecutable() ||
            !belongsToClaimedCodeTableRun(Value.ConstVal))
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
          if (Seg && !Seg->isExecutable() &&
              belongsToClaimedCodeTableRun(*FoldedLiteralBase))
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
          if (Seg && !Seg->isExecutable() &&
              belongsToClaimedCodeTableRun(*Folded)) {
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
  std::set<DataAddressIdentity> DataIdentities;
  if (recoverAbsoluteDataPointerLoadIdentities(V, DataIdentities))
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

bool MedLLVMEmitter::currentJumpTableOwnsStorageAddress(va_t Addr) const {
  for (const JumpTableStorageRange &Range : ModuleJumpTableStorageRanges)
    if (Range.ownsStorageAddress(Addr))
      return true;
  return false;
}

bool MedLLVMEmitter::currentJumpTableSuppressesRelocationSlot(va_t Addr) const {
  return ModuleSuppressibleJumpTableRelocationSlots.count(Addr) != 0;
}

bool MedLLVMEmitter::codeIdentityOccurrenceMayRelocate(
    const MedVar &V, bool IncludeLayoutCodeOwners) const {
  if (!Img || !CurMedFunc || !Mod)
    return false;

  if (CodeIdentityOccurrenceCacheFor != CurMedFunc) {
    CodeIdentityOccurrenceCacheFor = CurMedFunc;
    CodeIdentityOccurrenceCache.clear();
  }

  const AddressProvenanceVarKey RootKey = addressProvenanceVarKey(V);
  const auto RootCacheKey = std::make_pair(RootKey, IncludeLayoutCodeOwners);
  if (auto It = CodeIdentityOccurrenceCache.find(RootCacheKey);
      It != CodeIdentityOccurrenceCache.end())
    return It->second;

  // Build the uncached dependency subgraph iteratively, then solve the
  // reachability property from genuine identity leaves back to their users.
  // Publishing only after the whole graph is known makes false results safe
  // for SCCs: a recurrent edge is neither negative evidence nor a reason to
  // revisit the same exponentially branching graph.
  std::set<AddressProvenanceVarKey> Discovered{RootKey};
  std::map<AddressProvenanceVarKey, std::vector<AddressProvenanceVarKey>>
      ReverseEdges;
  std::set<AddressProvenanceVarKey> ContainsIdentity;
  std::vector<AddressProvenanceVarKey> IdentityWorklist;
  std::vector<MedVar> Worklist{V};

  auto MarkIdentity = [&](const AddressProvenanceVarKey &Key) {
    if (ContainsIdentity.insert(Key).second)
      IdentityWorklist.push_back(Key);
  };
  auto AddDependency = [&](const AddressProvenanceVarKey &UserKey,
                           const MedVar &Dependency) {
    const AddressProvenanceVarKey DependencyKey =
        addressProvenanceVarKey(Dependency);
    ReverseEdges[DependencyKey].push_back(UserKey);
    if (auto It = CodeIdentityOccurrenceCache.find(
            std::make_pair(DependencyKey, IncludeLayoutCodeOwners));
        It != CodeIdentityOccurrenceCache.end()) {
      if (It->second)
        MarkIdentity(DependencyKey);
      return;
    }
    if (Discovered.insert(DependencyKey).second)
      Worklist.push_back(Dependency);
  };

  while (!Worklist.empty()) {
    MedVar Current = Worklist.back();
    Worklist.pop_back();
    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Current);

    if (Current.isConst()) {
      const va_t Normalized =
          normalizeCodeAddress(Current.ConstVal, Img->Arch, Img->Mode);
      if (Current.Provenance == ConstantAddressProvenance::Address &&
          currentJumpTableOwnsStorageAddress(Normalized))
        continue;
      // Match the precise proof's null boundary.  In low-VA relocatable
      // objects, address-level CodeRefTargets may legitimately contain zero,
      // but an untagged zero occurrence is still a scalar/null value.  Using
      // the address map alone here would seed every arithmetic zero in the
      // function and send otherwise scalar SCCs into the rich code proof.
      if (Current.ConstVal == 0 &&
          !isExactAddressProvenance(Current.Provenance))
        continue;
      const bool HasStrongCodeProvenance =
          isCodeAddressProvenance(Current.Provenance) ||
          (Current.Provenance == ConstantAddressProvenance::Unknown &&
           Img->CodeRefTargets.count(Normalized) != 0);
      const bool HasAuthenticatedFunctionEntry =
          hasAuthenticatedFunctionEntryVA(Normalized);
      const bool HasLiftedFunctionIdentity =
          resolveLiftedFunctionEntry(Normalized) != nullptr;
      const bool HasLayoutCodeOwner =
          IncludeLayoutCodeOwners &&
          Current.Provenance == ConstantAddressProvenance::Address &&
          Img->hasExecutableCodeOwnerAt(Normalized);
      bool HasLiftedBlockIdentity = false;
      // A generic Address occurrence can name an exact emitted block on
      // targets without an architecturally readable PC GPR. AArch32 PC seeds
      // remain numeric in the narrow arithmetic role. X86 base-less indexed
      // displacements are tagged Scalar by the lifter, so numeric equality
      // with a lifted block cannot authorize them here.
      const bool AllowExactLiftedBlock =
          IncludeLayoutCodeOwners || TargetArch == Arch::X86 ||
          TargetArch == Arch::X64 || TargetArch == Arch::AArch64;
      if (AllowExactLiftedBlock)
        if (auto It = LiftedCodeBlocks.find(Normalized);
            It != LiftedCodeBlocks.end())
          HasLiftedBlockIdentity = It->second && It->second->getParent();
      if (!HasLiftedFunctionIdentity && !HasLiftedBlockIdentity &&
          !HasLayoutCodeOwner && !HasStrongCodeProvenance &&
          !HasAuthenticatedFunctionEntry)
        continue;
      const unsigned PointerSize = Img->getPointerSize();
      if (Current.Size != 0 && PointerSize != 0 && Current.Size < PointerSize)
        continue;
      if (HasStrongCodeProvenance ||
          (Current.Provenance == ConstantAddressProvenance::Address &&
           (HasAuthenticatedFunctionEntry || HasLiftedFunctionIdentity ||
            HasLiftedBlockIdentity || HasLayoutCodeOwner)) ||
          (Current.Provenance == ConstantAddressProvenance::Unknown &&
           constantOccurrenceMayRelocate(Current))) {
        MarkIdentity(Key);
      }
      continue;
    }

    if (const PhiNode *Phi = lookupPhi(Current)) {
      for (const auto &[Pred, Arg] : Phi->Args) {
        (void)Pred;
        AddDependency(Key, Arg);
      }
      continue;
    }

    const MedOp *Def = lookupDef(Current);
    if (!Def)
      continue;
    if (std::optional<MedVar> Forwarded = pointerPreservingInput(*Def)) {
      AddDependency(Key, *Forwarded);
      continue;
    }
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
      AddDependency(Key, Def->Inputs[1]);
      AddDependency(Key, Def->Inputs[2]);
      continue;
    }
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (collectFrameReloadSources(*Def, Sources))
        for (const MedVar &Source : Sources)
          AddDependency(Key, Source);
      continue;
    }

    switch (Def->Opcode) {
    case NdOp::CALL:
    case NdOp::INDIR_CALL:
    case NdOp::ATOMIC_XCHG:
    case NdOp::ATOMIC_ADD:
    case NdOp::ATOMIC_CMPXCHG:
      continue;
    case NdOp::INTRINSIC:
      if (atomicIntrinsicAddressInput(*Def))
        continue;
      break;
    default:
      break;
    }
    for (uint8_t I = 0; I < Def->NumInputs; ++I) {
      // Layout membership is final-sink evidence for an identity-preserving
      // value, not proof that every numeric operand of an arithmetic result
      // is a code address.  This distinction matters for low-VA ET_REL: an
      // index-only x86 LEA may contain a scalar displacement whose number also
      // lands inside .text.  Arithmetic emitters audit their operands through
      // the narrow role, so descend to that role at the operation boundary.
      if (IncludeLayoutCodeOwners) {
        if (codeIdentityOccurrenceMayRelocate(
                Def->Inputs[I], /*IncludeLayoutCodeOwners=*/false))
          MarkIdentity(Key);
      } else {
        AddDependency(Key, Def->Inputs[I]);
      }
    }
  }

  while (!IdentityWorklist.empty()) {
    AddressProvenanceVarKey Key = IdentityWorklist.back();
    IdentityWorklist.pop_back();
    auto It = ReverseEdges.find(Key);
    if (It == ReverseEdges.end())
      continue;
    for (const AddressProvenanceVarKey &UserKey : It->second)
      MarkIdentity(UserKey);
  }

  for (const AddressProvenanceVarKey &Key : Discovered)
    CodeIdentityOccurrenceCache.insert_or_assign(
        std::make_pair(Key, IncludeLayoutCodeOwners),
        ContainsIdentity.count(Key) != 0);
  return ContainsIdentity.count(RootKey) != 0;
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
  // Most observable integer values are pure runtime scalars. Avoid running the
  // richer identity/convergence proof unless the complete structural graph
  // contains either an explicit code occurrence or, at a final sink, an exact
  // generic Address owned by executable layout. Arithmetic operands use the
  // narrower default role so an architectural ARM PC seed remains numeric.
  // A forced callable target always runs the precise proof: explicit scalar,
  // data, or null targets must fail closed rather than take the raw fallback.
  if (!RequireCodeRole &&
      !codeIdentityOccurrenceMayRelocate(V,
                                         /*IncludeLayoutCodeOwners=*/true))
    return nullptr;

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
    MedVar Current = Start;
    while (!Current.isConst()) {
      if (addressProvenanceVarKey(Current) ==
          addressProvenanceVarKey(Root.Output))
        return true;
      const AddressProvenanceVarKey Key = addressProvenanceVarKey(Current);
      if (!Seen.insert(Key).second)
        return false;
      const MedOp *Def = lookupDef(Current);
      if (!Def)
        return false;
      std::optional<MedVar> Forwarded = pointerPreservingInput(*Def);
      if (!Forwarded || Forwarded->Size != Current.Size)
        return false;
      Current = *Forwarded;
    }
    return false;
  };

  auto finishValueMerge = [](CodeProof Result) {
    // Every exact arm is materialized by SELECT/PHI emission and every
    // dependency arm was produced by an operation that materializes its code
    // leaves.  The merged value is therefore relocation-aware but no longer a
    // single identity that may be replaced with its initializer.
    if (Result.SawCode && (Result.SawCodeDependency || Result.SawNull) &&
        !Result.SawNonCode && !Result.SawConflict &&
        !Result.SawUnsafeCodeDependency) {
      Result.SawCodeDependency = true;
      Result.SawCode = false;
      Result.SawUnresolved = false;
      Result.CommonTarget.reset();
    }
    return Result;
  };

  using CodeProofKey = std::pair<AddressProvenanceVarKey, bool>;

  auto classifyCodeConstant = [&](const MedVar &Current,
                                  bool IncludeLayoutCodeOwners) -> CodeProof {
    if (Current.Provenance == ConstantAddressProvenance::AddressFragment)
      return {.SawUnresolved = true, .SawExplicit = true};
    if (Current.ConstVal == 0 && !isExactAddressProvenance(Current.Provenance))
      return {.SawNull = true, .SawExplicit = true};
    if (Current.Provenance == ConstantAddressProvenance::Scalar ||
        Current.Provenance == ConstantAddressProvenance::DataAddress)
      return {.SawNonCode = true, .SawExplicit = true};
    const va_t Normalized =
        normalizeCodeAddress(Current.ConstVal, Img->Arch, Img->Mode);
    if (Current.Provenance == ConstantAddressProvenance::Address &&
        currentJumpTableOwnsStorageAddress(Normalized))
      return {.SawNonCode = true, .SawExplicit = true};
    const bool HasLoaderCodeProvenance =
        Current.Provenance == ConstantAddressProvenance::Unknown &&
        Img->CodeRefTargets.count(Normalized) != 0;
    if (Current.Provenance == ConstantAddressProvenance::Unknown &&
        !HasLoaderCodeProvenance)
      return {.SawUnresolved = true};
    const bool HasFunctionIdentity =
        resolveLiftedFunctionEntry(Normalized) != nullptr;
    const bool HasAuthenticatedFunctionEntry =
        hasAuthenticatedFunctionEntryVA(Normalized);
    bool HasLiftedCodeIdentity = HasFunctionIdentity;
    const bool AllowExactLiftedBlock =
        IncludeLayoutCodeOwners || TargetArch == Arch::X86 ||
        TargetArch == Arch::X64 || TargetArch == Arch::AArch64;
    if (AllowExactLiftedBlock)
      if (auto It = LiftedCodeBlocks.find(Normalized);
          !HasLiftedCodeIdentity && It != LiftedCodeBlocks.end())
        HasLiftedCodeIdentity = It->second && It->second->getParent();
    const bool HasLayoutCodeOwner =
        IncludeLayoutCodeOwners &&
        Current.Provenance == ConstantAddressProvenance::Address &&
        Img->hasExecutableCodeOwnerAt(Normalized);
    const bool IsCode =
        Current.Provenance == ConstantAddressProvenance::CodeAddress ||
        HasLoaderCodeProvenance ||
        (Current.Provenance == ConstantAddressProvenance::Address &&
         (HasLayoutCodeOwner || HasAuthenticatedFunctionEntry ||
          HasLiftedCodeIdentity));
    if (!IsCode)
      return {.SawNonCode = true, .SawExplicit = true};
    if (Current.Size != 0 && PointerSize != 0 && Current.Size < PointerSize)
      return {.SawCode = true, .SawUnresolved = true, .SawExplicit = true};
    return {.CommonTarget = Normalized,
            .SawCode = true,
            .SawExplicit = true,
            .SawFunctionIdentity = HasFunctionIdentity,
            .SawLiftedCodeIdentity = HasLiftedCodeIdentity,
            .SawStrongCodeProvenance =
                isCodeAddressProvenance(Current.Provenance) ||
                HasLoaderCodeProvenance || HasAuthenticatedFunctionEntry};
  };

  struct CodeProofResult {
    CodeProof Proof;
    bool Complete = true;
  };
  auto completeProof = [](CodeProof Proof) {
    return CodeProofResult{std::move(Proof), true};
  };

  // Some targets materialize one exact function address as arithmetic before
  // spilling it: i386 uses call/pop + GOTOFF, while ARM32 loads a relocated
  // PC-relative displacement from the literal pool and adds PC.  The generic
  // arithmetic proof correctly treats arbitrary code-address arithmetic as a
  // relation rather than a callable identity, so recognize this narrower
  // occurrence here only when the complete expression folds to an emitted
  // function entry and the expression itself contains occurrence-level code
  // evidence.  Local ARM literal pools can encode a resolved PC displacement
  // without a loader relocation, so an exact executable literal occurrence is
  // also evidence; a same-valued scalar/data word is not.
  auto exactFunctionMaterialization =
      [&](const MedVar &Root) -> std::optional<va_t> {
    bool SawLiteralLoad = false;
    bool SawArithmetic = false;
    auto Folded =
        traceTableBaseConst(Root, 0, &SawLiteralLoad, nullptr, &SawArithmetic);
    if (!Folded || !SawArithmetic)
      return std::nullopt;
    const va_t Target = normalizeCodeAddress(*Folded, Img->Arch, Img->Mode);
    if (!resolveLiftedFunctionEntry(Target))
      return std::nullopt;

    using EvidenceKey = std::tuple<int, int, int, uint16_t>;
    std::function<bool(const MedVar &, int, std::set<EvidenceKey>)>
        hasOccurrenceEvidence = [&](const MedVar &Value, int Depth,
                                    std::set<EvidenceKey> Seen) {
          if (Depth > 32)
            return false;
          if (Value.isConst())
            return codeIdentityOccurrenceMayRelocate(
                Value, /*IncludeLayoutCodeOwners=*/false);
          EvidenceKey Key{static_cast<int>(Value.Kind), Value.Id, Value.SSAVer,
                          Value.Size};
          if (!Seen.insert(Key).second)
            return false;
          const MedOp *Def = lookupDef(Value);
          if (!Def)
            return false;
          if (Def->Opcode == NdOp::LOAD && Def->NumInputs >= 1) {
            auto Slot = traceValueVA(Def->Inputs[0]);
            if (!Slot)
              return false;
            if (Img->hasRelocationProvenanceAt(*Slot))
              return true;
            const Segment *SlotSegment = Img->getSegmentFor(*Slot);
            return SawLiteralLoad && SlotSegment &&
                   SlotSegment->isExecutable() && !SlotSegment->isWritable();
          }
          if (auto Forwarded = pointerPreservingInput(*Def))
            return hasOccurrenceEvidence(*Forwarded, Depth + 1, Seen);
          if (Def->Opcode != NdOp::INT_ADD || Def->NumInputs < 2)
            return false;
          return hasOccurrenceEvidence(Def->Inputs[0], Depth + 1, Seen) ||
                 hasOccurrenceEvidence(Def->Inputs[1], Depth + 1, Seen);
        };
    return hasOccurrenceEvidence(Root, 0, {}) ? std::optional<va_t>(Target)
                                              : std::nullopt;
  };

  // Classify code identity through the SSA value graph without changing the
  // generic getVar policy.  Only pointer-preserving forwarders and value
  // merges can publish a unique identity.  Other operations are scanned for a
  // code-bearing leaf so relocation-dependent arithmetic cannot silently fall
  // back to its old numeric result.
  // The proof role is part of node identity. Layout ownership is valid at an
  // observable value/target and through identity-preserving transports, but a
  // generic arithmetic operand needs occurrence-level code evidence. Keeping
  // the roles separate prevents a low-VA scalar displacement that merely
  // lands in .text from poisoning the code-identity proof.
  std::set<CodeProofKey> Active;
  std::map<CodeProofKey, CodeProof> ProofCache;
  std::function<CodeProofResult(const MedVar &, bool)> proveCodeValue =
      [&](const MedVar &Current,
          bool IncludeLayoutCodeOwners) -> CodeProofResult {
    if (Current.isConst())
      return completeProof(
          classifyCodeConstant(Current, IncludeLayoutCodeOwners));

    const AddressProvenanceVarKey Key = addressProvenanceVarKey(Current);
    const CodeProofKey ProofKey{Key, IncludeLayoutCodeOwners};
    if (auto It = ProofCache.find(ProofKey); It != ProofCache.end())
      return completeProof(It->second);
    if (!Active.insert(ProofKey).second) {
      // Residual cycles are those not already discharged by the PHI
      // recurrence proof below. Cut them with a conservative, cacheable
      // summary so a shared recurrent DAG is linear rather than exponential.
      // The backedge itself contributes only a runtime, non-code value. Any
      // actual code initializer or other exit from the SCC is still visited on
      // the active DFS stack and merged by its owning transfer. Keeping this
      // cut neutral is essential: a graph-reachability upper bound cannot tell
      // code inside the residual cycle from a legitimate initializer entering
      // `PHI(&f, COPY^N(phi))`. NonCode keeps PHI/SELECT merges conservative;
      // a supported arithmetic user may clear it only while rebuilding every
      // code-bearing operand through the same relocation-aware emitter path.
      return completeProof({.SawNonCode = true, .SawUnresolved = true});
    }
    auto Finish = [&](CodeProofResult Result) {
      Active.erase(ProofKey);
      if (Result.Complete)
        ProofCache.insert_or_assign(ProofKey, Result.Proof);
      return Result;
    };

    if (const PhiNode *Phi = lookupPhi(Current)) {
      CodeProof Combined;
      bool SawArm = false;
      bool Complete = true;
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
        CodeProofResult Arm = proveCodeValue(Arg, IncludeLayoutCodeOwners);
        Combined = mergeProof(std::move(Combined), Arm.Proof);
        Complete &= Arm.Complete;
        SawArm = true;
      }
      if (!SawArm)
        Combined.SawUnresolved = true;
      return Finish({finishValueMerge(std::move(Combined)), Complete});
    }

    const MedOp *Def = lookupDef(Current);
    if (!Def)
      return Finish(completeProof({.SawUnresolved = true}));
    if (auto ExactTarget = exactFunctionMaterialization(Current))
      return Finish(completeProof({.CommonTarget = *ExactTarget,
                                   .SawCode = true,
                                   .SawExplicit = true,
                                   .SawFunctionIdentity = true,
                                   .SawLiftedCodeIdentity = true,
                                   .SawStrongCodeProvenance = true}));
    if (std::optional<MedVar> Forwarded = pointerPreservingInput(*Def)) {
      // Low-to-Med publishes entry-state registers as COPY R,R. This is a
      // runtime input boundary, not an incomplete SSA recurrence. Treat the
      // exact self-forwarder as one stable unknown leaf so every dependent
      // arithmetic node can be memoized; following it through Active would
      // otherwise make a large acyclic consumer DAG exponentially expensive.
      if (addressProvenanceVarKey(*Forwarded) == Key)
        return Finish(completeProof({.SawUnresolved = true}));
      return Finish(proveCodeValue(*Forwarded, IncludeLayoutCodeOwners));
    }
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
      CodeProofResult TrueArm =
          proveCodeValue(Def->Inputs[1], IncludeLayoutCodeOwners);
      CodeProofResult FalseArm =
          proveCodeValue(Def->Inputs[2], IncludeLayoutCodeOwners);
      CodeProof Combined = mergeProof(std::move(TrueArm.Proof), FalseArm.Proof);
      return Finish({finishValueMerge(std::move(Combined)),
                     TrueArm.Complete && FalseArm.Complete});
    }

    // A proven local-frame reload inherits the occurrences stored into that
    // slot. This is how an -O0 `fn_t p = target; p()` remains relocatable while
    // the intermediate STORE itself stays a local transport. An incomplete or
    // ambiguous reaching-store proof is intentionally unresolved.
    if (Def->Opcode == NdOp::LOAD) {
      std::vector<MedVar> Sources;
      if (!collectFrameReloadSources(*Def, Sources) || Sources.empty())
        return Finish(completeProof({.SawUnresolved = true}));
      CodeProof Combined;
      bool Complete = true;
      for (const MedVar &Source : Sources) {
        CodeProofResult SourceResult =
            proveCodeValue(Source, IncludeLayoutCodeOwners);
        Combined = mergeProof(std::move(Combined), SourceResult.Proof);
        Complete &= SourceResult.Complete;
      }
      return Finish({std::move(Combined), Complete});
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
      return Finish(completeProof({.SawUnresolved = true}));
    case NdOp::INTRINSIC:
      if (atomicIntrinsicAddressInput(*Def))
        return Finish(completeProof({.SawUnresolved = true}));
      break;
    default:
      break;
    }

    CodeProof Combined;
    bool Complete = true;
    for (uint8_t I = 0; I < Def->NumInputs; ++I) {
      // Operation results may carry a relocated relation, but mere executable
      // layout membership of one numeric operand is not code identity. The
      // operation emitter applies the same narrow role to each operand.
      CodeProofResult Input = proveCodeValue(Def->Inputs[I],
                                             /*IncludeLayoutCodeOwners=*/false);
      Combined = mergeProof(std::move(Combined), Input.Proof);
      Complete &= Input.Complete;
    }
    if (Combined.SawCode || Combined.SawCodeDependency) {
      // Arithmetic/bit operations may use a numeric architectural PC as an
      // offset base (ARM switch lowering). They depend on code layout but no
      // longer carry a code-pointer identity. A forced code use must reject
      // them; an ordinary scalar sink keeps its established owner instead of
      // reintroducing eager BlockAddress materialization.
      const bool RelocationAware =
          operationUsesRelocatableCodeIdentity(Def->Opcode);
      Combined.SawUnsafeCodeDependency |= (Combined.SawLiftedCodeIdentity ||
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
    return Finish({std::move(Combined), Complete});
  };

  const CodeProof Proof =
      proveCodeValue(V, /*IncludeLayoutCodeOwners=*/true).Proof;
  const bool UniqueCode = Proof.SawCode && Proof.CommonTarget &&
                          !Proof.SawNonCode && !Proof.SawUnresolved &&
                          !Proof.SawConflict;
  if (UniqueCode) {
    llvm::Constant *Identity = nullptr;
    if (RequireCodeRole) {
      // A CALL target must name a function entry.  An interior BlockAddress is
      // a relocatable code value, but calling it would bypass the lifted
      // function's ABI/prologue and is never a valid fallback.
      Identity = resolveLiftedFunctionEntry(*Proof.CommonTarget);
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
    const PointerTableLoadRoleSummary SlotRoles =
        classifyPointerTableLoadRoles(V);
    if (SlotRoles.Recognized) {
      // A code-bearing mixed run may still have adjacent unknown/data slots.
      // Preserve only an unadjusted value loaded through an architectural
      // register address; getVar retains its relocation-safe runtime value.
      if (SlotRoles.SawCode && SlotRoles.ValueAdjustment == 0 &&
          SlotRoles.Load && SlotRoles.Load->NumInputs >= 1 &&
          SlotRoles.Load->Inputs[0].Kind == MedVar::Reg)
        if (llvm::Value *Target = getVar(V, Builder))
          return Target;
      // The LOAD emitter has already routed this address through the mixed
      // relocation mirror. Reuse that relocation-safe runtime value only when
      // every reachable slot is a code/import field and no post-load pointer
      // arithmetic changed the function entry. Discovery of the surrounding
      // run alone is never role proof: records commonly interleave length,
      // handler, and name fields.
      if (SlotRoles.isCallableOnly() && SlotRoles.ValueAdjustment == 0)
        if (llvm::Value *Target = getVar(V, Builder))
          return Target;

      if (!FatalCodePointerResolution && !FatalDataPointerResolution) {
        if (SlotRoles.isDataOnly())
          syncError() << "med_llvm_emitter: data-pointer relocation used as an "
                         "indirect-call target in "
                      << CurMedFunc->Name
                      << "; refusing code-identity fallback\n";
        else
          syncError() << "med_llvm_emitter: pointer-table load used as an "
                         "indirect-call target in "
                      << CurMedFunc->Name
                      << " has no complete code-slot role; refusing "
                         "code-identity fallback\n";
      }
      FatalCodePointerResolution = true;
      return nullptr;
    }
    std::set<DataAddressIdentity> DataIdentities;
    if (recoverAbsoluteDataPointerLoadIdentities(V, DataIdentities)) {
      if (!FatalCodePointerResolution && !FatalDataPointerResolution)
        syncError() << "med_llvm_emitter: data-pointer relocation used as an "
                       "indirect-call target in "
                    << CurMedFunc->Name
                    << "; refusing code-identity fallback\n";
      FatalCodePointerResolution = true;
      return nullptr;
    }
    bool SawLoad = false;
    if (auto VA = traceTableBaseConst(V, 0, &SawLoad); VA && SawLoad) {
      const va_t Normalized = normalizeCodeAddress(*VA, Img->Arch, Img->Mode);
      if (llvm::Function *Function = resolveLiftedFunctionEntry(Normalized)) {
        const unsigned PtrSize =
            Img->getPointerSize()
                ? Img->getPointerSize()
                : static_cast<unsigned>(V.Size > 0 ? V.Size : 8);
        return Builder.CreatePtrToInt(
            Function,
            sizeToType(V.Size > 0 ? V.Size : static_cast<uint16_t>(PtrSize)),
            "icall.literal.target");
      }

      if (Img->hasExecutableCodeOwnerAt(Normalized) ||
          LiftedCodeBlocks.count(Normalized) ||
          Img->isImportStubAt(Normalized)) {
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
