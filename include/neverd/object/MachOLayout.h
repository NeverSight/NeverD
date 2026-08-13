//===- MachOLayout.h - Mach-O header layout helpers ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Mach-O load-command and segment helpers for loaders and codegen.
/// Field access uses llvm::MachO::* structs — no raw load-command offsets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_OBJECT_MACHOLAYOUT_H
#define NEVERD_OBJECT_MACHOLAYOUT_H

#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace neverd {

struct MachOHeaderInfo {
  uint32_t NCmds = 0;
  uint32_t SizeOfCmds = 0;
  uint32_t HeaderSize = 0;
  uint32_t Flags = 0;
  bool Is64 = false;
};

inline MachOHeaderInfo parseMachOHeader(const uint8_t *Data, size_t Size) {
  using namespace llvm::MachO;
  MachOHeaderInfo Info{};
  if (Size < sizeof(mach_header))
    return Info;
  uint32_t Magic;
  std::memcpy(&Magic, Data, sizeof(Magic));
  Info.Is64 = (Magic == MH_MAGIC_64);
  if (Info.Is64) {
    if (Size < sizeof(mach_header_64))
      return Info;
    auto *Hdr = reinterpret_cast<const mach_header_64 *>(Data);
    Info.NCmds = Hdr->ncmds;
    Info.SizeOfCmds = Hdr->sizeofcmds;
    Info.Flags = Hdr->flags;
    Info.HeaderSize = sizeof(mach_header_64);
  } else if (Magic == MH_MAGIC) {
    auto *Hdr = reinterpret_cast<const mach_header *>(Data);
    Info.NCmds = Hdr->ncmds;
    Info.SizeOfCmds = Hdr->sizeofcmds;
    Info.Flags = Hdr->flags;
    Info.HeaderSize = sizeof(mach_header);
  }
  return Info;
}

inline void clearMachOPIEFlag(uint8_t *Data, bool Is64) {
  if (Is64) {
    auto *H = reinterpret_cast<llvm::MachO::mach_header_64 *>(Data);
    H->flags &= ~llvm::MachO::MH_PIE;
  } else {
    auto *H = reinterpret_cast<llvm::MachO::mach_header *>(Data);
    H->flags &= ~llvm::MachO::MH_PIE;
  }
}

inline void setMachOHeaderCmds(uint8_t *Data, bool Is64, uint32_t NCmds,
                               uint32_t SizeOfCmds) {
  if (Is64) {
    auto *H = reinterpret_cast<llvm::MachO::mach_header_64 *>(Data);
    H->ncmds = NCmds;
    H->sizeofcmds = SizeOfCmds;
  } else {
    auto *H = reinterpret_cast<llvm::MachO::mach_header *>(Data);
    H->ncmds = NCmds;
    H->sizeofcmds = SizeOfCmds;
  }
}

inline void shiftLinkeditField(uint32_t &Field, int64_t Shift) {
  if (Field != 0)
    Field += static_cast<uint32_t>(Shift);
}

inline void shiftMachOLoadCommandOffsets(uint8_t *LCPtr, uint32_t Cmd,
                                         int64_t Shift) {
  using namespace llvm::MachO;
  switch (Cmd) {
  case LC_SYMTAB: {
    auto *ST = reinterpret_cast<symtab_command *>(LCPtr);
    shiftLinkeditField(ST->symoff, Shift);
    shiftLinkeditField(ST->stroff, Shift);
    break;
  }
  case LC_DYSYMTAB: {
    auto *DT = reinterpret_cast<dysymtab_command *>(LCPtr);
    shiftLinkeditField(DT->tocoff, Shift);
    shiftLinkeditField(DT->modtaboff, Shift);
    shiftLinkeditField(DT->extrefsymoff, Shift);
    shiftLinkeditField(DT->indirectsymoff, Shift);
    shiftLinkeditField(DT->extreloff, Shift);
    shiftLinkeditField(DT->locreloff, Shift);
    break;
  }
  case LC_DYLD_INFO:
  case LC_DYLD_INFO_ONLY: {
    auto *DI = reinterpret_cast<dyld_info_command *>(LCPtr);
    shiftLinkeditField(DI->rebase_off, Shift);
    shiftLinkeditField(DI->bind_off, Shift);
    shiftLinkeditField(DI->weak_bind_off, Shift);
    shiftLinkeditField(DI->lazy_bind_off, Shift);
    shiftLinkeditField(DI->export_off, Shift);
    break;
  }
  case LC_CODE_SIGNATURE:
  case LC_FUNCTION_STARTS:
  case LC_DATA_IN_CODE:
  case LC_DYLD_CHAINED_FIXUPS:
  case LC_DYLD_EXPORTS_TRIE:
  case LC_SEGMENT_SPLIT_INFO: {
    auto *LD = reinterpret_cast<linkedit_data_command *>(LCPtr);
    shiftLinkeditField(LD->dataoff, Shift);
    break;
  }
  default:
    break;
  }
}

