//===- MachOReloc.cpp - Mach-O relocation handling ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mach-O stub and relocation resolution implementation.  Parses the
/// __stubs section via the indirect symbol table to build a name-to-VA
/// mapping.  Handles both 32-bit and 64-bit Mach-O.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOReloc.h"

#include "neverd/object/MachOLayout.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-macho-reloc"

namespace neverd {

using namespace llvm::MachO;

bool MachORelocResolver::parse(const std::vector<uint8_t> &Binary,
                               Arch TargetArch) {
  Entries.clear();
  ByName.clear();

  auto Hdr = parseMachOHeader(Binary.data(), Binary.size());
  if (Hdr.HeaderSize == 0)
    return false;
  size_t NListSize = getMachONListSize(Hdr.Is64);

  std::vector<std::string> SymtabNames;
  uint32_t NSyms = 0;
  const char *StrTab = nullptr;
  uint32_t StrSize = 0;
  uint32_t SymOff = 0;
  uint32_t IndirectOff = 0;
  uint32_t NIndirect = 0;

  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t /*CmdSize*/,
          bool /*Is64*/) {
        if (Cmd == LC_SYMTAB) {
          auto *ST = reinterpret_cast<const symtab_command *>(LCPtr);
          // stroff/strsize are untrusted uint32 header fields; their plain sum
          // wraps at 2^32 and would admit a wild StrTab pointer, so use the
          // overflow-safe range check.
          if (rangeInBounds(ST->symoff,
                            static_cast<uint64_t>(ST->nsyms) * NListSize,
                            Binary.size()) &&
              rangeInBounds(ST->stroff, ST->strsize, Binary.size())) {
            SymOff = ST->symoff;
            NSyms = ST->nsyms;
            StrTab = reinterpret_cast<const char *>(Binary.data() + ST->stroff);
            StrSize = ST->strsize;
          }
        }
        if (Cmd == LC_DYSYMTAB) {
          auto *DT = reinterpret_cast<const dysymtab_command *>(LCPtr);
          IndirectOff = DT->indirectsymoff;
          NIndirect = DT->nindirectsyms;
        }
      });

  if (!StrTab || NSyms == 0) {
    llvm::WithColor::warning() << "macho_reloc: no symtab found\n";
    return false;
  }

  SymtabNames.resize(NSyms);
  for (uint32_t I = 0; I < NSyms; ++I) {
    size_t EntryOff = SymOff + I * NListSize;
    if (!rangeInBounds(EntryOff, NListSize, Binary.size()))
      break;
    uint32_t NStrx;
    if (Hdr.Is64)
      NStrx =
          reinterpret_cast<const nlist_64 *>(Binary.data() + EntryOff)->n_strx;
    else
      NStrx = reinterpret_cast<const nlist *>(Binary.data() + EntryOff)->n_strx;
    if (NStrx > 0 && NStrx < StrSize)
      SymtabNames[I] = readFixedName(StrTab + NStrx, StrSize - NStrx);
  }

  const uint32_t *IndirectSyms = nullptr;
  if (IndirectOff > 0 &&
      rangeInBounds(IndirectOff,
                    static_cast<uint64_t>(NIndirect) * sizeof(uint32_t),
                    Binary.size()))
    IndirectSyms =
        reinterpret_cast<const uint32_t *>(Binary.data() + IndirectOff);

  auto ParseStubSection = [&](uint64_t SectAddr, uint64_t SectSize,
                              uint32_t /*SectOffset*/, uint32_t SectReserved1,
                              uint32_t SectReserved2, const char *SectName) {
    std::string SName = readMachOName(SectName);
    if (SName != section_names::macho::Stubs || !IndirectSyms)
      return;

    uint32_t StubSz = SectReserved2;
    if (StubSz == 0)
      StubSz = macho::getStubSize(TargetArch);
    uint32_t NStubEntries = SectSize / StubSz;
    uint32_t IndirectBase = SectReserved1;

    for (uint32_t SI = 0; SI < NStubEntries; ++SI) {
      RelocEntry E;
      E.Addr = SectAddr + static_cast<uint64_t>(SI) * StubSz;
      E.Size = StubSz;
      E.IsCode = true;

      uint32_t IndirectIdx = IndirectBase + SI;
      if (IndirectIdx < NIndirect) {
        uint32_t SymIdx = IndirectSyms[IndirectIdx];
        if (SymIdx < NSyms)
          E.Name = SymtabNames[SymIdx];
      }

      if (!E.Name.empty()) {
        ByName[E.Name] = Entries.size();
        Entries.push_back(E);
        LLVM_DEBUG(llvm::dbgs()
                   << "macho_reloc: stub '" << E.Name << "' at VA=0x"
                   << llvm::utohexstr(E.Addr) << "\n");
      }
    }
  };

  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t /*CmdSize*/, bool Is64) {
        uint32_t SegCmdID = getMachOSegmentCmdID(Is64);
        if (Cmd != SegCmdID)
          return;
        forEachMachOSectionAuto(LCPtr, Is64, ParseStubSection);
      });

  LLVM_DEBUG(llvm::dbgs() << "macho_reloc: parsed " << Entries.size()
                          << " stubs\n");
  return true;
}

bool MachORelocResolver::populateFromImage(const BinaryImage &Image,
                                           Arch TargetArch) {
  Entries.clear();
  ByName.clear();

  uint32_t StubSz = macho::getStubSize(TargetArch);

  for (const auto &Imp : Image.Imports) {
    if (Imp.Name.empty() || Imp.IATAddr == 0)
      continue;
    if (ByName.count(Imp.Name))
      continue;
    RelocEntry E;
    E.Name = Imp.Name;
    E.Addr = Imp.IATAddr;
    E.Size = StubSz;
    const Segment *Seg = Image.getSegmentFor(E.Addr);
    E.IsCode = Seg && Seg->isExecutable();
    ByName[E.Name] = Entries.size();
    Entries.push_back(std::move(E));
  }

  LLVM_DEBUG(llvm::dbgs() << "macho_reloc: populated " << Entries.size()
                          << " stub entries from image\n");
  return !Entries.empty();
}

} // namespace neverd
