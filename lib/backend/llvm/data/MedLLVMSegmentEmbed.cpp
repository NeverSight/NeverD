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
#include "neverd/backend/LLVMOriginalImageStorage.h"
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
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace neverd {

bool MedLLVMEmitter::usesOriginalImageStorage() const {
  return OriginalImageStorageForSource && Img && !Img->IsRelocatable;
}

std::pair<llvm::GlobalVariable *, uint64_t>
MedLLVMEmitter::originalImageStorage(uint64_t OwnerVA) {
  if (!OriginalImageStorageForSource || !Img || Img->IsRelocatable)
    return {nullptr, 0};
  const Segment *Owner = Img->getSegmentFor(OwnerVA);
  if (!Owner || !Owner->isReadable())
    return {nullptr, 0};
  // A linked image preserves the distances between its mapped regions too.
  // A single external span keeps direct, indexed and one-past addresses in
  // the same coordinate system, without allocating or inspecting its bytes.
  uint64_t Begin = InvalidVA, End = 0;
  for (const Segment &Seg : Img->Segments) {
    const uint64_t Size = Seg.Size ? Seg.Size : Seg.Data.size();
    if (!Size)
      continue;
    if (Size > InvalidVA - Seg.VA)
      return {nullptr, 0};
    Begin = std::min(Begin, Seg.VA);
    End = std::max(End, Seg.VA + Size);
  }
  if (Begin >= End)
    return {nullptr, 0};
  const std::string Name =
      (kNdDataPrefix + llvm::utohexstr(Begin)).str() + ".image";
  auto *Global = Mod->getNamedGlobal(Name);
  if (!Global) {
    auto *Type = llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), End - Begin);
    Global = new llvm::GlobalVariable(
        *Mod, Type, false, llvm::GlobalValue::ExternalLinkage, nullptr, Name);
    llvm_original_image::mark(*Global);
  }
  return {Global, Begin};
}

// Whether an address-ordered slot set or map holds a slot inside \p S. Only
// the first slot at or above the segment base can be inside it.
template <typename Slots>
static bool segmentHoldsSlot(const Segment &S, const Slots &Ordered) {
  const auto It = Ordered.lower_bound(S.VA);
  if (It == Ordered.end())
    return false;
  if constexpr (std::is_same_v<typename Slots::key_type,
                               typename Slots::value_type>)
    return S.contains(*It);
  else
    return S.contains(It->first);
}

/// Whether read-only \p Next follows \p Prev across only alignment padding
/// (kMaxRodataRunPaddingBytes) that no other segment of \p Img maps, so one
/// embedded run may hold both.  Overflowing ranges never join.
static bool separatedOnlyByPadding(const BinaryImage &Img, const Segment *Prev,
                                   const Segment *Next) {
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  if (Prev->Data.size() > Max - Prev->VA)
    return false;
  const uint64_t PrevEnd = Prev->VA + static_cast<uint64_t>(Prev->Data.size());
  if (Next->VA < PrevEnd ||
      Next->VA - PrevEnd > limits::kMaxRodataRunPaddingBytes)
    return false;
  for (const Segment &Other : Img.Segments) {
    const uint64_t Extent = std::max<uint64_t>(
        Other.Size, static_cast<uint64_t>(Other.Data.size()));
    if (&Other == Prev || &Other == Next || Extent == 0)
      continue;
    const uint64_t OtherEnd = Extent > Max - Other.VA ? Max : Other.VA + Extent;
    if (Other.VA < Next->VA && OtherEnd > PrevEnd)
      return false;
  }
  return true;
}

