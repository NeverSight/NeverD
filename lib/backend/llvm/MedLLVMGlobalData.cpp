//===- MedLLVMGlobalData.cpp - Global data resolution ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Global-data resolution for MedLLVMEmitter: embedding read-only, executable
/// and writable segments as cohesive globals, the writable-data / code-pointer
/// address resolvers, and the tryResolveGlobalData constant-address entry
/// point.  SSA constant tracing and the read-only table resolvers live in
/// MedLLVMAddrResolve.cpp; the architecture-gated i386 PIC address recognizers
/// live in X86/MedLLVMX86GlobalData.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Object/SectionNames.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

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

//===----------------------------------------------------------------------===//
// Global data resolution
//===----------------------------------------------------------------------===//

std::pair<llvm::GlobalVariable *, uint64_t>
MedLLVMEmitter::embedRodataRun(uint64_t SegVA) {
  if (!Img)
    return {nullptr, 0};
  // Collect read-only, non-executable segments that carry data, sorted by VA.
  std::vector<const Segment *> Ro;
  for (const auto &S : Img->Segments)
    if (S.isReadable() && !S.isWritable() && !S.isExecutable() &&
        !S.Data.empty())
      Ro.push_back(&S);
  std::sort(Ro.begin(), Ro.end(),
            [](const Segment *A, const Segment *B) { return A->VA < B->VA; });
  size_t Idx = Ro.size();
  for (size_t I = 0; I < Ro.size(); ++I)
    if (Ro[I]->VA == SegVA) {
      Idx = I;
      break;
    }
  if (Idx == Ro.size())
    return {nullptr, 0};
  // Extend over neighbours separated only by a small alignment gap; a larger
  // gap means a distinct region that must not be merged.
  auto adjacent = [](const Segment *Prev, const Segment *Next) {
    uint64_t PrevEnd = Prev->VA + Prev->Data.size();
    return Next->VA >= PrevEnd && Next->VA - PrevEnd <= 16;
  };
  size_t Lo = Idx, Hi = Idx;
  while (Lo > 0 && adjacent(Ro[Lo - 1], Ro[Lo]))
    --Lo;
  while (Hi + 1 < Ro.size() && adjacent(Ro[Hi], Ro[Hi + 1]))
    ++Hi;
  uint64_t RunStart = Ro[Lo]->VA;
  uint64_t RunEnd = Ro[Hi]->VA + Ro[Hi]->Data.size();
  uint64_t RunLen64 = RunEnd - RunStart;
  if (RunLen64 == 0 || RunLen64 > limits::kMaxSingleGlobalEmbedLen)
    return {nullptr, 0};
  size_t RunLen = static_cast<size_t>(RunLen64);
  if (auto It = SegmentDataGlobals.find(RunStart);
      It != SegmentDataGlobals.end())
    return {It->second, RunStart};
  std::vector<uint8_t> Buf(RunLen, 0);
  for (size_t I = Lo; I <= Hi; ++I)
    std::memcpy(Buf.data() + static_cast<size_t>(Ro[I]->VA - RunStart),
                Ro[I]->Data.data(), Ro[I]->Data.size());
  auto *ArrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), RunLen);
  auto *Init = llvm::ConstantDataArray::get(*Ctx, llvm::ArrayRef<uint8_t>(Buf));
  auto *GV = new llvm::GlobalVariable(
      *Mod, ArrTy, /*isConstant=*/true, dataLinkage(), Init,
      (kNdDataPrefix + llvm::utohexstr(RunStart)).str() +
          section_names::elf::Rodata);
  GV->setAlignment(llvm::Align(16));
  markSharedLocal(GV);
  SegmentDataGlobals[RunStart] = GV;
  return {GV, RunStart};
}

std::pair<llvm::GlobalVariable *, uint64_t>
MedLLVMEmitter::embedExecSegmentRun(const Segment *Seg) {
  if (!Seg || Seg->Data.empty() ||
      Seg->Data.size() > limits::kMaxSingleGlobalEmbedLen)
    return {nullptr, 0};
  uint64_t RunStart = Seg->VA;
  if (auto It = SegmentDataGlobals.find(RunStart);
      It != SegmentDataGlobals.end())
    return {It->second, RunStart};
  auto *ArrTy =
      llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), Seg->Data.size());
  auto *Init =
      llvm::ConstantDataArray::get(*Ctx, llvm::ArrayRef<uint8_t>(Seg->Data));
  auto *GV = new llvm::GlobalVariable(
      *Mod, ArrTy, /*isConstant=*/true, dataLinkage(), Init,
      (kNdDataPrefix + llvm::utohexstr(RunStart)).str() +
          section_names::elf::Rodata);
  GV->setAlignment(llvm::Align(16));
  markSharedLocal(GV);
  SegmentDataGlobals[RunStart] = GV;
  return {GV, RunStart};
}

bool MedLLVMEmitter::isMutableDataSeg(const Segment *S) const {
  if (!S || !S->isReadable() || !S->isWritable() || S->isExecutable())
    return false;
  // RELRO is writable in section flags but a relocated pointer table that is
  // read-only after relocation; it is owned by the pointer-table machinery.
  if (section_names::isDataRelRoSectionName(S->Name))
    return false;
  if (!Img)
    return true;
  // A segment carrying any relocated pointer slot is likewise a pointer table,
  // not raw mutable scalar/array data.
  auto overlaps = [&](const std::set<uint64_t> &Slots) {
    for (uint64_t V : Slots)
      if (S->contains(V))
        return true;
    return false;
  };
  return !(overlaps(Img->CodePtrRelocSlots) ||
           overlaps(Img->DataPtrRelocSlots) ||
           overlaps(Img->RelCodeRelocSlots));
}

