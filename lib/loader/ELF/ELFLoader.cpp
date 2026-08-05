//===- ELFLoader.cpp - ELF binary format loader -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements ELF loading via LLVM's Object/ELF API (ELFFile and
/// ELFObjectFile).  Supports ELF32/ELF64 relocatable and executable
/// objects on little-endian hosts.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/ELF/ELFLoader.h"

#include "neverd/Limits.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/loader/ELF/EhFrameHdr.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <cstring>
#include <limits>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-elf-loader"

namespace neverd {

namespace {

Arch tripleToArch(llvm::Triple::ArchType TA) {
  switch (TA) {
  case llvm::Triple::x86_64:
    return Arch::X64;
  case llvm::Triple::aarch64:
    return Arch::AArch64;
  case llvm::Triple::x86:
    return Arch::X86;
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
    return Arch::ARM;
  default:
    return Arch::Unknown;
  }
}

template <typename ELFT>
llvm::Error loadELF(llvm::object::ELFObjectFile<ELFT> &Obj, BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Ehdr = typename ELFT::Ehdr;
  using Elf_Phdr = typename ELFT::Phdr;
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;

  const auto &ELF = Obj.getELFFile();
  const uint8_t *Data = reinterpret_cast<const uint8_t *>(Obj.getData().data());
  size_t Size = Obj.getData().size();

  const Elf_Ehdr &EH = ELF.getHeader();
  bool IsRelocatable = (EH.e_type == ET_REL);

  Img.Arch = tripleToArch(Obj.getArch());
  if (Img.Arch == Arch::Unknown)
    return llvm::make_error<llvm::StringError>("elf: unsupported architecture",
                                               llvm::inconvertibleErrorCode());

  Img.Bits = ELFT::Is64Bits ? Bitness::Bits64 : Bitness::Bits32;
  Img.Entry = EH.e_entry;
  if (Img.Arch == Arch::ARM)
    Img.Entry = clearThumbBit(Img.Entry);

  auto SectionsOr = ELF.sections();
  if (!SectionsOr)
    return SectionsOr.takeError();

  auto ShStrTabOr = ELF.getSectionStringTable(*SectionsOr);
  if (!ShStrTabOr)
    return ShStrTabOr.takeError();
  llvm::StringRef ShStrTab = *ShStrTabOr;

  auto GetSecName = [&](const Elf_Shdr &SH) -> llvm::StringRef {
    if (SH.sh_name >= ShStrTab.size())
      return {};
    return ShStrTab.substr(SH.sh_name);
  };

  // Relocatable .o files often use sh_addr==0 for every SHF_ALLOC section.
  // sh_offset is a file position, not a VMA — map each section to a unique
  // base.
  std::vector<va_t> SecBase(SectionsOr->size(), 0);
  if (IsRelocatable) {
    va_t Next = 0;
    for (uint32_t I = 0; I < SectionsOr->size(); ++I) {
      const Elf_Shdr &SH = (*SectionsOr)[I];
      if (!(SH.sh_flags & SHF_ALLOC) || SH.sh_size == 0)
        continue;
      if (SH.sh_addr != 0) {
        SecBase[I] = static_cast<va_t>(SH.sh_addr);
        if (SH.sh_size > InvalidVA - SecBase[I])
          return llvm::make_error<llvm::StringError>(
              "elf: section address range overflows",
              llvm::inconvertibleErrorCode());
        va_t End = SecBase[I] + static_cast<va_t>(SH.sh_size);
        if (End > Next)
          Next = End;
      } else {
        uint64_t A = SH.sh_addralign ? SH.sh_addralign : 1;
        if ((A & (A - 1)) != 0 || Next > InvalidVA - (A - 1))
          return llvm::make_error<llvm::StringError>(
              "elf: invalid section alignment",
              llvm::inconvertibleErrorCode());
        Next = (Next + A - 1) & ~(A - 1);
        if (SH.sh_size > InvalidVA - Next)
          return llvm::make_error<llvm::StringError>(
              "elf: synthesized section range overflows",
              llvm::inconvertibleErrorCode());
        SecBase[I] = Next;
        Next += static_cast<va_t>(SH.sh_size);
      }
    }
  }

  auto secVA = [&](const Elf_Shdr &SH, uint32_t Idx) -> va_t {
    if (IsRelocatable)
      return SecBase[Idx];
    return SH.sh_addr ? static_cast<va_t>(SH.sh_addr)
                      : static_cast<va_t>(SH.sh_offset);
  };

  // --- PT_LOAD segments ---
  va_t Lo = InvalidVA;
  auto PhdrsOr = ELF.program_headers();
  if (!PhdrsOr)
    return PhdrsOr.takeError();

  for (const Elf_Phdr &PH : *PhdrsOr) {
    if (PH.p_type != PT_LOAD)
      continue;
    if (PH.p_filesz > PH.p_memsz)
      return llvm::make_error<llvm::StringError>(
          "elf: PT_LOAD file size exceeds memory size",
          llvm::inconvertibleErrorCode());
    if (PH.p_memsz > InvalidVA - static_cast<va_t>(PH.p_vaddr))
      return llvm::make_error<llvm::StringError>(
          "elf: PT_LOAD virtual address range overflows",
          llvm::inconvertibleErrorCode());
    if (PH.p_filesz > 0 &&
        !rangeInBounds(PH.p_offset, PH.p_filesz, Size))
      return llvm::make_error<llvm::StringError>(
          "elf: PT_LOAD file range is out of bounds",
          llvm::inconvertibleErrorCode());

    Segment Seg;
    Seg.Name = (kLoadSegmentPrefix + std::to_string(Img.Segments.size())).str();
    Seg.VA = PH.p_vaddr;
    Seg.Size = PH.p_memsz;
    Seg.FileOff = PH.p_offset;
    Seg.FileSz = PH.p_filesz;
    Seg.Flags = elfPFlagsToNd(PH.p_flags);

    if (PH.p_filesz > 0)
      Seg.Data.assign(Data + PH.p_offset, Data + PH.p_offset + PH.p_filesz);
    // p_memsz is untrusted; only zero-fill up to the cap (see
    // kMaxSegmentZeroFill) so a crafted size cannot force a huge allocation.
    if (PH.p_memsz > PH.p_filesz &&
        PH.p_memsz <= limits::kMaxSegmentZeroFill)
      Seg.Data.resize(static_cast<size_t>(PH.p_memsz), 0);

    if (Seg.VA < Lo)
      Lo = Seg.VA;
    Img.Segments.push_back(std::move(Seg));
  }

  Img.Base = (Lo != InvalidVA) ? Lo : 0;

  // Relocatable objects: use SHF_ALLOC sections as load regions.
  if (Img.Segments.empty()) {
    for (uint32_t I = 0; I < SectionsOr->size(); ++I) {
      const Elf_Shdr &SH = (*SectionsOr)[I];
      if (!(SH.sh_flags & SHF_ALLOC) || SH.sh_size == 0)
        continue;
      Segment Seg;
      llvm::StringRef Name = GetSecName(SH);
      if (!Name.empty())
        Seg.Name = Name.str();
      Seg.VA = secVA(SH, I);
      Seg.Size = SH.sh_size;
      Seg.FileOff = SH.sh_offset;
      Seg.FileSz = SH.sh_type == SHT_NOBITS ? 0 : SH.sh_size;
      Seg.Flags = elfSHFlagsToNd(SH.sh_flags);
      if (Seg.Size > InvalidVA - Seg.VA)
        return llvm::make_error<llvm::StringError>(
            "elf: allocatable section range overflows",
            llvm::inconvertibleErrorCode());
      if (Seg.FileSz > 0 &&
          !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
        return llvm::make_error<llvm::StringError>(
            "elf: allocatable section file range is out of bounds",
            llvm::inconvertibleErrorCode());
      if (Seg.FileSz > 0)
        Seg.Data.assign(Data + SH.sh_offset, Data + SH.sh_offset + SH.sh_size);
      else if (Seg.Size <= limits::kMaxSegmentZeroFill)
        Seg.Data.resize(static_cast<size_t>(Seg.Size), 0);
      Img.Segments.push_back(std::move(Seg));
    }
    if (!Img.Segments.empty()) {
      Lo = InvalidVA;
      for (const auto &S : Img.Segments)
        if (S.VA < Lo)
          Lo = S.VA;
      Img.Base = (Lo != InvalidVA) ? Lo : 0;
    }
  }

  if (Img.Segments.empty())
    return llvm::make_error<llvm::StringError>(
        "elf: no loadable segments found", llvm::inconvertibleErrorCode());

  // Overlay section names onto segments.
  for (const Elf_Shdr &SH : *SectionsOr) {
    if (SH.sh_addr == 0 || SH.sh_size == 0)
      continue;
    llvm::StringRef SectName = GetSecName(SH);
    if (SectName.empty())
      continue;
    for (auto &Seg : Img.Segments) {
      if (Seg.contains(SH.sh_addr)) {
        Seg.Name = SectName.str();
        break;
      }
    }
  }

  // --- Sections ---
  for (uint32_t I = 0; I < SectionsOr->size(); ++I) {
    const Elf_Shdr &SH = (*SectionsOr)[I];
    if (SH.sh_type == SHT_NULL)
      continue;
    Section Sec;
    llvm::StringRef Name = GetSecName(SH);
    if (!Name.empty())
      Sec.Name = Name.str();
    Sec.VA = IsRelocatable ? SecBase[I] : static_cast<va_t>(SH.sh_addr);
    Sec.Size = SH.sh_size;
    Sec.FileOff = SH.sh_offset;
    Sec.FileSz = (SH.sh_type != SHT_NOBITS) ? SH.sh_size : 0;
    Sec.Type = SH.sh_type;
    if (SH.sh_addralign > std::numeric_limits<uint32_t>::max() ||
        (SH.sh_addralign != 0 &&
         (SH.sh_addralign & (SH.sh_addralign - 1)) != 0))
      return llvm::make_error<llvm::StringError>(
          "elf: invalid section alignment",
          llvm::inconvertibleErrorCode());
    Sec.Alignment =
        SH.sh_addralign ? static_cast<uint32_t>(SH.sh_addralign) : 1;
    Sec.Flags = elfSHFlagsToNd(SH.sh_flags);
    if (Sec.Size > InvalidVA - Sec.VA)
      return llvm::make_error<llvm::StringError>(
          "elf: section virtual address range overflows",
          llvm::inconvertibleErrorCode());
    if (Sec.FileSz > 0 &&
        !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      return llvm::make_error<llvm::StringError>(
          "elf: section file range is out of bounds",
          llvm::inconvertibleErrorCode());
    if (Sec.FileSz > 0)
      Sec.Data.assign(Data + SH.sh_offset, Data + SH.sh_offset + SH.sh_size);
    Img.Sections.push_back(std::move(Sec));
  }

  auto GetShdr = [&](uint32_t Idx) -> const Elf_Shdr * {
    if (Idx >= SectionsOr->size())
      return nullptr;
    return &(*SectionsOr)[Idx];
  };

  // --- Relocations ---
  for (const Elf_Shdr &SH : *SectionsOr) {
    bool IsRela = (SH.sh_type == SHT_RELA);
    if (!IsRela && SH.sh_type != SHT_REL)
      continue;
    if (SH.sh_entsize == 0 || !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      continue;

    const Elf_Shdr *SymSH = GetShdr(SH.sh_link);
    llvm::StringRef StrTab;
    if (SymSH) {
      auto TabOr = ELF.getStringTableForSymtab(*SymSH);
      if (TabOr)
        StrTab = *TabOr;
      else
        llvm::consumeError(TabOr.takeError());
    }

    llvm::StringRef SecName = GetSecName(SH);
    size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
    for (size_t I = 0; I < Count; ++I) {
      uint64_t ROff64 =
          SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
      RelocationEntry RE;
      uint32_t SymIdx = 0;
      if (IsRela) {
        if (!rangeInBounds(ROff64, sizeof(Elf_Rela), Size))
          break;
        Elf_Rela R;
        std::memcpy(&R, Data + static_cast<size_t>(ROff64), sizeof(R));
        RE.Address = R.r_offset;
        RE.Addend = R.r_addend;
        RE.Type = R.getType(false);
        SymIdx = R.getSymbol(false);
      } else {
        if (!rangeInBounds(ROff64, sizeof(Elf_Rel), Size))
          break;
        Elf_Rel R;
        std::memcpy(&R, Data + static_cast<size_t>(ROff64), sizeof(R));
        RE.Address = R.r_offset;
        RE.Type = R.getType(false);
        SymIdx = R.getSymbol(false);
      }
      RE.SymbolIndex = SymIdx;
      if (!SecName.empty())
        RE.SectionName = SecName.str();

      if (SymSH && SymIdx > 0 && !StrTab.empty()) {
        auto SymsOr = ELF.symbols(SymSH);
        if (SymsOr && SymIdx < SymsOr->size()) {
          auto SymNameOr = (*SymsOr)[SymIdx].getName(StrTab);
          if (SymNameOr)
            RE.SymbolName = SymNameOr->str();
          else
            llvm::consumeError(SymNameOr.takeError());
        }
      }
      Img.Relocations.push_back(std::move(RE));
    }
  }

  // --- Apply relocations for relocatable objects (.o files) ---
  // PC-relative references in .text to .rodata need fixup so the lifter
  // sees correct displacements for constant pool loads.
  if (IsRelocatable) {
    for (const Elf_Shdr &SH : *SectionsOr) {
      bool IsRela = (SH.sh_type == SHT_RELA);
      if (!IsRela && SH.sh_type != SHT_REL)
        continue;
      if (SH.sh_entsize == 0 || !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
        continue;

      const Elf_Shdr *ApplySH = GetShdr(SH.sh_info);
      if (!ApplySH || !(ApplySH->sh_flags & SHF_ALLOC))
        continue;

      va_t ApplyVA = secVA(*ApplySH, SH.sh_info);
      Segment *ApplySeg = nullptr;
      for (auto &Seg : Img.Segments) {
        if (Seg.VA == ApplyVA && !Seg.Data.empty()) {
          ApplySeg = &Seg;
          break;
        }
      }
      if (!ApplySeg)
        continue;

      const Elf_Shdr *SymSH2 = GetShdr(SH.sh_link);
      size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
      for (size_t I = 0; I < Count; ++I) {
        uint64_t ROff64 =
            SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
        va_t RAddr = 0;
        int64_t RAddend = 0;
        uint32_t RType = 0;
        uint32_t RSym = 0;
        if (IsRela) {
          if (!rangeInBounds(ROff64, sizeof(Elf_Rela), Size))
            break;
          Elf_Rela R;
          std::memcpy(&R, Data + static_cast<size_t>(ROff64), sizeof(R));
          RAddr = R.r_offset;
          RAddend = R.r_addend;
          RType = R.getType(false);
          RSym = R.getSymbol(false);
        } else {
          if (!rangeInBounds(ROff64, sizeof(Elf_Rel), Size))
            break;
          Elf_Rel R;
          std::memcpy(&R, Data + static_cast<size_t>(ROff64), sizeof(R));
          RAddr = R.r_offset;
          RType = R.getType(false);
          RSym = R.getSymbol(false);
        }

        va_t SymVal = 0;
        if (SymSH2 && RSym > 0) {
          auto SymsOr = ELF.symbols(SymSH2);
          if (SymsOr && RSym < SymsOr->size()) {
            const Elf_Sym &Sym = (*SymsOr)[RSym];
            SymVal = Sym.st_value;
            if (Sym.st_shndx != SHN_UNDEF &&
                Sym.st_shndx < SHN_LORESERVE) {
              if (const Elf_Shdr *TSH = GetShdr(Sym.st_shndx)) {
                if (SymVal > InvalidVA - SecBase[Sym.st_shndx])
                  continue;
                SymVal += SecBase[Sym.st_shndx];
              }
            }
          }
        }

        va_t P = ApplyVA + RAddr;
        va_t S = SymVal;

        if (RAddr >= ApplySeg->Data.size())
          continue;

        // Remember a relocation that lands in read-only data so the emitter can
        // recognize the resolved VA as a genuine rodata pointer (e.g. an i386
        // GOTOFF `.rodata` base reached through a low VA that the numeric-VA
        // heuristic would otherwise read as an integer literal).
        auto RecordDataTarget = [&](uint64_t TargetVA) {
          if (TargetVA == 0)
            return;
          const Segment *TSeg = Img.getSegmentFor(TargetVA);
          if (TSeg && TSeg->isReadable() && !TSeg->isWritable() &&
              !TSeg->isExecutable() && !TSeg->Data.empty())
            Img.RelocDataAddrs.insert(TargetVA);
        };

        // Writable counterpart: a relocation resolving into a mutable
        // .data/.bss segment proves the target VA is a genuine address (a taken
        // `&G`, or a global accessed/stored through), not an ordinary data
        // immediate that merely collides with the segment's VA range.  The
        // emitter gates writable store-value symbolization on this set so a
        // 32-bit data value carrying a plain immediate inside a wide low-VA run
        // (an LCG increment) is not mistaken for a stored pointer base.
        // Zero-init .bss carries no Data, so unlike RecordDataTarget this does
        // not require non-empty bytes.
        auto RecordWritableDataTarget = [&](uint64_t TargetVA) {
          if (TargetVA == 0)
            return;
          const Segment *TSeg = Img.getSegmentFor(TargetVA);
          if (TSeg && TSeg->isWritable() && !TSeg->isExecutable())
            Img.WritableRelocDataAddrs.insert(TargetVA);
        };

        // Record this slot (at VA P) when it holds an absolute pointer into
        // executable code — the entries of a computed-goto / threaded-dispatch
        // jump table.  The jump-table resolver uses the run length as the exact
        // entry count and to disambiguate absolute entries from PIC offsets.
        auto RecordCodePtr = [&](uint64_t TargetVA) {
          if (TargetVA == 0)
            return;
          const Segment *TSeg = Img.getSegmentFor(TargetVA);
          if (TSeg && TSeg->isExecutable())
            Img.CodePtrRelocSlots.insert(P);
        };

        // Record this slot when it holds an absolute pointer into read-only
        // DATA (not code) — an absolute data-pointer table entry, e.g. the
        // `.data.rel.ro` `const char*` array a 32-bit-target switch-returning-
        // string lowers to.  The emitter rebuilds these as `ptrtoint(@data)` so
        // they retarget the recompiled object instead of the stale original VA.
        auto RecordDataPtr = [&](uint64_t TargetVA) {
          if (TargetVA == 0)
            return;
          const Segment *TSeg = Img.getSegmentFor(TargetVA);
          if (TSeg && TSeg->isReadable() && !TSeg->isExecutable() &&
              !TSeg->Data.empty())
            Img.DataPtrRelocSlots.insert(P);
        };

        // Record a PC-relative relocation slot that sits in read-only data and
        // references executable code — a PIC `switch` jump-table entry.  A run
        // of these bounds a `switch(x % N)` table (modulus-bounded index, no
        // `cmp` guard).  The slot must be in .rodata (not .text, which would be
        // a call/branch).  The signal that it references code is the *symbol's*
        // section being executable: a PC-relative entry stores `S + A - P` (a
        // self-relative displacement), so the symbol value S — not the
        // addend-adjusted `S + A`, which overshoots — is what identifies the
        // target section.  S may legitimately be 0 (the .text base of a
        // relocatable object), so it is not filtered out here.
        auto RecordRelCodePtr = [&](uint64_t SymVA) {
          const Segment *PSeg = Img.getSegmentFor(P);
          const Segment *TSeg = Img.getSegmentFor(SymVA);
          if (PSeg && PSeg->isReadable() && !PSeg->isWritable() &&
              !PSeg->isExecutable() && TSeg && TSeg->isExecutable())
            Img.RelCodeRelocSlots.insert(P);
        };

        // Record a jump-table base anchor: a PC-relative relocation whose slot
        // is in *executable* code and whose target is *read-only data* — the
        // `lea table(%rip)` / `adrp+add` a PIC `switch` materializes its table
        // pointer from.  Two unguarded PIC tables laid out back-to-back in
        // rodata share one continuous RelCodeReloc entry run, so a run-length
        // count from the first table's base over-reads into the second; the
        // next table's own base anchor is the exact boundary that truncates the
        // run to the real entry count (§15.2 adjacent-unguarded-pic-table).  A
        // non-table rodata pointer (a string / constant `lea`) is also recorded
        // here, but the resolver only honours an anchor that is itself a
        // RelCodeReloc entry position, so those never truncate a real table.
        auto RecordRelTableAnchor = [&](uint64_t AnchorVA) {
          const Segment *PSeg = Img.getSegmentFor(P);
          const Segment *TSeg = Img.getSegmentFor(AnchorVA);
          if (PSeg && PSeg->isExecutable() && TSeg && TSeg->isReadable() &&
              !TSeg->isWritable() && !TSeg->isExecutable() && !TSeg->Data.empty())
            Img.RelCodeTableAnchors.insert(AnchorVA);
        };

        // Record an executable target whose address a relocation materializes
        // into a *register* value (a function pointer built by `adrp+add` or an
        // i386 GOTOFF `lea`), so the emitter symbolizes the resulting folded
        // constant as `ptrtoint @func` rather than leaving the stale original
        // VA. Only register-materialization relocation forms are recorded;
        // absolute data-slot pointers (function-pointer tables) are handled by
        // the code- pointer table mirror and must NOT enter this value-keyed
        // set, where a genuine integer equal to a low table-entry function VA
        // would be mis-symbolized.  VA 0 is skipped (indistinguishable from a
        // real zero).
        auto RecordCodeRef = [&](uint64_t TargetVA) {
          if (TargetVA == 0)
            return;
          const Segment *TSeg = Img.getSegmentFor(TargetVA);
          if (TSeg && TSeg->isExecutable())
            Img.CodeRefTargets.insert(TargetVA);
        };

        if constexpr (ELFT::Is64Bits) {
          if (RType == R_X86_64_PC32 || RType == R_X86_64_PLT32 ||
              RType == R_X86_64_GOTPCREL || RType == R_AARCH64_PREL32) {
            // 32-bit PC-relative slot `S + A - P`.  Besides PIC switch jump
            // tables (executable target, recorded below), this fills a
            // PC-relative DATA pointer table — clang lowers a switch returning
            // string literals to a `.rodata` table whose entries are
            // `str - table` (R_AARCH64_PREL32 on AArch64, R_X86_64_PC32 on
            // x86-64); leaving AArch64's unapplied left the raw +N*4 addends,
            // so `table_base + entry` pointed at garbage.
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t Val = static_cast<int32_t>(S + RAddend - P);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
            if (RType == R_X86_64_PC32 || RType == R_AARCH64_PREL32)
              RecordRelCodePtr(S);
            // A PC-relative `lea`/`mov` of a writable global takes/accesses its
            // address.  x86-64 RIP-relative measures the displacement from the
            // END of the (4-byte-disp-trailing) instruction, so the target VA
            // is `S + addend + 4` — clang uses a -4 addend for a bare `&g`, and
            // `S + addend + 4` for a section-symbol-relative `&g[k]` (a SECOND
            // global at a nonzero section offset, the ptrarr/gpstab shape).
            // Recording the symbol base S alone would miss those offset
            // globals.
            if (RType == R_X86_64_PC32 || RType == R_X86_64_PLT32 ||
                RType == R_X86_64_GOTPCREL)
              RecordWritableDataTarget(S + RAddend + 4);
            // The same `lea table(%rip)` that anchors a PIC switch table's base
            // in read-only data (target VA = S + addend + 4, the RIP measured
            // from the end of the 4-byte-disp instruction).  Recording it lets
            // the resolver truncate an over-long RelCodeReloc run at the next
            // table's base.
            if (RType == R_X86_64_PC32)
              RecordRelTableAnchor(S + RAddend + 4);
          } else if (RType == R_X86_64_64 || RType == R_AARCH64_ABS64) {
            // Absolute 64-bit data slot.  R_AARCH64_ABS64 fills the
            // .data.rel.ro code-pointer table a computed goto / threaded
            // dispatch jumps through; it was previously left unrelocated, so
            // the table read back as zeros and the indirect branch lost its
            // targets.
            if (RAddr + 8 > ApplySeg->Data.size())
              continue;
            uint64_t Val = S + RAddend;
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 8);
            RecordCodePtr(Val);
            RecordDataPtr(Val);
            RecordWritableDataTarget(Val);
          } else if (RType == R_X86_64_32 || RType == R_X86_64_32S) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Val = static_cast<uint32_t>(S + RAddend);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
            RecordCodePtr(Val);
            RecordWritableDataTarget(Val);
          } else if (RType == R_AARCH64_CALL26 || RType == R_AARCH64_JUMP26) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int64_t Disp = static_cast<int64_t>(S + RAddend - P);
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            Insn = (Insn & 0xFC000000u) |
                   (static_cast<uint32_t>((Disp >> 2) & 0x03FFFFFFu));
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
          } else if (RType == R_AARCH64_ADR_PREL_PG_HI21) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int64_t PageDelta = static_cast<int64_t>(
                ((S + RAddend) & ~0xFFFULL) - (P & ~0xFFFULL));
            uint32_t ImmLo = static_cast<uint32_t>((PageDelta >> 12) & 0x3);
            uint32_t ImmHi = static_cast<uint32_t>((PageDelta >> 14) & 0x7FFFF);
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            Insn = (Insn & 0x9F00001Fu) | (ImmLo << 29) | (ImmHi << 5);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
          } else if (RType == R_AARCH64_ADD_ABS_LO12_NC ||
                     RType == R_AARCH64_LDST64_ABS_LO12_NC ||
                     RType == R_AARCH64_LDST128_ABS_LO12_NC ||
                     RType == R_AARCH64_LDST32_ABS_LO12_NC ||
                     RType == R_AARCH64_LDST16_ABS_LO12_NC ||
                     RType == R_AARCH64_LDST8_ABS_LO12_NC) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Imm12 = static_cast<uint32_t>((S + RAddend) & 0xFFF);
            // `adrp+add` of a code symbol takes a function's address into a
            // register (a function pointer); record the full target VA.  The
            // LDST forms load *from* the address, so only the ADD form is an
            // address-of materialization.
            if (RType == R_AARCH64_ADD_ABS_LO12_NC)
              RecordCodeRef(S + RAddend);
            // The same `adrp+add` of a writable global takes its address; the
            // LDST forms access it.  Either way the target VA is a genuine
            // mutable-data address (S + addend).
            RecordWritableDataTarget(S + RAddend);
            // `adrp+add table@PAGE/@PAGEOFF` also anchors a PIC switch table's
            // base in read-only data — record it so the resolver bounds an
            // over-long RelCodeReloc run at the next table's base.
            if (RType == R_AARCH64_ADD_ABS_LO12_NC)
              RecordRelTableAnchor(S + RAddend);
            uint32_t Shift = 0;
            if (RType == R_AARCH64_LDST16_ABS_LO12_NC)
              Shift = 1;
            else if (RType == R_AARCH64_LDST32_ABS_LO12_NC)
              Shift = 2;
            else if (RType == R_AARCH64_LDST64_ABS_LO12_NC)
              Shift = 3;
            else if (RType == R_AARCH64_LDST128_ABS_LO12_NC)
              Shift = 4;
            Imm12 >>= Shift;
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            Insn = (Insn & 0xFFC003FFu) | ((Imm12 & 0xFFF) << 10);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
          }
        } else {
          // ELF32 (i386 / ARM) use REL relocations: the addend lives in-place
          // in the field being relocated, not in the relocation record.  Read
          // it so literal pools and PIC offsets resolve correctly.  i386 and
          // ARM share overlapping relocation type numbers, so dispatch on the
          // arch.
          int64_t InPlace = RAddend;
          if (!IsRela && RAddr + 4 <= ApplySeg->Data.size()) {
            int32_t Existing;
            std::memcpy(&Existing, ApplySeg->Data.data() + RAddr, 4);
            InPlace = Existing;
          }
          if (RAddr + 4 > ApplySeg->Data.size())
            continue;
          auto Put32 = [&](uint32_t V) {
            std::memcpy(ApplySeg->Data.data() + RAddr, &V, 4);
          };
          if (Img.Arch == Arch::X86) {
            // i386 PIC: model _GLOBAL_OFFSET_TABLE_ at base 0.  The get-PC seed
            // (lifted to the constant next-PC) plus GOTPC then fold to the GOT
            // base, and GOTOFF folds back to the symbol VA — the base choice
            // cancels (eax=GOTbase; eax+GOTOFF=symbol), so the constant-pool
            // load address becomes a known rodata VA the emitter redirects.
            switch (RType) {
            case R_386_GOTOFF: {
              // GOTOFF materializes a symbol's address into a register (`leal
              // sym@GOTOFF(%ebx)`); an executable target is a function pointer.
              uint32_t Folded = static_cast<uint32_t>(S + InPlace);
              Put32(Folded);
              RecordDataTarget(Folded);
              RecordWritableDataTarget(Folded);
              RecordCodePtr(Folded);
              RecordCodeRef(Folded);
              // `leal table@GOTOFF(%ebx)` anchors a PIC switch table's base in
              // read-only data; record it so the resolver bounds an over-long
              // RelCodeReloc run at the next table's base.
              RecordRelTableAnchor(Folded);
              // A switch-to-lookup-table indexed by the unbiased case value
              // folds the base to `table - min_case*stride` — a negative addend
              // that drops the base BEFORE the table's segment (often into
              // .text), so RecordDataTarget rejects it and the bare VA resolves
              // to the wrong segment.  When the *symbol* itself sits in clean
              // rodata, anchor the folded base to the symbol's own segment so
              // the emitter pins `base + runtime_index` back inside the table.
              const Segment *SymSeg =
                  Img.getSegmentFor(static_cast<uint32_t>(S));
              if (SymSeg && SymSeg->isReadable() && !SymSeg->isWritable() &&
                  !SymSeg->isExecutable() && !SymSeg->Data.empty() &&
                  SymSeg->VA > Folded &&
                  SymSeg->VA - Folded <= limits::kMaxRodataAnchorBackDistance)
                Img.RodataAnchorSeg[Folded] = SymSeg->VA;
              break;
            }
            case R_386_32:
              // Absolute 32-bit slot — a data word or a function-pointer
              // *table* entry (handled by the code-pointer table mirror), not a
              // register materialization, so it is not recorded as a scalar
              // code ref.
              Put32(static_cast<uint32_t>(S + InPlace));
              RecordDataTarget(static_cast<uint32_t>(S + InPlace));
              RecordWritableDataTarget(static_cast<uint32_t>(S + InPlace));
              RecordCodePtr(static_cast<uint32_t>(S + InPlace));
              RecordDataPtr(static_cast<uint32_t>(S + InPlace));
              break;
            case R_386_PC32:
            case R_386_PLT32:
              Put32(static_cast<uint32_t>(S + InPlace - P));
              break;
            case R_386_GOTPC:
              Put32(static_cast<uint32_t>(InPlace - P));
              break;
            default:
              break;
            }
          } else if (RType == R_ARM_ABS32) {
            // Absolute 32-bit slot — a function-pointer table entry (handled by
            // the code-pointer table mirror) or a literal-pool word, not a
            // register materialization, so not recorded as a scalar code ref.
            Put32(static_cast<uint32_t>(S + InPlace));
            RecordCodePtr(static_cast<uint32_t>(S + InPlace));
            RecordDataPtr(static_cast<uint32_t>(S + InPlace));
            RecordWritableDataTarget(static_cast<uint32_t>(S + InPlace));
          } else if (RType == R_ARM_REL32) {
            Put32(static_cast<int32_t>(S + InPlace - P));
            // ARM32 non-PIC takes a writable global's address via a PC-relative
            // literal pool (`ldr rN,[pc]; add rN,pc,rN`), emitted as
            // R_ARM_REL32 on the literal word.  The TARGET is the symbol VA S
            // (the in-place addend is the literal-to-symbol delta, not part of
            // the address), so record S — the base a runtime-indexed `&g[i]`
            // resolves to.
            RecordWritableDataTarget(S);
          } else if (RType == R_ARM_CALL || RType == R_ARM_JUMP24 ||
                     RType == R_ARM_PC24) {
            // ARM `bl`/`b`/`bcc`: the low 24 bits hold the target as a signed
            // word offset from PC+8.  The in-place addend is that imm24 field
            // (<<2), not the whole instruction word, so extract it directly
            // rather than from the generic 32-bit InPlace read.  (<<8 then >>6
            // sign-extends imm24 and rescales to bytes in one step.)  The top
            // byte carries the condition + opcode and is preserved.  ARM-state
            // callee only — Thumb interworking (BLX) is not modelled.
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            int32_t Addend = static_cast<int32_t>(Insn << 8) >> 6;
            int64_t Disp =
                static_cast<int64_t>(S) + Addend - static_cast<int64_t>(P);
            Put32((Insn & 0xFF000000u) |
                  (static_cast<uint32_t>(Disp >> 2) & 0x00FFFFFFu));
          }
        }
      }
    }
  }

