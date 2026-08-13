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
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

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

      va_t SlotAddr = Sect.Addr + SI * PtrSize;
      Img.ImportPtrSlots[SlotAddr] = SymName;

      LLVM_DEBUG(llvm::dbgs()
                 << "macho: ptr-slot 0x" << llvm::utohexstr(SlotAddr) << " -> "
                 << SymName << "\n");
    }
  }
}

} // namespace macho_loader
} // namespace neverd
