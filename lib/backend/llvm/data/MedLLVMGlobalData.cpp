//===- MedLLVMGlobalData.cpp - Global data resolution ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The tryResolveGlobalData constant-address entry point for
/// MedLLVMEmitter: mapping a constant VA to the LLVM global that backs
/// it.  Segment embedding lives in MedLLVMSegmentEmbed.cpp, writable-data
/// resolution in MedLLVMWritableData.cpp, symbolized-pointer resolution in
/// MedLLVMSymbolizedPtr.cpp, and the stack-spill predicates in
/// MedLLVMSpillPredicate.cpp.  Shared SSA/address-base tracing lives in
/// resolve/MedLLVMAddrResolve.cpp; literal/select and indexed/induction
/// table resolvers live in MedLLVMLiteralTable.cpp and
/// MedLLVMIndexedGlobal.cpp; code-pointer table mirroring lives in
/// resolve/MedLLVMCodePtrResolve.cpp.  The architecture-gated i386 PIC
/// address recognizers live in X86/MedLLVMX86GlobalData.cpp.
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

//===----------------------------------------------------------------------===//
// Global data resolution
//===----------------------------------------------------------------------===//

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

  // Objective-C metadata is live runtime state, not ordinary image data.
  // dyld fixes its class/selector/CFString pointers in place and libobjc keeps
  // referring to those original objects.  Copying an __objc_* object into the
  // generated segment (or, worse, classifying its pointer-looking bytes as a
  // compact string) creates a second identity whose isa/descriptor fields are
  // still encoded for the on-disk image.  Keep every direct reference into
  // these sections anchored at its original VA; the patch resolver turns the
  // synthetic symbol back into that slid address.
  if (Img->isMachO()) {
    const Section *Sec = Img->getSectionFor(Addr);
    if (Sec && (llvm::StringRef(Sec->Name).starts_with(
                    section_names::macho::ObjCMetadataPrefix) ||
                Sec->Name == section_names::macho::CfString)) {
      std::string Name = makeNdDataSymbol(Addr);
      llvm::GlobalVariable *GV = Mod->getNamedGlobal(Name);
      if (!GV)
        GV = new llvm::GlobalVariable(
            *Mod, llvm::Type::getInt8Ty(*Ctx), /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage, /*Initializer=*/nullptr, Name);
      GV->setDSOLocal(true);
      GlobalDataCache[Addr] = GV;
      return GV;
    }
  }

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
  // A relocated pointer slot can begin with printable low address bytes and an
  // early NUL (for example a Mach-O VTT entry).  Its address identity matters:
  // turning it into an unnamed compact string lets LLVM merge/rename it and
  // leaves patched code pointing at copied, unrebased pointer bytes.
  bool IsRelocatedPointerSlot = Img->CodePtrRelocSlots.count(Addr) != 0 ||
                                Img->DataPtrRelocSlots.count(Addr) != 0;
  if (IsString && StrLen > 0 && AtStringStart && !SizedObjectBeyondString &&
      !IsRelocatedPointerSlot && !isInductionRodataStringBase(Addr)) {
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