  // --- Symbol tables ---
  auto AddSymbolsFrom = [&](const Elf_Shdr &SH) {
    if (SH.sh_type != SHT_SYMTAB && SH.sh_type != SHT_DYNSYM)
      return;
    if (!rangeInBounds(SH.sh_offset, SH.sh_size, Size) || SH.sh_entsize == 0)
      return;

    auto SymsOr = ELF.symbols(&SH);
    if (!SymsOr) {
      llvm::consumeError(SymsOr.takeError());
      return;
    }
    auto StrTabOr = ELF.getStringTableForSymtab(SH);
    if (!StrTabOr) {
      llvm::consumeError(StrTabOr.takeError());
      return;
    }
    llvm::StringRef StrTab = *StrTabOr;

    for (size_t I = 1; I < SymsOr->size(); ++I) {
      const Elf_Sym &Sym = (*SymsOr)[I];
      if (Sym.st_shndx == SHN_UNDEF)
        continue;

      auto NameOr = Sym.getName(StrTab);
      if (!NameOr) {
        llvm::consumeError(NameOr.takeError());
        continue;
      }
      if (NameOr->empty())
        continue;

      va_t Value = Sym.st_value;
      if (IsRelocatable && Sym.st_shndx < SHN_LORESERVE &&
          Sym.st_shndx < SecBase.size()) {
        if (Value > InvalidVA - SecBase[Sym.st_shndx])
          continue;
        Value += SecBase[Sym.st_shndx];
      }
      // Relocatable .o symbols at section start have st_value==0; SecBase may
      // also be 0 — do not treat that as "no address".
      if (Value == 0 && !IsRelocatable)
        continue;

      uint8_t Bind = Sym.getBinding();
      uint8_t Type = Sym.getType();

      if (Img.Arch == Arch::ARM && Type == STT_FUNC)
        Value = clearThumbBit(Value);

      Symbol S;
      S.Name = NameOr->str();
      S.Addr = Value;
      S.Size = Sym.st_size;
      S.IsFunc = (Type == STT_FUNC);
      Img.Symbols.push_back(std::move(S));

      if (Type == STT_FUNC && (Bind == STB_GLOBAL || Bind == STB_WEAK)) {
        Export Exp;
        Exp.Name = NameOr->str();
        Exp.Addr = Value;
        Img.Exports.push_back(std::move(Exp));
      }
    }
  };