struct MachOSegFields {
  const char *SegName = nullptr;
  uint64_t VMAddr = 0;
  uint64_t VMSize = 0;
  uint64_t FileOff = 0;
  uint64_t FileSize = 0;
  uint32_t MaxProt = 0;
  uint32_t InitProt = 0;
  uint32_t NSects = 0;
};

inline MachOSegFields readMachOSegment(const uint8_t *LCPtr, bool Is64) {
  MachOSegFields F;
  if (Is64) {
    auto *S = reinterpret_cast<const llvm::MachO::segment_command_64 *>(LCPtr);
    F.SegName = S->segname;
    F.VMAddr = S->vmaddr;
    F.VMSize = S->vmsize;
    F.FileOff = S->fileoff;
    F.FileSize = S->filesize;
    F.MaxProt = S->maxprot;
    F.InitProt = S->initprot;
    F.NSects = S->nsects;
  } else {
    auto *S = reinterpret_cast<const llvm::MachO::segment_command *>(LCPtr);
    F.SegName = S->segname;
    F.VMAddr = S->vmaddr;
    F.VMSize = S->vmsize;
    F.FileOff = S->fileoff;
    F.FileSize = S->filesize;
    F.MaxProt = S->maxprot;
    F.InitProt = S->initprot;
    F.NSects = S->nsects;
  }
  return F;
}

inline void writeMachOSegment(uint8_t *LCPtr, bool Is64,
                              const MachOSegFields &F) {
  if (Is64) {
    auto *S = reinterpret_cast<llvm::MachO::segment_command_64 *>(LCPtr);
    S->vmaddr = F.VMAddr;
    S->vmsize = F.VMSize;
    S->fileoff = F.FileOff;
    S->filesize = F.FileSize;
    S->maxprot = F.MaxProt;
    S->initprot = F.InitProt;
  } else {
    auto *S = reinterpret_cast<llvm::MachO::segment_command *>(LCPtr);
    S->vmaddr = static_cast<uint32_t>(F.VMAddr);
    S->vmsize = static_cast<uint32_t>(F.VMSize);
    S->fileoff = static_cast<uint32_t>(F.FileOff);
    S->filesize = static_cast<uint32_t>(F.FileSize);
    S->maxprot = F.MaxProt;
    S->initprot = F.InitProt;
  }
}

template <typename SegCmd, typename Sect, typename Callback>
void forEachMachOSection(const uint8_t *LCPtr, Callback &&CB) {
  auto *Seg = reinterpret_cast<const SegCmd *>(LCPtr);
  auto *Sects = reinterpret_cast<const Sect *>(LCPtr + sizeof(SegCmd));
  for (uint32_t S = 0; S < Seg->nsects; ++S)
    CB(Sects[S].addr, Sects[S].size, Sects[S].offset, Sects[S].reserved1,
       Sects[S].reserved2, Sects[S].sectname);
}

/// Auto-dispatching overload that handles Is64 branching internally,
/// eliminating the repeated if/else pattern at every call site.
template <typename Callback>
void forEachMachOSectionAuto(const uint8_t *LCPtr, bool Is64, Callback &&CB) {
  using namespace llvm::MachO;
  if (Is64)
    forEachMachOSection<segment_command_64, section_64>(
        LCPtr, std::forward<Callback>(CB));
  else
    forEachMachOSection<segment_command, section>(LCPtr,
                                                  std::forward<Callback>(CB));
}

/// Get the Mach-O header size for the given class.
inline uint32_t getMachOHeaderSize(bool Is64) {
  return Is64 ? sizeof(llvm::MachO::mach_header_64)
              : sizeof(llvm::MachO::mach_header);
}

/// Get the segment command size for the given class.
inline uint32_t getMachOSegmentCmdSize(bool Is64) {
  return Is64 ? sizeof(llvm::MachO::segment_command_64)
              : sizeof(llvm::MachO::segment_command);
}

/// Get the LC_SEGMENT / LC_SEGMENT_64 command ID for the given class.
inline uint32_t getMachOSegmentCmdID(bool Is64) {
  return Is64 ? llvm::MachO::LC_SEGMENT_64 : llvm::MachO::LC_SEGMENT;
}

