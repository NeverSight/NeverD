//===- COFFLoaderUtils.cpp - COFF/PE loader helpers ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFLoaderUtils.h"

#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/CVDebugRecord.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <vector>

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

namespace {

llvm::Error codeViewRangeError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "coff CodeView: " + Message);
}

llvm::Error validateRawBackedSections(
    llvm::ArrayRef<detail::RawBackedSectionRange> Sections, uint64_t FileSize) {
  constexpr uint64_t RVAAddressSpace = uint64_t{1} << 32;
  for (const detail::RawBackedSectionRange &Section : Sections) {
    const uint64_t VirtualEnd =
        static_cast<uint64_t>(Section.RVA) + Section.VirtualSize;
    if (VirtualEnd > RVAAddressSpace)
      return codeViewRangeError("section virtual range overflows");
    if (Section.RawSize == 0)
      continue;
    const uint64_t RawEnd =
        static_cast<uint64_t>(Section.FileOffset) + Section.RawSize;
    if (Section.FileOffset == 0 || RawEnd > FileSize)
      return codeViewRangeError("section raw range is outside the file");
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> resolveUniqueRawFileRange(
    llvm::ArrayRef<detail::RawBackedSectionRange> Sections, uint64_t FileSize,
    uint32_t FileOffset, uint32_t Size) {
  if (FileOffset == 0 || Size == 0)
    return codeViewRangeError("raw range is absent or empty");
  if (auto Error = validateRawBackedSections(Sections, FileSize))
    return std::move(Error);

  const uint64_t RequestedBegin = FileOffset;
  const uint64_t RequestedEnd = RequestedBegin + Size;
  if (RequestedEnd > FileSize)
    return codeViewRangeError("raw range is outside the file");

  const detail::RawBackedSectionRange *Owner = nullptr;
  for (const detail::RawBackedSectionRange &Section : Sections) {
    if (Section.RawSize == 0)
      continue;
    const uint64_t SectionBegin = Section.FileOffset;
    const uint64_t SectionEnd = SectionBegin + Section.RawSize;
    if (RequestedBegin >= SectionEnd || SectionBegin >= RequestedEnd)
      continue;
    if (Owner)
      return codeViewRangeError("raw range has overlapping section owners");
    Owner = &Section;
  }
  if (!Owner)
    return codeViewRangeError("raw range has no section owner");

  const uint64_t OwnerBegin = Owner->FileOffset;
  const uint64_t OwnerEnd = OwnerBegin + Owner->RawSize;
  if (RequestedBegin < OwnerBegin || RequestedEnd > OwnerEnd)
    return codeViewRangeError("raw range crosses its section boundary");
  const uint64_t Delta = RequestedBegin - OwnerBegin;
  if (Delta + Size > Owner->VirtualSize)
    return codeViewRangeError("raw range crosses the section virtual tail");
  return RequestedBegin;
}

} // namespace

llvm::Expected<uint64_t> detail::resolveUniqueRawBackedFileOffset(
    llvm::ArrayRef<RawBackedSectionRange> Sections, uint64_t FileSize,
    uint32_t RVA, uint32_t Size) {
  if (RVA == 0 || Size == 0)
    return codeViewRangeError("RVA range is absent or empty");
  if (auto Error = validateRawBackedSections(Sections, FileSize))
    return std::move(Error);

  constexpr uint64_t RVAAddressSpace = uint64_t{1} << 32;
  const uint64_t RequestedBegin = RVA;
  const uint64_t RequestedEnd = RequestedBegin + Size;
  if (RequestedEnd > RVAAddressSpace)
    return codeViewRangeError("RVA range overflows");

  const RawBackedSectionRange *Owner = nullptr;
  for (const RawBackedSectionRange &Section : Sections) {
    if (Section.VirtualSize == 0)
      continue;
    const uint64_t SectionBegin = Section.RVA;
    const uint64_t SectionEnd = SectionBegin + Section.VirtualSize;
    if (RequestedBegin >= SectionEnd || SectionBegin >= RequestedEnd)
      continue;
    if (Owner)
      return codeViewRangeError("RVA range has overlapping section owners");
    Owner = &Section;
  }
  if (!Owner)
    return codeViewRangeError("RVA range has no section owner");

  const uint64_t OwnerBegin = Owner->RVA;
  const uint64_t OwnerEnd = OwnerBegin + Owner->VirtualSize;
  if (RequestedBegin < OwnerBegin || RequestedEnd > OwnerEnd)
    return codeViewRangeError("RVA range crosses its section boundary");
  const uint64_t Delta = RequestedBegin - OwnerBegin;
  if (Owner->RawSize == 0 || Delta + Size > Owner->RawSize)
    return codeViewRangeError("RVA range crosses the section raw tail");

  const uint64_t FileOffset = Owner->FileOffset + Delta;
  if (FileOffset + Size > FileSize)
    return codeViewRangeError("resolved RVA range is outside the file");
  return FileOffset;
}

llvm::Expected<llvm::ArrayRef<uint8_t>>
detail::resolveCodeViewPayload(llvm::ArrayRef<uint8_t> FileData,
                               llvm::ArrayRef<RawBackedSectionRange> Sections,
                               uint32_t RVA, uint32_t RawFileOffset,
                               uint32_t Size) {
  if (Size == 0 || (RVA == 0 && RawFileOffset == 0))
    return codeViewRangeError("payload is absent or empty");

  std::optional<uint64_t> RVAOffset;
  if (RVA != 0) {
    auto Offset =
        resolveUniqueRawBackedFileOffset(Sections, FileData.size(), RVA, Size);
    if (!Offset)
      return Offset.takeError();
    RVAOffset = *Offset;
  }

  std::optional<uint64_t> RawOffset;
  if (RawFileOffset != 0) {
    auto Offset = resolveUniqueRawFileRange(Sections, FileData.size(),
                                            RawFileOffset, Size);
    if (!Offset)
      return Offset.takeError();
    RawOffset = *Offset;
  }

  if (RVAOffset && RawOffset && *RVAOffset != *RawOffset)
    return codeViewRangeError(
        "RVA and PointerToRawData identify different file offsets");
  const uint64_t Offset = RVAOffset ? *RVAOffset : *RawOffset;
  return FileData.slice(static_cast<size_t>(Offset), Size);
}

llvm::Expected<detail::CodeViewRSDSRecord>
detail::parseCodeViewRSDS(llvm::ArrayRef<uint8_t> Bytes) {
  using PDB70 = llvm::codeview::PDB70DebugInfo;
  if (Bytes.size() < sizeof(PDB70) + 1)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView RSDS record is truncated");

  PDB70 Info{};
  std::memcpy(&Info, Bytes.data(), sizeof(Info));
  if (Info.CVSignature != llvm::OMF::Signature::PDB70)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView record is not RSDS/PDB70");

  CodeViewRSDSRecord Record;
  std::copy(std::begin(Info.Signature), std::end(Info.Signature),
            Record.Identity.Guid.begin());
  Record.Identity.Age = Info.Age;
  if (!Record.Identity.isValid())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView RSDS identity is invalid");

  llvm::ArrayRef<uint8_t> PathBytes = Bytes.drop_front(sizeof(PDB70));
  const auto End = std::find(PathBytes.begin(), PathBytes.end(), uint8_t{0});
  if (End == PathBytes.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView RSDS path is unterminated");
  Record.Path.assign(reinterpret_cast<const char *>(PathBytes.data()),
                     static_cast<size_t>(End - PathBytes.begin()));
  return Record;
}

void detail::CodeViewIdentityRegistry::observe(
    const CodeViewRSDSRecord &Record) {
  if (!Record.Identity.isValid()) {
    observeMalformed();
    return;
  }

  if (!Record.Path.empty() && (Path.empty() || Record.Path < Path))
    Path = Record.Path;

  if (State == PDBIdentityState::Ambiguous)
    return;
  if (State == PDBIdentityState::Absent) {
    State = PDBIdentityState::Unique;
    Identity = Record.Identity;
    return;
  }
  if (!Identity || *Identity != Record.Identity) {
    State = PDBIdentityState::Ambiguous;
    Identity.reset();
  }
}

void detail::CodeViewIdentityRegistry::observeMalformed() {
  State = PDBIdentityState::Ambiguous;
  Identity.reset();
}

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

  Img.recordImportStorageSlot(IATAddr, Imp.Name, 0,
                              ImportStorageEvidence::ImportDirectory);
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

    Img.recordImportStorageSlot(IATSlot, Imp.Name, 0,
                                ImportStorageEvidence::ImportDirectory);
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
                      uint64_t ImageBase, llvm::ArrayRef<va_t> SectionVAs) {
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
      const size_t SectionNumber =
          static_cast<size_t>(CoffSym.getSectionNumber());
      if (SectionNumber < SectionVAs.size() &&
          SectionVAs[SectionNumber] != InvalidVA) {
        if (CoffSym.getValue() > InvalidVA - SectionVAs[SectionNumber])
          continue;
        Addr = SectionVAs[SectionNumber] + CoffSym.getValue();
      } else {
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

      const uint32_t PtrSize = Img.getPointerSize();
      const bool IsFullPointer =
          (PtrSize == 8 && Type == llvm::COFF::IMAGE_REL_BASED_DIR64) ||
          (PtrSize == 4 && Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW);
      if (!IsFullPointer)
        continue;
      if (const uint8_t *Bytes = Img.readVA(BR.Address, PtrSize))
        recordAbsolutePointerRelocation(
            Img, BR.Address, static_cast<va_t>(readPtr(Bytes, PtrSize == 8)));
    }
    P += BlockSize;
  }
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Img.BaseRelocations.size()
                          << " base relocations\n");
}