  for (const Elf_Shdr &SH : *SectionsOr)
    AddSymbolsFrom(SH);

  // --- .dynamic ---
  for (const Elf_Shdr &SH : *SectionsOr) {
    if (SH.sh_type == SHT_DYNAMIC) {
      elf_loader::parseDynamic(ELF, SH, Data, Size, Img);
      break;
    }
  }

  // --- .rela.plt / .rel.plt imports ---
  elf_loader::parsePLTImports(ELF, *SectionsOr, Data, Size, Img);

  // --- .got / .got.plt ---
  elf_loader::parseGOTEntries(ELF, *SectionsOr, Data, Size, Img);

  // --- PT_NOTE (build-id, ABI tag) ---
  elf_loader::parseNotes(ELF, Data, Size, Img);

  // --- .eh_frame_hdr ---
  for (const Elf_Shdr &SH : *SectionsOr) {
    if (GetSecName(SH) != section_names::elf::EhFrameHdr)
      continue;
    elf_loader::addFunctionsFromEhFrameHdr(Data, Size, SH, Img);
    break;
  }

  return llvm::Error::success();
}

} // anonymous namespace

llvm::Expected<BinaryImage> ELFLoader::load(const std::filesystem::path &Path) {
  BinaryImage Img;
  auto BufOrErr = readFileInto(Path, Img, BinaryFormat::ELF);
  if (!BufOrErr)
    return BufOrErr.takeError();
  auto &Buf = *BufOrErr;

  if (Img.Raw.size() < llvm::ELF::EI_NIDENT)
    return llvm::make_error<llvm::StringError>("elf: file too small",
                                               llvm::inconvertibleErrorCode());

  const uint8_t *Data = Img.Raw.data();
  if (!std::equal(Data, Data + 4,
                  reinterpret_cast<const uint8_t *>(llvm::ELF::ElfMagic)))
    return llvm::make_error<llvm::StringError>("elf: bad magic",
                                               llvm::inconvertibleErrorCode());

  if (Data[llvm::ELF::EI_DATA] != llvm::ELF::ELFDATA2LSB)
    return llvm::make_error<llvm::StringError>("elf: big-endian not supported",
                                               llvm::inconvertibleErrorCode());

  auto ObjOrErr =
      llvm::object::ObjectFile::createObjectFile(Buf->getMemBufferRef());
  if (!ObjOrErr)
    return ObjOrErr.takeError();

  auto *Obj = ObjOrErr->get();
  llvm::Error Err = llvm::Error::success();

  if (auto *ELF64 = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(Obj))
    Err = loadELF(*ELF64, Img);
  else if (auto *ELF32 = llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(Obj))
    Err = loadELF(*ELF32, Img);
  else
    return llvm::make_error<llvm::StringError>("elf: unsupported ELF class",
                                               llvm::inconvertibleErrorCode());

  if (Err)
    return std::move(Err);

  runPostLoadDiscovery(Img, "elf: loaded " + Path.filename().string());
  return Img;
}

} // namespace neverd