/// Get the nlist entry size for the given class.
inline size_t getMachONListSize(bool Is64) {
  return Is64 ? sizeof(llvm::MachO::nlist_64) : sizeof(llvm::MachO::nlist);
}

/// Get the Mach-O section entry size for the given class.
inline uint32_t getMachOSectionSize(bool Is64) {
  return Is64 ? sizeof(llvm::MachO::section_64) : sizeof(llvm::MachO::section);
}

template <typename Callback>
void forEachMachOLoadCommand(const uint8_t *Data, size_t Size, Callback &&CB) {
  auto Hdr = parseMachOHeader(Data, Size);
  if (Hdr.HeaderSize == 0)
    return;
  uint32_t Off = Hdr.HeaderSize;
  for (uint32_t I = 0; I < Hdr.NCmds; ++I) {
    if (!rangeInBounds(Off, sizeof(llvm::MachO::load_command), Size))
      break;
    auto *LC = reinterpret_cast<const llvm::MachO::load_command *>(Data + Off);
    uint32_t CmdSize = LC->cmdsize;
    // A corrupt cmdsize must not let consumers read the full command past the
    // buffer end; require the whole command to fit and to advance the cursor by
    // at least a header so the walk cannot stall or run backward.
    if (CmdSize < sizeof(llvm::MachO::load_command) ||
        !rangeInBounds(Off, CmdSize, Size))
      break;
    CB(Data + Off, LC->cmd, CmdSize, Hdr.Is64);
    Off += CmdSize;
  }
}

template <typename Callback>
void forEachMachOSegment(const uint8_t *Data, size_t Size, Callback &&CB) {
  forEachMachOLoadCommand(
      Data, Size,
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t /*CmdSize*/, bool Is64) {
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;
        CB(readMachOSegment(LCPtr, Is64), LCPtr);
      });
}

/// Locate a Mach-O section by name (e.g. \c "__text") for inplace rewriting.
inline bool findMachOSection(const std::vector<uint8_t> &Binary,
                             llvm::StringRef SectionName, uint64_t &VA,
                             uint64_t &Size, uint32_t &FileOff) {
  bool Found = false;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t /*CmdSize*/, bool Is64) {
        if (Found)
          return;
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;
        forEachMachOSectionAuto(LCPtr, Is64,
                                [&](uint64_t Addr, uint64_t SecSize,
                                    uint32_t SectOff, uint32_t /*R1*/,
                                    uint32_t /*R2*/, const char *SectName) {
                                  if (Found)
                                    return;
                                  if (readMachOName(SectName) != SectionName)
                                    return;
                                  VA = Addr;
                                  Size = SecSize;
                                  FileOff = SectOff;
                                  Found = true;
                                });
      });
  return Found;
}

/// Locate a Mach-O segment by name (e.g. "__TEXT").
inline bool findMachOSegment(const uint8_t *Data, size_t Size,
                             llvm::StringRef SegName, MachOSegFields &Out) {
  bool Found = false;
  forEachMachOSegment(Data, Size,
                      [&](const MachOSegFields &F, const uint8_t *) {
                        if (Found)
                          return;
                        if (readMachOName(F.SegName) == SegName) {
                          Out = F;
                          Found = true;
                        }
                      });
  return Found;
}

/// Adjust section headers within a segment after a shift.
/// Shifts file offsets for sections after \p TextFileoff and
/// updates the __text section size to \p NewTextSize.
inline void adjustMachOSections(
    uint8_t *LCPtr, bool Is64, uint32_t NSects, uint64_t TextFileoff,
    int64_t Shift, uint64_t NewTextSize,
    llvm::StringRef TargetSectName = section_names::macho::Text) {
  auto Adjust = [&](auto *Sects) {
    for (uint32_t S = 0; S < NSects; ++S) {
      if (Sects[S].offset > TextFileoff)
        Sects[S].offset += static_cast<uint32_t>(Shift);
      if (readMachOName(Sects[S].sectname) == TargetSectName)
        Sects[S].size = NewTextSize;
    }
  };
  if (Is64)
    Adjust(reinterpret_cast<llvm::MachO::section_64 *>(
        LCPtr + sizeof(llvm::MachO::segment_command_64)));
  else
    Adjust(reinterpret_cast<llvm::MachO::section *>(
        LCPtr + sizeof(llvm::MachO::segment_command)));
}

// ===--------------------------------------------------------------------===//
// Apple packed version fields (e.g. LC_BUILD_VERSION minos/sdk)
// ===--------------------------------------------------------------------===//

