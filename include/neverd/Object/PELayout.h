//===- PELayout.h - PE/COFF header layout helpers ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PE/COFF optional-header and section-table helpers for loaders and
/// codegen.  Field access uses llvm::object::pe32_header /
/// pe32plus_header — no raw byte offsets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_OBJECT_PELAYOUT_H
#define NEVERD_OBJECT_PELAYOUT_H

#include "neverd/Support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"

#include <cstdint>
#include <cstring>

namespace neverd {

constexpr uint64_t kPE32PlusOrdinalFlag = 1ULL << 63;
constexpr uint64_t kPE32OrdinalFlag = 1ULL << 31;
constexpr uint32_t kILT_HintRVAMask = 0x7FFFFFFF;
constexpr uint32_t kHintNameHintSize = 2;

struct PEHeaderPtrs {
  uint32_t PeOffset = 0;
  bool Is64 = false;
  llvm::object::coff_file_header *FileHeader = nullptr;
  uint8_t *OptionalHeader = nullptr;
  uint8_t *SectionTable = nullptr;
  uint16_t NumSections = 0;

  bool valid() const { return FileHeader != nullptr; }
};

inline PEHeaderPtrs locatePEHeaders(uint8_t *Data, size_t Size) {
  using namespace llvm::object;
  PEHeaderPtrs P;
  if (Size < sizeof(dos_header))
    return P;
  auto *DOS = reinterpret_cast<const dos_header *>(Data);
  P.PeOffset = DOS->AddressOfNewExeHeader;
  if (!rangeInBounds(P.PeOffset, 4 + sizeof(coff_file_header), Size))
    return P;
  if (std::memcmp(Data + P.PeOffset, llvm::COFF::PEMagic, 4) != 0)
    return P;
  size_t OptOff =
      static_cast<size_t>(P.PeOffset) + 4 + sizeof(coff_file_header);
  // The optional-header magic (2 bytes) must be in bounds to classify the
  // format, and the full PE32/PE32+ struct must fit before any accessor reads
  // it — a truncated optional header otherwise triggers an out-of-bounds read.
  if (!rangeInBounds(OptOff, sizeof(uint16_t), Size))
    return P;
  uint8_t *OptHdr = Data + OptOff;
  bool Is64 = (readLE<uint16_t>(OptHdr) == llvm::COFF::PE32Header::PE32_PLUS);
  size_t OptStructSize = Is64 ? sizeof(pe32plus_header) : sizeof(pe32_header);
  if (!rangeInBounds(OptOff, OptStructSize, Size))
    return P;
  P.FileHeader = reinterpret_cast<coff_file_header *>(Data + P.PeOffset + 4);
  P.OptionalHeader = OptHdr;
  P.Is64 = Is64;
  P.NumSections = P.FileHeader->NumberOfSections;
  uint32_t OptSize = P.FileHeader->SizeOfOptionalHeader;
  P.SectionTable = P.OptionalHeader + OptSize;
  // NumSections and OptSize are untrusted: make sure the whole section table
  // lies within the buffer before any consumer indexes SectionTable[0..N).
  // Otherwise expose zero sections rather than let forEachPESection read OOB.
  size_t SecTableOff = OptOff + OptSize;
  size_t SecTableBytes =
      static_cast<size_t>(P.NumSections) * sizeof(coff_section);
  if (!rangeInBounds(SecTableOff, SecTableBytes, Size)) {
    P.NumSections = 0;
    P.SectionTable = nullptr;
  }
  return P;
}

inline const llvm::object::pe32plus_header *
getPE32PlusOptionalHeader(const PEHeaderPtrs &PE) {
  if (!PE.valid() || !PE.Is64)
    return nullptr;
  return reinterpret_cast<const llvm::object::pe32plus_header *>(
      PE.OptionalHeader);
}

inline const llvm::object::pe32_header *
getPE32OptionalHeader(const PEHeaderPtrs &PE) {
  if (!PE.valid() || PE.Is64)
    return nullptr;
  return reinterpret_cast<const llvm::object::pe32_header *>(PE.OptionalHeader);
}

inline void setPESizeOfImage(PEHeaderPtrs &PE, uint32_t NewSizeOfImage) {
  using namespace llvm::object;
  if (PE.Is64)
    reinterpret_cast<pe32plus_header *>(PE.OptionalHeader)->SizeOfImage =
        NewSizeOfImage;
  else
    reinterpret_cast<pe32_header *>(PE.OptionalHeader)->SizeOfImage =
        NewSizeOfImage;
}

inline uint32_t getPEFileAlignment(const PEHeaderPtrs &PE) {
  using namespace llvm::object;
  if (const auto *Opt = getPE32PlusOptionalHeader(PE))
    return Opt->FileAlignment;
  if (const auto *Opt = getPE32OptionalHeader(PE))
    return Opt->FileAlignment;
  return 0;
}

inline uint32_t getPESectionAlignment(const PEHeaderPtrs &PE) {
  using namespace llvm::object;
  if (const auto *Opt = getPE32PlusOptionalHeader(PE))
    return Opt->SectionAlignment;
  if (const auto *Opt = getPE32OptionalHeader(PE))
    return Opt->SectionAlignment;
  return 0;
}

inline uint32_t getPESizeOfImage(const PEHeaderPtrs &PE) {
  using namespace llvm::object;
  if (const auto *Opt = getPE32PlusOptionalHeader(PE))
    return Opt->SizeOfImage;
  if (const auto *Opt = getPE32OptionalHeader(PE))
    return Opt->SizeOfImage;
  return 0;
}

inline uint32_t getPESizeOfHeaders(const PEHeaderPtrs &PE) {
  using namespace llvm::object;
  if (const auto *Opt = getPE32PlusOptionalHeader(PE))
    return Opt->SizeOfHeaders;
  if (const auto *Opt = getPE32OptionalHeader(PE))
    return Opt->SizeOfHeaders;
  return 0;
}

inline uint32_t getPEAddressOfEntryPoint(const PEHeaderPtrs &PE) {
  using namespace llvm::object;
  if (const auto *Opt = getPE32PlusOptionalHeader(PE))
    return Opt->AddressOfEntryPoint;
  if (const auto *Opt = getPE32OptionalHeader(PE))
    return Opt->AddressOfEntryPoint;
  return 0;
}

inline uint64_t getPEImageBase(const PEHeaderPtrs &PE) {
  using namespace llvm::object;
  if (const auto *Opt = getPE32PlusOptionalHeader(PE))
    return Opt->ImageBase;
  if (const auto *Opt = getPE32OptionalHeader(PE))
    return Opt->ImageBase;
  return 0;
}

inline uint32_t getPENumberOfRvaAndSizes(const PEHeaderPtrs &PE) {
  using namespace llvm::object;
  if (const auto *Opt = getPE32PlusOptionalHeader(PE))
    return Opt->NumberOfRvaAndSize;
  if (const auto *Opt = getPE32OptionalHeader(PE))
    return Opt->NumberOfRvaAndSize;
  return 0;
}

inline void setPEAddressOfEntryPoint(PEHeaderPtrs &PE, uint32_t NewEntry) {
  using namespace llvm::object;
  if (PE.Is64)
    reinterpret_cast<pe32plus_header *>(PE.OptionalHeader)
        ->AddressOfEntryPoint = NewEntry;
  else
    reinterpret_cast<pe32_header *>(PE.OptionalHeader)->AddressOfEntryPoint =
        NewEntry;
}

// ===--------------------------------------------------------------------===//
// PE data directory access
// ===--------------------------------------------------------------------===//

inline const llvm::object::data_directory *
getPEDataDirectory(const PEHeaderPtrs &PE, uint32_t Index) {
  using namespace llvm::object;
  uint32_t NumDirs = getPENumberOfRvaAndSizes(PE);
  if (Index >= NumDirs)
    return nullptr;
  const uint8_t *DirBase = nullptr;
  if (PE.Is64)
    DirBase = PE.OptionalHeader + sizeof(pe32plus_header);
  else
    DirBase = PE.OptionalHeader + sizeof(pe32_header);
  return reinterpret_cast<const data_directory *>(DirBase) + Index;
}

// ===--------------------------------------------------------------------===//
// PE section table traversal
// ===--------------------------------------------------------------------===//

struct PESectionFields {
  const char *Name = nullptr;
  uint32_t VirtualSize = 0;
  uint32_t VirtualAddress = 0;
  uint32_t SizeOfRawData = 0;
  uint32_t PointerToRawData = 0;
  uint32_t Characteristics = 0;
};

template <typename Callback>
void forEachPESection(const PEHeaderPtrs &PE, Callback &&CB) {
  if (!PE.valid())
    return;
  const auto *SecTable =
      reinterpret_cast<const llvm::object::coff_section *>(PE.SectionTable);
  for (uint16_t I = 0; I < PE.NumSections; ++I) {
    PESectionFields F;
    F.Name = SecTable[I].Name;
    F.VirtualSize = SecTable[I].VirtualSize;
    F.VirtualAddress = SecTable[I].VirtualAddress;
    F.SizeOfRawData = SecTable[I].SizeOfRawData;
    F.PointerToRawData = SecTable[I].PointerToRawData;
    F.Characteristics = SecTable[I].Characteristics;
    CB(F, I);
  }
}

inline bool findPESection(const PEHeaderPtrs &PE, llvm::StringRef SectionName,
                          PESectionFields &Out) {
  bool Found = false;
  forEachPESection(PE, [&](const PESectionFields &F, uint16_t) {
    if (Found)
      return;
    if (readCOFFName(F.Name) == SectionName) {
      Out = F;
      Found = true;
    }
  });
  return Found;
}

/// Get the optional header size for the given PE class.
inline uint32_t getPEOptionalHeaderSize(bool Is64) {
  using namespace llvm::object;
  return Is64 ? sizeof(pe32plus_header) : sizeof(pe32_header);
}

/// Write a data directory entry at the given index.
inline void setPEDataDirectory(PEHeaderPtrs &PE, uint32_t Index, uint32_t RVA,
                               uint32_t Size) {
  using namespace llvm::object;
  uint32_t NumDirs = getPENumberOfRvaAndSizes(PE);
  if (Index >= NumDirs)
    return;
  uint8_t *DirBase = nullptr;
  if (PE.Is64)
    DirBase = PE.OptionalHeader + sizeof(pe32plus_header);
  else
    DirBase = PE.OptionalHeader + sizeof(pe32_header);
  auto *Dir = reinterpret_cast<data_directory *>(DirBase) + Index;
  Dir->RelativeVirtualAddress = RVA;
  Dir->Size = Size;
}

/// Clear the PE checksum field (invalidated by any binary modification).
inline void clearPEChecksum(PEHeaderPtrs &PE) {
  using namespace llvm::object;
  if (PE.Is64)
    reinterpret_cast<pe32plus_header *>(PE.OptionalHeader)->CheckSum = 0;
  else
    reinterpret_cast<pe32_header *>(PE.OptionalHeader)->CheckSum = 0;
}

/// Clear a data directory entry (e.g. certificates, debug, bound imports).
inline void clearPEDataDirectory(PEHeaderPtrs &PE, uint32_t Index) {
  setPEDataDirectory(PE, Index, 0, 0);
}

/// Get the mutable COFF section table pointer.
inline llvm::object::coff_section *getPESectionTable(PEHeaderPtrs &PE) {
  if (!PE.valid())
    return nullptr;
  return reinterpret_cast<llvm::object::coff_section *>(PE.SectionTable);
}

/// Find a writable PE section by name. Returns nullptr if not found.
inline llvm::object::coff_section *
findPESectionMut(PEHeaderPtrs &PE, llvm::StringRef SectionName) {
  auto *SecTable = getPESectionTable(PE);
  if (!SecTable)
    return nullptr;
  for (uint16_t I = 0; I < PE.NumSections; ++I) {
    if (readCOFFName(SecTable[I].Name) == SectionName)
      return &SecTable[I];
  }
  return nullptr;
}

} // namespace neverd

#endif // NEVERD_OBJECT_PELAYOUT_H
