//===- ELFLoaderUtils.cpp - ELF loader helpers --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/ELF/ELFLoaderUtils.h"

#include "neverd/Object/ELFLayout.h"
#include "neverd/Support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <optional>

#define DEBUG_TYPE "neverd-elf-loader"

namespace neverd {
namespace elf_loader {

using namespace llvm::ELF;

namespace {

va_t normalizeELFFunctionAddress(va_t Addr, const BinaryImage &Img) {
  return Img.Arch == Arch::ARM ? clearThumbBit(Addr) : Addr;
}

bool isIRelativeRelocation(uint32_t Type, Arch TargetArch) {
  switch (TargetArch) {
  case Arch::X86:
    return Type == R_386_IRELATIVE;
  case Arch::X64:
    return Type == R_X86_64_IRELATIVE;
  case Arch::ARM:
    return Type == R_ARM_IRELATIVE;
  case Arch::AArch64:
    return Type == R_AARCH64_IRELATIVE;
  default:
    return false;
  }
}

} // anonymous namespace

void recordRuntimePointerArray(va_t Addr, uint64_t Size, std::vector<va_t> &Out,
                               BinaryImage &Img) {
  const uint32_t PtrSize = Img.getPointerSize();
  if (Size < PtrSize)
    return;

  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable())
    return;
  size_t Offset = static_cast<size_t>(Addr - Seg->VA);
  if (Offset >= Seg->Data.size())
    return;

