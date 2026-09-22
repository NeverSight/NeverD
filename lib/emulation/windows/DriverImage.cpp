//===- DriverImage.cpp - Strict Windows driver execution image ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validates executable PE metadata and prepares a consistent relocated driver
/// image, with loader-owned security-cookie initialization before execution.
///
//===----------------------------------------------------------------------===//

#include "DriverImage.h"

#include "../GuestMemory.h"

#include "neverd/emulation/DriverProfile.h"
#include "neverd/loader/Loader.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace neverd::emulation {
namespace {
using namespace llvm::object;
constexpr uint64_t PageSize = profile::PageSize;
constexpr unsigned MaxImports = profile::MaxImports;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "driver image: " + Message);
}

bool powerOfTwo(uint64_t Value) { return Value && (Value & (Value - 1)) == 0; }

uint64_t pages(uint64_t Size) {
  return (Size + PageSize - 1) & ~(PageSize - 1);
}

struct SectionPlan {
  const coff_section *Header;
  llvm::ArrayRef<uint8_t> Contents;
  uint64_t Span;
};

/// LLVM's PE RVA accessor checks VirtualSize but does not establish that the
/// requested bytes exist on disk. Execution metadata must also be file backed.
llvm::Expected<llvm::ArrayRef<uint8_t>>
fileBytes(const COFFObjectFile &Object,
          const std::vector<SectionPlan> &Sections, uint64_t RVA,
          uint64_t Size) {
  if (RVA > UINT32_MAX || Size > UINT32_MAX || Size > UINT32_MAX - RVA)
    return invalid("metadata RVA range overflows");
  for (const auto &Section : Sections) {
    const uint64_t Start = Section.Header->VirtualAddress;
    const uint64_t VirtualSize = Section.Header->VirtualSize;
    if (RVA < Start || RVA - Start >= VirtualSize)
      continue;
    const uint64_t Offset = RVA - Start;
    if (Size > VirtualSize - Offset || Offset > Section.Contents.size() ||
        Size > Section.Contents.size() - Offset)
      return invalid("metadata extends beyond file-backed section bytes");
    llvm::ArrayRef<uint8_t> Bytes;
    if (auto Error = Object.getRvaAndSizeAsBytes(
            static_cast<uint32_t>(RVA), static_cast<uint32_t>(Size), Bytes))
      return std::move(Error);
    return Bytes;
  }
  return invalid("metadata RVA is not in a mapped section");
}

llvm::Expected<std::string> nameAt(const COFFObjectFile &Object,
                                   const std::vector<SectionPlan> &Sections,
                                   uint64_t RVA) {
  std::string Name;
  for (unsigned I = 0; I != 512; ++I) {
    auto Byte = fileBytes(Object, Sections, RVA + I, 1);
    if (!Byte)
      return Byte.takeError();
    const uint8_t Ch = (*Byte)[0];
    if (Ch == 0) {
      if (Name.empty())
        return invalid("empty import name");
      return Name;
    }
    if (Ch < 0x21 || Ch > 0x7e)
      return invalid("unsupported import name encoding");
    Name += static_cast<char>(Ch);
  }
  return invalid("unterminated or oversized import name");
}

