//===- ELFLoaderRelocations.cpp - ELF relocation tables -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads the SHT_REL / SHT_RELA tables of an ELF image, and, for a
/// relocatable object, applies them in place.  Applying them is what gives
/// the lifter correct displacements for constant-pool loads and jump tables,
/// and the recorded target sets are what let the emitter symbolize the
/// resolved addresses again instead of leaving stale original VAs behind.
///
//===----------------------------------------------------------------------===//

#include "ELFLoaderDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"

#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-elf-loader"

namespace neverd {
namespace elf_loader {
namespace detail {

template <typename ELFT>
void collectRelocations(const llvm::object::ELFFile<ELFT> &ELF,
                        llvm::ArrayRef<typename ELFT::Shdr> Sections,
                        llvm::StringRef ShStrTab, const uint8_t *Data,
                        size_t Size, bool IsRelocatable, BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;

  // --- Relocations ---
  for (const Elf_Shdr &SH : Sections) {
    bool IsRela = (SH.sh_type == SHT_RELA);
    if (!IsRela && SH.sh_type != SHT_REL)
      continue;
    if (SH.sh_entsize == 0 || !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      continue;

    const Elf_Shdr *SymSH = getShdr<ELFT>(Sections, SH.sh_link);
    llvm::StringRef StrTab;
    if (SymSH) {
      auto TabOr = ELF.getStringTableForSymtab(*SymSH);
      if (TabOr)
        StrTab = *TabOr;
      else
        llvm::consumeError(TabOr.takeError());
    }

    llvm::StringRef SecName = getSectionName<ELFT>(ShStrTab, SH);
    size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
    for (size_t I = 0; I < Count; ++I) {
      uint64_t ROff64 = SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
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

      if (!IsRelocatable)
        elf_loader::recordIRelativeResolver(
            RE.Type, RE.Address,
            IsRela ? std::optional<int64_t>(RE.Addend) : std::nullopt, Img);
      Img.Relocations.push_back(std::move(RE));
    }
  }
}

template <typename ELFT>
void applyRelocations(const llvm::object::ELFFile<ELFT> &ELF,
                      llvm::ArrayRef<typename ELFT::Shdr> Sections,
                      const uint8_t *Data, size_t Size,
                      const std::vector<va_t> &SecBase, bool IsRelocatable,
                      BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;

  for (const Elf_Shdr &SH : Sections) {
    bool IsRela = (SH.sh_type == SHT_RELA);
    if (!IsRela && SH.sh_type != SHT_REL)
      continue;
    if (SH.sh_entsize == 0 || !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      continue;

    const Elf_Shdr *ApplySH = getShdr<ELFT>(Sections, SH.sh_info);
    if (!ApplySH || !(ApplySH->sh_flags & SHF_ALLOC))
      continue;

    va_t ApplyVA =
        sectionVA<ELFT>(IsRelocatable, SecBase, *ApplySH, SH.sh_info);
    Segment *ApplySeg = nullptr;
    for (auto &Seg : Img.Segments) {
      if (Seg.VA == ApplyVA && !Seg.Data.empty()) {
        ApplySeg = &Seg;
        break;
      }
    }
    if (!ApplySeg)
      continue;

    const Elf_Shdr *SymSH2 = getShdr<ELFT>(Sections, SH.sh_link);
    size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
    for (size_t I = 0; I < Count; ++I) {
      uint64_t ROff64 = SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
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
          if (Sym.st_shndx != SHN_UNDEF && Sym.st_shndx < SHN_LORESERVE) {
            if (const Elf_Shdr *TSH = getShdr<ELFT>(Sections, Sym.st_shndx)) {
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

      // Relative data-pointer table entry.  The slot contains a signed
      // displacement and the generated code adds the table base after loading
      // it.  Record the slot (rather than only the source/target segment) so
      // the emitter can recover the exact contiguous table and its possible
      // target pointers without treating arbitrary integer tables as
      // provenance.
      auto RecordRelDataPtr = [&](uint64_t TargetVA) {
        const Segment *PSeg = Img.getSegmentFor(P);
        const Segment *TSeg = Img.getSegmentFor(TargetVA);
        if (PSeg && PSeg->isReadable() && !PSeg->isWritable() &&
            !PSeg->isExecutable() && !PSeg->Data.empty() && TSeg &&
            TSeg->isReadable() && !TSeg->isWritable() &&
            !TSeg->isExecutable() && !TSeg->Data.empty())
          Img.RelDataPtrRelocSlots.insert(P);
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
          if (RType == R_X86_64_PC32 || RType == R_AARCH64_PREL32)
            RecordRelDataPtr(S);
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
          int64_t PageDelta = static_cast<int64_t>(((S + RAddend) & ~0xFFFULL) -
                                                   (P & ~0xFFFULL));
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
            const Segment *SymSeg = Img.getSegmentFor(static_cast<uint32_t>(S));
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
            if (RType == R_386_PC32)
              RecordRelDataPtr(S);
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
          RecordRelDataPtr(S);
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

// ===--------------------------------------------------------------------===//
// Explicit template instantiations for ELF32 and ELF64
// ===--------------------------------------------------------------------===//

template void collectRelocations<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, llvm::StringRef,
    const uint8_t *, size_t, bool, BinaryImage &);
template void collectRelocations<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, llvm::StringRef,
    const uint8_t *, size_t, bool, BinaryImage &);

template void applyRelocations<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const uint8_t *, size_t,
    const std::vector<va_t> &, bool, BinaryImage &);
template void applyRelocations<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const uint8_t *, size_t,
    const std::vector<va_t> &, bool, BinaryImage &);

} // namespace detail
} // namespace elf_loader
} // namespace neverd
