//===- ELFLayout.h - ELF header layout helpers ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ELF program-header and section-header field access for loaders and
/// codegen.  Uses llvm::object::ELF{32,64}LE typedefs — no raw offsets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_OBJECT_ELFLAYOUT_H
#define NEVERD_OBJECT_ELFLAYOUT_H

#include "neverd/Support/BinaryEncoding.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"

#include <cstdint>
#include <cstring>

namespace neverd {

struct ELFHeaderInfo {
  bool Is64 = false;
  uint16_t HeaderSize = 0;
  uint64_t EntryPoint = 0;
  uint64_t PhOff = 0;
  uint64_t ShOff = 0;
  uint16_t PhEntSize = 0;
  uint16_t PhNum = 0;
  uint16_t ShEntSize = 0;
  uint16_t ShNum = 0;
  uint16_t ShStrNdx = 0;
};

inline ELFHeaderInfo parseELFHeader(const uint8_t *Data, size_t Size) {
  ELFHeaderInfo Info{};
  if (Size < llvm::ELF::EI_NIDENT)
    return Info;
  if (!std::equal(Data, Data + 4,
                  reinterpret_cast<const uint8_t *>(llvm::ELF::ElfMagic)))
    return Info;

  Info.Is64 = (Data[llvm::ELF::EI_CLASS] == llvm::ELF::ELFCLASS64);
  if (Info.Is64) {
    using Ehdr = llvm::object::ELF64LE::Ehdr;
    if (Size < sizeof(Ehdr))
      return ELFHeaderInfo{};
    Info.HeaderSize = sizeof(Ehdr);
    auto *EH = reinterpret_cast<const Ehdr *>(Data);
    Info.EntryPoint = EH->e_entry;
    Info.PhOff = EH->e_phoff;
    Info.ShOff = EH->e_shoff;
    Info.PhEntSize = EH->e_phentsize;
    Info.PhNum = EH->e_phnum;
    Info.ShEntSize = EH->e_shentsize;
    Info.ShNum = EH->e_shnum;
    Info.ShStrNdx = EH->e_shstrndx;
  } else {
    using Ehdr = llvm::object::ELF32LE::Ehdr;
    if (Size < sizeof(Ehdr))
      return ELFHeaderInfo{};
    Info.HeaderSize = sizeof(Ehdr);
    auto *EH = reinterpret_cast<const Ehdr *>(Data);
    Info.EntryPoint = EH->e_entry;
    Info.PhOff = EH->e_phoff;
    Info.ShOff = EH->e_shoff;
    Info.PhEntSize = EH->e_phentsize;
    Info.PhNum = EH->e_phnum;
    Info.ShEntSize = EH->e_shentsize;
    Info.ShNum = EH->e_shnum;
    Info.ShStrNdx = EH->e_shstrndx;
  }
  return Info;
}

struct ELFPhdrFields {
  uint32_t Type = 0;
  uint32_t Flags = 0;
  uint64_t Offset = 0;
  uint64_t VAddr = 0;
  uint64_t PAddr = 0;
  uint64_t FileSz = 0;
  uint64_t MemSz = 0;
  uint64_t Align = 0;
};

inline ELFPhdrFields readELFPhdr(const uint8_t *PH, bool Is64) {
  ELFPhdrFields F;
  if (Is64) {
    auto *P = reinterpret_cast<const llvm::object::ELF64LE::Phdr *>(PH);
    F.Type = P->p_type;
    F.Flags = P->p_flags;
    F.Offset = P->p_offset;
    F.VAddr = P->p_vaddr;
    F.PAddr = P->p_paddr;
    F.FileSz = P->p_filesz;
    F.MemSz = P->p_memsz;
    F.Align = P->p_align;
  } else {
    auto *P = reinterpret_cast<const llvm::object::ELF32LE::Phdr *>(PH);
    F.Type = P->p_type;
    F.Flags = P->p_flags;
    F.Offset = P->p_offset;
    F.VAddr = P->p_vaddr;
    F.PAddr = P->p_paddr;
    F.FileSz = P->p_filesz;
    F.MemSz = P->p_memsz;
    F.Align = P->p_align;
  }
  return F;
}

inline void writeELFPhdr(uint8_t *PH, bool Is64, const ELFPhdrFields &F) {
  if (Is64) {
    auto *P = reinterpret_cast<llvm::object::ELF64LE::Phdr *>(PH);
    P->p_type = F.Type;
    P->p_flags = F.Flags;
    P->p_offset = F.Offset;
    P->p_vaddr = F.VAddr;
    P->p_paddr = F.PAddr;
    P->p_filesz = F.FileSz;
    P->p_memsz = F.MemSz;
    P->p_align = F.Align;
  } else {
    auto *P = reinterpret_cast<llvm::object::ELF32LE::Phdr *>(PH);
    P->p_type = F.Type;
    P->p_flags = F.Flags;
    P->p_offset = static_cast<uint32_t>(F.Offset);
    P->p_vaddr = static_cast<uint32_t>(F.VAddr);
    P->p_paddr = static_cast<uint32_t>(F.PAddr);
    P->p_filesz = static_cast<uint32_t>(F.FileSz);
    P->p_memsz = static_cast<uint32_t>(F.MemSz);
    P->p_align = static_cast<uint32_t>(F.Align);
  }
}

inline void setELFPhdrTable(uint8_t *Data, bool Is64, uint64_t PhOff,
                            uint16_t PhNum) {
  if (Is64) {
    auto *EH = reinterpret_cast<llvm::object::ELF64LE::Ehdr *>(Data);
    EH->e_phoff = PhOff;
    EH->e_phnum = PhNum;
  } else {
    auto *EH = reinterpret_cast<llvm::object::ELF32LE::Ehdr *>(Data);
    EH->e_phoff = static_cast<uint32_t>(PhOff);
    EH->e_phnum = PhNum;
  }
}

// ===--------------------------------------------------------------------===//
// Section header traversal helpers
// ===--------------------------------------------------------------------===//

struct ELFShdrFields {
  uint32_t Name = 0;
  uint32_t Type = 0;
  uint64_t Flags = 0;
  uint64_t Addr = 0;
  uint64_t Offset = 0;
  uint64_t Size = 0;
  uint32_t Link = 0;
  uint32_t Info = 0;
  uint64_t AddrAlign = 0;
  uint64_t EntSize = 0;
};

inline ELFShdrFields readELFShdr(const uint8_t *SH, bool Is64) {
  ELFShdrFields F;
  if (Is64) {
    auto *S = reinterpret_cast<const llvm::object::ELF64LE::Shdr *>(SH);
    F.Name = S->sh_name;
    F.Type = S->sh_type;
    F.Flags = S->sh_flags;
    F.Addr = S->sh_addr;
    F.Offset = S->sh_offset;
    F.Size = S->sh_size;
    F.Link = S->sh_link;
    F.Info = S->sh_info;
    F.AddrAlign = S->sh_addralign;
    F.EntSize = S->sh_entsize;
  } else {
    auto *S = reinterpret_cast<const llvm::object::ELF32LE::Shdr *>(SH);
    F.Name = S->sh_name;
    F.Type = S->sh_type;
    F.Flags = S->sh_flags;
    F.Addr = S->sh_addr;
    F.Offset = S->sh_offset;
    F.Size = S->sh_size;
    F.Link = S->sh_link;
    F.Info = S->sh_info;
    F.AddrAlign = S->sh_addralign;
    F.EntSize = S->sh_entsize;
  }
  return F;
}

template <typename Callback>
void forEachELFPhdr(const uint8_t *Data, size_t Size, Callback &&CB) {
  auto Hdr = parseELFHeader(Data, Size);
  if (Hdr.PhNum == 0 || Hdr.PhEntSize == 0)
    return;
  for (uint16_t I = 0; I < Hdr.PhNum; ++I) {
    // PhOff/PhEntSize come from an untrusted header, so use a 64-bit index
    // product and rangeInBounds() to keep a crafted offset from wrapping the
    // bounds check into an out-of-bounds read.
    uint64_t Off = Hdr.PhOff + static_cast<uint64_t>(I) * Hdr.PhEntSize;
    if (!rangeInBounds(Off, Hdr.PhEntSize, Size))
      break;
    CB(readELFPhdr(Data + Off, Hdr.Is64), Data + Off, Hdr.Is64);
  }
}

template <typename Callback>
void forEachELFShdr(const uint8_t *Data, size_t Size, Callback &&CB) {
  auto Hdr = parseELFHeader(Data, Size);
  if (Hdr.ShNum == 0 || Hdr.ShEntSize == 0)
    return;
  for (uint16_t I = 0; I < Hdr.ShNum; ++I) {
    // ShOff/ShEntSize come from an untrusted header, so use a 64-bit index
    // product and rangeInBounds() to keep a crafted offset from wrapping the
    // check.
    uint64_t Off = Hdr.ShOff + static_cast<uint64_t>(I) * Hdr.ShEntSize;
    if (!rangeInBounds(Off, Hdr.ShEntSize, Size))
      break;
    CB(readELFShdr(Data + Off, Hdr.Is64), I);
  }
}

/// Locate an ELF section by name. Requires the section string table.
/// Returns true and fills Out on success.
inline bool findELFSection(const uint8_t *Data, size_t Size,
                           llvm::StringRef SectionName, ELFShdrFields &Out) {
  auto Hdr = parseELFHeader(Data, Size);
  if (Hdr.ShNum == 0 || Hdr.ShStrNdx == 0)
    return false;

  if (Hdr.ShEntSize == 0)
    return false;
  uint64_t ShStrOff =
      Hdr.ShOff + static_cast<uint64_t>(Hdr.ShStrNdx) * Hdr.ShEntSize;
  if (!rangeInBounds(ShStrOff, Hdr.ShEntSize, Size))
    return false;
  auto ShStr = readELFShdr(Data + ShStrOff, Hdr.Is64);
  if (!rangeInBounds(ShStr.Offset, ShStr.Size, Size))
    return false;

  const char *StrTab = reinterpret_cast<const char *>(Data + ShStr.Offset);

  bool Found = false;
  forEachELFShdr(Data, Size, [&](const ELFShdrFields &F, uint16_t) {
    if (Found)
      return;
    if (F.Name >= ShStr.Size)
      return;
    llvm::StringRef Name(StrTab + F.Name,
                         static_cast<size_t>(ShStr.Size - F.Name));
    Name = Name.split('\0').first;
    if (Name == SectionName) {
      Out = F;
      Found = true;
    }
  });
  return Found;
}

inline void setELFEntryPoint(uint8_t *Data, bool Is64, uint64_t Entry) {
  if (Is64) {
    auto *EH = reinterpret_cast<llvm::object::ELF64LE::Ehdr *>(Data);
    EH->e_entry = Entry;
  } else {
    auto *EH = reinterpret_cast<llvm::object::ELF32LE::Ehdr *>(Data);
    EH->e_entry = static_cast<uint32_t>(Entry);
  }
}

inline void writeELFShdr(uint8_t *SH, bool Is64, const ELFShdrFields &F) {
  if (Is64) {
    auto *S = reinterpret_cast<llvm::object::ELF64LE::Shdr *>(SH);
    S->sh_name = F.Name;
    S->sh_type = F.Type;
    S->sh_flags = F.Flags;
    S->sh_addr = F.Addr;
    S->sh_offset = F.Offset;
    S->sh_size = F.Size;
    S->sh_link = F.Link;
    S->sh_info = F.Info;
    S->sh_addralign = F.AddrAlign;
    S->sh_entsize = F.EntSize;
  } else {
    auto *S = reinterpret_cast<llvm::object::ELF32LE::Shdr *>(SH);
    S->sh_name = F.Name;
    S->sh_type = F.Type;
    S->sh_flags = static_cast<uint32_t>(F.Flags);
    S->sh_addr = static_cast<uint32_t>(F.Addr);
    S->sh_offset = static_cast<uint32_t>(F.Offset);
    S->sh_size = static_cast<uint32_t>(F.Size);
    S->sh_link = F.Link;
    S->sh_info = F.Info;
    S->sh_addralign = static_cast<uint32_t>(F.AddrAlign);
    S->sh_entsize = static_cast<uint32_t>(F.EntSize);
  }
}

inline void setELFShdrTable(uint8_t *Data, bool Is64, uint64_t ShOff,
                            uint16_t ShNum) {
  if (Is64) {
    auto *EH = reinterpret_cast<llvm::object::ELF64LE::Ehdr *>(Data);
    EH->e_shoff = ShOff;
    EH->e_shnum = ShNum;
  } else {
    auto *EH = reinterpret_cast<llvm::object::ELF32LE::Ehdr *>(Data);
    EH->e_shoff = static_cast<uint32_t>(ShOff);
    EH->e_shnum = ShNum;
  }
}

/// Find the first program header with the given type.
/// Returns true and fills Out on success.
inline bool findELFPhdrByType(const uint8_t *Data, size_t Size,
                              uint32_t PhdrType, ELFPhdrFields &Out) {
  bool Found = false;
  forEachELFPhdr(Data, Size,
                 [&](const ELFPhdrFields &F, const uint8_t *, bool) {
                   if (Found || F.Type != PhdrType)
                     return;
                   Out = F;
                   Found = true;
                 });
  return Found;
}

/// Get the phdr entry size for the given ELF class.
inline uint16_t getELFPhdrSize(bool Is64) {
  return Is64 ? sizeof(llvm::object::ELF64LE::Phdr)
              : sizeof(llvm::object::ELF32LE::Phdr);
}

/// Get the shdr entry size for the given ELF class.
inline uint16_t getELFShdrSize(bool Is64) {
  return Is64 ? sizeof(llvm::object::ELF64LE::Shdr)
              : sizeof(llvm::object::ELF32LE::Shdr);
}

/// Get the ELF header size for the given ELF class.
inline uint16_t getELFHeaderSize(bool Is64) {
  return Is64 ? sizeof(llvm::object::ELF64LE::Ehdr)
              : sizeof(llvm::object::ELF32LE::Ehdr);
}

// ===--------------------------------------------------------------------===//
// ELF Note header (Elf_Nhdr equivalent)
// ===--------------------------------------------------------------------===//

struct ELFNoteHeader {
  uint32_t NameSz = 0;
  uint32_t DescSz = 0;
  uint32_t Type = 0;
};

static_assert(sizeof(ELFNoteHeader) == 12,
              "ELFNoteHeader must be 12 bytes (namesz + descsz + type)");

inline ELFNoteHeader readELFNoteHeader(const uint8_t *P) {
  ELFNoteHeader NH;
  std::memcpy(&NH.NameSz, P, sizeof(uint32_t));
  std::memcpy(&NH.DescSz, P + offsetof(ELFNoteHeader, DescSz),
              sizeof(uint32_t));
  std::memcpy(&NH.Type, P + offsetof(ELFNoteHeader, Type), sizeof(uint32_t));
  return NH;
}

/// Aligned size of a note name or descriptor (4-byte aligned).
inline uint64_t elfNoteAlign(uint32_t Val) {
  return (static_cast<uint64_t>(Val) + 3) & ~uint64_t(3);
}

/// Well-known ELF note types.
namespace elf_notes {
constexpr uint32_t GnuBuildId = 3;
constexpr const char *GnuOwner = "GNU";
} // namespace elf_notes

} // namespace neverd

#endif // NEVERD_OBJECT_ELFLAYOUT_H