llvm::Expected<std::vector<DriverImport>>
validateImports(const COFFObjectFile &Object,
                const std::vector<SectionPlan> &Sections, uint64_t Base) {
  std::vector<DriverImport> Imports;
  const auto *Directory = Object.getDataDirectory(llvm::COFF::IMPORT_TABLE);
  if (!Directory || !Directory->Size)
    return Imports;
  auto Bytes = fileBytes(Object, Sections, Directory->RelativeVirtualAddress,
                         Directory->Size);
  if (!Bytes)
    return Bytes.takeError();
  bool Terminated = false;
  std::set<uint64_t> Slots;
  for (size_t Offset = 0;
       Offset + sizeof(coff_import_directory_table_entry) <= Bytes->size();
       Offset += sizeof(coff_import_directory_table_entry)) {
    coff_import_directory_table_entry Entry;
    std::memcpy(&Entry, Bytes->data() + Offset, sizeof(Entry));
    if (Entry.isNull()) {
      Terminated = true;
      break;
    }
    if (Entry.TimeDateStamp || Entry.ForwarderChain)
      return invalid("bound or forwarded imports are unsupported");
    if (!Entry.ImportAddressTableRVA)
      return invalid("import descriptor has no IAT");
    auto Module = nameAt(Object, Sections, Entry.NameRVA);
    if (!Module)
      return Module.takeError();
    const std::string Provider = llvm::StringRef(*Module).lower();
    if (Provider != "ntoskrnl.exe" && Provider != "ntkrnlmp.exe")
      return invalid("unsupported import provider: " + *Module);
    const uint64_t Lookup = Entry.ImportLookupTableRVA
                                ? uint32_t(Entry.ImportLookupTableRVA)
                                : uint32_t(Entry.ImportAddressTableRVA);
    if ((Lookup & 7) || (Entry.ImportAddressTableRVA & 7))
      return invalid("unaligned x64 import table");
    bool SymbolsTerminated = false;
    for (unsigned Index = 0; Index <= MaxImports; ++Index) {
      auto Symbol =
          fileBytes(Object, Sections, Lookup + uint64_t(Index) * 8, 8);
      if (!Symbol)
        return Symbol.takeError();
      auto IAT = fileBytes(
          Object, Sections,
          uint64_t(Entry.ImportAddressTableRVA) + uint64_t(Index) * 8, 8);
      if (!IAT)
        return IAT.takeError();
      const uint64_t Value = llvm::support::endian::read64le(Symbol->data());
      if (Value != llvm::support::endian::read64le(IAT->data()))
        return invalid("prebound IAT is unsupported");
      if (!Value) {
        SymbolsTerminated = true;
        break;
      }
      if (Value > UINT32_MAX)
        return invalid("ordinal or noncanonical import lookup is unsupported");
      if (Imports.size() >= MaxImports)
        return invalid("import count exceeds the execution profile");
      auto Hint = fileBytes(Object, Sections, Value, 2);
      if (!Hint)
        return Hint.takeError();
      auto Name = nameAt(Object, Sections, Value + 2);
      if (!Name)
        return Name.takeError();
      const uint64_t Slot =
          Base + Entry.ImportAddressTableRVA + uint64_t(Index) * 8;
      if (!Slots.insert(Slot).second)
        return invalid("overlapping import address slots");
      Imports.push_back({Slot, *Module, *Name});
    }
    if (!SymbolsTerminated)
      return invalid("unterminated import lookup table");
  }
  if (!Terminated)
    return invalid("unterminated import directory");
  return Imports;
}

llvm::Expected<std::set<uint64_t>>
validateBaseRelocations(llvm::ArrayRef<uint8_t> Bytes,
                        const std::vector<SectionPlan> &Sections) {
  std::set<uint64_t> Targets;
  size_t Offset = 0;
  while (Offset < Bytes.size()) {
    if (Bytes.size() - Offset < sizeof(coff_base_reloc_block_header))
      return invalid("truncated base relocation block");
    coff_base_reloc_block_header Block;
    std::memcpy(&Block, Bytes.data() + Offset, sizeof(Block));
    const uint64_t BlockSize = Block.BlockSize;
    if (BlockSize < sizeof(Block) || BlockSize % 4 ||
        BlockSize > Bytes.size() - Offset || Block.PageRVA % PageSize)
      return invalid("invalid base relocation block extent");
    for (size_t EntryOffset = sizeof(Block); EntryOffset < BlockSize;
         EntryOffset += 2) {
      const uint16_t Entry =
          llvm::support::endian::read16le(Bytes.data() + Offset + EntryOffset);
      const unsigned Type = Entry >> 12;
      if (Type == llvm::COFF::IMAGE_REL_BASED_ABSOLUTE)
        continue;
      if (Type != llvm::COFF::IMAGE_REL_BASED_DIR64)
        return invalid("unsupported x64 base relocation type");
      const uint64_t RVA = uint64_t(Block.PageRVA) + (Entry & 0xfff);
      bool Mapped = false;
      for (const auto &Section : Sections) {
        const uint64_t Start = Section.Header->VirtualAddress;
        const uint64_t Size = Section.Header->VirtualSize;
        if (RVA >= Start && RVA - Start <= Size && Size - (RVA - Start) >= 8)
          Mapped = true;
      }
      if (!Mapped)
        return invalid(
            "base relocation target is outside mapped section bytes");
      auto Next = Targets.lower_bound(RVA);
      if ((Next != Targets.end() && *Next < RVA + 8) ||
          (Next != Targets.begin() && *std::prev(Next) + 8 > RVA))
        return invalid("duplicate or overlapping DIR64 relocation targets");
      Targets.insert(RVA);
    }
    Offset += BlockSize;
  }
  return Targets;
}

