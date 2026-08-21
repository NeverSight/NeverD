//===- MedLLVMSegmentEmbed.cpp - Segment embedding as globals --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Segment embedding for MedLLVMEmitter: rebuilding a read-only,
/// executable or writable segment run as one cohesive LLVM global, and
/// the segment classification predicates (mutable data, relocated
/// pointer slots, read-only-after-relocation) that decide which model a
/// segment takes.  The resolvers that redirect an address into these
/// globals live in MedLLVMWritableData.cpp and MedLLVMGlobalData.cpp.
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
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

static bool collectRodataRun(const BinaryImage *Img, uint64_t SegVA,
                             std::vector<const Segment *> &Run,
                             uint64_t &RunStart, uint64_t &RunEnd) {
  Run.clear();
  RunStart = 0;
  RunEnd = 0;
  if (!Img)
    return false;

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
    return false;

  // Extend over neighbours separated only by a small alignment gap; a larger
  // gap means a distinct region that must not be merged. Reject overflowing
  // segment ranges rather than letting their wrapped end look adjacent.
  auto endOf = [](const Segment *S, uint64_t &End) {
    if (S->Data.size() > std::numeric_limits<uint64_t>::max() - S->VA)
      return false;
    End = S->VA + static_cast<uint64_t>(S->Data.size());
    return true;
  };
  auto adjacent = [&](const Segment *Prev, const Segment *Next) {
    uint64_t PrevEnd = 0;
    return endOf(Prev, PrevEnd) && Next->VA >= PrevEnd &&
           Next->VA - PrevEnd <= 16;
  };
  size_t Lo = Idx, Hi = Idx;
  while (Lo > 0 && adjacent(Ro[Lo - 1], Ro[Lo]))
    --Lo;
  while (Hi + 1 < Ro.size() && adjacent(Ro[Hi], Ro[Hi + 1]))
    ++Hi;

  uint64_t End = 0;
  if (!endOf(Ro[Hi], End) || End <= Ro[Lo]->VA ||
      End - Ro[Lo]->VA > limits::kMaxSingleGlobalEmbedLen)
    return false;
  RunStart = Ro[Lo]->VA;
  RunEnd = End;
  Run.assign(Ro.begin() + static_cast<std::ptrdiff_t>(Lo),
             Ro.begin() + static_cast<std::ptrdiff_t>(Hi + 1));
  return true;
}

bool MedLLVMEmitter::rodataRunBounds(uint64_t SegVA, uint64_t &RunStart,
                                     uint64_t &RunEnd) const {
  std::vector<const Segment *> Run;
  return collectRodataRun(Img, SegVA, Run, RunStart, RunEnd);
}

std::pair<llvm::GlobalVariable *, uint64_t>
MedLLVMEmitter::embedRodataRun(uint64_t SegVA) {
  uint64_t RunStart = 0, RunEnd = 0;
  std::vector<const Segment *> Run;
  if (!collectRodataRun(Img, SegVA, Run, RunStart, RunEnd))
    return {nullptr, 0};
  uint64_t RunLen64 = RunEnd - RunStart;
  size_t RunLen = static_cast<size_t>(RunLen64);
  if (auto It = SegmentDataGlobals.find(RunStart);
      It != SegmentDataGlobals.end())
    return {It->second, RunStart};
  std::vector<uint8_t> Buf(RunLen, 0);
  for (const Segment *S : Run)
    std::memcpy(Buf.data() + static_cast<size_t>(S->VA - RunStart),
                S->Data.data(), S->Data.size());
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
  // Some formats retain writable loader flags on relocated pointer tables that
  // become immutable after fixups; pointer-table machinery owns those ranges.
  if (section_names::isReadOnlyAfterRelocSectionName(S->Name))
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
  return !segHasPtrRelocSlots(S) && !overlaps(Img->RelCodeRelocSlots);
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
  auto overlapsImports = [&]() {
    for (const auto &[VA, Name] : Img->ImportPtrSlots) {
      (void)Name;
      if (S->contains(VA))
        return true;
    }
    for (const auto &[VA, Binding] : Img->DyldBindSlots) {
      (void)Binding;
      if (S->contains(VA))
        return true;
    }
    return false;
  };
  return overlaps(Img->CodePtrRelocSlots) || overlaps(Img->DataPtrRelocSlots) ||
         overlapsImports();
}

