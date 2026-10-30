//===- COFFLoaderUtils.cpp - COFF/PE loader helpers ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFLoaderUtils.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/InstructionRelocations.h"
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
#include <map>
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
  bool CrossesOwner = false;
  for (const detail::RawBackedSectionRange &Section : Sections) {
    if (Section.RawSize == 0)
      continue;
    const uint64_t SectionBegin = Section.FileOffset;
    const uint64_t SectionEnd = SectionBegin + Section.RawSize;
    if (RequestedBegin >= SectionEnd || SectionBegin >= RequestedEnd)
      continue;
    if (RequestedBegin < SectionBegin || RequestedEnd > SectionEnd) {
      CrossesOwner = true;
      continue;
    }
    if (Owner)
      return codeViewRangeError("raw range has overlapping section owners");
    Owner = &Section;
  }
  if (CrossesOwner)
    return codeViewRangeError("raw range crosses its section boundary");
  if (!Owner)
    // VC6 / PDB 2.00 often stores the CodeView NB10 blob in the PE overlay
    // immediately after the last section, with AddressOfRawData left zero.
    // A range that shares no bytes with any section and fits in the file is
    // that overlay, not a missing owner.
    return RequestedBegin;

  const uint64_t OwnerBegin = Owner->FileOffset;
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

llvm::Expected<detail::CodeViewRSDSRecord>
detail::parseCodeViewNB10(llvm::ArrayRef<uint8_t> Bytes) {
  using PDB20 = llvm::codeview::PDB20DebugInfo;
  if (Bytes.size() < sizeof(PDB20) + 1)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView NB10 record is truncated");

  PDB20 Info{};
  std::memcpy(&Info, Bytes.data(), sizeof(Info));
  if (Info.CVSignature != llvm::OMF::Signature::PDB20)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView record is not NB10/PDB20");

  CodeViewRSDSRecord Record;
  Record.Identity.Kind = PDBIdentityKind::NB10;
  Record.Identity.Signature = Info.Signature;
  Record.Identity.Age = Info.Age;
  if (!Record.Identity.isValid())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView NB10 identity is invalid");

  llvm::ArrayRef<uint8_t> PathBytes = Bytes.drop_front(sizeof(PDB20));
  const auto End = std::find(PathBytes.begin(), PathBytes.end(), uint8_t{0});
  if (End == PathBytes.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView NB10 path is unterminated");
  Record.Path.assign(reinterpret_cast<const char *>(PathBytes.data()),
                     static_cast<size_t>(End - PathBytes.begin()));
  return Record;
}

llvm::Expected<detail::CodeViewRSDSRecord>
detail::parseCodeViewIdentity(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < 4)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "CodeView record is truncated");
  uint32_t Signature = 0;
  std::memcpy(&Signature, Bytes.data(), sizeof(Signature));
  if (Signature == llvm::OMF::Signature::PDB70)
    return parseCodeViewRSDS(Bytes);
  if (Signature == llvm::OMF::Signature::PDB20)
    return parseCodeViewNB10(Bytes);
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "CodeView record is not RSDS/NB10");
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
    Imp.IsDelayImport = true;

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
  const auto *Directory = Obj.getDataDirectory(DELAY_IMPORT_DESCRIPTOR);
  if (!Directory || (!Directory->RelativeVirtualAddress && !Directory->Size))
    return;
  if (!Directory->RelativeVirtualAddress ||
      Directory->Size < sizeof(delay_import_directory_table_entry)) {
    Img.addLoadDiagnostic("pe.delay_import_directory_invalid",
                          "PE delay-import directory is incomplete; "
                          "delay imports were ignored.");
    return;
  }
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
    if (!Img.IsRelocatable && isDebugRecord(CoffSym))
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
    S.IsSection = !Img.IsRelocatable && isSectionDefinition(CoffSym);
    Img.Symbols.push_back(std::move(S));
  }
  LLVM_DEBUG(llvm::dbgs() << "coff: parsed " << Img.Symbols.size()
                          << " symbols from symbol table\n");
}

