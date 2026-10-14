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
#include "neverd/loader/PointerRelocation.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-elf-loader"

namespace neverd {
namespace elf_loader {
namespace detail {

namespace {

struct ObjectRelocationWriterFootprint {
  int64_t OffsetBias = 0;
  size_t Width = 4;
  bool Exact = true;
};

ObjectRelocationWriterFootprint objectRelocationWriterFootprint(Arch Target,
                                                                uint32_t Type) {
  using namespace llvm::ELF;
#define NEVERD_ELF_OBJECT_RELOCATION_WRITER(TargetArch, Kind, OffsetBias,      \
                                            Width)                             \
  if (Target == Arch::TargetArch && Type == Kind)                              \
    return {OffsetBias, Width};
#include "ELFObjectRelocationWriters.def"
  // An unlisted record conservatively claims one full word.  This may
  // suppress exact provenance but can never manufacture it.
  return {0, 4, false};
}

/// True when the writer table describes \p Target's object relocations.
bool hasObjectRelocationWriters(Arch Target) {
#define NEVERD_ELF_OBJECT_RELOCATION_WRITER(TargetArch, Kind, OffsetBias,      \
                                            Width)                             \
  if (Target == Arch::TargetArch)                                              \
    return true;
#include "ELFObjectRelocationWriters.def"
  return false;
}

std::optional<va_t> addSignedOffsetBias(va_t Offset, int64_t Bias) {
  if (Bias >= 0) {
    const uint64_t Magnitude = static_cast<uint64_t>(Bias);
    if (Magnitude > InvalidVA - Offset)
      return std::nullopt;
    return Offset + Magnitude;
  }
  const uint64_t Magnitude = static_cast<uint64_t>(-(Bias + 1)) + 1;
  if (Magnitude > Offset)
    return std::nullopt;
  return Offset - Magnitude;
}

} // namespace

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
    const bool IsRela = SH.sh_type == SHT_RELA;
    const bool IsRel = SH.sh_type == SHT_REL;
    const bool IsAndroidRela = SH.sh_type == SHT_ANDROID_RELA;
    const bool IsAndroidRel = SH.sh_type == SHT_ANDROID_REL;
    const bool IsAndroidPacked = IsAndroidRela || IsAndroidRel;
    if (!IsRela && !IsRel && !IsAndroidPacked)
      continue;
    const size_t MinEntrySize = IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
    if ((!IsAndroidPacked && SH.sh_entsize < MinEntrySize) ||
        !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      continue;

    const Elf_Shdr *SymSH = getShdr<ELFT>(Sections, SH.sh_link);
    llvm::StringRef StrTab;
    if (SymSH) {
      auto TabOr = ELF.getStringTableForSymtab(*SymSH, Sections);
      if (TabOr)
        StrTab = *TabOr;
      else
        llvm::consumeError(TabOr.takeError());
    }

    llvm::StringRef SecName = getSectionName<ELFT>(ShStrTab, SH);
    uint64_t Ordinal = 0;
    auto Record = [&](RelocationEntry RE, uint32_t SymIdx, uint64_t RawInfo) {
      RE.ELF =
          ELFRelocationProvenance{!IsRelocatable && Img.Sections.empty()
                                      ? ELFRelocationSource::ProgramDynamicTable
                                      : ELFRelocationSource::SectionTable,
                                  SH.sh_addr,
                                  SH.sh_offset,
                                  Ordinal++,
                                  RE.Address,
                                  RawInfo,
                                  std::nullopt};
      RE.SymbolIndex = SymIdx;
      if (!SecName.empty())
        RE.SectionName = SecName.str();

      bool Undefined = false;
      if (SymSH && SymIdx > 0 && !StrTab.empty()) {
        auto SymsOr = ELF.symbols(SymSH);
        if (!SymsOr) {
          llvm::consumeError(SymsOr.takeError());
        } else if (SymIdx < SymsOr->size()) {
          const auto &Sym = (*SymsOr)[SymIdx];
          RE.ELF->Symbol = ELFRelocationSymbol{
              !IsRelocatable && Img.Sections.empty() ? 0u
                                                     : uint32_t(SH.sh_link),
              Sym.st_name,
              Sym.st_shndx,
              Sym.st_info,
              Sym.getBinding(),
              Sym.getType(),
              Sym.st_other,
              Sym.st_value,
              Sym.st_size,
              std::nullopt};
          auto SymNameOr = Sym.getName(StrTab);
          if (SymNameOr)
            RE.SymbolName = SymNameOr->str();
          else
            llvm::consumeError(SymNameOr.takeError());
          RE.ELF->Symbol->Name = RE.SymbolName;
          Undefined = Sym.st_shndx == SHN_UNDEF;
        }
      }

      if (!IsRelocatable) {
        elf_loader::recordIRelativeResolver(
            RE.Type, RE.Address,
            RE.HasExplicitAddend ? std::optional<int64_t>(RE.Addend)
                                 : std::nullopt,
            Img);
        elf_loader::recordRuntimeCallableRelocation(RE.Type, RE.Address, Img);
        // The dynamic linker writes the symbol's address; a REL slot's
        // implicit addend is not added.
        elf_loader::recordImportSlotBinding(
            RE.Type, RE.Address, RE.SymbolName, Undefined,
            RE.HasExplicitAddend ? RE.Addend : 0, Img);
      }
      Img.Relocations.push_back(std::move(RE));
    };

    if (IsAndroidPacked) {
      auto RelasOr = ELF.android_relas(SH);
      if (!RelasOr) {
        llvm::consumeError(RelasOr.takeError());
        continue;
      }
      for (const Elf_Rela &R : *RelasOr) {
        RelocationEntry RE;
        RE.Address = R.r_offset;
        RE.Addend = R.r_addend;
        RE.HasExplicitAddend = IsAndroidRela;
        RE.Type = R.getType(false);
        Record(std::move(RE), R.getSymbol(false), R.r_info);
      }
      continue;
    }

    size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
    for (size_t I = 0; I < Count; ++I) {
      uint64_t ROff64 = SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
      RelocationEntry RE;
      uint32_t SymIdx = 0;
      uint64_t RawInfo = 0;
      if (IsRela) {
        if (!rangeInBounds(ROff64, sizeof(Elf_Rela), Size))
          break;
        Elf_Rela R;
        std::memcpy(&R, Data + static_cast<size_t>(ROff64), sizeof(R));
        RE.Address = R.r_offset;
        RE.Addend = R.r_addend;
        RE.HasExplicitAddend = true;
        RE.Type = R.getType(false);
        SymIdx = R.getSymbol(false);
        RawInfo = R.r_info;
      } else {
        if (!rangeInBounds(ROff64, sizeof(Elf_Rel), Size))
          break;
        Elf_Rel R;
        std::memcpy(&R, Data + static_cast<size_t>(ROff64), sizeof(R));
        RE.Address = R.r_offset;
        RE.Type = R.getType(false);
        SymIdx = R.getSymbol(false);
        RawInfo = R.r_info;
      }
      Record(std::move(RE), SymIdx, RawInfo);
    }
  }
}