void parseDebugDirectory(const COFFObjectFile &Obj, BinaryImage &Img) {
  Img.DynInfo.PDBPath.clear();
  Img.DynInfo.CodeViewPDBIdentityState = PDBIdentityState::Absent;
  Img.DynInfo.CodeViewPDBIdentity.reset();

  const data_directory *DebugDir =
      Obj.getDataDirectory(llvm::COFF::DEBUG_DIRECTORY);
  if (!DebugDir || DebugDir->RelativeVirtualAddress == 0 || DebugDir->Size == 0)
    return;

  detail::CodeViewIdentityRegistry Registry;
  auto Publish = [&] {
    Img.DynInfo.CodeViewPDBIdentityState = Registry.state();
    Img.DynInfo.CodeViewPDBIdentity = Registry.identity();
    Img.DynInfo.PDBPath = Registry.path();
  };
  if (DebugDir->Size < sizeof(llvm::object::debug_directory) ||
      DebugDir->Size % sizeof(llvm::object::debug_directory) != 0) {
    Registry.observeMalformed();
    Publish();
    return;
  }

  llvm::StringRef FileData = Obj.getData();
  const llvm::ArrayRef<uint8_t> FileBytes(
      reinterpret_cast<const uint8_t *>(FileData.data()), FileData.size());
  std::vector<detail::RawBackedSectionRange> Sections;
  Sections.reserve(Obj.getNumberOfSections());
  for (const SectionRef &SectionRef : Obj.sections()) {
    const coff_section *Section = Obj.getCOFFSection(SectionRef);
    Sections.push_back({Section->VirtualAddress, Section->VirtualSize,
                        Section->PointerToRawData, Section->SizeOfRawData});
  }

  auto DirectoryOffset = detail::resolveUniqueRawBackedFileOffset(
      Sections, FileBytes.size(), DebugDir->RelativeVirtualAddress,
      DebugDir->Size);
  if (!DirectoryOffset) {
    llvm::consumeError(DirectoryOffset.takeError());
    Registry.observeMalformed();
    Publish();
    return;
  }
  const llvm::ArrayRef<uint8_t> DirectoryBytes =
      FileBytes.slice(static_cast<size_t>(*DirectoryOffset), DebugDir->Size);
  const size_t NumEntries =
      DirectoryBytes.size() / sizeof(llvm::object::debug_directory);

  for (size_t I = 0; I < NumEntries; ++I) {
    llvm::object::debug_directory DbgEntry;
    std::memcpy(&DbgEntry, DirectoryBytes.data() + I * sizeof(DbgEntry),
                sizeof(DbgEntry));
    if (DbgEntry.Type != llvm::COFF::IMAGE_DEBUG_TYPE_CODEVIEW)
      continue;

    const uint32_t DataRVA = DbgEntry.AddressOfRawData;
    const uint32_t FileOffset = DbgEntry.PointerToRawData;
    const uint32_t DataSize = DbgEntry.SizeOfData;
    if (DataSize == 0 || (DataRVA == 0 && FileOffset == 0)) {
      Registry.observeMalformed();
      continue;
    }

    auto Payload = detail::resolveCodeViewPayload(FileBytes, Sections, DataRVA,
                                                  FileOffset, DataSize);
    if (!Payload) {
      llvm::consumeError(Payload.takeError());
      Registry.observeMalformed();
      continue;
    }

    auto Record = detail::parseCodeViewRSDS(*Payload);
    if (!Record) {
      llvm::consumeError(Record.takeError());
      Registry.observeMalformed();
      continue;
    }
    Registry.observe(*Record);
  }

  Publish();
  LLVM_DEBUG(llvm::dbgs() << "coff: CodeView PDB identity state = "
                          << static_cast<unsigned>(
                                 Img.DynInfo.CodeViewPDBIdentityState)
                          << ", path = " << Img.DynInfo.PDBPath << "\n");
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
  auto RecordRuntimeCallableSlot = [&](size_t Offset, size_t Width,
                                       uint64_t SlotVA,
                                       RuntimeCallablePointerSlotKind Kind) {
    if (!Has(Offset, Width) || SlotVA < ImageBase ||
        SlotVA > std::numeric_limits<va_t>::max())
      return;
    Img.recordRuntimeCallablePointerSlot(static_cast<va_t>(SlotVA), Kind);
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

  RecordRuntimeCallableSlot(offsetof(LoadCfgT, GuardCFCheckFunction),
                            sizeof(Cfg.GuardCFCheckFunction),
                            Cfg.GuardCFCheckFunction,
                            RuntimeCallablePointerSlotKind::GuardCFCheck);
  RecordRuntimeCallableSlot(offsetof(LoadCfgT, GuardCFCheckDispatch),
                            sizeof(Cfg.GuardCFCheckDispatch),
                            Cfg.GuardCFCheckDispatch,
                            RuntimeCallablePointerSlotKind::GuardCFDispatch);
  RecordRuntimeCallableSlot(
      offsetof(LoadCfgT, GuardRFFailureRoutineFunctionPointer),
      sizeof(Cfg.GuardRFFailureRoutineFunctionPointer),
      Cfg.GuardRFFailureRoutineFunctionPointer,
      RuntimeCallablePointerSlotKind::GuardRFFailureRoutine);
  RecordRuntimeCallableSlot(
      offsetof(LoadCfgT, GuardRFVerifyStackPointerFunctionPointer),
      sizeof(Cfg.GuardRFVerifyStackPointerFunctionPointer),
      Cfg.GuardRFVerifyStackPointerFunctionPointer,
      RuntimeCallablePointerSlotKind::GuardRFVerifyStackPointer);
  RecordRuntimeCallableSlot(offsetof(LoadCfgT, GuardXFGCheckFunctionPointer),
                            sizeof(Cfg.GuardXFGCheckFunctionPointer),
                            Cfg.GuardXFGCheckFunctionPointer,
                            RuntimeCallablePointerSlotKind::GuardXFGCheck);
  RecordRuntimeCallableSlot(offsetof(LoadCfgT, GuardXFGDispatchFunctionPointer),
                            sizeof(Cfg.GuardXFGDispatchFunctionPointer),
                            Cfg.GuardXFGDispatchFunctionPointer,
                            RuntimeCallablePointerSlotKind::GuardXFGDispatch);
  RecordRuntimeCallableSlot(
      offsetof(LoadCfgT, GuardXFGTableDispatchFunctionPointer),
      sizeof(Cfg.GuardXFGTableDispatchFunctionPointer),
      Cfg.GuardXFGTableDispatchFunctionPointer,
      RuntimeCallablePointerSlotKind::GuardXFGTableDispatch);
  RecordRuntimeCallableSlot(
      offsetof(LoadCfgT, CastGuardOsDeterminedFailureMode),
      sizeof(Cfg.CastGuardOsDeterminedFailureMode),
      Cfg.CastGuardOsDeterminedFailureMode,
      RuntimeCallablePointerSlotKind::CastGuardOsDeterminedFailureMode);

  // GuardMemcpyFunctionPointer immediately follows CastGuard in the
  // append-only IMAGE_LOAD_CONFIG_DIRECTORY ABI.  LLVM releases predating the
  // SDK field intentionally end their typed structure at CastGuard, so decode
  // this tail by its versioned byte offset instead of changing LLVM's sizeof
  // contract locally.  The structure's declared Size has already bounded
  // AvailableSize above.
  constexpr size_t GuardMemcpyOffset =
      offsetof(LoadCfgT, CastGuardOsDeterminedFailureMode) +
      sizeof(Cfg.CastGuardOsDeterminedFailureMode);
  constexpr size_t PointerWidth = sizeof(Cfg.CastGuardOsDeterminedFailureMode);
  if (Has(GuardMemcpyOffset, PointerWidth)) {
    const auto *Field =
        reinterpret_cast<const uint8_t *>(CfgPtr) + GuardMemcpyOffset;
    const uint64_t SlotVA = PointerWidth == sizeof(uint64_t)
                                ? readLE<uint64_t>(Field)
                                : readLE<uint32_t>(Field);
    RecordRuntimeCallableSlot(GuardMemcpyOffset, PointerWidth, SlotVA,
                              RuntimeCallablePointerSlotKind::GuardMemcpy);
  }
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
  if (AvailableSize < sizeof(uint32_t))
    return;
  // The structure's own leading `Size` decides which fields exist, not the
  // data directory's size.  Every MSVC link.exe since the field was added has
  // left the directory size at the value contemporary with the linker while
  // emitting the full modern structure, so clamping to it drops the SafeSEH
  // handler table and the whole Control Flow Guard block on images that
  // plainly carry them.  Windows itself reads this field, so trusting it is
  // what makes the decode agree with the loader.
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