void recordSymbolTableDataInCode(BinaryImage &Img) {
  if (Img.IsRelocatable || !Img.ReadsStaticSymbols)
    return;
  Decoder Dec;
  if (!Dec.init(Img))
    return;
  std::vector<va_t> Objects;
  for (const Symbol &Sym : Img.Symbols) {
    // A local label (`.l_startw`, an import thunk's `.text`) names code.
    if (Sym.IsFunc || Sym.IsSection || Sym.Name.empty() ||
        Sym.Name.front() == '.' || !Img.isCodeAddress(Sym.Addr) ||
        Img.findImportStubAt(Sym.Addr))
      continue;
    // An assembler's code label has no function type either; code begins
    // with an instruction, and a table such as GNU ld's -1-headed
    // __CTOR_LIST__ does not.
    size_t Available = 16;
    const uint8_t *Bytes = nullptr;
    while (Available && !(Bytes = Img.readVA(Sym.Addr, Available)))
      --Available;
    DecodedInsn Insn{};
    if (!Available ||
        (Dec.selectMode(Img, Sym.Addr) &&
         Dec.decodeOneLight(Bytes, Available, Sym.Addr, Insn) > 0))
      continue;
    Objects.push_back(Sym.Addr);
  }
  if (!recordDataInCode(Img, Objects))
    return;
  // Such an object runs from its symbol up to the next one.
  std::vector<va_t> Starts;
  for (const Symbol &Sym : Img.Symbols)
    if (!Sym.IsSection && !Sym.Name.empty())
      Starts.push_back(Sym.Addr);
  llvm::sort(Starts);
  auto InData = [&](va_t Addr) {
    const auto Object = Img.DataInCode.upper_bound(Addr);
    if (Object == Img.DataInCode.begin())
      return false;
    const va_t Start = *std::prev(Object);
    const auto Next = std::upper_bound(Starts.begin(), Starts.end(), Start);
    return Next == Starts.end() || *Next > Addr;
  };
  // A pointer to such an object, or into it, points to data.
  recordDataPointerTargets(Img, InData);
  // A pointer such an object holds, which the base relocations recorded as
  // an instruction's operand for its section's sake, is a pointer slot of
  // data: GNU ld's __CTOR_LIST__ lists the constructors to run.
  const uint32_t Width = Img.getPointerSize();
  std::vector<AbsolutePointerRelocationEffect> Slots;
  for (const auto &[SlotVA, Field] : Img.CodeAddressRelocOperands)
    if (Field.Width == Width && !Field.PCRelativeFromInstructionEnd &&
        Field.Kind == RelocatedAddressFieldKind::Generic && InData(SlotVA))
      Slots.push_back(decideAbsolutePointerRelocation(
          Img, SlotVA, Field.TargetVA, Field.TargetOwnerVA, [&](va_t Addr) {
            PointerRelocationAddressClass Class =
                classifyPointerRelocationAddress(Img, Addr);
            if (Addr == SlotVA)
              Class.Executable = false;
            return Class;
          }));
  for (const AbsolutePointerRelocationEffect &Slot : Slots)
    if (Slot.What != AbsolutePointerRelocationEffect::Kind::None) {
      Img.CodeAddressRelocOperands.erase(Slot.SlotVA);
      applyAbsolutePointerRelocation(Img, Slot);
    }
}