static bool collectRodataRun(const BinaryImage *Img, uint64_t SegVA,
                             std::vector<const Segment *> &Run,
                             uint64_t &RunStart, uint64_t &RunEnd) {
  Run.clear();
  RunStart = 0;
  RunEnd = 0;
  if (!Img)
    return false;

  // Collect read-only, non-executable segments that carry data, sorted by VA.
  // Mapped headers are `__ImageBase`, never part of a copied run.
  std::vector<const Segment *> Ro;
  for (const auto &S : Img->Segments)
    if (S.isReadable() && !S.isWritable() && !S.isExecutable() &&
        !S.Data.empty() && !Img->imageBaseOffset(S.VA))
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

  // Extend over neighbours separated only by alignment padding; a larger gap,
  // or another segment in it, means a distinct region that must not be merged.
  // Reject overflowing segment ranges rather than letting their wrapped end
  // look adjacent.
  auto endOf = [](const Segment *S, uint64_t &End) {
    if (S->Data.size() > std::numeric_limits<uint64_t>::max() - S->VA)
      return false;
    End = S->VA + static_cast<uint64_t>(S->Data.size());
    return true;
  };
  auto adjacent = [&](const Segment *Prev, const Segment *Next) {
    return separatedOnlyByPadding(*Img, Prev, Next);
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
  if (auto Storage = originalImageStorage(SegVA); Storage.first)
    return Storage;
  uint64_t RunStart = 0, RunEnd = 0;
  std::vector<const Segment *> Run;
  if (!collectRodataRun(Img, SegVA, Run, RunStart, RunEnd))
    return {nullptr, 0};
  if (Img->hasMachOAuthenticatedPointerIn(RunStart, RunEnd - RunStart)) {
    uint64_t Base = 0;
    auto *Global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
        buildCodePtrSegmentGlobal(SegVA, Base));
    return {Global, Base};
  }
  if (auto It = SegmentDataGlobals.find(RunStart);
      It != SegmentDataGlobals.end()) {
    const auto Base = SegmentDataBases.find(RunStart);
    return {It->second,
            Base == SegmentDataBases.end() ? RunStart : Base->second};
  }
  // A section in the run can need more alignment than the run's start has
  // (a 16-byte constant pool after a 4-byte one).  The copy then starts below
  // the run, so each address keeps its residue and an aligned access to it
  // (`addps xmm, m128`) does not fault in the rebuilt object.
  uint64_t Alignment = 1;
  for (const Section &Sec : Img->Sections)
    if (Sec.VA >= RunStart && Sec.VA < RunEnd && Sec.Alignment > Alignment)
      Alignment = std::min<uint64_t>(llvm::PowerOf2Ceil(Sec.Alignment),
                                     limits::kMaxEmbeddedDataAlignment);
  const uint64_t Base = RunStart - RunStart % Alignment;
  size_t RunLen = static_cast<size_t>(RunEnd - Base);
  std::vector<uint8_t> Buf(RunLen, 0);
  for (const Segment *S : Run)
    std::memcpy(Buf.data() + static_cast<size_t>(S->VA - Base), S->Data.data(),
                S->Data.size());
  auto *ArrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), RunLen);
  auto *Init = llvm::ConstantDataArray::get(*Ctx, llvm::ArrayRef<uint8_t>(Buf));
  auto *GV = new llvm::GlobalVariable(
      *Mod, ArrTy, /*isConstant=*/true, dataLinkage(), Init,
      (kNdDataPrefix + llvm::utohexstr(Base)).str() +
          section_names::elf::Rodata);
  GV->setAlignment(llvm::Align(std::max<uint64_t>(Alignment, 16)));
  markSharedLocal(GV);
  SegmentDataGlobals[RunStart] = GV;
  if (Base != RunStart)
    SegmentDataBases[RunStart] = Base;
  return {GV, Base};
}

std::pair<llvm::GlobalVariable *, uint64_t>
MedLLVMEmitter::embedExecSegmentRun(const Segment *Seg) {
  if (Seg)
    if (auto Storage = originalImageStorage(Seg->VA); Storage.first)
      return Storage;
  if (!Seg || Seg->Data.empty() ||
      Seg->Data.size() > limits::kMaxSingleGlobalEmbedLen)
    return {nullptr, 0};
  uint64_t RunStart = Seg->VA;
  if (auto It = SegmentDataGlobals.find(RunStart);
      It != SegmentDataGlobals.end())
    return {It->second, RunStart};
  // Code holds a relocated pointer slot only inside a data object the image
  // places among it, such as GNU ld's __CTOR_LIST__: the segment mirrors as
  // the pointer table it then is, or not at all, rather than keep a stale
  // address in raw bytes.
  if (segHasLoaderPtrRelocSlots(Seg)) {
    auto *Table = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
        buildCodePtrSegmentGlobal(Seg->VA, RunStart));
    if (!Table)
      return {nullptr, 0};
    SegmentDataGlobals[RunStart] = Table;
    return {Table, RunStart};
  }
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
  if (Img ? Img->readOnlyAfterRelocations(S->VA, S->Size)
          : S->ReadOnlyAfterRelocations)
    return false;
  if (!Img)
    return true;
  // A segment carrying any relocated pointer slot is likewise a pointer table,
  // not raw mutable scalar/array data.
  return !segHasPtrRelocSlots(S) &&
         !segmentHoldsSlot(*S, Img->RelCodeRelocSlots);
}

