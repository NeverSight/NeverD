//===- MachOIndirectSymbols.cpp - Mach-O indirect symbols ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "neverd/object/MachOLayout.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <limits>
#include <string>

#define DEBUG_TYPE "neverd-macho-loader"

namespace neverd {
namespace macho_loader {

using namespace llvm::MachO;

void parseStubImports(const llvm::object::MachOObjectFile &Obj,
                      const std::vector<SectionInfo> &Sections,
                      const uint8_t *BasePtr, size_t FileSize, bool Is64,
                      BinaryImage &Img) {
  symtab_command SymtabCmd = Obj.getSymtabLoadCommand();
  dysymtab_command DysymtabCmd = Obj.getDysymtabLoadCommand();
  const size_t NListSize = getMachONListSize(Is64);

  if (SymtabCmd.nsyms == 0 || DysymtabCmd.nindirectsyms == 0)
    return;
  if (!rangeInBounds(SymtabCmd.stroff, SymtabCmd.strsize, FileSize) ||
      !rangeInBounds(SymtabCmd.symoff,
                     static_cast<uint64_t>(SymtabCmd.nsyms) * NListSize,
                     FileSize) ||
      !rangeInBounds(
          DysymtabCmd.indirectsymoff,
          static_cast<uint64_t>(DysymtabCmd.nindirectsyms) * sizeof(uint32_t),
          FileSize))
    return;

  const char *StrTab =
      reinterpret_cast<const char *>(BasePtr + SymtabCmd.stroff);

  auto GetSymName = [&](uint32_t SymIdx) -> std::string {
    size_t EntryOff = SymtabCmd.symoff + SymIdx * NListSize;
    if (!rangeInBounds(EntryOff, NListSize, FileSize))
      return {};
    uint32_t NStrx;
    if (Is64) {
      auto *NL = reinterpret_cast<const nlist_64 *>(BasePtr + EntryOff);
      NStrx = NL->n_strx;
    } else {
      auto *NL = reinterpret_cast<const nlist *>(BasePtr + EntryOff);
      NStrx = NL->n_strx;
    }
    if (NStrx == 0 || NStrx >= SymtabCmd.strsize)
      return {};
    return readFixedName(StrTab + NStrx, SymtabCmd.strsize - NStrx);
  };

  for (const auto &Sect : Sections) {
    if (Sect.Flags != S_SYMBOL_STUBS || Sect.StubSize == 0)
      continue;

    uint32_t NStubs = static_cast<uint32_t>(Sect.Size / Sect.StubSize);
    uint32_t IndirectBase = Sect.Reserved1;

    for (uint32_t SI = 0; SI < NStubs; ++SI) {
      if (SI > std::numeric_limits<uint32_t>::max() - IndirectBase)
        break;
      uint32_t ISymIdx = IndirectBase + SI;
      if (ISymIdx >= DysymtabCmd.nindirectsyms)
        continue;

      uint32_t SymIdx = Obj.getIndirectSymbolTableEntry(DysymtabCmd, ISymIdx);
      if ((SymIdx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) != 0 ||
          SymIdx >= SymtabCmd.nsyms)
        continue;

      std::string SymName = GetSymName(SymIdx);
      if (SymName.empty())
        continue;

      va_t StubAddr = Sect.Addr + static_cast<uint64_t>(SI) * Sect.StubSize;

      Import Imp;
      Imp.Name = SymName;
      Imp.IATAddr = StubAddr;
      size_t ImportIndex = Img.Imports.size();
      Img.Imports.push_back(std::move(Imp));
      Img.recordImportStub(StubAddr, ImportIndex);

      Symbol Sym = Symbol::makeFunc(StubAddr);
      Sym.Name = SymName;
      Img.Symbols.push_back(std::move(Sym));

      LLVM_DEBUG(llvm::dbgs() << "macho: stub 0x" << llvm::utohexstr(StubAddr)
                              << " -> " << SymName << "\n");
    }
  }
}

void parseNonLazyPtrImports(const llvm::object::MachOObjectFile &Obj,
                            const std::vector<SectionInfo> &Sections,
                            const uint8_t *BasePtr, size_t FileSize, bool Is64,
                            BinaryImage &Img) {
  using namespace llvm::MachO;
  symtab_command SymtabCmd = Obj.getSymtabLoadCommand();
  dysymtab_command DysymtabCmd = Obj.getDysymtabLoadCommand();
  const size_t NListSize = getMachONListSize(Is64);

  if (SymtabCmd.nsyms == 0 || DysymtabCmd.nindirectsyms == 0)
    return;
  if (!rangeInBounds(SymtabCmd.stroff, SymtabCmd.strsize, FileSize) ||
      !rangeInBounds(SymtabCmd.symoff,
                     static_cast<uint64_t>(SymtabCmd.nsyms) * NListSize,
                     FileSize) ||
      !rangeInBounds(
          DysymtabCmd.indirectsymoff,
          static_cast<uint64_t>(DysymtabCmd.nindirectsyms) * sizeof(uint32_t),
          FileSize))
    return;

  const char *StrTab =
      reinterpret_cast<const char *>(BasePtr + SymtabCmd.stroff);

  auto GetSymName = [&](uint32_t SymIdx) -> std::string {
    size_t EntryOff = SymtabCmd.symoff + SymIdx * NListSize;
    if (!rangeInBounds(EntryOff, NListSize, FileSize))
      return {};
    uint32_t NStrx;
    if (Is64) {
      auto *NL = reinterpret_cast<const nlist_64 *>(BasePtr + EntryOff);
      NStrx = NL->n_strx;
    } else {
      auto *NL = reinterpret_cast<const nlist *>(BasePtr + EntryOff);
      NStrx = NL->n_strx;
    }
    if (NStrx == 0 || NStrx >= SymtabCmd.strsize)
      return {};
    return readFixedName(StrTab + NStrx, SymtabCmd.strsize - NStrx);
  };

  const uint64_t PtrSize = Is64 ? 8 : 4;
  for (const auto &Sect : Sections) {
    if (Sect.Flags != S_NON_LAZY_SYMBOL_POINTERS &&
        Sect.Flags != S_LAZY_SYMBOL_POINTERS)
      continue;

    uint64_t NSlots = Sect.Size / PtrSize;
    uint32_t IndirectBase = Sect.Reserved1;

    for (uint64_t SI = 0; SI < NSlots; ++SI) {
      if (SI > std::numeric_limits<uint32_t>::max() - IndirectBase)
        break;
      uint32_t ISymIdx = IndirectBase + static_cast<uint32_t>(SI);
      if (ISymIdx >= DysymtabCmd.nindirectsyms)
        continue;

      uint32_t SymIdx = Obj.getIndirectSymbolTableEntry(DysymtabCmd, ISymIdx);
      if ((SymIdx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) != 0 ||
          SymIdx >= SymtabCmd.nsyms)
        continue;

      std::string SymName = GetSymName(SymIdx);
      if (SymName.empty())
        continue;

      if (SI > (std::numeric_limits<va_t>::max() - Sect.Addr) / PtrSize)
        break;
      va_t SlotAddr = Sect.Addr + SI * PtrSize;
      Img.ImportPtrSlots[SlotAddr] = SymName;

      LLVM_DEBUG(llvm::dbgs()
                 << "macho: ptr-slot 0x" << llvm::utohexstr(SlotAddr) << " -> "
                 << SymName << "\n");
    }
  }
}

llvm::Expected<std::map<va_t, std::string>>
parseImportPtrSlots(llvm::ArrayRef<uint8_t> Binary) {
  const auto Fail = [](const llvm::Twine &Detail)
      -> llvm::Expected<std::map<va_t, std::string>> {
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        (llvm::Twine("macho pointer-slot provenance: ") + Detail).str());
  };
  if (Binary.empty())
    return Fail("empty image");