void recordConstantCodeSlots(BinaryImage &Img) {
  if (Img.IsRelocatable || (Img.Arch != Arch::X86 && Img.Arch != Arch::X64))
    return;
  const uint32_t Width = Img.getPointerSize();
  // MinGW's runtime defines `__imp_X` for each function it supplies in place
  // of an import (__MINGW_IMP_SYMBOL), initialized to that function.
  std::map<va_t, va_t> Candidates;
  for (const Symbol &Sym : Img.Symbols) {
    const llvm::StringRef Name = Sym.Name;
    if (Sym.IsFunc || Sym.IsSection ||
        (!Name.starts_with("__imp_") && !Name.starts_with("_imp_")))
      continue;
    const va_t Slot = Sym.Addr;
    const Section *Sec = Img.getSectionFor(Slot);
    if (!Sec || Sec->isExecutable() || !Img.CodePtrRelocSlots.count(Slot) ||
        Img.ImportStorageSlots.count(Slot) || Img.findImportAt(Slot))
      continue;
    const uint8_t *Bytes = Img.readVA(Slot, Width);
    if (!Bytes)
      continue;
    const va_t Target = normalizeCodeAddress(
        static_cast<va_t>(readPtr(Bytes, Width == 8)), Img.Arch, Img.Mode);
    if (Img.hasAuthenticatedFunctionEntryAt(Target))
      Candidates.emplace(Slot, Target);
  }
  if (Candidates.empty())
    return;
  // The candidate slot \p Size bytes at \p Addr overlap, if any.
  auto SlotAt = [&](va_t Addr, uint64_t Size) -> std::optional<va_t> {
    const va_t Last = Addr + (Size ? Size - 1 : 0);
    if (Last < Addr)
      return std::nullopt;
    auto It = Candidates.upper_bound(Last);
    if (It == Candidates.begin())
      return std::nullopt;
    --It;
    if (It->first > InvalidVA - Width || It->first + Width <= Addr)
      return std::nullopt;
    return It->first;
  };
  std::set<va_t> Escaped;
  // A pointer to the slot in data, or an export of it, lets it escape.
  for (const std::set<uint64_t> *Pointers :
       {&Img.CodePtrRelocSlots, &Img.DataPtrRelocSlots})
    for (const uint64_t Pointer : *Pointers)
      if (const uint8_t *Bytes = Img.readVA(Pointer, Width))
        if (auto Slot =
                SlotAt(static_cast<va_t>(readPtr(Bytes, Width == 8)), 1))
          Escaped.insert(*Slot);
  for (const Export &Exp : Img.Exports)
    if (auto Slot = SlotAt(Exp.Addr, 1))
      Escaped.insert(*Slot);
  Decoder Dec;
  if (!Dec.init(Img))
    return;
  // Instruction starts the image's metadata gives resynchronize the sweep.
  std::set<va_t> Starts;
  auto AddStart = [&](va_t Addr) {
    Starts.insert(normalizeCodeAddress(Addr, Img.Arch, Img.Mode));
  };
  if (Img.Entry)
    AddStart(Img.Entry);
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && !Sym.IsSection)
      AddStart(Sym.Addr);
  for (const va_t Addr : Img.RuntimeFunctionAddrs)
    AddStart(Addr);
  for (const auto &[Begin, End] : Img.KnownCodeRanges)
    AddStart(Begin);
  for (const Export &Exp : Img.Exports)
    AddStart(Exp.Addr);
  const uint64_t AddressMask =
      Img.Arch == Arch::X86 ? uint64_t{0xFFFFFFFF} : ~uint64_t{0};
  for (const Section &Sec : Img.Sections) {
    if (!Sec.isExecutable() || Sec.Size > InvalidVA - Sec.VA)
      continue;
    const va_t End = Sec.VA + Sec.Size;
    va_t Cur = Sec.VA;
    while (Cur < End && Escaped.size() < Candidates.size()) {
      size_t Available = static_cast<size_t>(std::min<va_t>(16, End - Cur));
      const uint8_t *Bytes = nullptr;
      while (Available && !(Bytes = Img.readVA(Cur, Available)))
        --Available;
      DecodedInsn Insn{};
      const int Len = Bytes && Dec.selectMode(Img, Cur)
                          ? Dec.decodeOne(Bytes, Available, Cur, Insn)
                          : 0;
      if (Len <= 0 || !Insn.Raw || !Insn.Raw->detail) {
        ++Cur;
        continue;
      }
      // Each memory operand at a fixed address names what it reads or
      // writes there; a load of the address itself, or the address as an
      // immediate, lets the slot escape.
      const cs_x86 &X86 = Insn.Raw->detail->x86;
      for (uint8_t I = 0; I < X86.op_count; ++I) {
        const cs_x86_op &Operand = X86.operands[I];
        if (Operand.type == X86_OP_IMM) {
          if (auto Slot =
                  SlotAt(static_cast<va_t>(static_cast<uint64_t>(Operand.imm) &
                                           AddressMask),
                         1))
            Escaped.insert(*Slot);
          continue;
        }
        if (Operand.type != X86_OP_MEM || Operand.mem.index != X86_REG_INVALID)
          continue;
        std::optional<va_t> Address;
        if (Operand.mem.base == X86_REG_RIP)
          Address =
              Insn.Addr + Insn.Size + static_cast<uint64_t>(Operand.mem.disp);
        else if (Operand.mem.base == X86_REG_INVALID)
          Address = static_cast<uint64_t>(Operand.mem.disp) & AddressMask;
        if (!Address)
          continue;
        if (auto Slot = SlotAt(*Address, Operand.size ? Operand.size : 1))
          if (Insn.Id == X86_INS_LEA || !(Operand.access & CS_AC_READ) ||
              (Operand.access & CS_AC_WRITE))
            Escaped.insert(*Slot);
      }
      // An instruction that runs over a known instruction start has lost the
      // boundaries: take them up again at that start.
      va_t After = Cur + static_cast<va_t>(Len);
      if (auto Inside = Starts.upper_bound(Cur);
          Inside != Starts.end() && *Inside < After)
        After = *Inside;
      Cur = After;
    }
  }
  for (const auto &[Slot, Target] : Candidates)
    if (!Escaped.count(Slot))
      Img.ConstantCodeSlots.emplace(Slot, Target);
}