struct CookiePlan {
  uint64_t RVA = 0;
  uint64_t PointerRVA = 0;
};

/// Size/timestamp/version are descriptive. SecurityCookie is the only
/// supported environment requirement. Every other present field must be zero.
/// Microsoft documents these fields at:
/// https://learn.microsoft.com/windows/win32/debug/pe-format#the-load-configuration-structure-image-only
llvm::Error validateLoadConfigurationBytes(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < 4)
    return invalid("truncated load configuration size");
  const uint64_t DeclaredSize = llvm::support::endian::read32le(Bytes.data());
  if (DeclaredSize < 12 || DeclaredSize > Bytes.size())
    return invalid("invalid load configuration declared size");
  constexpr size_t CookieStart =
      offsetof(coff_load_configuration64, SecurityCookie);
  constexpr size_t CookieEnd = CookieStart + 8;
  if (DeclaredSize > CookieStart && DeclaredSize < CookieEnd)
    return invalid("partial load configuration SecurityCookie field");
  // Named byte ranges also reject partial nonzero unsupported fields. Future
  // layouts can extend the table only when their runtime semantics are owned.
  struct Field {
    size_t Offset;
    size_t Size;
    const char *Name;
  };
#define LOAD_FIELD(Name)                                                       \
  {                                                                            \
    offsetof(coff_load_configuration64, Name),                                 \
        sizeof(coff_load_configuration64::Name), #Name                         \
  }
  static constexpr Field Unsupported[] = {
#include "DriverLoadConfiguration.def"
  };
#undef LOAD_FIELD
  for (const auto &Field : Unsupported) {
    const size_t End =
        std::min<size_t>(Field.Offset + Field.Size, DeclaredSize);
    for (size_t Offset = Field.Offset; Offset < End; ++Offset)
      if (Bytes[Offset])
        return invalid("unsupported load configuration field: " +
                       llvm::Twine(Field.Name));
  }
  for (size_t Offset = sizeof(coff_load_configuration64); Offset < DeclaredSize;
       ++Offset)
    if (Bytes[Offset])
      return invalid(
          "unsupported nonzero load configuration extension at byte " +
          llvm::Twine(Offset));
  for (size_t Offset = DeclaredSize; Offset < Bytes.size(); ++Offset)
    if (Bytes[Offset])
      return invalid("nonzero bytes beyond declared load configuration size");
  return llvm::Error::success();
}

