//===- COFFLoaderUtils.cpp - COFF/PE loader helpers ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFLoaderUtils.h"

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/ISAEncoding.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/CVDebugRecord.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <set>

#define DEBUG_TYPE "neverd-coff-loader"

namespace neverd {
namespace coff_loader {

using namespace llvm::COFF;
using namespace llvm::object;

namespace {

enum class DelayAddressMode { RVA, VA };

constexpr uint32_t kDelayAddressIsRVA = 1;

std::optional<va_t> resolveDelayField(uint32_t Raw, DelayAddressMode Mode,
                                      const BinaryImage &Img) {
  if (Raw == 0)
    return std::nullopt;
  va_t Addr = Raw;
  if (Mode == DelayAddressMode::RVA) {
    if (Raw > InvalidVA - Img.Base)
      return std::nullopt;
    Addr = Img.Base + Raw;
  }
  if (!Img.containsVA(Addr))
    return std::nullopt;
  return Addr;
}

std::optional<std::string> readMappedCString(const BinaryImage &Img,
                                             va_t Addr) {
  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable())
    return std::nullopt;
  size_t Off = static_cast<size_t>(Addr - Seg->VA);
  if (Off >= Seg->Data.size())
    return std::nullopt;
  const char *Begin = reinterpret_cast<const char *>(Seg->Data.data() + Off);
  size_t MaxLen = Seg->Data.size() - Off;
  const void *End = std::memchr(Begin, '\0', MaxLen);
  if (!End)
    return std::nullopt;
  return std::string(Begin, static_cast<const char *>(End));
}

size_t mappedEntryCapacity(const BinaryImage &Img, va_t Addr,
                           uint32_t EntrySize) {
  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable() || EntrySize == 0)
    return 0;
  size_t Off = static_cast<size_t>(Addr - Seg->VA);
  if (Off >= Seg->Data.size())
    return 0;
  return (Seg->Data.size() - Off) / EntrySize;
}

struct ResolvedDelayDescriptor {
  DelayAddressMode Mode;
  va_t IATAddr;
  va_t INTAddr;
  std::string Module;
};

std::optional<ResolvedDelayDescriptor>
resolveDelayDescriptor(const delay_import_directory_table_entry &Desc,
                       const BinaryImage &Img) {
  auto TryMode =
      [&](DelayAddressMode Mode) -> std::optional<ResolvedDelayDescriptor> {
    auto NameAddr = resolveDelayField(Desc.Name, Mode, Img);
    auto IATAddr = resolveDelayField(Desc.DelayImportAddressTable, Mode, Img);
    auto INTAddr = resolveDelayField(Desc.DelayImportNameTable, Mode, Img);
    if (!NameAddr || !IATAddr || !INTAddr)
      return std::nullopt;
    auto Module = readMappedCString(Img, *NameAddr);
    uint32_t PtrSize = Img.getPointerSize();
    if (!Module || Module->empty() || !Img.readVA(*IATAddr, PtrSize) ||
        !Img.readVA(*INTAddr, PtrSize))
      return std::nullopt;
    return ResolvedDelayDescriptor{Mode, *IATAddr, *INTAddr,
                                   std::move(*Module)};
  };

  // dlattrRva explicitly selects RVA fields.  Zero-attribute descriptors in
  // current PE files are also documented as RVAs, while old delayimp images
  // used absolute VAs; mapped-address validation makes both forms safe.
  if ((static_cast<uint32_t>(Desc.Attributes) & kDelayAddressIsRVA) != 0)
    return TryMode(DelayAddressMode::RVA);
  if (auto Resolved = TryMode(DelayAddressMode::RVA))
    return Resolved;
  return TryMode(DelayAddressMode::VA);
}

} // anonymous namespace

void addImportedSymbol(const llvm::object::imported_symbol_iterator &SI,
                       llvm::StringRef ModuleName, va_t IATAddr,
                       BinaryImage &Img) {
  Import Imp;
  Imp.Module = ModuleName.str();
  Imp.IATAddr = IATAddr;

  bool ByOrd = false;
  if (auto Err = SI->isOrdinal(ByOrd))
    llvm::consumeError(std::move(Err));

  if (ByOrd) {
    uint16_t Ord = 0;
    if (auto Err = SI->getOrdinal(Ord))
      llvm::consumeError(std::move(Err));
    Imp.Ordinal = Ord;
    Imp.Name = (kOrdinalPrefix + llvm::Twine(Ord)).str();
  } else {
    llvm::StringRef SymName;
    if (auto Err = SI->getSymbolName(SymName))
      llvm::consumeError(std::move(Err));
    else
      Imp.Name = SymName.str();
  }

  Img.Imports.push_back(std::move(Imp));
}