void recordIndexedJumpTableRuns(BinaryImage &Img) {
  if (Img.IsRelocatable || Img.Format != BinaryFormat::COFF ||
      (Img.Arch != Arch::X86 && Img.Arch != Arch::X64))
    return;
  const uint32_t Width = Img.getPointerSize();
  if (Width == 0)
    return;
  Decoder Dec;
  if (!Dec.init(Img))
    return;
  std::set<va_t> JumpReadBases;
  const uint64_t AddressMask =
      Img.Arch == Arch::X86 ? uint64_t{0xFFFFFFFF} : ~uint64_t{0};
  for (const Section &Sec : Img.Sections) {
    if (!Sec.isExecutable() || Sec.Size > InvalidVA - Sec.VA)
      continue;
    const va_t End = Sec.VA + Sec.Size;
    va_t Cur = Sec.VA;
    while (Cur < End) {
      size_t Available = static_cast<size_t>(std::min<va_t>(16, End - Cur));
      const uint8_t *Bytes = nullptr;
      while (Available && !(Bytes = Img.readVA(Cur, Available)))
        --Available;
      DecodedInsn Insn{};
      const int Len = Bytes && Dec.selectMode(Img, Cur)
                          ? Dec.decodeOne(Bytes, Available, Cur, Insn)
                          : 0;
      if (Len <= 0 || !Insn.Raw || !Insn.Raw->detail) {
        ++Cur;
        continue;
      }
      if (Insn.Id == X86_INS_JMP) {
        const cs_x86 &X86 = Insn.Raw->detail->x86;
        for (uint8_t I = 0; I < X86.op_count; ++I) {
          const cs_x86_op &Operand = X86.operands[I];
          if (Operand.type != X86_OP_MEM ||
              Operand.mem.index == X86_REG_INVALID)
            continue;
          std::optional<va_t> Base;
          if (Operand.mem.base == X86_REG_RIP)
            Base = Insn.Addr + Insn.Size +
                   static_cast<uint64_t>(Operand.mem.disp);
          else if (Operand.mem.base == X86_REG_INVALID)
            Base = static_cast<uint64_t>(Operand.mem.disp) & AddressMask;
          if (!Base || (*Base & (Width - 1)) != 0)
            continue;
          const va_t BaseVA = static_cast<va_t>(*Base & AddressMask);
          va_t RunEnd = BaseVA;
          while (Img.CodePtrRelocSlots.count(RunEnd) ||
                 Img.DataPtrRelocSlots.count(RunEnd))
            RunEnd += Width;
          if (RunEnd > BaseVA)
            Img.IndexedTableRuns.emplace_back(BaseVA, RunEnd);
        }
      }
      Cur += static_cast<va_t>(Len);
    }
  }
}