/// LLVM initializes CHPE and dynamic relocations inside COFFObjectFile::create.
/// Bound the public PE records before that call so unsupported load-config
/// pointers can never be followed merely while constructing the parser.
/// This is an execution-profile preflight, not a replacement format loader.
llvm::Error preflightLoadConfiguration(llvm::ArrayRef<uint8_t> Raw) {
  auto Copy = [&](uint64_t Offset, auto &Value) {
    if (Offset > Raw.size() || sizeof(Value) > Raw.size() - Offset)
      return false;
    std::memcpy(&Value, Raw.data() + Offset, sizeof(Value));
    return true;
  };
  dos_header DOS;
  if (!Copy(0, DOS) || DOS.Magic[0] != 'M' || DOS.Magic[1] != 'Z')
    return invalid("requires a complete DOS/PE image header");
  const uint64_t PEOffset = DOS.AddressOfNewExeHeader;
  if (PEOffset > Raw.size() || Raw.size() - PEOffset < 4 ||
      llvm::support::endian::read32le(Raw.data() + PEOffset) != 0x00004550)
    return invalid("invalid PE signature offset");
  coff_file_header COFF;
  if (!Copy(PEOffset + 4, COFF))
    return invalid("truncated COFF file header");
  const uint64_t OptionalOffset = PEOffset + 4 + sizeof(COFF);
  pe32plus_header Header;
  if (!Copy(OptionalOffset, Header) ||
      COFF.SizeOfOptionalHeader < sizeof(Header) ||
      Header.Magic != llvm::COFF::PE32Header::PE32_PLUS)
    return invalid("requires a complete PE32+ optional header");
  if (Header.NumberOfRvaAndSize > 16 ||
      sizeof(Header) +
              uint64_t(Header.NumberOfRvaAndSize) * sizeof(data_directory) >
          COFF.SizeOfOptionalHeader)
    return invalid("truncated optional-header data directories");
  const uint64_t SectionsOffset = OptionalOffset + COFF.SizeOfOptionalHeader;
  if (!COFF.NumberOfSections || COFF.NumberOfSections > 96 ||
      SectionsOffset > Raw.size() ||
      uint64_t(COFF.NumberOfSections) * sizeof(coff_section) >
          Raw.size() - SectionsOffset)
    return invalid("invalid or truncated PE section table");
  if (Header.NumberOfRvaAndSize <= llvm::COFF::LOAD_CONFIG_TABLE)
    return llvm::Error::success();
  data_directory Directory;
  if (!Copy(OptionalOffset + sizeof(Header) +
                llvm::COFF::LOAD_CONFIG_TABLE * sizeof(Directory),
            Directory))
    return invalid("truncated load configuration directory entry");
  if (!Directory.RelativeVirtualAddress && !Directory.Size)
    return llvm::Error::success();
  if (!Directory.RelativeVirtualAddress || !Directory.Size)
    return invalid("inconsistent load configuration RVA and size");
  for (unsigned Index = 0; Index < COFF.NumberOfSections; ++Index) {
    coff_section Section;
    if (!Copy(SectionsOffset + uint64_t(Index) * sizeof(Section), Section))
      return invalid("truncated PE section table");
    const uint64_t RVA = Directory.RelativeVirtualAddress;
    if (RVA < Section.VirtualAddress)
      continue;
    const uint64_t Offset = RVA - Section.VirtualAddress;
    if (Offset >= Section.VirtualSize)
      continue;
    if (4 > uint64_t(Section.VirtualSize) - Offset ||
        Offset > Section.SizeOfRawData ||
        4 > uint64_t(Section.SizeOfRawData) - Offset)
      return invalid("load configuration is not fully file backed");
    const uint64_t FileOffset = uint64_t(Section.PointerToRawData) + Offset;
    if (FileOffset > Raw.size() || 4 > Raw.size() - FileOffset)
      return invalid("truncated load configuration bytes");
    const uint64_t Span = std::max<uint64_t>(
        Directory.Size,
        llvm::support::endian::read32le(Raw.data() + FileOffset));
    if (Span > Raw.size() - FileOffset ||
        Span > uint64_t(Section.SizeOfRawData) - Offset ||
        Span > uint64_t(Section.VirtualSize) - Offset)
      return invalid("truncated declared load configuration bytes");
    return validateLoadConfigurationBytes(Raw.slice(FileOffset, Span));
  }
  return invalid("load configuration RVA is not mapped");
}