  uint64_t Count = Size / PtrSize;
  Count = std::min<uint64_t>(Count, (Seg->Data.size() - Offset) / PtrSize);
  for (uint64_t I = 0; I < Count; ++I) {
    if (I > (InvalidVA - Addr) / PtrSize)
      break;
    const uint8_t *P = Img.readVA(Addr + I * PtrSize, PtrSize);
    if (!P)
      break;
    va_t Target = static_cast<va_t>(readPtr(P, Img.is64Bit()));
    if (Target == 0)
      continue;
    Target = normalizeELFFunctionAddress(Target, Img);
    if (!Img.recordRuntimeFunction(Target))
      continue;
    if (std::find(Out.begin(), Out.end(), Target) == Out.end())
      Out.push_back(Target);
  }
}

void parseRuntimeSections(BinaryImage &Img) {
  for (const Section &Sec : Img.Sections) {
    std::vector<va_t> *Out = nullptr;
    if (Sec.Type == SHT_PREINIT_ARRAY ||
        Sec.Name == section_names::elf::PreinitArray)
      Out = &Img.DynInfo.PreinitArray;
    else if (Sec.Type == SHT_INIT_ARRAY ||
             Sec.Name == section_names::elf::InitArray ||
             Sec.Name == section_names::elf::Ctors)
      Out = &Img.DynInfo.InitArray;
    else if (Sec.Type == SHT_FINI_ARRAY ||
             Sec.Name == section_names::elf::FiniArray ||
             Sec.Name == section_names::elf::Dtors)
      Out = &Img.DynInfo.FiniArray;
    if (Out)
      recordRuntimePointerArray(Sec.VA, Sec.Size, *Out, Img);
  }
}

bool recordIRelativeResolver(uint32_t RelocType, va_t Slot,
                             std::optional<int64_t> Addend, BinaryImage &Img) {
  if (!isIRelativeRelocation(RelocType, Img.Arch))
    return false;

  va_t Resolver = 0;
  if (Addend) {
    if (*Addend < 0)
      return false;
    Resolver = static_cast<va_t>(*Addend);
  } else {
    const uint8_t *P = Img.readVA(Slot, Img.getPointerSize());
    if (!P)
      return false;
    Resolver = static_cast<va_t>(readPtr(P, Img.is64Bit()));
  }
  return Img.recordRuntimeFunction(normalizeELFFunctionAddress(Resolver, Img));
}

// ===--------------------------------------------------------------------===//
// .dynamic section parsing
// ===--------------------------------------------------------------------===//

template <typename ELFT>
void parseDynamic(const llvm::object::ELFFile<ELFT> &ELF,
                  const typename ELFT::Shdr &DynamicSH, const uint8_t *Data,
                  size_t Size, BinaryImage &Img) {
  using Elf_Dyn = typename ELFT::Dyn;
  using Elf_Shdr = typename ELFT::Shdr;
  if (DynamicSH.sh_entsize < sizeof(Elf_Dyn) ||
      !rangeInBounds(DynamicSH.sh_offset, DynamicSH.sh_size, Size))
    return;

  llvm::StringRef DynStrTab;
  auto GetShdr = [&](uint32_t Idx) -> const Elf_Shdr * {
    auto SectionsOr = ELF.sections();
    if (!SectionsOr) {
      llvm::consumeError(SectionsOr.takeError());
      return nullptr;
    }
    if (Idx >= SectionsOr->size())
      return nullptr;
    return &(*SectionsOr)[Idx];
  };

  if (const Elf_Shdr *StrSH = GetShdr(DynamicSH.sh_link)) {
    auto TabOr = ELF.getStringTable(*StrSH);
    if (TabOr)
      DynStrTab = *TabOr;
    else
      llvm::consumeError(TabOr.takeError());
  }

  struct DynEntry {
    int64_t Tag;
    uint64_t Val;
  };
  std::vector<DynEntry> Entries;

  size_t Count = static_cast<size_t>(DynamicSH.sh_size / DynamicSH.sh_entsize);
  for (size_t I = 0; I < Count; ++I) {
    uint64_t Off64 =
        DynamicSH.sh_offset + static_cast<uint64_t>(I) * DynamicSH.sh_entsize;
    if (!rangeInBounds(Off64, sizeof(Elf_Dyn), Size))
      break;
    Elf_Dyn D;
    std::memcpy(&D, Data + static_cast<size_t>(Off64), sizeof(D));
    if (D.d_tag == DT_NULL)
      break;
    Entries.push_back({D.d_tag, D.d_un.d_val});
  }

  auto FindVal = [&](int64_t Tag) -> uint64_t {
    for (auto &E : Entries)
      if (E.Tag == Tag)
        return E.Val;
    return 0;
  };

  for (auto &E : Entries) {
    if (!DynStrTab.empty()) {
      uint64_t NameOff = E.Val;
      switch (E.Tag) {
      case DT_NEEDED:
        if (NameOff < DynStrTab.size()) {
          std::string Lib = readFixedName(DynStrTab.data() + NameOff,
                                          DynStrTab.size() - NameOff);
          Img.DynInfo.NeededLibs.push_back(Lib);
          Import Dep;
          Dep.Module = Lib;
          Dep.Name = Lib;
          Img.Imports.push_back(std::move(Dep));
        }
        break;
      case DT_SONAME:
        if (NameOff < DynStrTab.size())
          Img.DynInfo.SOName = readFixedName(DynStrTab.data() + NameOff,
                                             DynStrTab.size() - NameOff);
        break;
      case DT_RPATH:
      case DT_RUNPATH:
        if (NameOff < DynStrTab.size())
          Img.DynInfo.RPaths.emplace_back(readFixedName(
              DynStrTab.data() + NameOff, DynStrTab.size() - NameOff));
        break;
      default:
        break;
      }
    }
    switch (E.Tag) {
    case DT_INIT:
      Img.DynInfo.InitAddr = normalizeELFFunctionAddress(E.Val, Img);
      Img.recordRuntimeFunction(Img.DynInfo.InitAddr);
      break;
    case DT_FINI:
      Img.DynInfo.FiniAddr = normalizeELFFunctionAddress(E.Val, Img);
      Img.recordRuntimeFunction(Img.DynInfo.FiniAddr);
      break;
    case DT_PREINIT_ARRAY:
      recordRuntimePointerArray(E.Val, FindVal(DT_PREINIT_ARRAYSZ),
                                Img.DynInfo.PreinitArray, Img);
      break;
    case DT_INIT_ARRAY:
      recordRuntimePointerArray(E.Val, FindVal(DT_INIT_ARRAYSZ),
                                Img.DynInfo.InitArray, Img);
      break;
    case DT_FINI_ARRAY:
      recordRuntimePointerArray(E.Val, FindVal(DT_FINI_ARRAYSZ),
                                Img.DynInfo.FiniArray, Img);
      break;
    default:
      break;
    }
  }
}

// ===--------------------------------------------------------------------===//
// .rela.plt / .rel.plt import parsing
// ===--------------------------------------------------------------------===//

template <typename ELFT>
void parsePLTImports(const llvm::object::ELFFile<ELFT> &ELF,
                     llvm::ArrayRef<typename ELFT::Shdr> Sections,
                     const uint8_t *Data, size_t Size, BinaryImage &Img) {
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Rel = typename ELFT::Rel;
  using Elf_Rela = typename ELFT::Rela;

  auto GetSecName = [&](const Elf_Shdr &SH) -> llvm::StringRef {
    auto ShStrTabOr = ELF.getSectionStringTable(Sections);
    if (!ShStrTabOr) {
      llvm::consumeError(ShStrTabOr.takeError());
      return {};
    }
    if (SH.sh_name >= ShStrTabOr->size())
      return {};
    return ShStrTabOr->substr(SH.sh_name).split('\0').first;
  };

  const Elf_Shdr *DynSymSH = nullptr;
  const Elf_Shdr *RelPltSH = nullptr;
  bool HasAddend = false;
  llvm::StringRef DynStr;

  for (const Elf_Shdr &SH : Sections) {
    llvm::StringRef Name = GetSecName(SH);
    if (SH.sh_type == SHT_DYNSYM)
      DynSymSH = &SH;
    if (Name == section_names::elf::RelaPlt ||
        Name == section_names::elf::RelPlt) {
      RelPltSH = &SH;
      HasAddend = (SH.sh_type == SHT_RELA);
    }
  }

  if (DynSymSH) {
    auto TabOr = ELF.getStringTableForSymtab(*DynSymSH);
    if (TabOr)
      DynStr = *TabOr;
    else
      llvm::consumeError(TabOr.takeError());
  }

  if (!DynSymSH || DynStr.empty() || !RelPltSH || RelPltSH->sh_entsize == 0)
    return;

  auto SymsOr = ELF.symbols(DynSymSH);
  if (!SymsOr) {
    llvm::consumeError(SymsOr.takeError());
    return;
  }

  size_t EntrySize = HasAddend ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
  // sh_offset/sh_size are untrusted; validate the whole relocation region up
  // front so a crafted offset cannot wrap the per-entry check into an
  // out-of-bounds read and a bogus sh_size cannot spin the loop past EOF.
  if (!rangeInBounds(RelPltSH->sh_offset, RelPltSH->sh_size, Size))
    return;
  size_t RelaCount =
      static_cast<size_t>(RelPltSH->sh_size / RelPltSH->sh_entsize);

  for (size_t I = 0; I < RelaCount; ++I) {
    size_t ROff = static_cast<size_t>(RelPltSH->sh_offset) +
                  I * static_cast<size_t>(RelPltSH->sh_entsize);
    if (!rangeInBounds(ROff, EntrySize, Size))
      break;

    uint32_t SymIdx = 0;
    uint64_t Offset = 0;
    if (HasAddend) {
      Elf_Rela Rela;
      std::memcpy(&Rela, Data + ROff, sizeof(Rela));
      SymIdx = Rela.getSymbol(false);
      Offset = Rela.r_offset;
    } else {
      Elf_Rel Rel;
      std::memcpy(&Rel, Data + ROff, sizeof(Rel));
      SymIdx = Rel.getSymbol(false);
      Offset = Rel.r_offset;
    }

    if (SymIdx >= SymsOr->size())
      continue;
    auto NameOr = (*SymsOr)[SymIdx].getName(DynStr);
    if (!NameOr) {
      llvm::consumeError(NameOr.takeError());
      continue;
    }
    if (NameOr->empty())
      continue;

    Import Imp;
    Imp.Module = kExternModule.str();
    Imp.Name = NameOr->str();
    Imp.IATAddr = Offset;
    Img.Imports.push_back(std::move(Imp));
  }
}

// ===--------------------------------------------------------------------===//
// .got / .got.plt GOT entry parsing
// ===--------------------------------------------------------------------===//

template <typename ELFT>
void parseGOTEntries(const llvm::object::ELFFile<ELFT> &ELF,
                     llvm::ArrayRef<typename ELFT::Shdr> Sections,
                     const uint8_t *Data, size_t Size, BinaryImage &Img) {
  using Elf_Shdr = typename ELFT::Shdr;
  constexpr size_t PtrSize = sizeof(typename ELFT::Addr);

  auto ShStrTabOr = ELF.getSectionStringTable(Sections);
  if (!ShStrTabOr) {
    llvm::consumeError(ShStrTabOr.takeError());
    return;
  }

  auto GetSecName = [&](const Elf_Shdr &SH) -> llvm::StringRef {
    if (SH.sh_name >= ShStrTabOr->size())
      return {};
    return ShStrTabOr->substr(SH.sh_name).split('\0').first;
  };

  for (const Elf_Shdr &SH : Sections) {
    llvm::StringRef Name = GetSecName(SH);
    if (Name != section_names::elf::Got && Name != section_names::elf::GotPlt)
      continue;
    if (!rangeInBounds(SH.sh_offset, SH.sh_size, Size) || SH.sh_size < PtrSize)
      continue;

    size_t Count = static_cast<size_t>(SH.sh_size / PtrSize);
    for (size_t I = 0; I < Count; ++I) {
      size_t Off = static_cast<size_t>(SH.sh_offset) + I * PtrSize;
      if (!rangeInBounds(Off, PtrSize, Size))
        break;
      typename ELFT::Addr Val;
      std::memcpy(&Val, Data + Off, PtrSize);
      if (Val == 0)
        continue;

      BaseRelocation BR;
      BR.Address = SH.sh_addr + I * PtrSize;
      BR.Type = 0;
      Img.BaseRelocations.push_back(BR);
    }
    LLVM_DEBUG(llvm::dbgs() << "elf: parsed " << Count << " GOT entries from "
                            << Name << "\n");
  }
}

// ===--------------------------------------------------------------------===//
// ELF notes parsing (PT_NOTE / SHT_NOTE)
// ===--------------------------------------------------------------------===//

template <typename ELFT>
void parseNotes(const llvm::object::ELFFile<ELFT> &ELF, const uint8_t *Data,
                size_t Size, BinaryImage &Img) {
  auto PhdrsOr = ELF.program_headers();
  if (!PhdrsOr) {
    llvm::consumeError(PhdrsOr.takeError());
    return;
  }

  for (const auto &PH : *PhdrsOr) {
    if (PH.p_type != PT_NOTE)
      continue;
    if (!rangeInBounds(PH.p_offset, PH.p_filesz, Size) ||
        PH.p_filesz < sizeof(ELFNoteHeader))
      continue;

    const uint8_t *P = Data + PH.p_offset;
    const uint8_t *End = P + PH.p_filesz;
    while (static_cast<size_t>(End - P) >= sizeof(ELFNoteHeader)) {
      auto NH = readELFNoteHeader(P);

      uint64_t NameAlign = elfNoteAlign(NH.NameSz);
      uint64_t DescAlign = elfNoteAlign(NH.DescSz);
      uint64_t TotalSize = sizeof(ELFNoteHeader) + NameAlign + DescAlign;
      if (TotalSize > static_cast<uint64_t>(End - P))
        break;

      const char *NoteName =
          reinterpret_cast<const char *>(P + sizeof(ELFNoteHeader));
      llvm::StringRef NoteNameRef(NoteName, NH.NameSz);
      if (!NoteNameRef.empty() && NoteNameRef.back() == '\0')
        NoteNameRef = NoteNameRef.drop_back();

      if (NoteNameRef == elf_notes::GnuOwner &&
          NH.Type == elf_notes::GnuBuildId && NH.DescSz > 0) {
        const uint8_t *Desc =
            P + sizeof(ELFNoteHeader) + static_cast<size_t>(NameAlign);
        std::string BuildId;
        for (uint32_t I = 0; I < NH.DescSz; ++I)
          BuildId += llvm::utohexstr(Desc[I], /*LowerCase=*/true, 2);
        Img.DynInfo.PDBPath = (kBuildIdPrefix + BuildId).str();
        LLVM_DEBUG(llvm::dbgs() << "elf: build-id = " << BuildId << "\n");
      }

      P += static_cast<size_t>(TotalSize);
    }
  }
}

// ===--------------------------------------------------------------------===//
// Explicit template instantiations for ELF32 and ELF64
// ===--------------------------------------------------------------------===//

template void parseDynamic<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    const llvm::object::ELF32LE::Shdr &, const uint8_t *, size_t,
    BinaryImage &);
template void parseDynamic<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    const llvm::object::ELF64LE::Shdr &, const uint8_t *, size_t,
    BinaryImage &);

template void parsePLTImports<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const uint8_t *, size_t,
    BinaryImage &);
template void parsePLTImports<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const uint8_t *, size_t,
    BinaryImage &);

template void parseGOTEntries<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &,
    llvm::ArrayRef<llvm::object::ELF32LE::Shdr>, const uint8_t *, size_t,
    BinaryImage &);
template void parseGOTEntries<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &,
    llvm::ArrayRef<llvm::object::ELF64LE::Shdr>, const uint8_t *, size_t,
    BinaryImage &);

template void parseNotes<llvm::object::ELF32LE>(
    const llvm::object::ELFFile<llvm::object::ELF32LE> &, const uint8_t *,
    size_t, BinaryImage &);
template void parseNotes<llvm::object::ELF64LE>(
    const llvm::object::ELFFile<llvm::object::ELF64LE> &, const uint8_t *,
    size_t, BinaryImage &);

} // namespace elf_loader
} // namespace neverd