void recordIndexedTableBasesInCode(BinaryImage &Img) {
  if (Img.IsRelocatable || (Img.Arch != Arch::X86 && Img.Arch != Arch::X64))
    return;
  const uint32_t Width = Img.getPointerSize();
  if (Width == 0)
    return;
  // Full-width relocated fields the current classification treats as
  // instruction operands because they lie in an executable section.
  std::map<va_t, RelocatedAddressField> Candidates;
  auto Collect = [&](const std::map<va_t, RelocatedAddressField> &Operands) {
    for (const auto &[SlotVA, Field] : Operands) {
      const Segment *Seg = Img.getSegmentFor(SlotVA);
      if (!Seg || !Seg->isExecutable())
        continue;
      if (Field.Width == Width && !Field.PCRelativeFromInstructionEnd &&
          Field.Kind == RelocatedAddressFieldKind::Generic)
        Candidates.emplace(SlotVA, Field);
    }
  };
  Collect(Img.CodeAddressRelocOperands);
  Collect(Img.DataAddressRelocOperands);
  if (Candidates.empty())
    return;

  std::set<va_t> Starts;
  auto AddStart = [&](va_t Addr) {
    Starts.insert(normalizeCodeAddress(Addr, Img.Arch, Img.Mode));
  };
  if (Img.Entry)
    AddStart(Img.Entry);
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && !Sym.IsSection)
      AddStart(Sym.Addr);
  for (const va_t Addr : Img.RuntimeFunctionAddrs)
    AddStart(Addr);
  for (const auto &[Begin, End] : Img.KnownCodeRanges)
    AddStart(Begin);
  for (const Export &Exp : Img.Exports)
    AddStart(Exp.Addr);

  Decoder Dec;
  if (!Dec.init(Img))
    return;
  // A memory operand at a fixed base with a runtime index — `[base+idx*s]` —
  // is a table access. No immediate inside an instruction is ever read that
  // way, so a base in an executable section names data the linker placed
  // among the code (MinGW's CRT keeps its constructor list in .text): the
  // relocation-covered words of that table are pointer slots, not operands.
  std::set<va_t> TableBases;
  std::set<va_t> JumpReadBases;
  const uint64_t AddressMask =
      Img.Arch == Arch::X86 ? uint64_t{0xFFFFFFFF} : ~uint64_t{0};
  for (const Section &Sec : Img.Sections) {
    if (!Sec.isExecutable() || Sec.Size > InvalidVA - Sec.VA)
      continue;
    const va_t End = Sec.VA + Sec.Size;
    va_t Cur = Sec.VA;
    while (Cur < End) {
      size_t Available = static_cast<size_t>(std::min<va_t>(16, End - Cur));
      const uint8_t *Bytes = nullptr;
      while (Available && !(Bytes = Img.readVA(Cur, Available)))
        --Available;
      DecodedInsn Insn{};
      const int Len = Bytes && Dec.selectMode(Img, Cur)
                          ? Dec.decodeOne(Bytes, Available, Cur, Insn)
                          : 0;
      if (Len <= 0 || !Insn.Raw || !Insn.Raw->detail) {
        ++Cur;
        continue;
      }
      const cs_x86 &X86 = Insn.Raw->detail->x86;
      for (uint8_t I = 0; I < X86.op_count; ++I) {
        const cs_x86_op &Operand = X86.operands[I];
        if (Operand.type != X86_OP_MEM ||
            Operand.mem.index == X86_REG_INVALID)
          continue;
        std::optional<va_t> Base;
        if (Operand.mem.base == X86_REG_RIP)
          Base = Insn.Addr + Insn.Size +
                 static_cast<uint64_t>(Operand.mem.disp);
        else if (Operand.mem.base == X86_REG_INVALID)
          Base = static_cast<uint64_t>(Operand.mem.disp) & AddressMask;
        if (!Base || (*Base & (Width - 1)) != 0)
          continue;
        const Segment *Seg = Img.getSegmentFor(*Base);
        if (Seg && Seg->isReadable()) {
          const va_t BaseVA = static_cast<va_t>(*Base & AddressMask);
          TableBases.insert(BaseVA);
          // A table a CALL reads through holds callable entries, not another
          // function's case labels: its slots promote as callbacks.
          if (Insn.Id == X86_INS_CALL)
            Img.CallDispatchTableBases.insert(BaseVA);
          // A table an indirect JUMP reads holds that function's case
          // labels: the discovery scan must not turn them into functions.
          if (Insn.Id == X86_INS_JMP)
            JumpReadBases.insert(BaseVA);
        }
      }
      va_t After = Cur + static_cast<va_t>(Len);
      if (auto Inside = Starts.upper_bound(Cur);
          Inside != Starts.end() && *Inside < After)
        After = *Inside;
      Cur = After;
    }
  }
  if (TableBases.empty())
    return;

  // A relocated field within a bounded window after a table base belongs to
  // the table. Re-decide it as the slot it would have been had the section
  // kept data only.
  std::vector<AbsolutePointerRelocationEffect> Slots;
  std::vector<va_t> DataStarts;
  for (const auto &[SlotVA, Field] : Candidates) {
    auto It = TableBases.upper_bound(SlotVA);
    if (It == TableBases.begin())
      continue;
    const va_t Base = *std::prev(It);
    if (SlotVA - Base > 8 * Width)
      continue;
    Slots.push_back(decideAbsolutePointerRelocation(
        Img, SlotVA, Field.TargetVA, Field.TargetOwnerVA, [&](va_t Addr) {
          PointerRelocationAddressClass Class =
              classifyPointerRelocationAddress(Img, Addr);
          if (Addr == SlotVA)
            Class.Executable = false;
          return Class;
        }));
  }
  for (const AbsolutePointerRelocationEffect &Slot : Slots)
    if (Slot.What != AbsolutePointerRelocationEffect::Kind::None) {
      Img.CodeAddressRelocOperands.erase(Slot.SlotVA);
      Img.DataAddressRelocOperands.erase(Slot.SlotVA);
      applyAbsolutePointerRelocation(Img, Slot);
    }
  // Name the tables as data objects so later passes treat them as objects,
  // not instruction bytes.  A table in a non-executable section (lld-link's
  // .rdata jump tables) is dropped by the code-address filter inside
  // recordDataInCode, so record it directly: the reader discovery in the
  // function detector needs the run start.
  for (const va_t Base : TableBases) {
    if (Img.isCodeAddress(Base))
      DataStarts.push_back(Base);
    else
      Img.DataInCode.insert(Base);
    // Jump-read runs were recorded before discovery; nothing more here.
  }
  recordDataInCode(Img, DataStarts);
}