bool MedLLVMEmitter::segHasPtrRelocSlots(const Segment *S) const {
  if (!S || !Img)
    return false;
  auto overlapsRuntimeCallableSlots = [&]() {
    for (const RuntimeCallablePointerSlot &Slot :
         Img->RuntimeCallablePointerSlots)
      if (S->contains(Slot.SlotVA))
        return true;
    return false;
  };
  return segmentHoldsSlot(*S, Img->CodePtrRelocSlots) ||
         segmentHoldsSlot(*S, Img->DataPtrRelocSlots) ||
         Img->hasMachOAuthenticatedPointerIn(S->VA, S->Size) ||
         segmentHoldsSlot(*S, EffectiveImportStorageSlots) ||
         segmentHoldsSlot(*S, ConflictingImportStorageSlots) ||
         segmentHoldsSlot(*S, ExternalFunctionPointerSlots) ||
         overlapsRuntimeCallableSlots();
}

bool MedLLVMEmitter::segHasLoaderPtrRelocSlots(const Segment *S) const {
  if (!S || !Img)
    return false;
  auto holds = [&](const std::set<uint64_t> &Slots) {
    auto It = Slots.lower_bound(S->VA);
    return It != Slots.end() && S->contains(*It);
  };
  return holds(Img->CodePtrRelocSlots) || holds(Img->DataPtrRelocSlots) ||
         Img->hasMachOAuthenticatedPointerIn(S->VA, S->Size);
}

