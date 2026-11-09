//===- MachOStrictLayout.h - Fail-closed Mach-O layout checks -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared structural validation used by Mach-O metadata installers.  These
/// helpers validate known load-command structure, reserved header storage,
/// linear file-to-virtual mappings, and unique file/VM ownership before a
/// caller reasons about section capacity.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSTRICTLAYOUT_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSTRICTLAYOUT_H

#include "neverd/object/MachOLayout.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

namespace neverd::macho_patch_detail {

inline bool checkedAdd(uint64_t Left, uint64_t Right, uint64_t &Result) {
  if (Right > std::numeric_limits<uint64_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
}

inline bool checkedAlignUp(uint64_t Value, uint64_t Alignment,
                           uint64_t &Result) {
  if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
    return false;
  uint64_t Biased = 0;
  if (!checkedAdd(Value, Alignment - 1, Biased))
    return false;
  Result = Biased & ~(Alignment - 1);
  return true;
}

inline uint32_t getKnownLoadCommandMinimumSize(uint32_t CommandID) {
  using namespace llvm::MachO;
  switch (CommandID) {
#define HANDLE_LOAD_COMMAND(Name, Value, Type)                                 \
  case Name:                                                                   \
    return static_cast<uint32_t>(sizeof(Type));
#include "llvm/BinaryFormat/MachO.def"
  default:
    return static_cast<uint32_t>(sizeof(load_command));
  }
}

inline bool validateLoadCommandRegion(llvm::ArrayRef<uint8_t> Binary,
                                      MachOHeaderInfo &Header) {
  using namespace llvm::MachO;
  Header = parseMachOHeader(Binary.data(), Binary.size());
  if (Header.HeaderSize == 0 || Header.NCmds == 0 ||
      Header.SizeOfCmds >
          std::numeric_limits<uint64_t>::max() - Header.HeaderSize)
    return false;
  const uint64_t End = Header.HeaderSize + Header.SizeOfCmds;
  if (End > Binary.size())
    return false;

  uint64_t Cursor = Header.HeaderSize;
  for (uint32_t I = 0; I < Header.NCmds; ++I) {
    if (!rangeInBounds(Cursor, sizeof(load_command), End))
      return false;
    const auto *Command = reinterpret_cast<const load_command *>(
        Binary.data() + static_cast<size_t>(Cursor));
    if (Command->cmdsize < sizeof(load_command) ||
        !rangeInBounds(Cursor, Command->cmdsize, End) ||
        (Command->cmdsize % (Header.Is64 ? 8u : 4u)) != 0 ||
        Command->cmdsize < getKnownLoadCommandMinimumSize(Command->cmd) ||
        (Header.Is64 && Command->cmd == LC_SEGMENT) ||
        (!Header.Is64 && Command->cmd == LC_SEGMENT_64))
      return false;
    if (Command->cmd == LC_BUILD_VERSION) {
      const auto *Build =
          reinterpret_cast<const build_version_command *>(Command);
      const uint64_t ExpectedSize =
          sizeof(*Build) + uint64_t(Build->ntools) * sizeof(build_tool_version);
      if (ExpectedSize != Command->cmdsize)
        return false;
    }
    if (Command->cmd == getMachOSegmentCmdID(Header.Is64)) {
      const uint32_t BaseSize = getMachOSegmentCmdSize(Header.Is64);
      const uint32_t SectionSize = getMachOSectionSize(Header.Is64);
      if (Command->cmdsize < BaseSize)
        return false;
      const uint32_t SectionCount =
          Header.Is64
              ? reinterpret_cast<const segment_command_64 *>(Command)->nsects
              : reinterpret_cast<const segment_command *>(Command)->nsects;
      if (SectionCount >
              (std::numeric_limits<uint32_t>::max() - BaseSize) / SectionSize ||
          BaseSize + SectionCount * SectionSize != Command->cmdsize)
        return false;
    }
    Cursor += Command->cmdsize;
  }
  return Cursor == End;
}

struct MachOFileRange {
  uint64_t Begin = 0;
  uint64_t End = 0;
  uint64_t HeaderOff = 0;
};

/// Enumerate external load-command payloads after structural validation. Zero
/// bytes still belong to their declared owner; unknown commands reject reuse.
inline bool collectMachOPayloadRanges(llvm::ArrayRef<uint8_t> Binary,
                                      std::vector<MachOFileRange> &Ranges) {
  using namespace llvm::MachO;
  bool Valid = true;
  const auto Owner = [&](uint64_t Off, uint64_t Size) {
    if (!Size)
      return;
    if (!rangeInBounds(Off, Size, Binary.size())) {
      Valid = false;
      return;
    }
    Ranges.push_back({Off, Off + Size});
  };
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool Is64) {
        if (ID == getMachOSegmentCmdID(Is64)) {
          auto G = readMachOSegment(C, Is64);
          auto *P = C + getMachOSegmentCmdSize(Is64);
          for (unsigned I = 0; I < G.NSects;
               ++I, P += getMachOSectionSize(Is64)) {
            if (Is64) {
              const auto *S = reinterpret_cast<const section_64 *>(P);
              Owner(S->reloff, uint64_t(S->nreloc) * 8);
            } else {
              const auto *S = reinterpret_cast<const section *>(P);
              Owner(S->reloff, uint64_t(S->nreloc) * 8);
            }
          }
          return;
        }
        switch (ID) {
        case LC_SYMTAB: {
          const auto *T = reinterpret_cast<const symtab_command *>(C);
          Owner(T->symoff,
                uint64_t(T->nsyms) * (Is64 ? sizeof(nlist_64) : sizeof(nlist)));
          Owner(T->stroff, T->strsize);
          break;
        }
        case LC_DYSYMTAB: {
          const auto *D = reinterpret_cast<const dysymtab_command *>(C);
          Owner(D->tocoff, uint64_t(D->ntoc) * 8);
          Owner(D->modtaboff, uint64_t(D->nmodtab) * (Is64 ? 56 : 52));
          Owner(D->extrefsymoff, uint64_t(D->nextrefsyms) * 4);
          Owner(D->indirectsymoff, uint64_t(D->nindirectsyms) * 4);
          Owner(D->extreloff, uint64_t(D->nextrel) * 8);
          Owner(D->locreloff, uint64_t(D->nlocrel) * 8);
          break;
        }
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY: {
          const auto *D = reinterpret_cast<const dyld_info_command *>(C);
          Owner(D->rebase_off, D->rebase_size);
          Owner(D->bind_off, D->bind_size);
          Owner(D->weak_bind_off, D->weak_bind_size);
          Owner(D->lazy_bind_off, D->lazy_bind_size);
          Owner(D->export_off, D->export_size);
          break;
        }
        case LC_DYLD_CHAINED_FIXUPS:
        case LC_DYLD_EXPORTS_TRIE:
        case LC_FUNCTION_STARTS:
        case LC_DATA_IN_CODE:
        case LC_CODE_SIGNATURE: {
          const auto *D = reinterpret_cast<const linkedit_data_command *>(C);
          Owner(D->dataoff, D->datasize);
          break;
        }
        case LC_NOTE: {
          const auto *N = reinterpret_cast<const note_command *>(C);
          Owner(N->offset, N->size);
          break;
        }
        case LC_MAIN:
        case LC_UNIXTHREAD:
        case LC_UUID:
        case LC_BUILD_VERSION:
        case LC_VERSION_MIN_MACOSX:
        case LC_VERSION_MIN_IPHONEOS:
        case LC_VERSION_MIN_TVOS:
        case LC_VERSION_MIN_WATCHOS:
        case LC_LOAD_DYLINKER:
        case LC_ID_DYLIB:
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB:
        case LC_RPATH:
        case LC_SOURCE_VERSION:
          break;
        default:
          Valid = false;
          break;
        }
      });
  return Valid;
}