bool MedLLVMEmitter::segHasPtrRelocSlots(const Segment *S) const {
  if (!S || !Img)
    return false;
  auto overlaps = [&](const std::set<uint64_t> &Slots) {
    for (uint64_t V : Slots)
      if (S->contains(V))
        return true;
    return false;
  };
  return overlaps(Img->CodePtrRelocSlots) || overlaps(Img->DataPtrRelocSlots);
}

bool MedLLVMEmitter::isReadOnlyAfterReloc(const Segment *S) const {
  return S && S->isReadable() && !S->isExecutable() && !S->Data.empty() &&
         (!S->isWritable() ||
          section_names::isDataRelRoSectionName(S->Name));
}

void MedLLVMEmitter::readOnlyAfterRelocRun(const Segment *Seg,
                                           uint64_t &RunStart,
                                           uint64_t &RunEnd) const {
  RunStart = Seg->VA;
  RunEnd = Seg->VA + Seg->Data.size();
  if (!Img || !isReadOnlyAfterReloc(Seg))
    return;
  std::vector<const Segment *> RO;
  for (const auto &S : Img->Segments)
    if (isReadOnlyAfterReloc(&S))
      RO.push_back(&S);
  std::sort(RO.begin(), RO.end(),
            [](const Segment *A, const Segment *B) { return A->VA < B->VA; });
  size_t Idx = RO.size();
  for (size_t I = 0; I < RO.size(); ++I)
    if (RO[I]->VA == Seg->VA) {
      Idx = I;
      break;
    }
  if (Idx == RO.size())
    return;
  auto adjacent = [](const Segment *P, const Segment *N) {
    uint64_t PEnd = P->VA + P->Data.size();
    return N->VA >= PEnd && N->VA - PEnd <= 16;
  };
  size_t Lo = Idx, Hi = Idx;
  while (Lo > 0 && adjacent(RO[Lo - 1], RO[Lo]))
    --Lo;
  while (Hi + 1 < RO.size() && adjacent(RO[Hi], RO[Hi + 1]))
    ++Hi;
  uint64_t S0 = RO[Lo]->VA;
  uint64_t E0 = RO[Hi]->VA + RO[Hi]->Data.size();
  if (E0 > S0 && E0 - S0 <= limits::kMaxEmbeddedDataLen) {
    RunStart = S0;
    RunEnd = E0;
  }
}

bool MedLLVMEmitter::addrInCodePtrMirrorRun(uint64_t VA) const {
  if (!Img || VA == 0)
    return false;
  const Segment *Seg = Img->getSegmentFor(VA);
  if (!isReadOnlyAfterReloc(Seg))
    return false;
  uint64_t RunStart = 0, RunEnd = 0;
  readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
  if (VA < RunStart || VA >= RunEnd)
    return false;
  // A recovered switch JUMP TABLE's entries also land in CodePtrRelocSlots, but
  // a jump table is dispatched by the recovered high-level `switch` (its
  // residual table load is dead/volatile) — it is NOT re-symbolized through the
  // data mirror.  When such a table shares a rodata run with unrelated data
  // (clang lays a `static const` array right after the switch table), the
  // jump-table slots must NOT make the run a code-pointer mirror: that would
  // wrongly keep a DATA pointer into the trailing array raw, and a consumer
  // that walks it with pointer arithmetic never re-symbolizes (it would read
  // the stale original VA → unmapped).  Exclude code-pointer slots that fall
  // inside a recovered table.
  auto inJumpTable = [&](uint64_t S) -> bool {
    if (!CurMedFunc)
      return false;
    for (const auto &JT : CurMedFunc->JumpTables) {
      if (JT.BaseAddr == 0 || JT.EntrySize == 0 || JT.Targets.empty())
        continue;
      uint64_t TblEnd =
          JT.BaseAddr + static_cast<uint64_t>(JT.EntrySize) * JT.Targets.size();
      if (S >= JT.BaseAddr && S < TblEnd)
        return true;
    }
    return false;
  };
  auto inRun = [&](const std::set<uint64_t> &Slots, bool ExcludeJumpTables) {
    for (uint64_t S : Slots)
      if (S >= RunStart && S < RunEnd) {
        if (ExcludeJumpTables && inJumpTable(S))
          continue;
        return true;
      }
    return false;
  };
  return inRun(Img->CodePtrRelocSlots, /*ExcludeJumpTables=*/true) ||
         inRun(Img->DataPtrRelocSlots, /*ExcludeJumpTables=*/false);
}