llvm::Constant *MedLLVMEmitter::tryResolveDirectDataBytes(uint64_t Addr,
                                                          uint16_t Size,
                                                          uint64_t OwnerVA) {
  // Shards must choose the same storage for a segment even if a different
  // shard takes its address. Keep their existing whole-run contract; local
  // mirrors below can instead rebind all earlier uses when demand expands.
  // A header byte is never copied: the headers are `__ImageBase`.
  if (!Img || !Size || MergeableGlobals ||
      (OriginalImageStorageForSource && !Img->IsRelocatable) ||
      (Addr == 0 && OwnerVA == InvalidVA) || Img->imageBaseOffset(Addr))
    return nullptr;
  ensureImportStorageSnapshot();
  const Segment *Seg = Img->getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable() || Seg->isExecutable() ||
      !segHasPtrRelocSlots(Seg) || Seg->Data.size() > InvalidVA - Seg->VA ||
      Addr - Seg->VA >= Seg->Data.size() ||
      Size > Seg->Data.size() - (Addr - Seg->VA))
    return nullptr;

  if (OwnerVA != InvalidVA) {
    const Segment *Owner = Img->getSegmentFor(OwnerVA);
    const Section *Sec = Img->getSectionFor(OwnerVA);
    const uint64_t Begin = Sec ? Sec->VA : Owner ? Owner->VA : 0;
    const uint64_t Extent = Sec ? Sec->Size : Owner ? Owner->Size : 0;
    if (Extent > InvalidVA - Begin || Owner != Seg ||
        Img->getSectionFor(Addr) != Sec || Addr < Begin ||
        Addr - Begin >= Extent || Size > Extent - (Addr - Begin))
      return nullptr;
  }
  const Section *Sec = Img->getSectionFor(Addr);
  if (Img->isMachO() && Sec &&
      (llvm::StringRef(Sec->Name).starts_with(
           section_names::macho::ObjCMetadataPrefix) ||
       Sec->Name == section_names::macho::CfString))
    return nullptr;

  uint64_t MirrorBegin = 0, MirrorEnd = 0;
  readOnlyAfterRelocRun(Seg, MirrorBegin, MirrorEnd);
  if (CodePtrTableGlobals.count(MirrorBegin) ||
      Img->hasMachOAuthenticatedPointerIn(MirrorBegin,
                                          MirrorEnd - MirrorBegin) ||
      preservesRegistrationImageStorage(MirrorBegin, MirrorEnd))
    return nullptr;
  // These ranges belong to live original-image storage, not a private copy of
  // the file's initial bytes, even when the direct access misses their slots.
  for (const RuntimeCallablePointerSlot &Slot :
       Img->RuntimeCallablePointerSlots)
    if (Slot.SlotVA >= MirrorBegin && Slot.SlotVA < MirrorEnd)
      return nullptr;
  if (TargetArch == Arch::X86 && TargetFormat == BinaryFormat::COFF)
    for (const auto &EH : Img->ExceptionMetadata.Functions)
      if (EH.Registration && EH.Registration->ScopeTableVA >= MirrorBegin &&
          EH.Registration->ScopeTableVA < MirrorEnd)
        return nullptr;

  uint64_t Begin = Seg->VA, End = Seg->VA + Seg->Data.size();
  // Use the maximal interval between relocation fields, rather than one
  // allocation per load width, so overlapping direct loads/stores stay aliased.
  // Include the preceding slot: a load can start inside its last few bytes.
  auto restrictToGap = [&](const auto &Slots, unsigned Width) {
    auto address = [](const auto &Entry) -> uint64_t {
      if constexpr (requires { Entry.first; })
        return Entry.first;
      else
        return Entry;
    };
    auto Next = Slots.upper_bound(Addr);
    if (Next != Slots.end())
      End = std::min(End, address(*Next));
    if (Next != Slots.begin()) {
      const uint64_t Previous = address(*std::prev(Next));
      if (Addr - Previous < Width)
        return false;
      Begin = std::max(Begin, Previous + Width);
    }
    return Size <= End - Addr;
  };
  const unsigned PtrSize = Img->getPointerSize();
  if (!PtrSize || !restrictToGap(Img->CodePtrRelocSlots, PtrSize) ||
      !restrictToGap(Img->DataPtrRelocSlots, PtrSize) ||
      !restrictToGap(EffectiveImportStorageSlots, PtrSize) ||
      !restrictToGap(ConflictingImportStorageSlots, PtrSize) ||
      !restrictToGap(ExternalFunctionPointerSlots, PtrSize) ||
      !restrictToGap(DataSymbolBindings, PtrSize) ||
      !restrictToGap(Img->RelCodeRelocSlots, 4))
    return nullptr;

  uint64_t Prefix = 0;
  if (End - Begin > limits::kMaxSingleGlobalEmbedLen) {
    // The padded symbol's virtual base must remain in its original mapping,
    // including for source-view externalization and patch-object rebinding.
    if (Seg->VA % limits::kMaxEmbeddedDataAlignment)
      return nullptr;
    // Only these finite bytes are observed. A large relocation-free gap must
    // not force resolution of every unrelated pointer in its segment. Grow
    // overlapping intervals on demand and redirect their previous accesses;
    // address-valued uses still promote them to the canonical whole mirror.
    const uint64_t GapBegin = Begin, GapEnd = End;
    Begin = Addr;
    End = Addr + Size;
    auto It = DirectDataByteGlobals.lower_bound(Begin);
    if (It != DirectDataByteGlobals.begin())
      --It;
    for (; It != DirectDataByteGlobals.end() && It->first <= End; ++It) {
      const uint64_t PreviousEnd = It->first + It->second.Size;
      // Address adjacency does not give another mapping the same backing
      // bytes or permissions. Growth must stay inside this segment's gap.
      if (It->first < GapBegin || PreviousEnd > GapEnd || PreviousEnd < Begin)
        continue;
      Begin = std::min(Begin, It->first);
      End = std::max(End, PreviousEnd);
    }
    if (End - Begin > limits::kMaxSingleGlobalEmbedLen)
      return nullptr;
    Prefix = Begin % limits::kMaxEmbeddedDataAlignment;
    if (Prefix > limits::kMaxSingleGlobalEmbedLen ||
        End - Begin > limits::kMaxSingleGlobalEmbedLen - Prefix)
      return nullptr;
  }

  auto It = DirectDataByteGlobals.find(Begin);
  llvm::GlobalVariable *GV =
      It == DirectDataByteGlobals.end() ? nullptr : It->second.Global;
  if (GV && It->second.Size != End - Begin)
    GV = nullptr;
  if (!GV) {
    const bool IsConstant = !Seg->isWritable() || isReadOnlyAfterReloc(Seg);
    const auto Bytes = llvm::ArrayRef<uint8_t>(
        Seg->Data.data() + (Begin - Seg->VA), End - Begin);
    // Padding has no image identity and cannot be observed by finite accesses.
    // Align the allocation, then place Begin at its original residue within
    // that alignment. Rebinding must account for this prefix as well.
    llvm::Constant *Init = nullptr;
    if (Prefix) {
      std::vector<uint8_t> Padded(Prefix, 0);
      Padded.insert(Padded.end(), Bytes.begin(), Bytes.end());
      Init = llvm::ConstantDataArray::get(*Ctx, Padded);
    } else {
      Init = llvm::ConstantDataArray::get(*Ctx, Bytes);
    }
    GV = new llvm::GlobalVariable(
        *Mod, Init->getType(), IsConstant, dataLinkage(), Init,
        // The symbolized base includes padding: source views and patch-object
        // rebinding recover its original VA from this name before adding GEPs.
        (kNdCodePtrPrefix + llvm::utohexstr(Begin - Prefix)).str() + ".bytes");
    GV->setAlignment(
        llvm::Align(Prefix ? limits::kMaxEmbeddedDataAlignment
                           : uint64_t{1} << llvm::countr_zero(
                                 Begin | limits::kMaxEmbeddedDataAlignment)));
    auto *Base = llvm::ConstantExpr::getGetElementPtr(
        llvm::Type::getInt8Ty(*Ctx), GV,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), Prefix));
    rebindDirectDataBytes(Base, Begin, End);
    DirectDataByteGlobals[Begin] = {GV, End - Begin, Prefix};
  }
  return llvm::ConstantExpr::getGetElementPtr(
      llvm::Type::getInt8Ty(*Ctx), GV,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx),
                             Addr - Begin + Prefix));
}