inline bool collectMachOFileRanges(llvm::ArrayRef<uint8_t> Binary,
                                   std::vector<MachOFileRange> &Segments,
                                   std::vector<MachOFileRange> &Sections) {
  using namespace llvm::MachO;
  MachOHeaderInfo Header;
  if (!validateLoadCommandRegion(Binary, Header))
    return false;
  const uint64_t LoadCommandsEnd = Header.HeaderSize + Header.SizeOfCmds;

  std::vector<MachOFileRange> SegmentVMRanges;
  std::vector<MachOFileRange> SectionVMRanges;
  bool Valid = true;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t CmdSize, bool Is64) {
        if (!Valid || Cmd != getMachOSegmentCmdID(Is64))
          return;
        const MachOSegFields Segment = readMachOSegment(LCPtr, Is64);
        const std::string SegmentName = readMachOName(Segment.SegName);
        uint64_t SegmentFileEnd = 0;
        uint64_t SegmentVMEnd = 0;
        if (!checkedAdd(Segment.FileOff, Segment.FileSize, SegmentFileEnd) ||
            !checkedAdd(Segment.VMAddr, Segment.VMSize, SegmentVMEnd) ||
            Segment.FileSize > Segment.VMSize ||
            !rangeInBounds(Segment.FileOff, Segment.FileSize, Binary.size())) {
          Valid = false;
          return;
        }
        if (Segment.FileSize != 0)
          Segments.push_back({Segment.FileOff, SegmentFileEnd,
                              static_cast<uint64_t>(LCPtr - Binary.data())});
        if (Segment.VMSize != 0)
          SegmentVMRanges.push_back(
              {Segment.VMAddr, SegmentVMEnd,
               static_cast<uint64_t>(LCPtr - Binary.data())});

        const uint32_t BaseSize = getMachOSegmentCmdSize(Is64);
        const uint32_t SectionSize = getMachOSectionSize(Is64);
        const uint32_t Count =
            Is64 ? reinterpret_cast<const segment_command_64 *>(LCPtr)->nsects
                 : reinterpret_cast<const segment_command *>(LCPtr)->nsects;
        if (BaseSize + uint64_t(Count) * SectionSize != CmdSize) {
          Valid = false;
          return;
        }
        for (uint32_t I = 0; I < Count; ++I) {
          const uint8_t *SectionPtr =
              LCPtr + BaseSize + uint64_t(I) * SectionSize;
          const auto *Section64 =
              Is64 ? reinterpret_cast<const section_64 *>(SectionPtr) : nullptr;
          const auto *Section32 =
              Is64 ? nullptr : reinterpret_cast<const section *>(SectionPtr);
          if (readMachOName(Is64 ? Section64->segname : Section32->segname) !=
              SegmentName) {
            Valid = false;
            return;
          }
          const uint64_t Address = Is64 ? Section64->addr : Section32->addr;
          const uint64_t Size = Is64 ? Section64->size : Section32->size;
          const uint32_t FileOff = Is64 ? Section64->offset : Section32->offset;
          const uint32_t Flags = Is64 ? Section64->flags : Section32->flags;
          uint64_t SectionVMEnd = 0;
          if (!checkedAdd(Address, Size, SectionVMEnd) ||
              Address < Segment.VMAddr || Address > SegmentVMEnd ||
              Size > SegmentVMEnd - Address) {
            Valid = false;
            return;
          }
          if (Size != 0)
            SectionVMRanges.push_back(
                {Address, SectionVMEnd,
                 static_cast<uint64_t>(SectionPtr - Binary.data())});
          if (Size == 0 ||
              isVirtualSection(static_cast<uint8_t>(Flags & SECTION_TYPE)))
            continue;
          uint64_t End = 0;
          if (!checkedAdd(FileOff, Size, End) || FileOff < LoadCommandsEnd ||
              FileOff < Segment.FileOff || End > SegmentFileEnd ||
              !rangeInBounds(FileOff, Size, Binary.size())) {
            Valid = false;
            return;
          }
          const uint64_t Delta = FileOff - Segment.FileOff;
          if (Delta > Segment.VMSize || Size > Segment.VMSize - Delta ||
              Delta > std::numeric_limits<uint64_t>::max() - Segment.VMAddr ||
              Segment.VMAddr + Delta != Address) {
            Valid = false;
            return;
          }
          Sections.push_back(
              {FileOff, End,
               static_cast<uint64_t>(SectionPtr - Binary.data())});
        }
      });
  if (!Valid)
    return false;

  const auto ByBegin = [](const MachOFileRange &Left,
                          const MachOFileRange &Right) {
    return std::tie(Left.Begin, Left.End, Left.HeaderOff) <
           std::tie(Right.Begin, Right.End, Right.HeaderOff);
  };
  std::sort(Segments.begin(), Segments.end(), ByBegin);
  std::sort(Sections.begin(), Sections.end(), ByBegin);
  std::sort(SegmentVMRanges.begin(), SegmentVMRanges.end(), ByBegin);
  std::sort(SectionVMRanges.begin(), SectionVMRanges.end(), ByBegin);
  const auto HasOverlap = [](const std::vector<MachOFileRange> &Ranges) {
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].Begin < Ranges[I - 1].End)
        return true;
    return false;
  };
  return !HasOverlap(Segments) && !HasOverlap(Sections) &&
         !HasOverlap(SegmentVMRanges) && !HasOverlap(SectionVMRanges);
}

} // namespace neverd::macho_patch_detail

#endif // NEVERD_LIB_BACKEND_CODEGEN_MACHO_MACHOSTRICTLAYOUT_H