  const llvm::StringRef Bytes(reinterpret_cast<const char *>(Binary.data()),
                              Binary.size());
  auto Object = llvm::object::ObjectFile::createMachOObjectFile(
      llvm::MemoryBufferRef(Bytes, "Mach-O pointer-slot provenance"));
  if (!Object)
    return Object.takeError();
  if (!(*Object)->isLittleEndian())
    return Fail("big-endian images are unsupported");

  const bool Is64 = (*Object)->is64Bit();
  const uint64_t PointerSize = Is64 ? 8 : 4;
  struct PointerSection {
    uint64_t Address = 0;
    uint64_t Size = 0;
    uint32_t IndirectBase = 0;
  };
  std::vector<PointerSection> PointerSections;
  unsigned SymtabCount = 0;
  unsigned DysymtabCount = 0;
  for (const auto &Load : (*Object)->load_commands()) {
    SymtabCount += Load.C.cmd == LC_SYMTAB;
    DysymtabCount += Load.C.cmd == LC_DYSYMTAB;
    if (Load.C.cmd == LC_SEGMENT_64 && Is64) {
      const segment_command_64 Segment =
          (*Object)->getSegment64LoadCommand(Load);
      for (uint32_t I = 0; I < Segment.nsects; ++I) {
        const section_64 Section = (*Object)->getSection64(Load, I);
        const uint32_t Type = Section.flags & SECTION_TYPE;
        if (Type == S_NON_LAZY_SYMBOL_POINTERS ||
            Type == S_LAZY_SYMBOL_POINTERS)
          PointerSections.push_back(
              {Section.addr, Section.size, Section.reserved1});
      }
      continue;
    }
    if (Load.C.cmd == LC_SEGMENT && !Is64) {
      const segment_command Segment = (*Object)->getSegmentLoadCommand(Load);
      for (uint32_t I = 0; I < Segment.nsects; ++I) {
        const section Section = (*Object)->getSection(Load, I);
        const uint32_t Type = Section.flags & SECTION_TYPE;
        if (Type == S_NON_LAZY_SYMBOL_POINTERS ||
            Type == S_LAZY_SYMBOL_POINTERS)
          PointerSections.push_back(
              {Section.addr, Section.size, Section.reserved1});
      }
    }
  }