size_t
parseDelayImportDescriptor(const delay_import_directory_table_entry &Desc,
                           BinaryImage &Img) {
  auto Resolved = resolveDelayDescriptor(Desc, Img);
  if (!Resolved)
    return 0;

  const uint32_t PtrSize = Img.getPointerSize();
  size_t Count = std::min(mappedEntryCapacity(Img, Resolved->IATAddr, PtrSize),
                          mappedEntryCapacity(Img, Resolved->INTAddr, PtrSize));
  size_t Added = 0;
  for (size_t I = 0; I < Count; ++I) {
    if (I > (InvalidVA - Resolved->INTAddr) / PtrSize ||
        I > (InvalidVA - Resolved->IATAddr) / PtrSize)
      break;
    va_t INTSlot = Resolved->INTAddr + I * PtrSize;
    va_t IATSlot = Resolved->IATAddr + I * PtrSize;
    const uint8_t *P = Img.readVA(INTSlot, PtrSize);
    if (!P || !Img.readVA(IATSlot, PtrSize))
      break;
    uint64_t Raw = readPtr(P, Img.is64Bit());
    if (Raw == 0)
      break;

    Import Imp;
    Imp.Module = (llvm::Twine(Resolved->Module) + kDelayImportSuffix).str();
    Imp.IATAddr = IATSlot;

    const uint64_t OrdinalMask =
        Img.is64Bit() ? (uint64_t(1) << 63) : (uint64_t(1) << 31);
    if ((Raw & OrdinalMask) != 0) {
      Imp.Ordinal = static_cast<uint16_t>(Raw & 0xffffu);
      Imp.Name = (kOrdinalPrefix + llvm::Twine(Imp.Ordinal)).str();
    } else {
      if (Raw > std::numeric_limits<uint32_t>::max())
        continue;
      auto HintNameAddr =
          resolveDelayField(static_cast<uint32_t>(Raw), Resolved->Mode, Img);
      if (!HintNameAddr || *HintNameAddr > InvalidVA - sizeof(uint16_t))
        continue;
      auto Name = readMappedCString(Img, *HintNameAddr + sizeof(uint16_t));
      if (!Name || Name->empty())
        continue;
      Imp.Name = std::move(*Name);
    }

    Img.Imports.push_back(std::move(Imp));
    ++Added;
  }
  return Added;
}

void parseDelayImports(const COFFObjectFile &Obj, BinaryImage &Img) {
  [[maybe_unused]] size_t Added = 0;
  for (auto I = Obj.delay_import_directory_begin(),
            E = Obj.delay_import_directory_end();
       I != E; ++I) {
    const delay_import_directory_table_entry *Desc = nullptr;
    if (auto Err = I->getDelayImportTable(Desc)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    if (Desc)
      Added += parseDelayImportDescriptor(*Desc, Img);
  }
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Added << " delay imports\n");
}

void parseSymbolTable(const COFFObjectFile &Obj, BinaryImage &Img,
                      uint64_t ImageBase) {
  for (const auto &SymRef : Obj.symbols()) {
    COFFSymbolRef CoffSym = Obj.getCOFFSymbol(SymRef);

    auto NameOrErr = Obj.getSymbolName(CoffSym);
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (NameOrErr->empty())
      continue;

    if (CoffSym.getSectionNumber() == llvm::COFF::IMAGE_SYM_UNDEFINED)
      continue;

    uint64_t Addr = CoffSym.getValue();
    if (CoffSym.getSectionNumber() > 0) {
      auto SecOrErr = Obj.getSection(CoffSym.getSectionNumber());
      if (SecOrErr) {
        uint64_t Offset = static_cast<uint64_t>((*SecOrErr)->VirtualAddress) +
                          CoffSym.getValue();
        if (Offset > InvalidVA - ImageBase)
          continue;
        Addr = ImageBase + Offset;
      } else {
        llvm::consumeError(SecOrErr.takeError());
      }
    }
    bool IsFunc =
        CoffSym.getComplexType() == llvm::COFF::IMAGE_SYM_DTYPE_FUNCTION;
    if (IsFunc)
      Addr = normalizeCodeAddress(Addr, Img.Arch, Img.Mode);
    if (Addr == 0)
      continue;

    Symbol S;
    S.Name = NameOrErr->str();
    S.Addr = Addr;
    S.IsFunc = IsFunc;
    Img.Symbols.push_back(std::move(S));
  }
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Img.Symbols.size()
                          << " symbols from symbol table\n");
}