std::pair<llvm::GlobalVariable *, uint64_t>
MedLLVMEmitter::embedWritableRun(uint64_t SegVA) {
  if (!Img)
    return {nullptr, 0};
  const Segment *Seg = Img->getSegmentFor(SegVA);
  if (!isMutableDataSeg(Seg))
    return {nullptr, 0};
  uint64_t RunStart = Seg->VA;
  uint64_t RunLen64 = Seg->Size ? Seg->Size : Seg->Data.size();
  // One cohesive mutable global per writable segment, GEP'd into for every
  // access — the writable counterpart of embedRodataRun.  Bounded by the
  // single-global cap (not the per-access kMaxEmbeddedDataLen): a single whole-
  // segment global is the smallest possible form, so a large .bss/.data array
  // (e.g. `static unsigned G[2048]`, 8 KiB) is embedded once rather than left
  // as a bare inttoptr(VA) the relinked object never maps ->
  // WRITE/READ_UNMAPPED.
  if (RunLen64 == 0 || RunLen64 > limits::kMaxSingleGlobalEmbedLen)
    return {nullptr, 0};
  if (auto It = WritableSegmentGlobals.find(RunStart);
      It != WritableSegmentGlobals.end())
    return {It->second, RunStart};

  size_t RunLen = static_cast<size_t>(RunLen64);
  std::vector<uint8_t> Buf(RunLen, 0);
  // Fill from SECTIONS, not the segment: a relocatable .o's .bss segment
  // carries no reliable file data, but its Section carries none (FileSz 0) so
  // .bss stays zero, while .data sections copy their bytes.  A linked PT_LOAD
  // writable segment is also covered (its sections overlay the same range).
  bool FilledFromSections = false;
  for (const auto &Sec : Img->Sections) {
    if (Sec.Data.empty() || Sec.VA < RunStart || Sec.VA >= RunStart + RunLen)
      continue;
    size_t Off = static_cast<size_t>(Sec.VA - RunStart);
    size_t N = std::min(Sec.Data.size(), RunLen - Off);
    std::memcpy(Buf.data() + Off, Sec.Data.data(), N);
    FilledFromSections = true;
  }
  // Fallback ONLY for images without section metadata: a PROGBITS segment whose
  // own data is trustworthy (file-backed, zero-extended to memsz by the
  // loader). When sections exist, never copy the segment bytes: a relocatable
  // .o's .bss (SHT_NOBITS) segment carries garbage (file bytes read at its
  // placeholder offset), and the absent .bss section correctly leaves the run
  // zero-filled.
  if (!FilledFromSections && Img->Sections.empty() && !Seg->Data.empty()) {
    size_t N = std::min(Seg->Data.size(), RunLen);
    std::memcpy(Buf.data(), Seg->Data.data(), N);
  }

  auto *ArrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), RunLen);
  auto *Init = llvm::ConstantDataArray::get(*Ctx, llvm::ArrayRef<uint8_t>(Buf));
  auto *GV = new llvm::GlobalVariable(
      *Mod, ArrTy, /*isConstant=*/false, dataLinkage(), Init,
      (kNdDataPrefix + llvm::utohexstr(RunStart)).str() +
          section_names::elf::Data);
  GV->setAlignment(llvm::Align(16));
  markSharedLocal(GV);
  WritableSegmentGlobals[RunStart] = GV;
  return {GV, RunStart};
}

void MedLLVMEmitter::ensureAddrPredCache() const {
  if (AddrPredCacheFor == CurMedFunc)
    return;
  AddrPredCacheFor = CurMedFunc;
  SlotAddressEscapesCache.clear();
  SlotMatchingKeyLoadCache.clear();
  SlotReloadUsedLocallyCache.clear();
  WritableDataSegCache.clear();
  PtrTableUniqueSegCache.clear();
}

bool MedLLVMEmitter::stackSlotAddressEscapes(const MedVar &SlotAddr) const {
  if (!CurMedFunc)
    return false;
  // Canonicalize the slot through register copies so a parameter-register copy
  // of the address compares equal to the load/store form (both ThroughRegs).
  auto Target = addrSlotKey(SlotAddr, /*Depth=*/0, /*ThroughRegs=*/true);
  if (!Target)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotAddressEscapesCache.find(*Target);
      It != SlotAddressEscapesCache.end())
    return It->second;
  bool Result = false;
  // The slot's address passed as a call argument: a callee may write an
  // already- symbolized pointer through it (the escaping output-pointer shape,
  // #475).
  for (const auto &CI : CurMedFunc->CallInfos) {
    for (const auto &Arg : CI.Args)
      if (auto K = addrSlotKey(Arg, 0, true); K && *K == *Target) {
        Result = true;
        break;
      }
    if (Result)
      break;
  }
  // The slot's address stored to memory escapes the same way.
  if (!Result)
    for (const auto &Blk : CurMedFunc->Blocks) {
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
          if (auto K = addrSlotKey(Op.Inputs[1], 0, true); K && *K == *Target) {
            Result = true;
            break;
          }
      if (Result)
        break;
    }
  SlotAddressEscapesCache[*Target] = Result;
  return Result;
}

bool MedLLVMEmitter::frameSlotHasMatchingKeyLoad(
    const MedVar &StoreAddr) const {
  if (!CurMedFunc)
    return false;
  auto SK = addrSlotKey(StoreAddr);
  if (!SK)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotMatchingKeyLoadCache.find(*SK);
      It != SlotMatchingKeyLoadCache.end())
    return It->second;
  bool Result = false;
  for (const auto &Blk : CurMedFunc->Blocks) {
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
        if (auto LK = addrSlotKey(Op.Inputs[0]); LK && *LK == *SK) {
          Result = true;
          break;
        }
    if (Result)
      break;
  }
  SlotMatchingKeyLoadCache[*SK] = Result;
  return Result;
}