bool MedLLVMEmitter::isReadOnlyAfterReloc(const Segment *S) const {
  return S && S->isReadable() && !S->isExecutable() && !S->Data.empty() &&
         (!S->isWritable() ||
          section_names::isReadOnlyAfterRelocSectionName(S->Name));
}

std::pair<llvm::Constant *, uint64_t>
MedLLVMEmitter::materializeReadOnlyDataRun(const Segment *Seg) {
  if (!Seg || !Seg->isReadable() || Seg->Data.empty())
    return {nullptr, 0};
  if (Seg->isExecutable()) {
    auto [Run, RunStart] = embedExecSegmentRun(Seg);
    return {Run, RunStart};
  }
  if (segHasPtrRelocSlots(Seg)) {
    uint64_t RunStart = 0;
    // A failed relocation mirror is authoritative. Falling through to raw
    // bytes would preserve the exact stale pointer the mirror rejected.
    return {buildCodePtrSegmentGlobal(Seg->VA, RunStart), RunStart};
  }
  auto [Run, RunStart] = embedRodataRun(Seg->VA);
  return {Run, RunStart};
}

bool MedLLVMEmitter::hasObjectDataProvenance(uint64_t VA) const {
  return Img && Img->hasObjectDataProvenance(VA);
}

bool MedLLVMEmitter::isMaterializableReadOnlyDataAddress(uint64_t VA) const {
  if (!Img)
    return false;
  const Segment *Seg = Img->getSegmentFor(VA);
  if (!Seg || !Seg->isReadable() || Seg->Data.empty() ||
      !hasObjectDataProvenance(VA) || VA < Seg->VA ||
      VA - Seg->VA >= Seg->Data.size())
    return false;

  // A format-native section is finer provenance than its load segment.  In a
  // linked ELF, lld can place `.data.rel.ro` in an ordinary writable PT_LOAD
  // together with unrelated sections; the segment name/permissions therefore
  // cannot express the post-relocation immutability of this exact address.
  // Mach-O has the inverse coarse-layout case: read-only data can share the
  // executable __TEXT segment.  Accept exact readable, non-executable section
  // evidence when the enclosing segment is too coarse, including writable
  // RELRO sections that become immutable after the dynamic loader fixes them.
  bool HasReadOnlySectionEvidence = false;
  if (const Section *Sec = Img->getSectionFor(VA))
    HasReadOnlySectionEvidence =
        Sec->isReadable() &&
        (Img->isMachO() ? !Img->isCodeAddress(VA) : !Sec->isExecutable()) &&
        (!Sec->isWritable() ||
         section_names::isReadOnlyAfterRelocSectionName(Sec->Name) ||
         section_names::isReadOnlyAfterRelocSectionName(Sec->SegmentName));
  if (Seg->isWritable() && !isReadOnlyAfterReloc(Seg) &&
      !HasReadOnlySectionEvidence)
    return false;
  return canResolveGlobalDataConstant(VA);
}