void MedLLVMEmitter::rebindDirectDataBytes(llvm::Constant *Mirror,
                                           uint64_t Begin, uint64_t End) {
  auto It = DirectDataByteGlobals.lower_bound(Begin);
  while (It != DirectDataByteGlobals.end() && It->first < End) {
    auto *GV = It->second.Global;
    auto *Address = llvm::ConstantExpr::getGetElementPtr(
        llvm::Type::getInt8Ty(*Ctx), Mirror,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx),
                               It->first - Begin - It->second.Prefix));
    GV->replaceAllUsesWith(Address);
    GV->eraseFromParent();
    It = DirectDataByteGlobals.erase(It);
  }
}

bool MedLLVMEmitter::isReadOnlyAfterReloc(const Segment *S) const {
  return S && S->isReadable() && !S->isExecutable() && !S->Data.empty() &&
         (Img ? Img->readOnlyAfterRelocations(S->VA, S->Size)
              : !S->isWritable() || S->ReadOnlyAfterRelocations);
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

  // A data section can share an executable mapping, and an exact RELRO range
  // can share a writable mapping. Keep data ownership separate from the
  // loader's address-specific protection proof.
  bool HasReadOnlySectionEvidence = false;
  if (const Section *Sec = Img->getSectionFor(VA))
    HasReadOnlySectionEvidence =
        Sec->isReadable() &&
        (Img->isMachO() ? !Img->isCodeAddress(VA) : !Sec->isExecutable()) &&
        Img->readOnlyAfterRelocations(VA, 1);
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
    HasReadOnlySectionEvidence = OwnerSec->isReadable() &&
                                 (Img->isMachO() ? !Img->isCodeAddress(OwnerVA)
                                                 : !OwnerSec->isExecutable()) &&
                                 Img->readOnlyAfterRelocations(OwnerVA, 1);
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
    if (isReadOnlyAfterReloc(&S) && !Img->imageBaseOffset(S.VA))
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
  auto adjacent = [&](const Segment *P, const Segment *N) {
    return separatedOnlyByPadding(*Img, P, N);
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

std::optional<uint64_t>
MedLLVMEmitter::completePointerMirrorRun(uint64_t VA) const {
  if (!addrInCodePtrMirrorRun(VA))
    return std::nullopt;
  const Segment *Seg = Img->getSegmentFor(VA);
  if (isReadOnlyAfterReloc(Seg)) {
    if (!isMaterializableReadOnlyDataAddress(VA))
      return std::nullopt;
  } else {
    // A writable segment with pointer slots already has one mutable mirror:
    // direct addresses, selected addresses and STOREs share the same global.
    // Its on-image bytes (including loader-authenticated zero fill) must cover
    // the entire memory extent before an unbounded offset may retain it.
    if (!Seg || !Seg->isReadable() || !Seg->isWritable() ||
        Seg->isExecutable() || Seg->Size == 0 ||
        Seg->Data.size() != Seg->Size ||
        Seg->Size > std::numeric_limits<uint64_t>::max() - Seg->VA ||
        !canResolveGlobalDataConstant(VA))
      return std::nullopt;
  }
  uint64_t RunStart = 0, RunEnd = 0;
  readOnlyAfterRelocRun(Seg, RunStart, RunEnd);
  return VA >= RunStart && VA < RunEnd ? std::optional<uint64_t>(RunStart)
                                       : std::nullopt;
}

bool MedLLVMEmitter::addrInCodePtrMirrorRun(uint64_t VA) const {
  // The headers' ImageBase slot is no mirror: the headers are `__ImageBase`.
  if (!Img || Img->imageBaseOffset(VA))
    return false;
  const Segment *Seg = Img->getSegmentFor(VA);
  // Keep this ownership predicate in lockstep with
  // buildCodePtrSegmentGlobal(): both immutable relocation tables and mutable
  // data segments with proven pointer slots are represented by a pointer
  // mirror.  Restricting this check to segment-level RELRO names loses that
  // fact for linked ELF PT_LOADs whose exact `.data.rel.ro` section is nested
  // inside a coarsely writable segment.
  if (!Seg || Seg->Data.empty() || Seg->isExecutable())
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
  // the stale original VA → unmapped).  Exclude only relocation slots that
  // the candidate-local proof marked exclusively consumed by the dispatch;
  // physical table storage may also contain an independently used gap field.
  auto inJumpTable = [&](uint64_t S) -> bool {
    return currentJumpTableSuppressesRelocationSlot(S);
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
    for (const auto &[S, Binding] : EffectiveImportStorageSlots) {
      (void)Binding;
      if (S >= RunStart && S < RunEnd)
        return true;
    }
    for (va_t S : ConflictingImportStorageSlots)
      if (S >= RunStart && S < RunEnd)
        return true;
    for (const auto &[S, Name] : ExternalFunctionPointerSlots) {
      (void)Name;
      if (S >= RunStart && S < RunEnd)
        return true;
    }
    return false;
  };
  auto runtimeCallableInRun = [&]() {
    for (const RuntimeCallablePointerSlot &Slot :
         Img->RuntimeCallablePointerSlots)
      if (Slot.SlotVA >= RunStart && Slot.SlotVA < RunEnd)
        return true;
    return false;
  };
  return inRun(Img->CodePtrRelocSlots, /*ExcludeJumpTables=*/true) ||
         inRun(Img->DataPtrRelocSlots, /*ExcludeJumpTables=*/false) ||
         importInRun() || runtimeCallableInRun();
}

std::pair<llvm::GlobalVariable *, uint64_t>
MedLLVMEmitter::embedWritableRun(uint64_t SegVA) {
  if (auto Storage = originalImageStorage(SegVA); Storage.first)
    return Storage;
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
  if (preservesRegistrationImageStorage(RunStart, RunEnd)) {
    auto *ArrTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), RunLen);
    auto *GV = new llvm::GlobalVariable(
        *Mod, ArrTy, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr,
        (kNdDataPrefix + llvm::utohexstr(RunStart)).str());
    GV->setAlignment(llvm::Align(16));
    GV->setDSOLocal(true);
    WritableSegmentGlobals[RunStart] = GV;
    return {GV, RunStart};
  }
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
  if (RunLen == 0 ||
      (RunLen > limits::kMaxSingleGlobalEmbedLen &&
       !(OriginalImageStorageForSource && !Img->IsRelocatable)) ||
      RunLen > std::numeric_limits<uint64_t>::max() - Seg->VA)
    return false;
  RunStart = Seg->VA;
  RunEnd = Seg->VA + RunLen;
  return true;
}

} // namespace neverd