void parseTLSDirectory(const COFFObjectFile &Obj, BinaryImage &Img,
                       uint64_t /*ImageBase*/) {
  bool Is64 = (Obj.getPE32PlusHeader() != nullptr);
  uint32_t PtrSize = getPointerSize(Is64);

  uint64_t CallbackTableVA = 0;
  if (Is64) {
    if (const auto *TLS = Obj.getTLSDirectory64())
      CallbackTableVA = TLS->AddressOfCallBacks;
  } else {
    if (const auto *TLS = Obj.getTLSDirectory32())
      CallbackTableVA = TLS->AddressOfCallBacks;
  }

  if (CallbackTableVA == 0)
    return;

  const Segment *TableSeg = Img.getSegmentFor(CallbackTableVA);
  if (!TableSeg || !TableSeg->isReadable())
    return;
  size_t TableOff = static_cast<size_t>(CallbackTableVA - TableSeg->VA);
  if (TableOff >= TableSeg->Data.size())
    return;

  [[maybe_unused]] size_t Added = 0;
  auto Existing = Img.getSymbolAddresses();
  size_t MaxCallbacks = (TableSeg->Data.size() - TableOff) / PtrSize;
  for (size_t I = 0; I < MaxCallbacks; ++I) {
    if (I > (InvalidVA - CallbackTableVA) / PtrSize)
      break;
    const uint8_t *P = Img.readVA(CallbackTableVA + I * PtrSize, PtrSize);
    if (!P)
      break;
    uint64_t RawAddr = readPtr(P, Is64);
    if (RawAddr == 0)
      break;
    uint64_t Addr = normalizeCodeAddress(RawAddr, Img.Arch, Img.Mode);
    if (!Img.recordRuntimeFunction(Addr))
      continue;
    if (Existing.insert(Addr).second) {
      Symbol S;
      S.Name = (kTLSCallbackPrefix + llvm::utohexstr(Addr)).str();
      S.Addr = Addr;
      S.IsFunc = true;
      Img.Symbols.push_back(std::move(S));
      ++Added;
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "coff: TLS directory found " << Added
                          << " callbacks\n");
}

void parseBaseRelocations(const COFFObjectFile &Obj, BinaryImage &Img,
                          uint64_t ImageBase) {
  const data_directory *RelocDir =
      Obj.getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
  if (!RelocDir || RelocDir->RelativeVirtualAddress == 0 || RelocDir->Size == 0)
    return;

  uintptr_t RelocPtr;
  if (auto Err = Obj.getRvaPtr(RelocDir->RelativeVirtualAddress, RelocPtr)) {
    llvm::consumeError(std::move(Err));
    return;
  }

  using BaseRelocBlock = llvm::object::coff_base_reloc_block_header;
  constexpr uint32_t BlockHeaderSize = sizeof(BaseRelocBlock);
  // RelocDir->Size is untrusted and getRvaPtr only validates the start; bound
  // the walk by the smaller of the directory size and the bytes remaining in
  // the file buffer.
  llvm::StringRef FileData = Obj.getData();
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(FileData.data());
  uintptr_t FileEnd = FileBegin + FileData.size();
  if (RelocPtr < FileBegin || RelocPtr > FileEnd)
    return;
  size_t AvailBytes = FileEnd - RelocPtr;
  const uint8_t *P = reinterpret_cast<const uint8_t *>(RelocPtr);
  const uint8_t *End = P + std::min<size_t>(RelocDir->Size, AvailBytes);
  while (static_cast<size_t>(End - P) >= BlockHeaderSize) {
    BaseRelocBlock Block;
    std::memcpy(&Block, P, sizeof(Block));
    uint32_t PageRVA = Block.PageRVA;
    uint32_t BlockSize = Block.BlockSize;
    if (BlockSize < BlockHeaderSize || BlockSize > static_cast<size_t>(End - P))
      break;
    uint32_t Count = (BlockSize - BlockHeaderSize) / 2;
    for (uint32_t I = 0; I < Count; ++I) {
      uint16_t Entry;
      std::memcpy(&Entry, P + BlockHeaderSize + I * sizeof(Entry),
                  sizeof(Entry));
      uint8_t Type = Entry >> kBaseRelocOffsetBits;
      uint16_t Offset = Entry & kBaseRelocOffsetMask;
      if (Type == llvm::COFF::IMAGE_REL_BASED_ABSOLUTE)
        continue;
      uint64_t RelocRVA = static_cast<uint64_t>(PageRVA) + Offset;
      if (RelocRVA > InvalidVA - ImageBase)
        continue;
      BaseRelocation BR;
      BR.Address = ImageBase + RelocRVA;
      BR.Type = Type;
      Img.BaseRelocations.push_back(BR);
    }
    P += BlockSize;
  }
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Img.BaseRelocations.size()
                          << " base relocations\n");
}