bool MedLLVMEmitter::isMaterializableReadOnlyDataAddress(
    uint64_t VA, uint64_t OwnerVA) const {
  if (OwnerVA == InvalidVA)
    return isMaterializableReadOnlyDataAddress(VA);
  if (!Img)
    return false;

  const Segment *OwnerSeg = Img->getSegmentFor(OwnerVA);
  if (!OwnerSeg || !OwnerSeg->isReadable() || OwnerSeg->Data.empty())
    return false;
  const Section *OwnerSec = Img->getSectionFor(OwnerVA);
  const uint64_t Begin = OwnerSec ? OwnerSec->VA : OwnerSeg->VA;
  const uint64_t Size = OwnerSec ? OwnerSec->Size : OwnerSeg->Size;
  if (Size > InvalidVA - Begin || VA < Begin || VA > Begin + Size)
    return false;

  bool HasReadOnlySectionEvidence = false;
  if (OwnerSec)
    HasReadOnlySectionEvidence =
        OwnerSec->isReadable() &&
        (Img->isMachO() ? !Img->isCodeAddress(OwnerVA)
                        : !OwnerSec->isExecutable()) &&
        (!OwnerSec->isWritable() ||
         section_names::isReadOnlyAfterRelocSectionName(OwnerSec->Name) ||
         section_names::isReadOnlyAfterRelocSectionName(OwnerSec->SegmentName));
  if (OwnerSeg->isWritable() && !isReadOnlyAfterReloc(OwnerSeg) &&
      !HasReadOnlySectionEvidence)
    return false;
  if (OwnerSeg->isExecutable() && Img->hasExecutableCodeOwnerAt(OwnerVA) &&
      !HasReadOnlySectionEvidence)
    return false;
  return true;
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
  if (!Img)
    return false;
  const Segment *Seg = Img->getSegmentFor(VA);
  // Keep this ownership predicate in lockstep with
  // buildCodePtrSegmentGlobal(): both immutable relocation tables and mutable
  // data segments with proven pointer slots are represented by a pointer
  // mirror.  Restricting this check to segment-level RELRO names loses that
  // fact for linked ELF PT_LOADs whose exact `.data.rel.ro` section is nested
  // inside a coarsely writable segment.
  if (!Seg || Seg->Data.empty() || Seg->isExecutable() ||
      !segHasPtrRelocSlots(Seg))
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
      if (!JT.HasBaseAddr || JT.EntrySize == 0 || JT.Targets.empty())
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
  auto importInRun = [&]() {
    for (const auto &[S, Name] : Img->ImportPtrSlots) {
      (void)Name;
      if (S >= RunStart && S < RunEnd)
        return true;
    }
    for (const auto &[S, Binding] : Img->DyldBindSlots) {
      (void)Binding;
      if (S >= RunStart && S < RunEnd)
        return true;
    }
    return false;
  };
  return inRun(Img->CodePtrRelocSlots, /*ExcludeJumpTables=*/true) ||
         inRun(Img->DataPtrRelocSlots, /*ExcludeJumpTables=*/false) ||
         importInRun();
}

std::pair<llvm::GlobalVariable *, uint64_t>
MedLLVMEmitter::embedWritableRun(uint64_t SegVA) {
  uint64_t RunStart = 0, RunEnd = 0;
  if (!writableRunBounds(SegVA, RunStart, RunEnd))
    return {nullptr, 0};
  const Segment *Seg = Img->getSegmentFor(SegVA);
  uint64_t RunLen64 = RunEnd - RunStart;
  // One cohesive mutable global per writable segment, GEP'd into for every
  // access — the writable counterpart of embedRodataRun.  Bounded by the
  // single-global cap (not the per-access kMaxEmbeddedDataLen): a single whole-
  // segment global is the smallest possible form, so a large .bss/.data array
  // (e.g. `static unsigned G[2048]`, 8 KiB) is embedded once rather than left
  // as a bare inttoptr(VA) the relinked object never maps ->
  // WRITE/READ_UNMAPPED.
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

bool MedLLVMEmitter::writableRunBounds(uint64_t SegVA, uint64_t &RunStart,
                                       uint64_t &RunEnd) const {
  RunStart = 0;
  RunEnd = 0;
  if (!Img)
    return false;
  const Segment *Seg = Img->getSegmentFor(SegVA);
  if (!isMutableDataSeg(Seg))
    return false;
  uint64_t RunLen = Seg->Size ? Seg->Size : Seg->Data.size();
  if (RunLen == 0 || RunLen > limits::kMaxSingleGlobalEmbedLen ||
      RunLen > std::numeric_limits<uint64_t>::max() - Seg->VA)
    return false;
  RunStart = Seg->VA;
  RunEnd = Seg->VA + RunLen;
  return true;
}

} // namespace neverd