llvm::Expected<CookiePlan>
validateCookie(const COFFObjectFile &Object,
               const std::vector<SectionPlan> &Sections,
               uint64_t PreferredBase) {
  const auto *Directory =
      Object.getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  if (!Directory || !Directory->Size)
    return CookiePlan{};
  auto Prefix =
      fileBytes(Object, Sections, Directory->RelativeVirtualAddress, 4);
  if (!Prefix)
    return Prefix.takeError();
  const uint64_t Span = std::max<uint64_t>(
      Directory->Size, llvm::support::endian::read32le(Prefix->data()));
  auto Bytes =
      fileBytes(Object, Sections, Directory->RelativeVirtualAddress, Span);
  if (!Bytes)
    return Bytes.takeError();
  if (auto Error = validateLoadConfigurationBytes(*Bytes))
    return std::move(Error);
  constexpr size_t CookieOffset =
      offsetof(coff_load_configuration64, SecurityCookie);
  if (llvm::support::endian::read32le(Bytes->data()) < CookieOffset + 8)
    return CookiePlan{};
  const uint64_t Address =
      llvm::support::endian::read64le(Bytes->data() + CookieOffset);
  if (!Address)
    return CookiePlan{};
  if (Address < PreferredBase || (Address & 7))
    return invalid("invalid load configuration SecurityCookie address");
  const uint64_t RVA = Address - PreferredBase;
  for (const auto &Section : Sections) {
    const uint64_t Start = Section.Header->VirtualAddress;
    const uint64_t Size = Section.Header->VirtualSize;
    if (RVA < Start || RVA - Start >= Size || Size - (RVA - Start) < 8)
      continue;
    if (!(Section.Header->Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE) ||
        (Section.Header->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
      return invalid(
          "SecurityCookie requires writable nonexecutable image storage");
    return CookiePlan{RVA, uint64_t(Directory->RelativeVirtualAddress) +
                               CookieOffset};
  }
  return invalid("SecurityCookie is outside mapped image storage");
}
} // namespace

llvm::Expected<DriverImage> loadDriverImage(const std::filesystem::path &Path,
                                            uint64_t MemoryLimit,
                                            uint64_t LoadAddress) {
  std::error_code FileError;
  const uint64_t FileSize = std::filesystem::file_size(Path, FileError);
  if (FileError)
    return invalid("cannot read " + Path.string());
  if (FileSize > MemoryLimit)
    return invalid("input file exceeds memory limit");
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string());
  if (!Buffer)
    return invalid("cannot read " + Path.string());
  if (auto Error = preflightLoadConfiguration(llvm::ArrayRef<uint8_t>(
          reinterpret_cast<const uint8_t *>((*Buffer)->getBufferStart()),
          (*Buffer)->getBufferSize())))
    return std::move(Error);
  auto Parsed = COFFObjectFile::create((*Buffer)->getMemBufferRef());
  if (!Parsed)
    return Parsed.takeError();
  const auto &Object = **Parsed;
  const auto *Header = Object.getPE32PlusHeader();
  if (!Header || Object.getMachine() != llvm::COFF::IMAGE_FILE_MACHINE_AMD64 ||
      Object.isRelocatableObject() ||
      !(Object.getCharacteristics() &
        llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE) ||
      Header->Subsystem != llvm::COFF::IMAGE_SUBSYSTEM_NATIVE)
    return invalid("requires an executable x64 PE32+ native-subsystem image");
  if (Header->NumberOfRvaAndSize > 16 || Header->LoaderFlags ||
      Header->Win32VersionValue)
    return invalid("unsupported optional-header fields");
  if (Header->DLLCharacteristics &
      llvm::COFF::IMAGE_DLL_CHARACTERISTICS_GUARD_CF)
    return invalid("CFG initialization is unsupported");
  const uint64_t Base = Header->ImageBase;
  const uint64_t ActualBase = LoadAddress ? LoadAddress : Base;
  const uint64_t Size = Header->SizeOfImage;
  const uint64_t HeadersSize = Header->SizeOfHeaders;
  const uint64_t SectionAlignment = Header->SectionAlignment;
  const uint64_t FileAlignment = Header->FileAlignment;
  if (!powerOfTwo(SectionAlignment) || SectionAlignment < PageSize ||
      !powerOfTwo(FileAlignment) || FileAlignment < 512 ||
      FileAlignment > 65536 || SectionAlignment < FileAlignment ||
      Base < 65536 || (Base & 65535) || !Size || (Size % SectionAlignment) ||
      Size > MemoryLimit || Size > UINT64_MAX - Base || !HeadersSize ||
      HeadersSize % FileAlignment || HeadersSize > (*Buffer)->getBufferSize() ||
      pages(HeadersSize) > Size)
    return invalid("invalid image size, alignment, base, or memory limit");
  auto CanonicalImageRange = [&](uint64_t Address) {
    if (Address < 65536 || (Address & 65535) || Size > UINT64_MAX - Address)
      return false;
    return (Address <= 0x00007fffffffffffULL &&
            Address + Size - 1 <= 0x00007fffffffffffULL) ||
           Address >= 0xffff800000000000ULL;
  };
  if (!CanonicalImageRange(Base) || !CanonicalImageRange(ActualBase))
    return invalid(
        "image base has invalid alignment or noncanonical x64 range");
  const uint64_t End = ActualBase + Size;
  for (auto [Start, Limit] :
       {std::pair{profile::KernelArenaBase,
                  profile::KernelArenaBase + profile::KernelArenaSize},
        std::pair{profile::StackBase, profile::StackBase + profile::StackSize},
        std::pair{profile::ThunkBase, profile::ThunkBase + profile::ThunkSize}})
    if (ActualBase < Limit && End > Start)
      return invalid("image overlaps reserved emulation memory");

  std::vector<SectionPlan> Sections;
  std::vector<std::pair<uint64_t, uint64_t>> FileRanges;
  uint64_t LastEnd = pages(HeadersSize);
  bool EntryExecutable = false;
  for (const auto &Reference : Object.sections()) {
    const auto *Section = Object.getCOFFSection(Reference);
    const uint64_t RVA = Section->VirtualAddress;
    const uint64_t VirtualSize = Section->VirtualSize;
    const uint64_t RawSize = Section->SizeOfRawData;
    const uint64_t RawOffset = Section->PointerToRawData;
    const uint64_t Span = pages(std::max(VirtualSize, RawSize));
    if (!VirtualSize || RVA % SectionAlignment || RVA < LastEnd ||
        RVA >= Size || Span > Size - RVA ||
        (RawSize &&
         (RawOffset < HeadersSize || RawOffset % FileAlignment ||
          RawSize % FileAlignment || RawOffset > (*Buffer)->getBufferSize() ||
          RawSize > (*Buffer)->getBufferSize() - RawOffset)) ||
        Section->NumberOfRelocations || Section->PointerToRelocations)
      return invalid("invalid, overlapping, truncated, or relocatable section");
    for (auto [Start, Limit] : FileRanges)
      if (RawSize && RawOffset < Limit && RawOffset + RawSize > Start)
        return invalid("overlapping section file ranges");
    if (RawSize)
      FileRanges.emplace_back(RawOffset, RawOffset + RawSize);
    llvm::ArrayRef<uint8_t> Contents;
    if (auto Error = Object.getSectionContents(Section, Contents))
      return std::move(Error);
    Sections.push_back({Section, Contents, Span});
    LastEnd = RVA + Span;
    const uint64_t EntryRVA = Header->AddressOfEntryPoint;
    if ((Section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) &&
        EntryRVA >= RVA && EntryRVA - RVA < std::min(VirtualSize, RawSize))
      EntryExecutable = true;
  }
  if (Sections.empty() || !EntryExecutable)
    return invalid("entry point is not in file-backed executable bytes");
  const auto *LastSection = Sections.back().Header;
  const auto *BufferStart =
      reinterpret_cast<const uint8_t *>((*Buffer)->getBufferStart());
  const uint64_t SectionTableEnd =
      reinterpret_cast<const uint8_t *>(LastSection + 1) - BufferStart;
  if (SectionTableEnd > HeadersSize)
    return invalid("section table extends beyond SizeOfHeaders");

  std::set<uint64_t> Relocations;
  // Runtime requirements absent from the driver profile remain explicit errors.
  for (unsigned Index = 0; Index != 16; ++Index) {
    const auto *Directory = Object.getDataDirectory(Index);
    if (!Directory)
      continue;
    const uint64_t RVA = Directory->RelativeVirtualAddress;
    const uint64_t DirectorySize = Directory->Size;
    if (!RVA && !DirectorySize)
      continue;
    if (!RVA || !DirectorySize)
      return invalid("inconsistent data directory RVA and size");
    if (Index == llvm::COFF::TLS_TABLE ||
        Index == llvm::COFF::DELAY_IMPORT_DESCRIPTOR ||
        Index == llvm::COFF::BOUND_IMPORT ||
        Index == llvm::COFF::CLR_RUNTIME_HEADER ||
        Index == llvm::COFF::ARCHITECTURE || Index == llvm::COFF::GLOBAL_PTR ||
        Index == llvm::COFF::EXPORT_TABLE || Index == 15)
      return invalid("unsupported loader data directory " + llvm::Twine(Index));
    if (Index == llvm::COFF::CERTIFICATE_TABLE) {
      if (RVA > (*Buffer)->getBufferSize() ||
          DirectorySize > (*Buffer)->getBufferSize() - RVA)
        return invalid("truncated certificate table");
      continue; // Authenticode is metadata; this emulator does not establish
                // trust.
    }
    auto Bytes = fileBytes(Object, Sections, RVA, DirectorySize);
    if (!Bytes)
      return Bytes.takeError();
    if (Index == llvm::COFF::BASE_RELOCATION_TABLE) {
      auto Targets = validateBaseRelocations(*Bytes, Sections);
      if (!Targets)
        return Targets.takeError();
      Relocations = std::move(*Targets);
    }
    if (Index == llvm::COFF::EXCEPTION_TABLE && DirectorySize % 12)
      return invalid("x64 exception directory has an incomplete entry");
    if (Index == llvm::COFF::DEBUG_DIRECTORY) {
      if (DirectorySize % sizeof(debug_directory))
        return invalid("incomplete debug directory entry");
      for (size_t Offset = 0; Offset < Bytes->size();
           Offset += sizeof(debug_directory)) {
        debug_directory Entry;
        std::memcpy(&Entry, Bytes->data() + Offset, sizeof(Entry));
        if (Entry.Type != llvm::COFF::IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS)
          continue;
        if (Entry.SizeOfData < 4 ||
            Entry.PointerToRawData > (*Buffer)->getBufferSize() ||
            Entry.SizeOfData >
                (*Buffer)->getBufferSize() - Entry.PointerToRawData)
          return invalid("truncated extended DLL characteristics");
        const auto *Data =
            reinterpret_cast<const uint8_t *>((*Buffer)->getBufferStart()) +
            Entry.PointerToRawData;
        if (llvm::support::endian::read32le(Data))
          return invalid(
              "unsupported extended DLL characteristics (including CET)");
      }
    }
  }
  auto Imports = validateImports(Object, Sections, Base);
  if (!Imports)
    return Imports.takeError();
  auto Cookie = validateCookie(Object, Sections, Base);
  if (!Cookie)
    return Cookie.takeError();
  const bool Rebased = ActualBase != Base;
  if (Rebased &&
      ((Object.getCharacteristics() & llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED) ||
       Relocations.empty()))
    return invalid("requested rebase requires unstripped DIR64 relocations");
  if (Rebased && Cookie->RVA && !Relocations.count(Cookie->PointerRVA))
    return invalid("rebasing SecurityCookie requires its load configuration "
                   "DIR64 relocation");
  if (Cookie->RVA)
    for (unsigned Index = 0; Index != 16; ++Index) {
      const auto *Directory = Object.getDataDirectory(Index);
      if (!Directory || !Directory->Size ||
          Index == llvm::COFF::CERTIFICATE_TABLE)
        continue;
      const uint64_t Start = Directory->RelativeVirtualAddress;
      const uint64_t Limit = Start + Directory->Size;
      if (Cookie->RVA < Limit && Cookie->RVA + 8 > Start)
        return invalid("SecurityCookie storage overlaps loader metadata");
    }
  for (uint64_t RVA : Relocations) {
    for (const auto &Import : *Imports) {
      const uint64_t Slot = Import.Slot - Base;
      if (RVA < Slot + 8 && RVA + 8 > Slot)
        return invalid("DIR64 relocation overlaps import binding storage");
    }
    if (Cookie->RVA && RVA < Cookie->RVA + 8 && RVA + 8 > Cookie->RVA)
      return invalid("DIR64 relocation overlaps SecurityCookie storage");
  }

  // Keep the repository loader authoritative for the BinaryImage model. The
  // execution preflight above is deliberately stricter than analysis loading.
  auto Loader = neverd::Loader::create(BinaryFormat::COFF);
  auto Loaded = Loader->load(Path);
  if (!Loaded)
    return Loaded.takeError();
  const auto Original = (*Buffer)->getBuffer();
  if (Loaded->Raw.size() != Original.size() ||
      std::memcmp(Loaded->Raw.data(), Original.data(), Original.size()) != 0)
    return invalid("input changed while loading");
  if (Loaded->Base != Base ||
      Loaded->Entry != Base + Header->AddressOfEntryPoint ||
      Loaded->Arch != Arch::X64 || Loaded->Segments.size() != Sections.size() ||
      Loaded->Imports.size() != Imports->size())
    return invalid("loader metadata disagrees with validated execution image");
  if (Loaded->DynInfo.SecurityCookieRVA != Cookie->RVA)
    return invalid(
        "loader SecurityCookie identity disagrees with execution image");
  std::set<uint64_t> LoaderRelocations;
  for (const auto &Relocation : Loaded->BaseRelocations)
    if (Relocation.Type == llvm::COFF::IMAGE_REL_BASED_DIR64)
      LoaderRelocations.insert(Relocation.Address - Base);
  if (LoaderRelocations != Relocations)
    return invalid("loader DIR64 identities disagree with execution image");
  for (const auto &Import : *Imports) {
    const auto Found = std::find_if(
        Loaded->Imports.begin(), Loaded->Imports.end(), [&](const auto &Item) {
          return Item.IATAddr == Import.Slot && Item.Name == Import.Name &&
                 Item.Module == Import.Module;
        });
    if (Found == Loaded->Imports.end())
      return invalid("loader import identity disagrees with validated IAT");
  }

  DriverImage Image;
  Image.Base = ActualBase;
  Image.PreferredBase = Loaded->Base;
  Image.Entry = ActualBase + (Loaded->Entry - Base);
  Image.SecurityCookieAddress = Cookie->RVA ? ActualBase + Cookie->RVA : 0;
  Image.Size = Size;
  Image.Imports = std::move(*Imports);
  for (auto &Import : Image.Imports)
    Import.Slot = ActualBase + (Import.Slot - Base);
  DriverImageRegion Headers;
  Headers.Address = ActualBase;
  Headers.Permissions = Read;
  Headers.Bytes.resize(pages(HeadersSize), 0);
  std::copy_n(Loaded->Raw.begin(), HeadersSize, Headers.Bytes.begin());
  Image.Regions.push_back(std::move(Headers));
  for (size_t Index = 0; Index < Sections.size(); ++Index) {
    const auto &Segment = Loaded->Segments[Index];
    const auto &Plan = Sections[Index];
    if (Segment.VA != Base + Plan.Header->VirtualAddress ||
        Segment.Size != Plan.Header->VirtualSize)
      return invalid("loader section mapping disagrees with execution image");
    DriverImageRegion Region;
    Region.Address = ActualBase + (Segment.VA - Base);
    Region.Permissions = (Segment.isReadable() ? Read : 0) |
                         (Segment.isWritable() ? Write : 0) |
                         (Segment.isExecutable() ? Execute : 0);
    Region.Bytes.resize(Plan.Span, 0);
    const size_t CopySize = std::min<uint64_t>(Segment.Size, Segment.FileSz);
    if (CopySize > Segment.Data.size())
      return invalid("loader lost file-backed section bytes");
    std::copy_n(Segment.Data.begin(), CopySize, Region.Bytes.begin());
    Image.Regions.push_back(std::move(Region));
  }
  auto MutableBytes = [&](uint64_t RVA) -> uint8_t * {
    const uint64_t Address = ActualBase + RVA;
    for (auto &Region : Image.Regions)
      if (Address >= Region.Address &&
          Address - Region.Address <= Region.Bytes.size() &&
          Region.Bytes.size() - (Address - Region.Address) >= 8)
        return Region.Bytes.data() + (Address - Region.Address);
    return nullptr;
  };
  if (Rebased) {
    // DIR64 is defined as addition of the base difference to a 64-bit field.
    // Unsigned subtraction/addition preserve PE's modulo-2^64 field encoding,
    // including downward rebases and loading into the canonical kernel range.
    const uint64_t Difference = ActualBase - Base;
    for (uint64_t RVA : Relocations) {
      uint8_t *Target = MutableBytes(RVA);
      if (!Target)
        return invalid("relocation target disappeared from mapping plan");
      llvm::support::endian::write64le(
          Target, llvm::support::endian::read64le(Target) + Difference);
    }
  }
  if (Cookie->RVA) {
    uint8_t *Target = MutableBytes(Cookie->RVA);
    if (!Target)
      return invalid("SecurityCookie disappeared from mapping plan");
    // A concrete deterministic guest input, not a claim of host entropy. This
    // must happen before the PE entry wrapper or any /GS-protected function.
    // https://learn.microsoft.com/cpp/c-runtime-library/reference/security-init-cookie
    llvm::support::endian::write64le(Target, DriverSecurityCookie);
  }
  return Image;
}
} // namespace neverd::emulation