namespace packed_version {
constexpr uint32_t kMajorShift = 16;
constexpr uint32_t kMinorShift = 8;
constexpr uint32_t kMajorMask = 0xFFFF;
constexpr uint32_t kMinorMask = 0xFF;
constexpr uint32_t kPatchMask = 0xFF;

inline uint32_t getMajor(uint32_t V) { return (V >> kMajorShift) & kMajorMask; }
inline uint32_t getMinor(uint32_t V) { return (V >> kMinorShift) & kMinorMask; }
inline uint32_t getPatch(uint32_t V) { return V & kPatchMask; }

inline std::string toString(uint32_t V) {
  return std::to_string(getMajor(V)) + "." + std::to_string(getMinor(V)) + "." +
         std::to_string(getPatch(V));
}
} // namespace packed_version

// ===--------------------------------------------------------------------===//
// Codegen-layer helpers (segment building, chained fixups, droppable LCs)
// ===--------------------------------------------------------------------===//

/// Load commands that are safe to drop when inserting a new segment
/// (they reference __LINKEDIT data that may shift or become stale).
constexpr uint32_t kDroppableLinkEditCmds[] = {
    llvm::MachO::LC_CODE_SIGNATURE,
    llvm::MachO::LC_SOURCE_VERSION,
    llvm::MachO::LC_DATA_IN_CODE,
    llvm::MachO::LC_FUNCTION_STARTS,
};

inline bool shouldDropLoadCommand(uint32_t Cmd) {
  for (uint32_t C : kDroppableLinkEditCmds)
    if (C == Cmd)
      return true;
  return false;
}

/// Find the lowest file offset among all sections in the binary.
/// This is the upper bound for the header / load-command area.
inline uint64_t findFirstMachODataOffset(const uint8_t *Data, size_t Size) {
  uint64_t FirstOff = UINT64_MAX;
  auto Visitor = [&](uint64_t /*Addr*/, uint64_t /*SecSize*/, uint32_t SectOff,
                     uint32_t /*R1*/, uint32_t /*R2*/, const char * /*Name*/) {
    if (SectOff > 0 && SectOff < FirstOff)
      FirstOff = SectOff;
  };
  forEachMachOLoadCommand(
      Data, Size,
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t /*CmdSize*/, bool Is64) {
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;
        forEachMachOSectionAuto(LCPtr, Is64, Visitor);
      });
  return FirstOff;
}

/// Build a zero-initialized LC_SEGMENT / LC_SEGMENT_64 load command
/// buffer with the given segment name.
inline std::vector<uint8_t> buildMachOSegmentCmd(bool Is64,
                                                 llvm::StringRef SegName) {
  using namespace llvm::MachO;
  uint32_t CmdSize =
      Is64 ? sizeof(segment_command_64) : sizeof(segment_command);
  std::vector<uint8_t> Buf(CmdSize, 0);
  if (Is64) {
    auto *Seg = reinterpret_cast<segment_command_64 *>(Buf.data());
    Seg->cmd = LC_SEGMENT_64;
    Seg->cmdsize = CmdSize;
    std::memset(Seg->segname, 0, sizeof(Seg->segname));
    std::memcpy(Seg->segname, SegName.data(),
                std::min(SegName.size(), sizeof(Seg->segname)));
  } else {
    auto *Seg = reinterpret_cast<segment_command *>(Buf.data());
    Seg->cmd = LC_SEGMENT;
    Seg->cmdsize = CmdSize;
    std::memset(Seg->segname, 0, sizeof(Seg->segname));
    std::memcpy(Seg->segname, SegName.data(),
                std::min(SegName.size(), sizeof(Seg->segname)));
  }
  return Buf;
}

/// Alias for the LLVM chained fixups header type.
using ChainedFixupsHeader = llvm::MachO::dyld_chained_fixups_header;