void recordRelocatedPointerRunsInCode(BinaryImage &Img) {
  if (Img.IsRelocatable)
    return;
  const uint32_t Width = Img.getPointerSize();
  const std::optional<unsigned> PerInstruction =
      maxAdjacentRelocatedFields(Img.Arch, Width);
  if (!PerInstruction)
    return;
  // The absolute pointer-wide fields among the code, whatever they name.
  std::vector<std::pair<va_t, RelocatedAddressField>> Fields;
  auto Collect = [&](const std::map<va_t, RelocatedAddressField> &Operands) {
    for (const auto &[SlotVA, Field] : Operands)
      if (Field.Width == Width && !Field.PCRelativeFromInstructionEnd &&
          Field.Kind == RelocatedAddressFieldKind::Generic)
        Fields.emplace_back(SlotVA, Field);
  };
  Collect(Img.CodeAddressRelocOperands);
  Collect(Img.DataAddressRelocOperands);
  llvm::sort(Fields, llvm::less_first());
  std::vector<AbsolutePointerRelocationEffect> Slots;
  std::vector<std::pair<va_t, va_t>> Runs;
  for (size_t Begin = 0; Begin < Fields.size();) {
    size_t End = Begin + 1;
    while (End < Fields.size() &&
           Fields[End].first - Fields[End - 1].first == Width)
      ++End;
    if (End - Begin > *PerInstruction) {
      Runs.emplace_back(Fields[Begin].first, Fields[End - 1].first + Width);
      for (size_t I = Begin; I < End; ++I) {
        const auto &[SlotVA, Field] = Fields[I];
        // The relocation as it would have been recorded had the slot's
        // section kept data only.
        Slots.push_back(decideAbsolutePointerRelocation(
            Img, SlotVA, Field.TargetVA, Field.TargetOwnerVA, [&](va_t Addr) {
              PointerRelocationAddressClass Class =
                  classifyPointerRelocationAddress(Img, Addr);
              if (Addr == SlotVA)
                Class.Executable = false;
              return Class;
            }));
      }
    }
    Begin = End;
  }
  if (Runs.empty())
    return;
  auto InRun = [&](va_t Addr) {
    const auto Run = std::upper_bound(
        Runs.begin(), Runs.end(), Addr,
        [](va_t A, const std::pair<va_t, va_t> &R) { return A < R.first; });
    return Run != Runs.begin() && Addr < std::prev(Run)->second;
  };
  // An operand naming such a run names data, as one naming a table in .rdata
  // does.
  for (const auto &[SlotVA, Field] : Img.CodeAddressRelocOperands)
    if (!InRun(SlotVA) && InRun(Field.TargetVA) &&
        !Field.PCRelativeFromInstructionEnd &&
        Field.Kind == RelocatedAddressFieldKind::Generic) {
      // Decided as it would have been had the run's section kept data only;
      // the target owns itself, as recordDataPointerTargets has it.
      const va_t Target = Field.TargetVA;
      Slots.push_back(decideAbsolutePointerRelocation(
          Img, SlotVA, Target, InvalidVA, [&](va_t Addr) {
            PointerRelocationAddressClass Class =
                classifyPointerRelocationAddress(Img, Addr);
            if (Addr == Target)
              Class.Executable = false;
            return Class;
          }));
    }
  for (const AbsolutePointerRelocationEffect &Slot : Slots)
    if (Slot.What != AbsolutePointerRelocationEffect::Kind::None) {
      Img.CodeAddressRelocOperands.erase(Slot.SlotVA);
      Img.DataAddressRelocOperands.erase(Slot.SlotVA);
      applyAbsolutePointerRelocation(Img, Slot);
    }
  std::vector<va_t> Starts;
  for (const auto &[Begin, End] : Runs)
    Starts.push_back(Begin);
  recordDataInCode(Img, Starts);
  // A run in a non-executable section (lld-link's .rdata jump tables) is as
  // much a data object as one among the code; the code-address filter above
  // drops it, so record it directly for the reader discovery that follows.
  for (const auto &[Begin, End] : Runs)
    if (!Img.isCodeAddress(Begin))
      Img.DataInCode.insert(Begin);
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

void parseDebugDirectory(const COFFObjectFile &Obj, BinaryImage &Img,
                         llvm::ArrayRef<uint8_t> OriginalBytes) {
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

  // File-only records can name header bytes. Read their original occurrence,
  // including on restricted loads that do not retain BinaryImage::Raw.
  const auto FileBytes = OriginalBytes.empty()
                             ? llvm::arrayRefFromStringRef(Obj.getData())
                             : OriginalBytes;
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

    auto Record = detail::parseCodeViewIdentity(*Payload);
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
llvm::ArrayRef<LoadConfigCallableField> callableFields() {
  static constexpr LoadConfigCallableField Fields[] = {
      {offsetof(LoadCfgT, GuardCFCheckFunction),
       RuntimeCallablePointerSlotKind::GuardCFCheck},
      {offsetof(LoadCfgT, GuardCFCheckDispatch),
       RuntimeCallablePointerSlotKind::GuardCFDispatch},
      {offsetof(LoadCfgT, GuardRFFailureRoutineFunctionPointer),
       RuntimeCallablePointerSlotKind::GuardRFFailureRoutine},
      {offsetof(LoadCfgT, GuardRFVerifyStackPointerFunctionPointer),
       RuntimeCallablePointerSlotKind::GuardRFVerifyStackPointer},
      {offsetof(LoadCfgT, GuardXFGCheckFunctionPointer),
       RuntimeCallablePointerSlotKind::GuardXFGCheck},
      {offsetof(LoadCfgT, GuardXFGDispatchFunctionPointer),
       RuntimeCallablePointerSlotKind::GuardXFGDispatch},
      {offsetof(LoadCfgT, GuardXFGTableDispatchFunctionPointer),
       RuntimeCallablePointerSlotKind::GuardXFGTableDispatch},
      {offsetof(LoadCfgT, CastGuardOsDeterminedFailureMode),
       RuntimeCallablePointerSlotKind::CastGuardOsDeterminedFailureMode},
      // The append-only ABI places GuardMemcpy after CastGuard. LLVM's
      // older typed structures stop at CastGuard; don't change their sizeof.
      {offsetof(LoadCfgT, CastGuardOsDeterminedFailureMode) +
           sizeof(LoadCfgT::CastGuardOsDeterminedFailureMode),
       RuntimeCallablePointerSlotKind::GuardMemcpy}};
  return Fields;
}
} // namespace

llvm::ArrayRef<LoadConfigCallableField> loadConfigCallableFields(Bitness Bits) {
  if (Bits == Bitness::Bits64)
    return callableFields<coff_load_configuration64>();
  if (Bits == Bitness::Bits32)
    return callableFields<coff_load_configuration32>();
  return {};
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
    if (!Img.recordRuntimeCallablePointerSlot(static_cast<va_t>(SlotVA), Kind))
      return;
    // Until the loader writes the slot, it names a function of this image
    // the runtime calls instead, such as _guard_check_icall_nop.
    if (const uint8_t *P =
            Img.readVA(static_cast<va_t>(SlotVA), Img.getPointerSize()))
      if (const uint64_t Default = readPtr(P, Img.is64Bit()))
        Img.recordRuntimeFunction(static_cast<va_t>(Default));
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

  const size_t PointerWidth = sizeof(Cfg.CastGuardOsDeterminedFailureMode);
  for (const auto &Field : loadConfigCallableFields(Img.Bits)) {
    if (!Has(Field.Offset, PointerWidth))
      continue;
    const auto *Bytes =
        reinterpret_cast<const uint8_t *>(CfgPtr) + Field.Offset;
    RecordRuntimeCallableSlot(Field.Offset, PointerWidth,
                              readPtr(Bytes, PointerWidth == sizeof(uint64_t)),
                              Field.Kind);
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