  if (SymtabCount > 1 || DysymtabCount > 1)
    return Fail("symbol or dynamic-symbol command is ambiguous");

  uint64_t TotalPointerSlots = 0;
  for (const PointerSection &Section : PointerSections) {
    if (Section.Size % PointerSize != 0)
      return Fail("pointer-section size is not pointer aligned");
    if (Section.Size > std::numeric_limits<va_t>::max() - Section.Address)
      return Fail("pointer-section virtual range overflows");
    const uint64_t SlotCount = Section.Size / PointerSize;
    if (SlotCount > std::numeric_limits<uint64_t>::max() - TotalPointerSlots)
      return Fail("pointer-slot count overflows");
    TotalPointerSlots += SlotCount;
  }

  if (TotalPointerSlots == 0)
    return std::map<va_t, std::string>{};
  if (SymtabCount != 1 || DysymtabCount != 1)
    return Fail("pointer sections require one symbol and dynamic-symbol table");

  const symtab_command Symtab = (*Object)->getSymtabLoadCommand();
  const dysymtab_command Dysymtab = (*Object)->getDysymtabLoadCommand();
  const uint64_t NListSize = getMachONListSize(Is64);
  if (!rangeInBounds(Symtab.symoff, uint64_t(Symtab.nsyms) * NListSize,
                     Binary.size()) ||
      !rangeInBounds(Symtab.stroff, Symtab.strsize, Binary.size()) ||
      !rangeInBounds(Dysymtab.indirectsymoff,
                     uint64_t(Dysymtab.nindirectsyms) * sizeof(uint32_t),
                     Binary.size()))
    return Fail("symbol, string, or indirect-symbol table leaves the image");

  std::map<va_t, std::string> Result;
  const char *StringTable =
      reinterpret_cast<const char *>(Binary.data() + Symtab.stroff);
  for (const PointerSection &Section : PointerSections) {
    const uint64_t SlotCount = Section.Size / PointerSize;
    if (Section.IndirectBase > Dysymtab.nindirectsyms ||
        SlotCount > uint64_t(Dysymtab.nindirectsyms - Section.IndirectBase))
      return Fail("pointer section leaves the indirect-symbol table");

    for (uint64_t I = 0; I < SlotCount; ++I) {
      const uint64_t IndirectIndex = uint64_t(Section.IndirectBase) + I;
      const uint64_t IndirectOffset =
          uint64_t(Dysymtab.indirectsymoff) +
          IndirectIndex * sizeof(uint32_t);
      const uint32_t SymbolIndex =
          llvm::support::endian::read32le(Binary.data() + IndirectOffset);
      if ((SymbolIndex & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) != 0)
        continue;
      if (SymbolIndex >= Symtab.nsyms)
        return Fail("external indirect symbol leaves the symbol table");

      const uint64_t SymbolOffset =
          uint64_t(Symtab.symoff) + uint64_t(SymbolIndex) * NListSize;
      uint32_t StringOffset = 0;
      if (Is64) {
        nlist_64 Symbol;
        std::memcpy(&Symbol, Binary.data() + SymbolOffset, sizeof(Symbol));
        StringOffset = Symbol.n_strx;
      } else {
        nlist Symbol;
        std::memcpy(&Symbol, Binary.data() + SymbolOffset, sizeof(Symbol));
        StringOffset = Symbol.n_strx;
      }
      if (StringOffset == 0 || StringOffset >= Symtab.strsize)
        return Fail("external indirect symbol has no valid name offset");
      const char *Name = StringTable + StringOffset;
      const size_t Available = Symtab.strsize - StringOffset;
      const auto *Terminator =
          static_cast<const char *>(std::memchr(Name, '\0', Available));
      if (!Terminator || Terminator == Name)
        return Fail("external indirect symbol name is empty or unterminated");

      if (I > (std::numeric_limits<va_t>::max() - Section.Address) /
                  PointerSize)
        return Fail("pointer-slot virtual address overflows");
      const va_t SlotAddress = Section.Address + I * PointerSize;
      const std::string SymbolName(Name, Terminator);
      if (!Result.emplace(SlotAddress, SymbolName).second)
        return Fail("pointer-slot virtual address occurs more than once");
    }
  }
  return Result;
}

} // namespace macho_loader
} // namespace neverd