/// Insert one new segment slot into the chained fixups starts table.
/// The new entry (with offset 0 = "no fixups") is placed before
/// __LINKEDIT (i.e. at index SegCount-1), which mirrors the segment
/// insertion point used by MachOPatcher.
///
/// Rebuilds the entire chained fixups blob in a temporary buffer,
/// grows the binary, and writes the new data back.
inline bool insertChainedFixupsSegment(std::vector<uint8_t> &Binary,
                                       uint32_t DataOff, uint32_t &DataSize) {
  if (!rangeInBounds(DataOff, DataSize, Binary.size()) ||
      DataSize < sizeof(ChainedFixupsHeader))
    return false;

  std::vector<uint8_t> OldBlob(Binary.data() + DataOff,
                               Binary.data() + DataOff + DataSize);

  auto *CFH = reinterpret_cast<const ChainedFixupsHeader *>(OldBlob.data());
  uint32_t StartsOff = CFH->starts_offset;
  uint32_t ImportsOff = CFH->imports_offset;
  uint32_t SymbolsOff = CFH->symbols_offset;

  if (!rangeInBounds(StartsOff, sizeof(uint32_t), DataSize))
    return false;

  uint32_t SegCount = readLE<uint32_t>(OldBlob.data() + StartsOff);
  uint32_t NewSegCount = SegCount + 1;
  constexpr uint32_t kSlot = sizeof(uint32_t);

  // SegCount is read from the (untrusted) blob; ensure the whole starts table
  // (the count word plus SegCount offset slots) fits so the per-entry reads
  // below stay in bounds and Offsets is not sized from a corrupt count.
  if (!rangeInBounds(static_cast<uint64_t>(StartsOff) + kSlot,
                     static_cast<uint64_t>(SegCount) * kSlot, DataSize))
    return false;

  uint32_t OldStartsHdrBytes = kSlot + SegCount * kSlot;
  uint32_t OldStartsEnd = StartsOff + OldStartsHdrBytes;

  uint32_t StartsBodyEnd = DataSize;
  if (ImportsOff > StartsOff && ImportsOff < StartsBodyEnd)
    StartsBodyEnd = ImportsOff;
  if (SymbolsOff > StartsOff && SymbolsOff < StartsBodyEnd)
    StartsBodyEnd = SymbolsOff;

  uint32_t SegInfoBytes =
      (StartsBodyEnd > OldStartsEnd) ? StartsBodyEnd - OldStartsEnd : 0;

  std::vector<uint32_t> Offsets(SegCount);
  for (uint32_t I = 0; I < SegCount; ++I)
    Offsets[I] =
        readLE<uint32_t>(OldBlob.data() + StartsOff + kSlot + I * kSlot);

  uint32_t InsertIdx = (SegCount > 0) ? SegCount - 1 : 0;
  Offsets.insert(Offsets.begin() + InsertIdx, 0u);

  for (uint32_t I = 0; I < NewSegCount; ++I)
    if (Offsets[I] != 0)
      Offsets[I] += kSlot;

  uint32_t NewStartsHdrBytes = kSlot + NewSegCount * kSlot;
  uint32_t Growth = NewStartsHdrBytes - OldStartsHdrBytes;
  uint32_t AlignGrowth = Growth;
  uint32_t RawEnd = DataSize + AlignGrowth;
  uint32_t Pad = (8 - (RawEnd % 8)) % 8;
  AlignGrowth += Pad;

  uint32_t NewDataSize = DataSize + AlignGrowth;
  std::vector<uint8_t> NewBlob(NewDataSize, 0);

  std::memcpy(NewBlob.data(), OldBlob.data(), std::min(StartsOff, DataSize));

  auto *NewHdr = reinterpret_cast<ChainedFixupsHeader *>(NewBlob.data());
  NewHdr->starts_offset = StartsOff;
  if (ImportsOff > StartsOff)
    NewHdr->imports_offset = ImportsOff + AlignGrowth;
  if (SymbolsOff > StartsOff)
    NewHdr->symbols_offset = SymbolsOff + AlignGrowth;

  uint8_t *NewStarts = NewBlob.data() + StartsOff;
  writeLE<uint32_t>(NewStarts, NewSegCount);
  for (uint32_t I = 0; I < NewSegCount; ++I)
    writeLE<uint32_t>(NewStarts + kSlot + I * kSlot, Offsets[I]);

  if (SegInfoBytes > 0 && OldStartsEnd + SegInfoBytes <= DataSize)
    std::memcpy(NewStarts + NewStartsHdrBytes, OldBlob.data() + OldStartsEnd,
                SegInfoBytes);

  uint32_t SrcOff = StartsBodyEnd;
  uint32_t DstOff = StartsBodyEnd + AlignGrowth;
  if (SrcOff < DataSize && DstOff < NewDataSize) {
    uint32_t Remaining = DataSize - SrcOff;
    std::memcpy(NewBlob.data() + DstOff, OldBlob.data() + SrcOff, Remaining);
  }

  size_t TrailingStart = DataOff + DataSize;
  size_t TrailingBytes =
      (Binary.size() > TrailingStart) ? Binary.size() - TrailingStart : 0;

  Binary.resize(Binary.size() + AlignGrowth, 0);

  if (TrailingBytes > 0)
    std::memmove(Binary.data() + TrailingStart + AlignGrowth,
                 Binary.data() + TrailingStart, TrailingBytes);

  std::memcpy(Binary.data() + DataOff, NewBlob.data(), NewDataSize);

  DataSize = NewDataSize;
  return true;
}

} // namespace neverd

#endif // NEVERD_OBJECT_MACHOLAYOUT_H