void parseDebugDirectory(const COFFObjectFile &Obj, BinaryImage &Img) {
  const data_directory *DebugDir =
      Obj.getDataDirectory(llvm::COFF::DEBUG_DIRECTORY);
  if (!DebugDir || DebugDir->RelativeVirtualAddress == 0 ||
      DebugDir->Size < sizeof(llvm::object::debug_directory))
    return;

  uintptr_t DebugPtr;
  if (auto Err = Obj.getRvaPtr(DebugDir->RelativeVirtualAddress, DebugPtr)) {
    llvm::consumeError(std::move(Err));
    return;
  }

  // DebugDir->Size is untrusted and getRvaPtr only validates the start; clamp
  // the entry count to the bytes present up to the end of the file buffer.
  llvm::StringRef FileData = Obj.getData();
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(FileData.data());
  uintptr_t FileEnd = FileBegin + FileData.size();
  size_t DbgAvail =
      (DebugPtr >= FileBegin && DebugPtr <= FileEnd) ? (FileEnd - DebugPtr) : 0;
  uint32_t NumEntries =
      static_cast<uint32_t>(std::min<size_t>(DebugDir->Size, DbgAvail) /
                            sizeof(llvm::object::debug_directory));
  const auto *EntryBytes = reinterpret_cast<const uint8_t *>(DebugPtr);

  using PDB70 = llvm::codeview::PDB70DebugInfo;
  for (uint32_t I = 0; I < NumEntries; ++I) {
    llvm::object::debug_directory DbgEntry;
    std::memcpy(&DbgEntry, EntryBytes + I * sizeof(DbgEntry), sizeof(DbgEntry));
    if (DbgEntry.Type != llvm::COFF::IMAGE_DEBUG_TYPE_CODEVIEW)
      continue;
    uint32_t DataRVA = DbgEntry.AddressOfRawData;
    uint32_t DataSize = DbgEntry.SizeOfData;
    if (DataRVA == 0 || DataSize < sizeof(PDB70))
      continue;
    uintptr_t CvPtr;
    if (auto Err = Obj.getRvaPtr(DataRVA, CvPtr)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    // getRvaPtr validates only the start RVA; bound the record (and the
    // trailing PDB path string) by the bytes actually present in the file
    // buffer.
    size_t CvAvail =
        (CvPtr >= FileBegin && CvPtr <= FileEnd) ? (FileEnd - CvPtr) : 0;
    if (CvAvail < sizeof(PDB70))
      continue;
    PDB70 Info;
    std::memcpy(&Info, reinterpret_cast<const void *>(CvPtr), sizeof(Info));
    if (Info.CVSignature == llvm::OMF::Signature::PDB70) {
      const char *Path = reinterpret_cast<const char *>(CvPtr + sizeof(PDB70));
      size_t MaxLen = std::min<size_t>(DataSize, CvAvail) - sizeof(PDB70);
      Img.DynInfo.PDBPath = readFixedName(Path, MaxLen);
      LLVM_DEBUG(llvm::dbgs()
                 << "coff: PDB path = " << Img.DynInfo.PDBPath << "\n");
    }
  }
}

namespace {
template <typename LoadCfgT>
void extractLoadCfgFields(uintptr_t CfgPtr, size_t AvailableSize,
                          uint64_t ImageBase, BinaryImage &Img) {
  if (AvailableSize < sizeof(uint32_t))
    return;
  LoadCfgT Cfg{};
  std::memcpy(&Cfg, reinterpret_cast<const void *>(CfgPtr),
              std::min(AvailableSize, sizeof(Cfg)));
  auto Has = [&](size_t Offset, size_t Size) {
    return rangeInBounds(Offset, Size, AvailableSize);
  };
  auto ToRVA = [&](uint64_t VA) -> va_t {
    return VA >= ImageBase ? VA - ImageBase : 0;
  };

  if (Has(offsetof(LoadCfgT, SecurityCookie), sizeof(Cfg.SecurityCookie)) &&
      Cfg.SecurityCookie >= ImageBase)
    Img.DynInfo.SecurityCookieRVA = Cfg.SecurityCookie - ImageBase;
  if (Has(offsetof(LoadCfgT, GuardCFCheckFunction),
          sizeof(Cfg.GuardCFCheckFunction)) &&
      Cfg.GuardCFCheckFunction >= ImageBase)
    Img.DynInfo.GuardCFCheckFunctionRVA = Cfg.GuardCFCheckFunction - ImageBase;
  if (Has(offsetof(LoadCfgT, GuardFlags), sizeof(Cfg.GuardFlags)))
    Img.DynInfo.GuardFlags = Cfg.GuardFlags;
  if (Has(offsetof(LoadCfgT, GuardCFFunctionTable),
          sizeof(Cfg.GuardCFFunctionTable)))
    Img.DynInfo.GuardCFFunctionTableRVA = ToRVA(Cfg.GuardCFFunctionTable);
  if (Has(offsetof(LoadCfgT, GuardCFFunctionCount),
          sizeof(Cfg.GuardCFFunctionCount)))
    Img.DynInfo.GuardCFFunctionCount = Cfg.GuardCFFunctionCount;
  if (Has(offsetof(LoadCfgT, GuardEHContinuationTable),
          sizeof(Cfg.GuardEHContinuationTable)))
    Img.DynInfo.GuardEHContinuationTableRVA =
        ToRVA(Cfg.GuardEHContinuationTable);
  if (Has(offsetof(LoadCfgT, GuardEHContinuationCount),
          sizeof(Cfg.GuardEHContinuationCount)))
    Img.DynInfo.GuardEHContinuationCount = Cfg.GuardEHContinuationCount;
}
} // anonymous namespace

void parseLoadConfiguration(const COFFObjectFile &Obj, BinaryImage &Img,
                            uint64_t ImageBase) {
  bool Is64 = (Obj.getPE32PlusHeader() != nullptr);
  const data_directory *LoadCfgDir =
      Obj.getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  if (!LoadCfgDir || LoadCfgDir->RelativeVirtualAddress == 0 ||
      LoadCfgDir->Size == 0)
    return;

  Img.DynInfo.LoadConfigRVA = LoadCfgDir->RelativeVirtualAddress;

  uintptr_t CfgPtr;
  if (auto Err = Obj.getRvaPtr(LoadCfgDir->RelativeVirtualAddress, CfgPtr)) {
    llvm::consumeError(std::move(Err));
    return;
  }

  llvm::StringRef FileData = Obj.getData();
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(FileData.data());
  uintptr_t FileEnd = FileBegin + FileData.size();
  size_t AvailableSize =
      (CfgPtr >= FileBegin && CfgPtr <= FileEnd) ? (FileEnd - CfgPtr) : 0;
  AvailableSize = std::min<size_t>(AvailableSize, LoadCfgDir->Size);
  if (AvailableSize < sizeof(uint32_t))
    return;
  uint32_t DeclaredSize =
      readLE<uint32_t>(reinterpret_cast<const uint8_t *>(CfgPtr));
  if (DeclaredSize < sizeof(uint32_t))
    return;
  AvailableSize = std::min<size_t>(AvailableSize, DeclaredSize);
  Img.DynInfo.LoadConfigSize = static_cast<uint32_t>(AvailableSize);

  if (Is64)
    extractLoadCfgFields<llvm::object::coff_load_configuration64>(
        CfgPtr, AvailableSize, ImageBase, Img);
  else
    extractLoadCfgFields<llvm::object::coff_load_configuration32>(
        CfgPtr, AvailableSize, ImageBase, Img);

  LLVM_DEBUG({
    if (Img.DynInfo.SecurityCookieRVA != 0)
      llvm::dbgs() << "coff: security cookie RVA=0x"
                   << llvm::utohexstr(Img.DynInfo.SecurityCookieRVA) << "\n";
    if (Img.DynInfo.GuardCFCheckFunctionRVA != 0)
      llvm::dbgs() << "coff: CF Guard check RVA=0x"
                   << llvm::utohexstr(Img.DynInfo.GuardCFCheckFunctionRVA)
                   << "\n";
  });
}

} // namespace coff_loader
} // namespace neverd