template <typename ELFT>
void applyDynamicRelativeRelocations(
    const llvm::object::ELFFile<ELFT> &ELF,
    llvm::ArrayRef<typename ELFT::Shdr> Sections, const uint8_t *Data,
    size_t Size, BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;
  using Elf_Shdr = typename ELFT::Shdr;

  auto IsRelative = [&](uint32_t Type) {
    if constexpr (ELFT::Is64Bits) {
      return (Img.Arch == Arch::X64 && Type == R_X86_64_RELATIVE) ||
             (Img.Arch == Arch::AArch64 && Type == R_AARCH64_RELATIVE);
    }
    return (Img.Arch == Arch::X86 && Type == R_386_RELATIVE) ||
           (Img.Arch == Arch::ARM && Type == R_ARM_RELATIVE);
  };

  constexpr size_t PtrSize = sizeof(typename ELFT::Addr);
  // Slots that already carry a base relocation.  A shared object lists tens
  // of thousands of relative relocations, so a scan per slot is quadratic.  A
  // malformed r_offset may be any value, including a DenseSet reserved key.
  std::unordered_set<va_t> BaseRelocated;
  BaseRelocated.reserve(Img.BaseRelocations.size());
  for (const BaseRelocation &R : Img.BaseRelocations)
    BaseRelocated.insert(R.Address);
  auto ApplyPointerSlot = [&](va_t SlotVA, uint64_t TargetVA) {
    if (!Img.patchPtr(SlotVA, TargetVA))
      return false;

    // Keep the section view coherent with the mapped segment view.  Public
    // BinaryImage consumers can read either, and RELA slots are commonly zero
    // in the file before the dynamic loader writes them.
    for (Section &Sec : Img.Sections) {
      if (!Sec.contains(SlotVA))
        continue;
      const size_t Off = static_cast<size_t>(SlotVA - Sec.VA);
      if (rangeInBounds(Off, PtrSize, Sec.Data.size()))
        writePtr(Sec.Data.data() + Off, TargetVA, ELFT::Is64Bits);
      break;
    }

    if (BaseRelocated.insert(SlotVA).second)
      Img.BaseRelocations.push_back(BaseRelocation{SlotVA, 0});
    recordAbsolutePointerRelocation(Img, SlotVA, TargetVA);
    return true;
  };

  for (const Elf_Shdr &SH : Sections) {
    const bool IsAndroidPacked =
        SH.sh_type == SHT_ANDROID_REL || SH.sh_type == SHT_ANDROID_RELA;
    if (IsAndroidPacked) {
      if (!(SH.sh_flags & SHF_ALLOC))
        continue;
      auto RelasOr = ELF.android_relas(SH);
      if (!RelasOr) {
        llvm::consumeError(RelasOr.takeError());
        continue;
      }
      const bool HasExplicitAddend = SH.sh_type == SHT_ANDROID_RELA;
      for (const Elf_Rela &R : *RelasOr) {
        const va_t SlotVA = static_cast<va_t>(R.r_offset);
        const uint32_t Type = R.getType(false);
        const uint32_t Symbol = R.getSymbol(false);
        if (Symbol != 0 || !IsRelative(Type))
          continue;
        uint64_t TargetVA = 0;
        if (HasExplicitAddend) {
          TargetVA = static_cast<typename ELFT::Addr>(R.r_addend);
        } else {
          const uint8_t *Existing = Img.readVA(SlotVA, PtrSize);
          if (!Existing)
            continue;
          TargetVA = readPtr(Existing, ELFT::Is64Bits);
        }
        ApplyPointerSlot(SlotVA, TargetVA);
      }
      continue;
    }

    const bool IsRelr =
        SH.sh_type == SHT_RELR || SH.sh_type == SHT_ANDROID_RELR;
    if (IsRelr) {
      if (!(SH.sh_flags & SHF_ALLOC) || SH.sh_entsize < PtrSize ||
          !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
        continue;

      // RELR alternates direct slot addresses with bitmaps for the following
      // pointer-sized words.  The slot itself carries the implicit addend, so
      // in NeverD's link-time address model the patched target is its current
      // value (the runtime load bias is deliberately zero here).
      const size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
      va_t Cursor = 0;
      bool HaveCursor = false;
      constexpr unsigned BitmapBits = PtrSize * 8 - 1;
      constexpr uint64_t BitmapSpan = uint64_t(BitmapBits) * PtrSize;
      for (size_t I = 0; I < Count; ++I) {
        const uint64_t EntryOff =
            SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
        if (!rangeInBounds(EntryOff, PtrSize, Size))
          break;
        const uint64_t Entry =
            readPtr(Data + static_cast<size_t>(EntryOff), ELFT::Is64Bits);
        if ((Entry & 1) == 0) {
          if (const uint8_t *P = Img.readVA(Entry, PtrSize))
            ApplyPointerSlot(Entry, readPtr(P, ELFT::Is64Bits));
          HaveCursor = Entry <= InvalidVA - PtrSize;
          if (HaveCursor)
            Cursor = Entry + PtrSize;
          continue;
        }
        if (!HaveCursor)
          continue;

        const uint64_t Bitmap = Entry >> 1;
        for (unsigned Bit = 0; Bit < BitmapBits; ++Bit) {
          if ((Bitmap & (uint64_t{1} << Bit)) == 0)
            continue;
          const uint64_t Delta = uint64_t(Bit) * PtrSize;
          if (Cursor > InvalidVA - Delta)
            break;
          const va_t SlotVA = Cursor + Delta;
          if (const uint8_t *P = Img.readVA(SlotVA, PtrSize))
            ApplyPointerSlot(SlotVA, readPtr(P, ELFT::Is64Bits));
        }
        HaveCursor = Cursor <= InvalidVA - BitmapSpan;
        if (HaveCursor)
          Cursor += BitmapSpan;
      }
      continue;
    }

    const bool IsRela = SH.sh_type == SHT_RELA;
    if ((!IsRela && SH.sh_type != SHT_REL) || !(SH.sh_flags & SHF_ALLOC))
      continue;
    const size_t EntrySize = IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
    if (SH.sh_entsize < EntrySize ||
        !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
      continue;

    // The symbols the section's relocations name, for slots bound to a
    // definition in this file.
    llvm::ArrayRef<typename ELFT::Sym> Symbols;
    if (SH.sh_link < Sections.size()) {
      if (auto SymsOr = ELF.symbols(&Sections[SH.sh_link]))
        Symbols = *SymsOr;
      else
        llvm::consumeError(SymsOr.takeError());
    }
    const size_t Count = static_cast<size_t>(SH.sh_size / SH.sh_entsize);
    for (size_t I = 0; I < Count; ++I) {
      const uint64_t EntryOff =
          SH.sh_offset + static_cast<uint64_t>(I) * SH.sh_entsize;
      va_t SlotVA = 0;
      uint32_t Type = 0;
      uint32_t Symbol = 0;
      uint64_t TargetVA = 0;
      if (IsRela) {
        if (!rangeInBounds(EntryOff, sizeof(Elf_Rela), Size))
          break;
        Elf_Rela R;
        std::memcpy(&R, Data + static_cast<size_t>(EntryOff), sizeof(R));
        SlotVA = static_cast<va_t>(R.r_offset);
        Type = R.getType(false);
        Symbol = R.getSymbol(false);
        TargetVA = static_cast<typename ELFT::Addr>(R.r_addend);
      } else {
        if (!rangeInBounds(EntryOff, sizeof(Elf_Rel), Size))
          break;
        Elf_Rel R;
        std::memcpy(&R, Data + static_cast<size_t>(EntryOff), sizeof(R));
        SlotVA = static_cast<va_t>(R.r_offset);
        Type = R.getType(false);
        Symbol = R.getSymbol(false);
        const uint8_t *Existing = Img.readVA(SlotVA, PtrSize);
        if (!Existing)
          continue;
        TargetVA = readPtr(Existing, ELFT::Is64Bits);
      }
      if (Symbol == 0) {
        if (IsRelative(Type))
          ApplyPointerSlot(SlotVA, TargetVA);
        continue;
      }
      // A slot bound to a symbol this file defines: the dynamic linker
      // writes its address -- or, for a GNU indirect function, what its
      // resolver returns -- unless another module interposes a definition.
      // TargetVA holds the explicit or the implicit addend.
      if (Symbol >= Symbols.size())
        continue;
      const typename ELFT::Sym &Sym = Symbols[Symbol];
      if (Sym.st_shndx == llvm::ELF::SHN_UNDEF ||
          Sym.st_shndx >= llvm::ELF::SHN_LORESERVE || Sym.st_value == 0 ||
          Sym.getType() == llvm::ELF::STT_TLS)
        continue;
      const std::optional<uint64_t> Value = elf_loader::symbolRelocationValue(
          Img.Arch, Type, Sym.st_value, static_cast<int64_t>(TargetVA));
      if (!Value)
        continue;
      if (Sym.getType() == llvm::ELF::STT_GNU_IFUNC)
        Img.IndirectFunctionSlots.insert(SlotVA);
      else
        ApplyPointerSlot(SlotVA, *Value);
    }
  }
}

template <typename ELFT>
llvm::Error applyRelocations(const llvm::object::ELFFile<ELFT> &ELF,
                             llvm::ArrayRef<typename ELFT::Shdr> Sections,
                             const uint8_t *Data, size_t Size,
                             const std::vector<va_t> &SecBase,
                             bool IsRelocatable, const ObjectExterns &Externs,
                             BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;

  struct I386RelocationFieldState {
    int32_t OriginalAddend = 0;
    size_t ValueWriterCount = 0;
    bool HasGOTPCWriter = false;
  };
  std::map<va_t, I386RelocationFieldState> I386RelocationFields;
  std::map<va_t, size_t> I386RelocationByteWriterCounts;
  std::set<va_t> I386GOTPCWriterStarts;
  std::set<va_t> I386GOTOFFWriterStarts;

  // ELF32 REL entries all read their addend from the encoded field.  Snapshot
  // every i386 field before applying anything, and count all non-NONE
  // relocation records that can claim that mapped location.  A real linker
  // evaluates multiple REL records from the original section bytes; feeding
  // one relocation's write-back into the next changes the expression and can
  // manufacture an exact GOTPC/GOTOFF occurrence.  Never publish provenance
  // for a multiply-owned field: this loader does not model the linker's
  // composition rules for such inputs. The same scan certifies the complete
  // write footprint for supported ELF object architectures. An unknown writer
  // disables that certificate, so absence cannot be mistaken for scalar proof.
  if (hasObjectRelocationWriters(Img.Arch)) {
    if (IsRelocatable || Img.Arch == Arch::X86) {
      Img.ObjectRelocationWriteBytes.reset();
      if (IsRelocatable)
        Img.ObjectRelocationWriteBytes.emplace();
      for (const Elf_Shdr &RelocSH : Sections) {
        if (RelocSH.sh_type == SHT_ANDROID_REL ||
            RelocSH.sh_type == SHT_ANDROID_RELA ||
            RelocSH.sh_type == SHT_RELR ||
            RelocSH.sh_type == SHT_ANDROID_RELR ||
            RelocSH.sh_type == SHT_AARCH64_AUTH_RELR)
          Img.ObjectRelocationWriteBytes.reset();
        const bool PreIsRela = RelocSH.sh_type == SHT_RELA;
        if (!PreIsRela && RelocSH.sh_type != SHT_REL)
          continue;
        const size_t MinEntrySize =
            PreIsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
        if (RelocSH.sh_entsize < MinEntrySize ||
            RelocSH.sh_size % RelocSH.sh_entsize != 0 ||
            !rangeInBounds(RelocSH.sh_offset, RelocSH.sh_size, Size)) {
          Img.ObjectRelocationWriteBytes.reset();
          continue;
        }

        const Elf_Shdr *PreApplySH = getShdr<ELFT>(Sections, RelocSH.sh_info);
        if (!PreApplySH) {
          Img.ObjectRelocationWriteBytes.reset();
          continue;
        }
        if (!(PreApplySH->sh_flags & SHF_ALLOC))
          continue;
        const va_t PreApplyVA = sectionVA<ELFT>(IsRelocatable, SecBase,
                                                *PreApplySH, RelocSH.sh_info);
        const Segment *PreApplySeg = nullptr;
        for (const Segment &Seg : Img.Segments)
          if (Seg.VA == PreApplyVA && !Seg.Data.empty()) {
            PreApplySeg = &Seg;
            break;
          }
        if (!PreApplySeg) {
          Img.ObjectRelocationWriteBytes.reset();
          continue;
        }

        const size_t PreCount =
            static_cast<size_t>(RelocSH.sh_size / RelocSH.sh_entsize);
        for (size_t I = 0; I < PreCount; ++I) {
          const uint64_t EntryOff =
              RelocSH.sh_offset + static_cast<uint64_t>(I) * RelocSH.sh_entsize;
          va_t FieldOffset = 0;
          uint32_t Type = R_386_NONE;
          if (PreIsRela) {
            if (!rangeInBounds(EntryOff, sizeof(Elf_Rela), Size))
              break;
            Elf_Rela R;
            std::memcpy(&R, Data + static_cast<size_t>(EntryOff), sizeof(R));
            FieldOffset = R.r_offset;
            Type = R.getType(false);
          } else {
            if (!rangeInBounds(EntryOff, sizeof(Elf_Rel), Size))
              break;
            Elf_Rel R;
            std::memcpy(&R, Data + static_cast<size_t>(EntryOff), sizeof(R));
            FieldOffset = R.r_offset;
            Type = R.getType(false);
          }
          const ObjectRelocationWriterFootprint Footprint =
              objectRelocationWriterFootprint(Img.Arch, Type);
          if (!Footprint.Exact)
            Img.ObjectRelocationWriteBytes.reset();
          if (Footprint.Width == 0)
            continue;
          const std::optional<va_t> WriterOffset =
              addSignedOffsetBias(FieldOffset, Footprint.OffsetBias);
          if (!WriterOffset ||
              !rangeInBounds(*WriterOffset, Footprint.Width,
                             PreApplySeg->Data.size()) ||
              *WriterOffset > InvalidVA - PreApplyVA) {
            Img.ObjectRelocationWriteBytes.reset();
            continue;
          }

          const va_t FieldVA = PreApplyVA + *WriterOffset;
          for (size_t Byte = 0; Byte < Footprint.Width; ++Byte) {
            if (Byte > InvalidVA - FieldVA) {
              Img.ObjectRelocationWriteBytes.reset();
              break;
            }
            if (Img.Arch == Arch::X86)
              ++I386RelocationByteWriterCounts[FieldVA + Byte];
            if (Img.ObjectRelocationWriteBytes)
              Img.ObjectRelocationWriteBytes->insert(FieldVA + Byte);
          }
          if (Img.Arch == Arch::X86 && Type == R_386_GOTPC)
            I386GOTPCWriterStarts.insert(FieldVA);
          if (Img.Arch == Arch::X86 && Type == R_386_GOTOFF)
            I386GOTOFFWriterStarts.insert(FieldVA);
          if (Img.Arch == Arch::X86 && Footprint.Width == 4) {
            auto [It, Inserted] = I386RelocationFields.try_emplace(FieldVA);
            if (Inserted)
              std::memcpy(&It->second.OriginalAddend,
                          PreApplySeg->Data.data() + *WriterOffset, 4);
            ++It->second.ValueWriterCount;
            It->second.HasGOTPCWriter |= Type == R_386_GOTPC;
          }
        }
      }

      // Publish ambiguity before applying any record.  The set is a negative
      // certificate only: it lets the exact decoded use fail closed, but can
      // never authenticate GOT-base zero or any relocated address.
      for (va_t FieldVA : I386GOTPCWriterStarts) {
        bool HasUniqueFourByteOwner = true;
        for (size_t Byte = 0; Byte < 4; ++Byte) {
          const auto Count =
              I386RelocationByteWriterCounts.find(FieldVA + Byte);
          if (Count == I386RelocationByteWriterCounts.end() ||
              Count->second != 1) {
            HasUniqueFourByteOwner = false;
            break;
          }
        }
        if (!HasUniqueFourByteOwner)
          Img.AmbiguousI386GOTPCFields.insert(FieldVA);
      }
      for (va_t FieldVA : I386GOTOFFWriterStarts) {
        bool HasUniqueFourByteOwner = true;
        for (size_t Byte = 0; Byte < 4; ++Byte) {
          const auto Count =
              I386RelocationByteWriterCounts.find(FieldVA + Byte);
          if (Count == I386RelocationByteWriterCounts.end() ||
              Count->second != 1) {
            HasUniqueFourByteOwner = false;
            break;
          }
        }
        if (!HasUniqueFourByteOwner)
          Img.AmbiguousI386GOTOFFFields.insert(FieldVA);
      }
    }
  }

  for (const Elf_Shdr &SH : Sections) {
    bool IsRela = (SH.sh_type == SHT_RELA);
    if (!IsRela && SH.sh_type != SHT_REL)
      continue;
    const size_t MinEntrySize = IsRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
    if (SH.sh_entsize < MinEntrySize ||
        !rangeInBounds(SH.sh_offset, SH.sh_size, Size))
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
    llvm::StringRef SymStrTab;
    if (SymSH2 &&
        (!Externs.SymbolSlots.empty() || !Externs.CommonSlots.empty())) {
      if (auto TabOr = ELF.getStringTableForSymtab(*SymSH2))
        SymStrTab = *TabOr;
      else
        llvm::consumeError(TabOr.takeError());
    }
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
      va_t SymOwnerVA = InvalidVA;
      bool SymIsFunction = false;
      if (SymSH2 && RSym > 0) {
        auto SymsOr = ELF.symbols(SymSH2);
        if (!SymsOr) {
          llvm::consumeError(SymsOr.takeError());
        } else if (RSym < SymsOr->size()) {
          const Elf_Sym &Sym = (*SymsOr)[RSym];
          SymVal = Sym.st_value;
          SymIsFunction =
              Sym.getType() == STT_FUNC || Sym.getType() == STT_GNU_IFUNC;
          // An undefined or common symbol resolves to its extern address.  A
          // call reaches the import there; any other reference reaches
          // storage the extern segment owns.
          if ((Sym.st_shndx == SHN_UNDEF || Sym.st_shndx == SHN_COMMON) &&
              !SymStrTab.empty()) {
            const auto &Slots = Sym.st_shndx == SHN_UNDEF ? Externs.SymbolSlots
                                                          : Externs.CommonSlots;
            if (auto NameOr = Sym.getName(SymStrTab)) {
              if (auto It = Slots.find(NameOr->str()); It != Slots.end()) {
                SymVal = It->second;
                if (!isBranchReference(Img.Arch, RType))
                  SymOwnerVA = Externs.ExternBase;
              }
            } else {
              llvm::consumeError(NameOr.takeError());
            }
          }
          if (Sym.st_shndx != SHN_UNDEF && Sym.st_shndx < SHN_LORESERVE) {
            if (const Elf_Shdr *TSH = getShdr<ELFT>(Sections, Sym.st_shndx)) {
              SymOwnerVA =
                  sectionVA<ELFT>(IsRelocatable, SecBase, *TSH, Sym.st_shndx);
              if (SymVal > InvalidVA - SecBase[Sym.st_shndx])
                continue;
              SymVal += SecBase[Sym.st_shndx];
            }
          }
        }
      }

      va_t P = ApplyVA + RAddr;
      va_t S = SymVal;
      // A GOT reference applies its direct form against the GOT entry, which
      // holds the symbol's address.
      if (auto Direct = directTypeOfGOTReference(Img.Arch, RType))
        if (auto It = Externs.GOTEntries.find({SH.sh_link, RSym});
            It != Externs.GOTEntries.end()) {
          S = It->second;
          SymOwnerVA = Externs.CellBase;
          SymIsFunction = false;
          RType = *Direct;
        }

      if (RAddr >= ApplySeg->Data.size())
        continue;

      // Remember a relocation that lands in read-only data so the emitter can
      // recognize the resolved VA as a genuine rodata pointer (e.g. an i386
      // GOTOFF `.rodata` base reached through a low VA that the numeric-VA
      // heuristic would otherwise read as an integer literal).
      auto RecordDataTarget = [&](uint64_t TargetVA) {
        const Segment *TSeg = Img.getSegmentFor(TargetVA);
        if (TSeg && TSeg->isReadable() && !TSeg->isWritable() &&
            !Img.hasExecutableCodeOwnerAt(TargetVA) &&
            Img.hasObjectDataProvenance(TargetVA))
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
        const Segment *TSeg = Img.getSegmentFor(TargetVA);
        if (TSeg && TSeg->isWritable() &&
            !Img.hasExecutableCodeOwnerAt(TargetVA))
          Img.WritableRelocDataAddrs.insert(TargetVA);
      };

      // Normalize full-width absolute fields through the shared slot/target
      // classifier.  In particular, an absolute relocation embedded in an
      // instruction proves the referenced address, but the instruction bytes
      // themselves are not a pointer-table slot.  Narrow absolute fields on a
      // wide target likewise cannot be published as pointer slots; when they
      // live in code, retain only the executable target provenance so later
      // consumers can fail closed rather than treating the truncated bits as
      // a complete pointer.
      auto RecordAbsoluteField = [&](uint64_t EncodedValue, uint64_t TargetVA,
                                     uint64_t TargetOwnerVA,
                                     size_t FieldWidth) {
        if (!Img.relocatedTargetBelongsToOwner(TargetVA, TargetOwnerVA))
          return;
        if (FieldWidth == Img.getPointerSize()) {
          recordAbsolutePointerRelocation(Img, P, TargetVA, TargetOwnerVA);
          return;
        }
        if (!Img.hasExecutableCodeOwnerAt(P))
          return;
        const Segment *OwnerSeg = Img.getSegmentFor(TargetOwnerVA);
        if (!OwnerSeg || !OwnerSeg->isReadable())
          return;
        va_t OwnerBegin = OwnerSeg->VA;
        bool OwnerWritable = OwnerSeg->isWritable();
        if (const Section *OwnerSec = Img.getSectionFor(TargetOwnerVA)) {
          OwnerBegin = OwnerSec->VA;
          OwnerWritable =
              OwnerSec->isWritable() &&
              !section_names::isReadOnlyAfterRelocSectionName(OwnerSec->Name) &&
              !section_names::isReadOnlyAfterRelocSectionName(
                  OwnerSec->SegmentName);
        }
        const bool OwnerIsCode = Img.hasExecutableCodeOwnerAt(TargetOwnerVA);
        const RelocatedAddressField Field{EncodedValue, TargetVA,
                                          static_cast<uint8_t>(FieldWidth),
                                          OwnerBegin};
        if (OwnerIsCode) {
          Img.DataAddressRelocOperands.erase(P);
          Img.CodeAddressRelocOperands[P] = Field;
          Img.CodeRefTargets.insert(
              normalizeCodeAddress(TargetVA, Img.Arch, Img.Mode));
          return;
        }
        Img.CodeAddressRelocOperands.erase(P);
        Img.DataAddressRelocOperands[P] = Field;
        if (OwnerWritable)
          Img.WritableRelocDataAddrs.insert(TargetVA);
        else
          Img.RelocDataAddrs.insert(TargetVA);
      };

      auto RecordPCRelativeInstructionField = [&](uint64_t EncodedValue,
                                                  uint64_t TargetOwnerVA,
                                                  size_t FieldWidth) {
        if (!Img.hasExecutableCodeOwnerAt(P) || TargetOwnerVA == InvalidVA ||
            FieldWidth == 0 || FieldWidth > sizeof(uint64_t))
          return;
        const Section *OwnerSec = Img.getSectionFor(TargetOwnerVA);
        const Segment *OwnerSeg = Img.getSegmentFor(TargetOwnerVA);
        if ((!OwnerSec || OwnerSec->VA != TargetOwnerVA) &&
            (!OwnerSeg || OwnerSeg->VA != TargetOwnerVA))
          return;
        const bool OwnerIsCode =
            OwnerSec ? OwnerSec->isExecutable() : OwnerSeg->isExecutable();
        const RelocatedAddressField Field{EncodedValue, InvalidVA,
                                          static_cast<uint8_t>(FieldWidth),
                                          TargetOwnerVA, true};
        if (OwnerIsCode) {
          Img.DataAddressRelocOperands.erase(P);
          Img.CodeAddressRelocOperands[P] = Field;
        } else {
          Img.CodeAddressRelocOperands.erase(P);
          Img.DataAddressRelocOperands[P] = Field;
        }
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
        const bool HasMappedSlotOwner =
            Img.getSectionFor(P) != nullptr ||
            (PSeg && !Img.segmentHasReadableSectionMetadata(*PSeg));
        if (PSeg && PSeg->isReadable() && !PSeg->isWritable() &&
            !Img.hasExecutableCodeOwnerAt(P) && HasMappedSlotOwner && TSeg &&
            TSeg->isReadable() && !TSeg->isWritable() &&
            !Img.hasExecutableCodeOwnerAt(TargetVA) &&
            Img.hasObjectDataProvenance(TargetVA))
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
        const bool HasMappedSlotOwner =
            Img.getSectionFor(P) != nullptr ||
            (PSeg && !Img.segmentHasReadableSectionMetadata(*PSeg));
        if (PSeg && PSeg->isReadable() && !PSeg->isWritable() &&
            !Img.hasExecutableCodeOwnerAt(P) && HasMappedSlotOwner &&
            Img.hasExecutableCodeOwnerAt(SymVA))
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
        if (PSeg && Img.hasExecutableCodeOwnerAt(P) && TSeg &&
            TSeg->isReadable() && !TSeg->isWritable() &&
            !Img.hasExecutableCodeOwnerAt(AnchorVA) &&
            Img.hasObjectDataProvenance(AnchorVA))
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
      // would be mis-symbolized. A mapped VA 0 is accepted only here, where
      // the relocation itself distinguishes an address from an integer null.
      auto RecordCodeRef = [&](uint64_t TargetVA) {
        if (Img.hasExecutableCodeOwnerAt(TargetVA))
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
          if (RType == R_X86_64_PC32 || RType == R_X86_64_PLT32 ||
              RType == R_X86_64_GOTPCREL) {
            RecordPCRelativeInstructionField(static_cast<uint32_t>(Val),
                                             SymOwnerVA, 4);
          }
          if (RType == R_X86_64_PC32 || RType == R_AARCH64_PREL32)
            RecordRelCodePtr(S);
          if (RType == R_X86_64_PC32 || RType == R_AARCH64_PREL32)
            RecordRelDataPtr(S);
          // Do not publish `S + A + 4` as a global address/anchor here.  Four
          // is the displacement width, not necessarily the distance from the
          // relocation field to the instruction end (for example a RIP-
          // relative compare can encode an immediate after the displacement).
          // CFG lifting recomputes `InsnEnd + sext(encoded)` after exact
          // operand matching and records the occurrence-local target instead.
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
          RecordAbsoluteField(Val, Val, SymOwnerVA, 8);
        } else if (RType == R_X86_64_32 || RType == R_X86_64_32S) {
          if (RAddr + 4 > ApplySeg->Data.size())
            continue;
          const uint64_t Full = S + RAddend;
          const uint32_t Val = static_cast<uint32_t>(Full);
          std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          const uint64_t Extended =
              RType == R_X86_64_32S
                  ? static_cast<uint64_t>(
                        static_cast<int64_t>(static_cast<int32_t>(Val)))
                  : static_cast<uint64_t>(Val);
          // ELF linkers reject a relocation overflow.  This loader has no
          // diagnostic channel here, so retain the historical written bytes
          // but never publish a truncated field as complete address
          // provenance.
          if (Extended == Full) {
            RecordAbsoluteField(Val, Full, SymOwnerVA, 4);
          }
        } else if (RType == R_AARCH64_CALL26 || RType == R_AARCH64_JUMP26) {
          if (RAddr + 4 > ApplySeg->Data.size())
            continue;
          int64_t Disp = static_cast<int64_t>(S + RAddend - P);
          uint32_t Insn;
          std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
          Insn = branch::withA64BranchDisplacement(Insn, Disp);
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
          const va_t PageTarget = S + RAddend;
          if (Img.hasExecutableCodeOwnerAt(P) &&
              Img.relocatedTargetBelongsToOwner(PageTarget, SymOwnerVA))
            Img.InstructionPageAddressFragments[P] = {PageTarget, SymOwnerVA};
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
          if (RType == R_AARCH64_ADD_ABS_LO12_NC) {
            RecordCodeRef(S + RAddend);
            const va_t MaterializedTarget = S + RAddend;
            if (Img.hasExecutableCodeOwnerAt(P) &&
                Img.relocatedTargetBelongsToOwner(MaterializedTarget,
                                                  SymOwnerVA))
              Img.InstructionAddressMaterializations[P] = {MaterializedTarget,
                                                           SymOwnerVA};
          }
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
          const auto Field = I386RelocationFields.find(P);
          if (Img.Arch == Arch::X86 && Field != I386RelocationFields.end()) {
            InPlace = Field->second.OriginalAddend;
          } else {
            int32_t Existing;
            std::memcpy(&Existing, ApplySeg->Data.data() + RAddr, 4);
            InPlace = Existing;
          }
        }
        if (RAddr + 4 > ApplySeg->Data.size())
          continue;
        auto Put32 = [&](uint32_t V) {
          std::memcpy(ApplySeg->Data.data() + RAddr, &V, 4);
        };
        if (Img.Arch == Arch::X86) {
          const auto Field = I386RelocationFields.find(P);
          bool HasUniqueFourByteSpan = true;
          for (size_t Byte = 0; Byte < 4; ++Byte) {
            const auto Count = I386RelocationByteWriterCounts.find(P + Byte);
            if (Count == I386RelocationByteWriterCounts.end() ||
                Count->second != 1) {
              HasUniqueFourByteSpan = false;
              break;
            }
          }
          const bool HasUniqueValueWriter =
              Field != I386RelocationFields.end() &&
              Field->second.ValueWriterCount == 1 && HasUniqueFourByteSpan &&
              !Img.AmbiguousI386GOTPCFields.count(P) &&
              !Img.AmbiguousI386GOTOFFFields.count(P);
          // i386 PIC: model _GLOBAL_OFFSET_TABLE_ at base 0.  The get-PC seed
          // (lifted to the constant next-PC) plus GOTPC then fold to the GOT
          // base, and GOTOFF folds back to the symbol VA — the base choice
          // cancels (eax=GOTbase; eax+GOTOFF=symbol), so the constant-pool
          // load address becomes a known rodata VA the emitter redirects.
          switch (RType) {
          case R_386_GOTOFF: {
            // Under the loader's GOT-base-zero model a GOTOFF field contains
            // the complete folded address S+A.  Its semantic owner is still
            // the relocation symbol S, not the segment in which the biased
            // numeric result happens to land: a lookup table at .rodata+0x60
            // with addend -0x10 legitimately encodes 0x50 inside .text.
            // Likewise, a field stored in data is a pointer-table slot, not an
            // instruction operand.  Decide both axes before publishing any
            // downstream provenance so one occurrence never claims code and
            // data simultaneously.
            uint32_t Folded = static_cast<uint32_t>(S + InPlace);
            Put32(Folded);
            if (!HasUniqueValueWriter)
              break;
            const va_t SymbolVA = static_cast<uint32_t>(S);
            const va_t SymbolOwnerAnchor = SymOwnerVA;
            const bool PlaceIsCode = Img.hasExecutableCodeOwnerAt(P);
            const Segment *PlaceSeg = Img.getSegmentFor(P);
            const bool PlaceIsData =
                PlaceSeg && PlaceSeg->isReadable() && !PlaceIsCode;
            const bool SymbolIsCode =
                SymbolOwnerAnchor != InvalidVA &&
                Img.hasExecutableCodeOwnerAt(SymbolOwnerAnchor);
            const Segment *SymSeg = SymbolOwnerAnchor != InvalidVA
                                        ? Img.getSegmentFor(SymbolOwnerAnchor)
                                        : nullptr;
            const bool SymbolIsData =
                SymSeg && SymSeg->isReadable() && !SymbolIsCode;

            if (PlaceIsCode && SymbolIsCode) {
              RecordCodeRef(Folded);
              Img.CodeAddressRelocOperands[P] = {Folded, Folded, 4,
                                                 SymbolOwnerAnchor};
            } else if (PlaceIsCode && SymbolIsData) {
              RecordDataTarget(Folded);
              RecordWritableDataTarget(Folded);
              Img.DataAddressRelocOperands[P] = {
                  Folded,
                  Folded,
                  4,
                  SymbolOwnerAnchor,
                  /*PCRelativeFromInstructionEnd=*/false,
                  RelocatedAddressFieldKind::I386ELFGOTOFF};
              // `leal table@GOTOFF(%ebx)` anchors a PIC switch table's base in
              // read-only data; record it so the resolver bounds an over-long
              // RelCodeReloc run at the next table's base.
              RecordRelTableAnchor(Folded);
            } else if (PlaceIsData) {
              recordAbsolutePointerRelocation(Img, P, Folded,
                                              SymbolOwnerAnchor);
            }

            // A switch-to-lookup-table indexed by the unbiased case value
            // folds the base to `table - min_case*stride` — a negative addend
            // that drops the base BEFORE the table's segment (often into
            // .text), so RecordDataTarget rejects it and the bare VA resolves
            // to the wrong segment.  When the *symbol* itself sits in clean
            // rodata, anchor the folded base to the symbol's own segment so
            // the emitter pins `base + runtime_index` back inside the table.
            const uint32_t AnchorBackDistance =
                SymSeg ? static_cast<uint32_t>(SymSeg->VA) - Folded : 0;
            if (PlaceIsCode && SymbolIsData && SymSeg &&
                !SymSeg->isWritable() &&
                Img.hasObjectDataProvenance(SymbolVA) &&
                !SymSeg->Data.empty() && AnchorBackDistance != 0 &&
                AnchorBackDistance <= limits::kMaxRodataAnchorBackDistance) {
              Img.RodataAnchorSeg[Folded] = SymSeg->VA;
              Img.DataAddressRelocOperands[P] = {
                  Folded,
                  Folded,
                  4,
                  SymbolOwnerAnchor,
                  /*PCRelativeFromInstructionEnd=*/false,
                  RelocatedAddressFieldKind::I386ELFGOTOFF};
            }
            break;
          }
          case R_386_32:
            // Absolute 32-bit slot — a data word or a function-pointer
            // *table* entry (handled by the code-pointer table mirror), not a
            // register materialization, so it is not recorded as a scalar
            // code ref.
            Put32(static_cast<uint32_t>(S + InPlace));
            if (HasUniqueValueWriter)
              RecordAbsoluteField(static_cast<uint32_t>(S + InPlace),
                                  static_cast<uint32_t>(S + InPlace),
                                  SymOwnerVA, 4);
            break;
          case R_386_PC32:
          case R_386_PLT32:
            Put32(static_cast<uint32_t>(S + InPlace - P));
            if (HasUniqueValueWriter && RType == R_386_PC32)
              RecordRelDataPtr(S);
            break;
          case R_386_GOTPC: {
            const uint32_t Encoded = static_cast<uint32_t>(InPlace - P);
            Put32(Encoded);
            // Keep the mapped field occurrence.  RelocationEntry::Address is
            // the section-relative r_offset in ET_REL and cannot safely be
            // matched against a decoded instruction VA later.  The additive
            // inverse is the exact get-PC seed this relocation expects under
            // the loader's model-zero GOT convention.
            if (HasUniqueValueWriter && Img.hasExecutableCodeOwnerAt(P))
              Img.I386GOTPCFields[P] = {
                  Encoded, static_cast<uint32_t>(uint32_t{0} - Encoded)};
            break;
          }
          default:
            break;
          }
        } else if (RType == R_ARM_MOVW_ABS_NC || RType == R_ARM_MOVT_ABS) {
          uint32_t Insn = 0;
          std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
          const uint32_t ExpectedOpcode =
              RType == R_ARM_MOVW_ABS_NC ? 0x03000000u : 0x03400000u;
          if ((Insn & 0x0ff00000u) != ExpectedOpcode)
            continue;
          const uint16_t Encoded =
              static_cast<uint16_t>(((Insn >> 4) & 0xf000u) | (Insn & 0x0fffu));
          const int64_t Addend =
              IsRela ? RAddend : static_cast<int16_t>(Encoded);
          uint64_t Target = S;
          if (Addend >= 0) {
            if (static_cast<uint64_t>(Addend) > InvalidVA - Target)
              continue;
            Target += static_cast<uint64_t>(Addend);
          } else {
            const uint64_t Magnitude = static_cast<uint64_t>(-(Addend + 1)) + 1;
            if (Magnitude > Target)
              continue;
            Target -= Magnitude;
          }
          const uint16_t Result = static_cast<uint16_t>(
              RType == R_ARM_MOVW_ABS_NC ? Target : Target >> 16);
          Insn = (Insn & ~0x000f0fffu) |
                 ((static_cast<uint32_t>(Result) & 0xf000u) << 4) |
                 (static_cast<uint32_t>(Result) & 0x0fffu);
          Put32(Insn);
          RecordCodeRef(Target);
          RecordDataTarget(Target);
          RecordWritableDataTarget(Target);
        } else if (RType == R_ARM_THM_MOVW_ABS_NC ||
                   RType == R_ARM_THM_MOVT_ABS) {
          uint16_t Hi = 0, Lo = 0;
          std::memcpy(&Hi, ApplySeg->Data.data() + RAddr, 2);
          std::memcpy(&Lo, ApplySeg->Data.data() + RAddr + 2, 2);
          const uint16_t ExpectedOpcode =
              RType == R_ARM_THM_MOVW_ABS_NC ? 0xf240u : 0xf2c0u;
          if ((Hi & 0xfbf0u) != ExpectedOpcode)
            continue;
          const uint16_t Encoded = static_cast<uint16_t>(
              ((Hi & 0x000fu) << 12) | ((Hi & 0x0400u) << 1) |
              ((Lo & 0x7000u) >> 4) | (Lo & 0x00ffu));
          const int64_t Addend =
              IsRela ? RAddend : static_cast<int16_t>(Encoded);
          uint64_t Target = S;
          if (Addend >= 0) {
            if (static_cast<uint64_t>(Addend) > InvalidVA - Target)
              continue;
            Target += static_cast<uint64_t>(Addend);
          } else {
            const uint64_t Magnitude = static_cast<uint64_t>(-(Addend + 1)) + 1;
            if (Magnitude > Target)
              continue;
            Target -= Magnitude;
          }
          const uint16_t Result = static_cast<uint16_t>(
              RType == R_ARM_THM_MOVW_ABS_NC ? Target : Target >> 16);
          Hi = static_cast<uint16_t>((Hi & ~0x040fu) |
                                     ((Result >> 12) & 0x000fu) |
                                     ((Result >> 1) & 0x0400u));
          Lo = static_cast<uint16_t>(
              (Lo & ~0x70ffu) | ((Result << 4) & 0x7000u) | (Result & 0x00ffu));
          std::memcpy(ApplySeg->Data.data() + RAddr, &Hi, 2);
          std::memcpy(ApplySeg->Data.data() + RAddr + 2, &Lo, 2);
          RecordCodeRef(Target);
          RecordDataTarget(Target);
          RecordWritableDataTarget(Target);
        } else if (RType == R_ARM_ABS32) {
          // Absolute 32-bit slot — a function-pointer table entry (handled by
          // the code-pointer table mirror) or a literal-pool word, not a
          // register materialization, so not recorded as a scalar code ref.
          Put32(static_cast<uint32_t>(S + InPlace));
          RecordAbsoluteField(static_cast<uint32_t>(S + InPlace),
                              static_cast<uint32_t>(S + InPlace), SymOwnerVA,
                              4);
        } else if (RType == R_ARM_REL32) {
          const uint32_t Encoded = static_cast<uint32_t>(S + InPlace - P);
          Put32(Encoded);
          RecordRelDataPtr(S);
          // ARM32 non-PIC takes a writable global's address via a PC-relative
          // literal pool (`ldr rN,[pc]; add rN,pc,rN`), emitted as
          // R_ARM_REL32 on the literal word.  The TARGET is the symbol VA S
          // (the in-place addend is the literal-to-symbol delta, not part of
          // the address), so record S — the base a runtime-indexed `&g[i]`
          // resolves to.
          RecordWritableDataTarget(S);
          if (SymOwnerVA != InvalidVA &&
              Img.relocatedTargetBelongsToOwner(S, SymOwnerVA)) {
            const AppliedARMRelativeLiteral Applied{Encoded, S, SymOwnerVA};
            auto [It, Inserted] =
                Img.ARMRelativeLiteralFields.emplace(P, Applied);
            if (!Inserted && It->second != Applied) {
              // Multiple incompatible relocations for one applied field are
              // malformed.  Keep an explicit invalid tombstone so a later
              // duplicate cannot accidentally re-enable the occurrence.
              It->second = {};
            }
          }
        } else if (RType == R_ARM_CALL || RType == R_ARM_JUMP24 ||
                   RType == R_ARM_PC24) {
          // Branch REL addends occupy the signed imm24 field, not the whole
          // instruction word. A function symbol's low bit selects the target
          // state; an unconditional call may change BL to BLX or back.
          uint32_t Insn;
          std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
          const int64_t Addend =
              IsRela ? RAddend : static_cast<int32_t>(Insn << 8) >> 6;
          const va_t BranchS = SymIsFunction ? clearThumbBit(S) : S;
          const int64_t Disp =
              static_cast<int64_t>(BranchS) + Addend - static_cast<int64_t>(P);
          const bool WasBLX = (Insn & 0xfe000000u) == 0xfa000000u;
          const bool ToThumb = SymIsFunction ? (S & 1u) != 0 : WasBLX;
          // BLX is unconditional; a malformed relocation must not erase a
          // conditional BL's predicate while changing its target state.
          if (RType == R_ARM_CALL && !WasBLX &&
              (Insn & 0xff000000u) != 0xeb000000u)
            return llvm::make_error<llvm::StringError>(
                "elf: malformed ARM call relocation",
                llvm::inconvertibleErrorCode());
          if (Disp < -(int64_t{1} << 25) || Disp >= (int64_t{1} << 25))
            return llvm::make_error<llvm::StringError>(
                "elf: ARM branch relocation exceeds direct range",
                llvm::inconvertibleErrorCode());
          if (RType == R_ARM_CALL) {
            if (ToThumb) {
              Put32(0xfa000000u | ((static_cast<uint32_t>(Disp) & 2u) << 23) |
                    ((static_cast<uint32_t>(Disp) >> 2) & 0x00ffffffu));
            } else {
              if ((Disp & 3) != 0)
                return llvm::make_error<llvm::StringError>(
                    "elf: unaligned ARM call relocation",
                    llvm::inconvertibleErrorCode());
              Put32(0xeb000000u |
                    ((static_cast<uint32_t>(Disp) >> 2) & 0x00ffffffu));
            }
          } else {
            if (ToThumb || (Disp & 3) != 0)
              return llvm::make_error<llvm::StringError>(
                  "elf: ARM branch needs an unsupported interworking veneer",
                  llvm::inconvertibleErrorCode());
            Put32((Insn & 0xff000000u) |
                  ((static_cast<uint32_t>(Disp) >> 2) & 0x00ffffffu));
          }
        } else if (RType == R_ARM_THM_CALL || RType == R_ARM_THM_JUMP24 ||
                   RType == R_ARM_THM_JUMP19) {
          uint16_t Hi, Lo;
          std::memcpy(&Hi, ApplySeg->Data.data() + RAddr, 2);
          std::memcpy(&Lo, ApplySeg->Data.data() + RAddr + 2, 2);
          const bool Conditional = RType == R_ARM_THM_JUMP19;
          if ((Hi & 0xf800u) != 0xf000u ||
              (Conditional               ? (Lo & 0xd000u) != 0x8000u
               : RType == R_ARM_THM_CALL ? (Lo & 0xc000u) != 0xc000u
                                         : (Lo & 0xd000u) != 0x9000u))
            return llvm::make_error<llvm::StringError>(
                "elf: malformed Thumb branch relocation",
                llvm::inconvertibleErrorCode());
          if (RType != R_ARM_THM_CALL && SymIsFunction && !(S & 1u))
            return llvm::make_error<llvm::StringError>(
                "elf: Thumb branch needs an unsupported interworking veneer",
                llvm::inconvertibleErrorCode());
          if (Conditional) {
            const uint32_t Encoded =
                ((Hi & 0x0400u) << 10) | ((Lo & 0x0800u) << 8) |
                ((Lo & 0x2000u) << 5) | ((Hi & 0x003fu) << 12) |
                ((Lo & 0x07ffu) << 1);
            const int64_t Addend =
                IsRela ? RAddend : static_cast<int32_t>(Encoded << 12) >> 12;
            const va_t BranchS = SymIsFunction ? clearThumbBit(S) : S;
            const int64_t Disp = static_cast<int64_t>(BranchS) + Addend -
                                 static_cast<int64_t>(P);
            if (Disp < -(int64_t{1} << 20) || Disp >= (int64_t{1} << 20))
              return llvm::make_error<llvm::StringError>(
                  "elf: Thumb conditional branch exceeds direct range",
                  llvm::inconvertibleErrorCode());
            const uint32_t Value = static_cast<uint32_t>(Disp);
            Hi = static_cast<uint16_t>((Hi & 0xfbc0u) |
                                       ((Value >> 10) & 0x0400u) |
                                       ((Value >> 12) & 0x003fu));
            Lo = static_cast<uint16_t>(0x8000u | ((Value >> 8) & 0x0800u) |
                                       ((Value >> 5) & 0x2000u) |
                                       ((Value >> 1) & 0x07ffu));
            std::memcpy(ApplySeg->Data.data() + RAddr, &Hi, 2);
            std::memcpy(ApplySeg->Data.data() + RAddr + 2, &Lo, 2);
            continue;
          }
          const uint32_t Sign = (Hi >> 10) & 1u;
          const uint32_t I1 = (~((Lo >> 13) ^ Sign)) & 1u;
          const uint32_t I2 = (~((Lo >> 11) ^ Sign)) & 1u;
          const uint32_t Encoded = (Sign << 24) | (I1 << 23) | (I2 << 22) |
                                   ((Hi & 0x03ffu) << 12) |
                                   ((Lo & 0x07ffu) << 1);
          const int64_t Addend =
              IsRela ? RAddend : static_cast<int32_t>(Encoded << 7) >> 7;
          const va_t BranchS = SymIsFunction ? clearThumbBit(S) : S;
          int64_t Disp =
              static_cast<int64_t>(BranchS) + Addend - static_cast<int64_t>(P);
          const bool WasBLX = (Lo & 0x1000u) == 0;
          const bool ToARM = RType == R_ARM_THM_CALL &&
                             (SymIsFunction ? (S & 1u) == 0 : WasBLX);
          if (ToARM)
            Disp += (4 - (Disp & 3)) & 3;
          if (Disp < -(int64_t{1} << 24) || Disp >= (int64_t{1} << 24))
            return llvm::make_error<llvm::StringError>(
                "elf: Thumb call relocation exceeds direct range",
                llvm::inconvertibleErrorCode());
          const uint32_t Value = static_cast<uint32_t>(Disp);
          Hi = static_cast<uint16_t>(0xf000u | ((Value >> 14) & 0x0400u) |
                                     ((Value >> 12) & 0x03ffu));
          Lo = static_cast<uint16_t>(
              (RType == R_ARM_THM_JUMP24 ? 0x9000u
               : ToARM                   ? 0xc000u
                                         : 0xd000u) |
              (((~(Value >> 10)) ^ (Value >> 11)) & 0x2000u) |
              (((~(Value >> 11)) ^ (Value >> 13)) & 0x0800u) |
              ((Value >> 1) & 0x07ffu));
          std::memcpy(ApplySeg->Data.data() + RAddr, &Hi, 2);
          std::memcpy(ApplySeg->Data.data() + RAddr + 2, &Lo, 2);
        }
      }
    }
  }
  return llvm::Error::success();
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

template llvm::Error applyRelocations<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const uint8_t *, size_t,
    const std::vector<va_t> &, bool, const ObjectExterns &, BinaryImage &);
template llvm::Error applyRelocations<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const uint8_t *, size_t,
    const std::vector<va_t> &, bool, const ObjectExterns &, BinaryImage &);

template void applyDynamicRelativeRelocations<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const uint8_t *, size_t,
    BinaryImage &);
template void applyDynamicRelativeRelocations<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const uint8_t *, size_t,
    BinaryImage &);

} // namespace detail

std::optional<size_t> objectRelocationFieldWidth(Arch Target, uint32_t Type) {
  const auto Field = detail::objectRelocationWriterFootprint(Target, Type);
  if (!Field.Exact || Field.OffsetBias != 0)
    return std::nullopt;
  return Field.Width;
}

} // namespace elf_loader
} // namespace neverd