std::optional<uint64_t> MedLLVMEmitter::traceValueVA(const MedVar &V,
                                                     int Depth) const {
  if (V.isConst())
    return V.ConstVal;
  if (!CurMedFunc || Depth > 12)
    return std::nullopt;
  const MedOp *Def = lookupDef(V);
  if (!Def)
    return std::nullopt;
  auto mask = [](uint64_t X, uint16_t Size) -> uint64_t {
    if (Size == 0 || Size >= 8)
      return X;
    return X & ((1ULL << (Size * 8)) - 1);
  };
  switch (Def->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    // Width change preserves the low value an address carries; no mask needed
    // (a zero-extended narrow value already fits, a sign-extended address stays
    // positive in the low bits the rebase consumes).
    return Def->NumInputs >= 1 ? traceValueVA(Def->Inputs[0], Depth + 1)
                               : std::nullopt;
  case NdOp::SUBBYTES:
    if (Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
        Def->Inputs[1].ConstVal == 0)
      if (auto B = traceValueVA(Def->Inputs[0], Depth + 1))
        return mask(*B, Def->Output.Size);
    return std::nullopt;
  case NdOp::INT_ADD:
    if (Def->NumInputs >= 2)
      if (auto A = traceValueVA(Def->Inputs[0], Depth + 1))
        if (auto B = traceValueVA(Def->Inputs[1], Depth + 1))
          return mask(*A + *B, Def->Output.Size);
    return std::nullopt;
  case NdOp::INT_SUB:
    if (Def->NumInputs >= 2)
      if (auto A = traceValueVA(Def->Inputs[0], Depth + 1))
        if (auto B = traceValueVA(Def->Inputs[1], Depth + 1))
          return mask(*A - *B, Def->Output.Size);
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

bool MedLLVMEmitter::frameSlotReloadUsedLocally(const MedVar &StoreAddr) const {
  if (!CurMedFunc)
    return false;
  auto SK = addrSlotKey(StoreAddr);
  if (!SK)
    return false;
  ensureAddrPredCache();
  if (auto It = SlotReloadUsedLocallyCache.find(*SK);
      It != SlotReloadUsedLocallyCache.end())
    return It->second;

  auto isFwd = [](NdOp Op) {
    return Op == NdOp::COPY || Op == NdOp::INT_ZEXT || Op == NdOp::INT_SEXT ||
           Op == NdOp::SUBBYTES;
  };
  auto sameVar = [](const MedVar &A, const MedVar &B) {
    return !A.isConst() && !B.isConst() && A.Kind == B.Kind && A.Id == B.Id &&
           A.SSAVer == B.SSAVer;
  };
  auto compute = [&]() -> bool {
    // The reload values (LOAD outputs of slot SK) plus everything they flow
    // into through pure-forwarding ops (COPY/widen/low-slice) — the values that
    // still carry the reloaded pointer.
    std::vector<MedVar> ReloadVals;
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1)
          if (auto LK = addrSlotKey(Op.Inputs[0]); LK && *LK == *SK)
            ReloadVals.push_back(Op.Output);
    if (ReloadVals.empty())
      return false;
    auto inReloadSet = [&](const MedVar &V) {
      for (const auto &R : ReloadVals)
        if (sameVar(V, R))
          return true;
      return false;
    };
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const auto &Blk : CurMedFunc->Blocks)
        for (const auto &Op : Blk.Ops)
          if (isFwd(Op.Opcode) && Op.NumInputs >= 1 &&
              inReloadSet(Op.Inputs[0]) && !inReloadSet(Op.Output)) {
            if (Op.Opcode == NdOp::SUBBYTES &&
                !(Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                  Op.Inputs[1].ConstVal == 0))
              continue; // a high-slice extract drops the pointer, not forwarding
            ReloadVals.push_back(Op.Output);
            Changed = true;
          }
    }
    // Any NON-forwarding op consuming a reloaded value uses the pointer locally
    // (dereference address, `p++`, `p - base`, comparison) — so it must keep
    // the original VA.  Pure forwarding to the return register, and a RETURN
    // that takes the reload directly as its value operand (x86-64 lowers
    // `return p` to `RETURN <reload>`), are escapes, not local uses.
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops) {
        if (isFwd(Op.Opcode) || Op.Opcode == NdOp::RETURN)
          continue;
        for (int I = 0; I < Op.NumInputs; ++I)
          if (inReloadSet(Op.Inputs[I]))
            return true;
      }
    return false;
  };

  bool Result = compute();
  SlotReloadUsedLocallyCache[*SK] = Result;
  return Result;
}

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
        // as a base; the runtime mask operand carries no base, so descending it
        // is inert.
        for (int I = 0; I < Def->NumInputs; ++I) {
          if (Def->Inputs[I].isConst()) {
            uint64_t C = Def->Inputs[I].ConstVal;
            // A contiguous low-bit mask (C == 2^k - 1, e.g. 0xff/0xffff from a
            // byte/half-word extraction) is a bitmask, not a base address —
            // even when it coincidentally lands inside a small relocatable
            // object's writable segment VA range (a .bss at 0xD8 spans 0xFF).
            // A genuine base-blend constant is a full global VA, essentially
            // never of that form; treating the mask as a base mis-symbolizes an
            // RMW value `(x & 0xff) + load(G)` stored back as a G pointer.
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
        // slot and reloads it to blend into a call-arg / stored pointer. Follow
        // the matching STORE's value to reach the spilled base constant.
        // addrSlotKey only keys on a register-rooted slot, so a load from a
        // global pointer (constant address) yields no key and is left as an
        // index computation. A slot whose ADDRESS escapes is skipped: a callee
        // may have written an already-symbolized pointer there, so the
        // in-function constant base no longer matches the runtime value
        // (double-relocation, the #475 shape).
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
        // INT_MULT/XOR/shift: an index computation, not a base — its constants
        // are scales/masks, so do not search inside it for a base.
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
                                                  llvm::IRBuilder<> &Builder) {
  if (!Img)
    return nullptr;
  // A direct &global (writable or read-only) folds to a constant address.
  uint64_t ConstAddr = 0;
  if (AddrVar.isConst())
    ConstAddr = AddrVar.ConstVal;
  else if (auto Traced = traceSSAConst(AddrVar))
    ConstAddr = *Traced;
  if (ConstAddr != 0) {
    unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
    // A data-pointer argument never points into executable code, so a small
    // integer argument that merely equals a low .text offset (e.g. an unrolled
    // loop counter 3..9 passed by value) must NOT be mistaken for a
    // code-segment "global" and embedded as data — only a non-executable
    // data/rodata segment qualifies.
    const Segment *Seg = Img->getSegmentFor(ConstAddr);
    if (Seg && !Seg->isExecutable() &&
        !isFrameRelativeDisplacement(ConstAddr, AddrBits))
      if (auto *G = tryResolveGlobalData(ConstAddr, /*DataSizeHint=*/0))
        return G;
  }
  // A computed `&global[index]`: same resolver sequence as a LOAD address, with
  // the writable-data path first since the callee may store through the
  // pointer.
  if (auto *P = tryResolveWritableData(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  if (auto *P = tryResolveIndexedGlobalPtr(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  if (auto *P = tryResolveLiteralPoolTable(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  if (auto *P = tryResolveInductionGlobalPtr(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  if (auto *P = tryResolveSelectMergeTable(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  if (auto *P = tryResolveLiteralPoolBase(AddrVar, /*SizeHint=*/0, Builder))
    return P;
  return nullptr;
}

llvm::Constant *MedLLVMEmitter::tryResolveGlobalData(uint64_t Addr,
                                                     uint16_t DataSizeHint) {
  if (!Img || Addr == 0)
    return nullptr;

  auto CacheIt = GlobalDataCache.find(Addr);
  if (CacheIt != GlobalDataCache.end()) {
    // A genuine multi-byte data load must not reuse a C-string interpretation
    // cached from an earlier address-taken (size-0) access: the region is a
    // table whose string view was truncated at its first NUL byte.  Re-resolve.
    if (!(DataSizeHint > 1 && StringDataAddrs.count(Addr)))
      return CacheIt->second;
    StringDataAddrs.erase(Addr);
    GlobalDataCache.erase(Addr);
  }

  // A GOTOFF-folded rodata table base the loader anchored to a specific rodata
  // segment (it folds to `table - min_case*stride`, landing before that
  // segment, often inside .text — so getSegmentFor would pick the wrong
  // segment).  Pin it to the anchored segment's embedded global with a flat —
  // possibly negative — byte offset; the runtime index added to this base
  // brings the effective address back inside the table.
  if (auto AIt = Img->RodataAnchorSeg.find(Addr);
      AIt != Img->RodataAnchorSeg.end()) {
    auto [RunGV, RunStart] = embedRodataRun(AIt->second);
    if (RunGV) {
      int64_t Off = static_cast<int64_t>(Addr) - static_cast<int64_t>(RunStart);
      auto *GEP = llvm::ConstantExpr::getGetElementPtr(
          llvm::Type::getInt8Ty(*Ctx), RunGV,
          llvm::ConstantInt::getSigned(llvm::Type::getInt64Ty(*Ctx), Off));
      GlobalDataCache[Addr] = GEP;
      return GEP;
    }
  }

  // A one-past-the-end pointer into a read-only rodata table (`&tab[N]`, the
  // loop-bound `for (p = tab; p < tab + N; ...)`) lands exactly on the segment
  // end, which getSegmentFor rejects.  When such a table is symbolized to its
  // recompiled run, the bound must be symbolized too — `tab + N` to the run's
  // one-past-end GEP — so a pointer comparison `p < tab + N` stays in the same
  // addressing model as the recompiled pointers; left as the original VA it
  // mixes models and the comparison fires at the wrong element.  Only when no
  // other mapped segment begins here and the table carries no relocated pointer
  // slots (those use the mirror path).
  if (!Img->getSegmentFor(Addr)) {
    for (const auto &S : Img->Segments) {
      if (S.isExecutable() || S.isWritable() || S.Data.empty())
        continue;
      if (Addr != S.VA + S.Data.size() || segHasPtrRelocSlots(&S))
        continue;
      if (auto [RunGV, RunStart] = embedRodataRun(S.VA); RunGV) {
        int64_t Off =
            static_cast<int64_t>(Addr) - static_cast<int64_t>(RunStart);
        auto *GEP = llvm::ConstantExpr::getGetElementPtr(
            llvm::Type::getInt8Ty(*Ctx), RunGV,
            llvm::ConstantInt::getSigned(llvm::Type::getInt64Ty(*Ctx), Off));
        GlobalDataCache[Addr] = GEP;
        return GEP;
      }
    }
  }

  // A one-past-the-end pointer of a WRITABLE mutable segment (`&G[N]`, the walk
  // bound `for (p = G; p < G + N; p++)`): symbolize to the whole-segment
  // writable run's one-past-end GEP so a `p < &G[N]` comparison stays in the
  // recompiled- pointer model the walked pointer (init `&G`, reset `&G`)
  // already uses; left raw it mixes models and the bound fires at the wrong
  // element (the ptrcmp family).  Reuses the same cohesive embedWritableRun the
  // interior accesses use.
  if (!Img->getSegmentFor(Addr)) {
    for (const auto &S : Img->Segments) {
      if (!isMutableDataSeg(&S))
        continue;
      uint64_t End = S.VA + (S.Size ? S.Size : S.Data.size());
      if (Addr != End)
        continue;
      if (auto [RunGV, RunStart] = embedWritableRun(S.VA); RunGV) {
        int64_t Off =
            static_cast<int64_t>(Addr) - static_cast<int64_t>(RunStart);
        auto *GEP = llvm::ConstantExpr::getGetElementPtr(
            llvm::Type::getInt8Ty(*Ctx), RunGV,
            llvm::ConstantInt::getSigned(llvm::Type::getInt64Ty(*Ctx), Off));
        GlobalDataCache[Addr] = GEP;
        return GEP;
      }
    }
  }

  auto *Seg = Img->getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable())
    return nullptr;

  // Writable data (.data / .bss): recreate the WHOLE segment as one cohesive
  // mutable global and GEP into it, so this direct access aliases every other
  // access into the segment — crucially the runtime-indexed loads/stores routed
  // through tryResolveWritableData.  Per-access globals (the previous behavior)
  // diverged from those indexed accesses, splitting one mutable object across
  // several disjoint recompiled globals and dropping stores.  RELRO / pointer-
  // table segments are excluded (isMutableDataSeg) — those keep their existing
  // pointer-table handling.
  if (isMutableDataSeg(Seg)) {
    if (auto [RunGV, RunStart] = embedWritableRun(Seg->VA); RunGV) {
      auto *I64 = llvm::Type::getInt64Ty(*Ctx);
      auto *Off = llvm::ConstantInt::getSigned(
          I64, static_cast<int64_t>(Addr) - static_cast<int64_t>(RunStart));
      auto *GEP = llvm::ConstantExpr::getGetElementPtr(
          llvm::Type::getInt8Ty(*Ctx), RunGV, Off);
      GlobalDataCache[Addr] = GEP;
      return GEP;
    }
    return nullptr;
  }

  // A non-mutable segment carrying relocated pointer slots (a `.data.rel.ro`
  // pointer table loaded whole into a stack array, or a string-pointer table):
  // mirror it through buildCodePtrSegmentGlobal so each pointer slot is
  // relocated to its recompiled target, then GEP to the requested byte offset.
  // Embedding the raw post-link bytes would bake in stale absolute target VAs
  // the loaded pointers dereference into unmapped memory (the `int*[]={&A,&B}`
  // table clang materializes for a local pointer array loaded whole).  If the
  // mirror cannot be built (a code-pointer slot that does not resolve), fall
  // through to the verbatim embed below — no worse than the original VA.
  if (segHasPtrRelocSlots(Seg)) {
    uint64_t SegOut = 0;
    if (llvm::Constant *Tbl = buildCodePtrSegmentGlobal(Seg->VA, SegOut)) {
      // GEP relative to the mirror's run base (SegOut), which may start before
      // Seg->VA when the run extends backward over an adjacent read-only
      // neighbour.
      auto *I64 = llvm::Type::getInt64Ty(*Ctx);
      auto *GEP = llvm::ConstantExpr::getGetElementPtr(
          llvm::Type::getInt8Ty(*Ctx), Tbl,
          llvm::ConstantInt::getSigned(I64, static_cast<int64_t>(Addr) -
                                                static_cast<int64_t>(SegOut)));
      GlobalDataCache[Addr] = GEP;
      return GEP;
    }
  }

  size_t Off = static_cast<size_t>(Addr - Seg->VA);
  if (Off >= Seg->Data.size())
    return nullptr;

  const uint8_t *Start = Seg->Data.data() + Off;
  size_t MaxLen = Seg->Data.size() - Off;

  size_t StrLen = 0;
  bool IsString = false;
  // Only attempt C-string classification for byte-oriented accesses (an
  // address-taken pointer with no known width, or a single-byte load).  A
  // known multi-byte access width (a 2/4/8/16-byte scalar or SIMD load) means
  // this constant is *data*, not a NUL-terminated string: classifying it as a
  // string truncates at the first 0x00 byte and silently corrupts wide loads.
  // e.g. `pxor (%rip), %xmm` reads a 16-byte SIMD vector whose lanes contain
  // zero bytes (a per-lane constant such as {0, k, 2k, 3k}); truncating it to
  // the leading printable bytes yields a 3-byte ".str" and a garbage i128.
  // Such loads must resolve to the raw embedded data path below.
  if (DataSizeHint <= 1) {
    constexpr size_t kMaxStringScanLen = limits::kMaxStringScanLen;
    for (size_t I = 0; I < MaxLen && I < kMaxStringScanLen; ++I) {
      if (Start[I] == 0) {
        IsString = (StrLen >= 2);
        break;
      }
      uint8_t C = Start[I];
      if ((C >= 0x20 && C < 0x7F) || C == '\n' || C == '\r' || C == '\t' ||
          C >= 0x80)
        StrLen++;
      else
        break;
    }
  }

  // An induction-pointer C-string base resolves to the ONE canonical rodata run
  // global (the embedRodataRun path below), not a private `.str` copy, so every
  // base materialization of the walked pointer (`&W`, `&W+1`, …) lands in the
  // same global at consistent offsets and the merged pointer is uniform.  A
  // lone-string access (not an induction base) keeps the compact `.str` copy.
  // A compact `.str` copy is only correct when Addr is the START of a
  // NUL-terminated string (the segment start, or the byte before it is a
  // terminator).  An INTERIOR string address (`&W[k]`, k>0, mid-string) walked
  // backward needs the whole string, not a suffix copy starting at the interior
  // byte; route it to the contiguous run global below so the full layout is
  // preserved (i386/ARM32 PIC interior string pointer, #490).
  bool AtStringStart = (Off == 0) || (Seg->Data[Off - 1] == 0);
  // A sized data symbol (a const array/table) extending beyond the would-be
  // string is data, not a NUL-terminated string: its bytes merely begin with a
  // printable run plus an embedded 0 (e.g. `const unsigned tab[]` whose first
  // element's low bytes are ASCII).  A truncated `.str` copy drops the rest of
  // the table, so a pointer walked across it reads past the copy — i386/ARM32
  // -O0 spills the induction pointer to the stack, hiding the walk from
  // isInductionRodataStringBase, so honor the symbol's real size here too.
  bool SizedObjectBeyondString = Img->dataObjectSizeAt(Addr) > StrLen + 1;
  if (IsString && StrLen > 0 && AtStringStart && !SizedObjectBeyondString &&
      !isInductionRodataStringBase(Addr)) {
    std::string StrVal(reinterpret_cast<const char *>(Start), StrLen);
    auto *StrConst = llvm::ConstantDataArray::getString(*Ctx, StrVal, true);
    // In mergeable (sharded) mode the name must be a pure function of the
    // address (not a per-emitter counter) and the linkage linkonce_odr, so the
    // same string materialized by two shards collapses to one symbol; the
    // content at a given VA is identical, so the merge is ODR-safe.  Standalone
    // mode keeps the private, counter-named form (byte-identical to before).
    std::string StrName =
        MergeableGlobals
            ? ((kNdDataPrefix + llvm::utohexstr(Addr)).str() + ".str")
            : (".str." + std::to_string(GlobalStrCounter++));
    auto *GV = new llvm::GlobalVariable(
        *Mod, StrConst->getType(), true,
        MergeableGlobals ? llvm::GlobalValue::LinkOnceODRLinkage
                         : llvm::GlobalValue::PrivateLinkage,
        StrConst, StrName);
    GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    markSharedLocal(GV);
    GV->setAlignment(llvm::Align(1));

    auto *Zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    llvm::Constant *Indices[] = {Zero, Zero};
    auto *GEP = llvm::ConstantExpr::getInBoundsGetElementPtr(
        StrConst->getType(), GV, Indices);
    GlobalDataCache[Addr] = GEP;
    StringDataAddrs.insert(Addr);
    return GEP;
  }

  std::string SymName = (kNdDataPrefix + llvm::utohexstr(Addr)).str();

  uint16_t TypeSize = 0;
  if (const auto *Sym = Img->findSymbolAt(Addr)) {
    if (Sym->Size > 0 && Sym->Size <= 8 && llvm::isPowerOf2_64(Sym->Size))
      TypeSize = static_cast<uint16_t>(Sym->Size);
  }
  if (TypeSize == 0 && DataSizeHint > 0 && DataSizeHint <= 8 &&
      llvm::isPowerOf2_64(DataSizeHint))
    TypeSize = DataSizeHint;
  if (TypeSize == 0) {
    auto HintIt = DataSizeHints.find(Addr);
    if (HintIt != DataSizeHints.end() && HintIt->second > 0 &&
        HintIt->second <= 8 && llvm::isPowerOf2_64(HintIt->second))
      TypeSize = HintIt->second;
  }

  bool IsConst = !Seg->isWritable();

  // Read-only, non-executable data (.rodata): represent the WHOLE segment run
  // as one embedded global and GEP to the byte offset, for ANY offset and
  // access width — not only MaxLen>=4 multi-byte loads.  An interior pointer
  // near the segment end (`&bc[last]`, MaxLen<4) that is later walked with a
  // negative stride must stay anchored to the contiguous run global; a
  // standalone per-address copy isolates it so the backward walk reads unmapped
  // memory before it (i386/ARM32 PIC revwalk).  embedRodataRun returns null
  // when the run exceeds the embed cap, falling through to the per-constant
  // paths below.
  if (IsConst && !Seg->isExecutable()) {
    if (auto [RunGV, RunStart] = embedRodataRun(Seg->VA); RunGV) {
      auto *I64 = llvm::Type::getInt64Ty(*Ctx);
      auto *Zero = llvm::ConstantInt::get(I64, 0);
      llvm::Constant *Indices[] = {
          Zero, llvm::ConstantInt::get(I64, Addr - RunStart)};
      auto *GEP = llvm::ConstantExpr::getInBoundsGetElementPtr(
          RunGV->getValueType(), RunGV, Indices);
      GlobalDataCache[Addr] = GEP;
      return GEP;
    }
  }

  // A `.text`-embedded literal pool (ARM32 NEON constant pool, x86/i386 PIC
  // anchor) lives in an executable segment past the code (Off>0).  Embed the
  // segment once and GEP to the byte offset — the executable-segment dual of
  // the rodata run above.  Without it each `vldr`/`adr` load fell through to
  // the per-constant `[Addr, segment_end]` copy below, duplicating the pool
  // O(N) times (one overlapping global per load → tens of KB of `.rodata`).
  if (IsConst && Seg->isExecutable() && Off > 0) {
    if (auto [RunGV, RunStart] = embedExecSegmentRun(Seg); RunGV) {
      auto *I64 = llvm::Type::getInt64Ty(*Ctx);
      auto *Zero = llvm::ConstantInt::get(I64, 0);
      llvm::Constant *Indices[] = {
          Zero, llvm::ConstantInt::get(I64, Addr - RunStart)};
      auto *GEP = llvm::ConstantExpr::getInBoundsGetElementPtr(
          RunGV->getValueType(), RunGV, Indices);
      GlobalDataCache[Addr] = GEP;
      return GEP;
    }
  }

  if (IsConst && MaxLen >= 4 && (Off > 0 || !Seg->isExecutable())) {
    auto *I64 = llvm::Type::getInt64Ty(*Ctx);
    auto *Zero = llvm::ConstantInt::get(I64, 0);
    // For a pure read-only data segment (.rodata, non-executable), embed the
    // WHOLE segment exactly once and GEP into it at `Off`.  Previously each
    // constant embedded its own `[Addr, segment_end]` copy, duplicating the
    // segment O(N) times — e.g. 96 references into a ~1.2 KB rodata produced a
    // ~67 KB recompiled .rodata that overflowed the emulator's mapping and
    // faulted (UC_ERR_READ_UNMAPPED).
    size_t SegLen =
        std::min(Seg->Data.size(), size_t(limits::kMaxEmbeddedDataLen));
    if (Off < SegLen) {
      // Embed the contiguous run of read-only data segments (preserving
      // relative layout) so a PC-relative reference that crosses a section
      // boundary — a switch-to-string `.rodata` offset table pointing into
      // `.rodata.str1.1` — stays valid.  A lone rodata segment yields a run of
      // just itself, so this is byte-identical to the prior single-segment
      // embed for the common case.
      if (auto [RunGV, RunStart] = embedRodataRun(Seg->VA); RunGV) {
        uint64_t RunOff = Addr - RunStart;
        llvm::Constant *Indices[] = {Zero, llvm::ConstantInt::get(I64, RunOff)};
        auto *GEP = llvm::ConstantExpr::getInBoundsGetElementPtr(
            RunGV->getValueType(), RunGV, Indices);
        GlobalDataCache[Addr] = GEP;
        return GEP;
      }
    }
    // Executable-segment literal pools, or offsets beyond the embed cap: keep
    // the per-constant embedded copy starting at `Addr`.
    size_t EmbedLen = std::min(MaxLen, size_t(limits::kMaxEmbeddedDataLen));
    auto *ArrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), EmbedLen);
    auto *Init = llvm::ConstantDataArray::get(
        *Ctx, llvm::ArrayRef<uint8_t>(Start, EmbedLen));
    auto *GV = new llvm::GlobalVariable(*Mod, ArrTy, true, dataLinkage(), Init,
                                        SymName + section_names::elf::Rodata);
    GV->setAlignment(llvm::Align(4));
    markSharedLocal(GV);
    llvm::Constant *Indices[] = {Zero, Zero};
    auto *GEP =
        llvm::ConstantExpr::getInBoundsGetElementPtr(ArrTy, GV, Indices);
    GlobalDataCache[Addr] = GEP;
    return GEP;
  }

  // The data IS present in the segment (Off < Data.size(), checked above), but
  // the embedded-array path was skipped — most commonly a small constant (e.g.
  // an i16) near the END of the rodata segment, where MaxLen < 4 fails the
  // `MaxLen >= 4` guard.  Previously this fell through to an *external*
  // (data-less) declaration, which the backend lowers to a reference to an
  // undefined symbol → an unmapped address → UC_ERR_READ_UNMAPPED at runtime
  // (e.g. clang's `*2.0f` / FP constant tail in VectorAlgo8).  Emit the actual
  // bytes as an internal constant so the value travels with the recompiled obj.
  {
    size_t NBytes = (TypeSize > 0)
                        ? std::min(static_cast<size_t>(TypeSize), MaxLen)
                        : MaxLen;
    if (NBytes >= 1) {
      auto *ArrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), NBytes);
      auto *Init = llvm::ConstantDataArray::get(
          *Ctx, llvm::ArrayRef<uint8_t>(Start, NBytes));
      // NBytes depends on the access width (a symbol/size hint), so the SAME
      // address may embed a different length in two shards.  In mergeable mode
      // fold the length into the name so two globals only ever share a name
      // when they share content (ODR-safe); standalone mode keeps the plain
      // per-address name (one emitter, one length).
      std::string GName = MergeableGlobals
                              ? (SymName + "." + llvm::utostr(NBytes) +
                                 section_names::elf::Rodata)
                              : (SymName + section_names::elf::Rodata);
      auto *GV = new llvm::GlobalVariable(*Mod, ArrTy, IsConst, dataLinkage(),
                                          Init, GName);
      unsigned Al = (TypeSize > 0 && TypeSize <= 16) ? TypeSize : 1;
      GV->setAlignment(llvm::Align(Al));
      markSharedLocal(GV);
      auto *Zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
      llvm::Constant *Indices[] = {Zero, Zero};
      auto *GEP =
          llvm::ConstantExpr::getInBoundsGetElementPtr(ArrTy, GV, Indices);
      GlobalDataCache[Addr] = GEP;
      return GEP;
    }
  }

  llvm::Type *DataTy =
      TypeSize > 0 ? sizeToType(TypeSize) : llvm::Type::getInt8Ty(*Ctx);

  auto *GV = new llvm::GlobalVariable(*Mod, DataTy, IsConst,
                                      llvm::GlobalValue::ExternalLinkage,
                                      nullptr, SymName);
  GV->setDSOLocal(true);
  GlobalDataCache[Addr] = GV;
  return GV;
}

} // namespace neverd
