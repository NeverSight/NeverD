//===- MachODyldBind.cpp - Mach-O dyld bind streams ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachODyldFixups.h"

#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/LEB128.h"

#include <cstring>
#include <limits>
#include <map>
#include <string>

namespace neverd {
namespace macho_loader {

using namespace llvm::MachO;

void parseBindStreams(const uint8_t *BasePtr, size_t FileSize,
                      const DyldInfoOffsets &DyldInfo, BinaryImage &Img) {
  if (!BasePtr)
    return;
  uint32_t BindOff = DyldInfo.BindOff;
  uint32_t BindSize = DyldInfo.BindSize;
  uint32_t LazyBindOff = DyldInfo.LazyBindOff;
  uint32_t LazyBindSize = DyldInfo.LazyBindSize;
  std::map<std::string, size_t> ImportIndex;
  for (size_t I = 0; I < Img.Imports.size(); ++I)
    ImportIndex[Img.Imports[I].Name] = I;

  auto ParseBindStream = [&](uint32_t Off, uint32_t Sz, bool IsLazy) {
    if (Off == 0 || Sz == 0 || !rangeInBounds(Off, Sz, FileSize))
      return;
    const uint8_t *P = BasePtr + Off;
    const uint8_t *End = P + Sz;

    std::string SymName;
    int64_t LibOrdinal = 0;
    uint8_t SegIdx = 0;
    uint64_t SegOff = 0;
    int64_t Addend = 0;
    uint8_t BindType = BIND_TYPE_POINTER;

    auto ReadULEB = [&](uint64_t &Val) -> bool {
      if (P >= End)
        return false;
      unsigned BytesRead = 0;
      const char *Error = nullptr;
      Val = llvm::decodeULEB128(P, &BytesRead, End, &Error);
      if (Error || BytesRead == 0)
        return false;
      P += BytesRead;
      return true;
    };
    auto ReadSLEB = [&](int64_t &Val) -> bool {
      if (P >= End)
        return false;
      unsigned BytesRead = 0;
      const char *Error = nullptr;
      Val = llvm::decodeSLEB128(P, &BytesRead, End, &Error);
      if (Error || BytesRead == 0)
        return false;
      P += BytesRead;
      return true;
    };

    while (P < End) {
      uint8_t Byte = *P++;
      uint8_t Opcode = Byte & BIND_OPCODE_MASK;
      uint8_t Imm = Byte & BIND_IMMEDIATE_MASK;

      switch (Opcode) {
      case BIND_OPCODE_DONE:
        // The ordinary bind stream ends here.  Lazy bindings instead use DONE
        // between independently interpretable entries, so their scan resumes
        // at the next opcode.
        if (!IsLazy)
          return;
        SymName.clear();
        LibOrdinal = 0;
        SegIdx = 0;
        SegOff = 0;
        Addend = 0;
        BindType = BIND_TYPE_POINTER;
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
        LibOrdinal = Imm;
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: {
        uint64_t Ordinal = 0;
        if (!ReadULEB(Ordinal) ||
            Ordinal >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
          return;
        LibOrdinal = static_cast<int64_t>(Ordinal);
        break;
      }
      case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
        if (Imm == 0)
          LibOrdinal = 0;
        else
          LibOrdinal = static_cast<int8_t>(BIND_OPCODE_MASK | Imm);
        break;
      case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
        size_t MaxLen = static_cast<size_t>(End - P);
        const void *Term = std::memchr(P, 0, MaxLen);
        if (!Term) {
          P = End;
          break;
        }
        const auto *TermPtr = static_cast<const uint8_t *>(Term);
        SymName.assign(reinterpret_cast<const char *>(P),
                       static_cast<size_t>(TermPtr - P));
        P = TermPtr + 1;
        break;
      }
      case BIND_OPCODE_SET_TYPE_IMM:
        BindType = Imm;
        break;
      case BIND_OPCODE_SET_ADDEND_SLEB: {
        if (!ReadSLEB(Addend))
          return;
        break;
      }
      case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: {
        SegIdx = Imm;
        if (!ReadULEB(SegOff))
          return;
        break;
      }
      case BIND_OPCODE_ADD_ADDR_ULEB: {
        uint64_t Delta = 0;
        if (!ReadULEB(Delta) || Delta > InvalidVA - SegOff)
          return;
        SegOff += Delta;
        break;
      }
      case BIND_OPCODE_DO_BIND:
      case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
      case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
      case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
        uint64_t Count = 1;
        uint64_t Skip = 0;
        if (Opcode == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB) {
          if (!ReadULEB(Count) || !ReadULEB(Skip))
            return;
          if (Count == 0)
            break;
        }

        const uint32_t PtrSz = Img.getPointerSize();
        if (PtrSz == 0)
          return;

        auto RecordAt = [&](uint64_t Offset) {
          if (SymName.empty() || SegIdx >= Img.Segments.size())
            return;
          const Segment &Seg = Img.Segments[SegIdx];
          if (Seg.Size < PtrSz || Offset > Seg.Size - PtrSz ||
              Offset > InvalidVA - Seg.VA)
            return;
          const va_t BindAddr = Seg.VA + Offset;
          std::string DylibName;
          if (LibOrdinal > 0 &&
              static_cast<size_t>(LibOrdinal) <= Img.DynInfo.NeededLibs.size())
            DylibName =
                Img.DynInfo.NeededLibs[static_cast<size_t>(LibOrdinal - 1)];

          auto It = ImportIndex.find(SymName);
          if (It != ImportIndex.end()) {
            if (!DylibName.empty())
              Img.Imports[It->second].Module = DylibName;
            if (Img.Imports[It->second].IATAddr == 0)
              Img.Imports[It->second].IATAddr = BindAddr;
          } else {
            Import Imp;
            Imp.Name = SymName;
            Imp.Module = DylibName.empty() ? kExternModule.str() : DylibName;
            Imp.IATAddr = BindAddr;
            ImportIndex[SymName] = Img.Imports.size();
            Img.Imports.push_back(std::move(Imp));
          }

          if (BindType == BIND_TYPE_POINTER) {
            detail::clearLocalPointerClassification(Img, BindAddr);
            Img.DyldBindSlots[BindAddr] = ImportBindSlot{SymName, Addend};
            Img.recordImportStorageSlot(BindAddr, SymName, Addend,
                                        ImportStorageEvidence::LoaderBind);
          }
        };

        if (Opcode == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB) {
          if (Skip > InvalidVA - PtrSz)
            return;
          const uint64_t Stride = PtrSz + Skip;
          constexpr uint64_t MaxBindSlots = 1u << 22;
          if (Count > MaxBindSlots || Count > (InvalidVA - SegOff) / Stride ||
              SymName.empty() || SegIdx >= Img.Segments.size())
            return;
          const Segment &Seg = Img.Segments[SegIdx];
          const uint64_t LastOff = SegOff + (Count - 1) * Stride;
          if (Seg.Size < PtrSz || SegOff > Seg.Size - PtrSz ||
              LastOff > Seg.Size - PtrSz)
            return;
          for (uint64_t I = 0; I < Count; ++I) {
            RecordAt(SegOff);
            if (Stride > InvalidVA - SegOff)
              return;
            SegOff += Stride;
          }
          break;
        }

        RecordAt(SegOff);
        if (PtrSz > InvalidVA - SegOff)
          return;
        SegOff += PtrSz;
        if (Opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB) {
          uint64_t Delta = 0;
          if (!ReadULEB(Delta) || Delta > InvalidVA - SegOff)
            return;
          SegOff += Delta;
        } else if (Opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED) {
          uint64_t Delta = static_cast<uint64_t>(Imm) * PtrSz;
          if (Delta > InvalidVA - SegOff)
            return;
          SegOff += Delta;
        }
        break;
      }
      default:
        break;
      }
    }
  };

  ParseBindStream(BindOff, BindSize, false);
  ParseBindStream(LazyBindOff, LazyBindSize, true);
}

} // namespace macho_loader
} // namespace neverd
