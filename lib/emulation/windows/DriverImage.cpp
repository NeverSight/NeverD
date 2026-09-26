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
#include "GuardControlFlow.h"
#include "KernelExportRegistry.h"

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
constexpr unsigned MaxImportNameLength = 512;
constexpr unsigned MaxPESections = 96;
// LLVM names the 15 defined directory indices; PE32+ also has one reserved
// directory slot, so the header permits 16 entries.
constexpr unsigned MaxPEDirectoryEntries = llvm::COFF::NUM_DATA_DIRECTORIES + 1;
constexpr uint64_t MinPEFileAlignment = 512;
constexpr uint64_t PEImageBaseAlignment = 64 * 1024;
constexpr uint64_t MaxPEFileAlignment = PEImageBaseAlignment;

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
  for (unsigned I = 0; I != MaxImportNameLength; ++I) {
    auto Byte = fileBytes(Object, Sections, RVA + I, 1);
    if (!Byte)
      return Byte.takeError();
    const uint8_t Ch = (*Byte)[0];
    if (Ch == 0) {
      if (Name.empty())
        return invalid("empty import name");
      return Name;
    }
    if (Ch < '!' || Ch > '~')
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
    if (!KernelExportRegistry::canonicalImportModule(*Module))
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
      const uint64_t RVA = uint64_t(Block.PageRVA) + (Entry & (PageSize - 1));
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

/// Size/timestamp/version are descriptive. Cookie and supported guard fields
/// have dedicated validation. Other environment requirements must be zero.
/// Microsoft documents these fields at:
/// https://learn.microsoft.com/windows/win32/debug/pe-format#the-load-configuration-structure-image-only
llvm::Error validateLoadConfigurationBytes(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < 4)
    return invalid("truncated load configuration size");
  const uint64_t DeclaredSize = llvm::support::endian::read32le(Bytes.data());
  if (DeclaredSize < 12 || DeclaredSize > Bytes.size())
    return invalid("invalid load configuration declared size");
  constexpr size_t CookieOffset =
      offsetof(coff_load_configuration64, SecurityCookie);
  if (DeclaredSize > CookieOffset && DeclaredSize < CookieOffset + 8)
    return invalid("partial load configuration SecurityCookie field");
  const size_t PointerOffsets[] = {
      offsetof(coff_load_configuration64, SecurityCookie),
      offsetof(coff_load_configuration64, GuardCFCheckFunction),
      offsetof(coff_load_configuration64, GuardCFCheckDispatch),
      offsetof(coff_load_configuration64, GuardCFFunctionTable),
      offsetof(coff_load_configuration64, GuardCFFunctionCount),
      offsetof(coff_load_configuration64, GuardAddressTakenIatEntryTable),
      offsetof(coff_load_configuration64, GuardAddressTakenIatEntryCount),
      offsetof(coff_load_configuration64, GuardXFGCheckFunctionPointer),
      offsetof(coff_load_configuration64, GuardXFGDispatchFunctionPointer),
      offsetof(coff_load_configuration64, GuardXFGTableDispatchFunctionPointer),
      offsetof(coff_load_configuration64, CastGuardOsDeterminedFailureMode),
      guard::GuardMemcpyPointerOffset,
      guard::UmaPointersOffset};
  for (size_t Offset : PointerOffsets)
    if (DeclaredSize > Offset && DeclaredSize < Offset + 8)
      return invalid("partial load configuration pointer or count field");
  constexpr size_t FlagsOffset =
      offsetof(coff_load_configuration64, GuardFlags);
  if (DeclaredSize > FlagsOffset && DeclaredSize < FlagsOffset + 4)
    return invalid("partial load configuration GuardFlags field");
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
  for (size_t Offset = guard::KnownLoadConfigurationSize; Offset < DeclaredSize;
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
  if (PEOffset > Raw.size() ||
      Raw.size() - PEOffset < sizeof(llvm::COFF::PEMagic) ||
      std::memcmp(Raw.data() + PEOffset, llvm::COFF::PEMagic,
                  sizeof(llvm::COFF::PEMagic)))
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
  if (Header.NumberOfRvaAndSize > MaxPEDirectoryEntries ||
      sizeof(Header) +
              uint64_t(Header.NumberOfRvaAndSize) * sizeof(data_directory) >
          COFF.SizeOfOptionalHeader)
    return invalid("truncated optional-header data directories");
  const uint64_t SectionsOffset = OptionalOffset + COFF.SizeOfOptionalHeader;
  if (!COFF.NumberOfSections || COFF.NumberOfSections > MaxPESections ||
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
/// Public CFG metadata and AMD64 helper ABI:
/// https://learn.microsoft.com/windows/win32/secbp/pe-metadata
/// A library may advertise dormant guard slots without DLL_GUARD_CF. Those
/// slots retain their guest fallback code; only an enabled image is patched.
llvm::Expected<DriverGuardControlFlow>
validateGuard(const COFFObjectFile &Object,
              const std::vector<SectionPlan> &Sections,
              const std::vector<DriverImport> &Imports,
              const std::set<uint64_t> &Relocations, uint64_t ActualBase) {
  DriverGuardControlFlow Guard;
  const auto *Header = Object.getPE32PlusHeader();
  const uint64_t Base = Header->ImageBase;
  Guard.Enabled = Header->DLLCharacteristics &
                  llvm::COFF::IMAGE_DLL_CHARACTERISTICS_GUARD_CF;
  const auto *Directory =
      Object.getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
  if (!Directory || !Directory->Size) {
    if (Guard.Enabled)
      return invalid("CFG image has no load configuration");
    return Guard;
  }
  const uint64_t ConfigRVA = Directory->RelativeVirtualAddress;
  auto Prefix = fileBytes(Object, Sections, ConfigRVA, 4);
  if (!Prefix)
    return Prefix.takeError();
  const uint64_t DeclaredSize = llvm::support::endian::read32le(Prefix->data());
  auto Bytes = fileBytes(Object, Sections, ConfigRVA, DeclaredSize);
  if (!Bytes)
    return Bytes.takeError();
  auto Read64 = [&](size_t Offset) -> uint64_t {
    return Offset + 8 <= Bytes->size()
               ? llvm::support::endian::read64le(Bytes->data() + Offset)
               : 0;
  };
  constexpr size_t FlagsOffset =
      offsetof(coff_load_configuration64, GuardFlags);
  const uint32_t Flags =
      FlagsOffset + 4 <= Bytes->size()
          ? llvm::support::endian::read32le(Bytes->data() + FlagsOffset)
          : 0;
  constexpr uint64_t SupportedFlags =
      guard::Instrumented | guard::FunctionTablePresent |
      guard::SecurityCookieUnused | guard::LongJumpTablePresent |
      guard::FunctionTableSizeMask;
  if (Flags & ~SupportedFlags)
    return invalid(
        "unsupported CFG GuardFlags (including XFG/export suppression)");
  const uint64_t ExtraBytes =
      (Flags & guard::FunctionTableSizeMask) >> guard::FunctionTableSizeShift;
  if (ExtraBytes > 1)
    return invalid("unsupported CFG function table metadata stride");
  if (Guard.Enabled && (!(Flags & guard::Instrumented) ||
                        !(Flags & guard::FunctionTablePresent)))
    return invalid("CFG image requires instrumented and function-table flags");

  auto Executable = [&](uint64_t RVA) {
    for (const auto &Section : Sections) {
      const uint64_t Start = Section.Header->VirtualAddress;
      if (RVA >= Start && RVA - Start < Section.Header->VirtualSize &&
          RVA - Start < Section.Contents.size() &&
          (Section.Header->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
        return true;
    }
    return false;
  };
  auto ReadOnly = [&](uint64_t RVA, uint64_t Size) {
    for (const auto &Section : Sections) {
      const uint64_t Start = Section.Header->VirtualAddress;
      const uint64_t Span = Section.Header->VirtualSize;
      if (RVA >= Start && RVA - Start <= Span && Size <= Span - (RVA - Start))
        return (Section.Header->Characteristics &
                llvm::COFF::IMAGE_SCN_MEM_READ) &&
               !(Section.Header->Characteristics &
                 (llvm::COFF::IMAGE_SCN_MEM_WRITE |
                  llvm::COFF::IMAGE_SCN_MEM_EXECUTE));
    }
    return false;
  };
  std::set<uint64_t> PointerFields;
  std::set<uint64_t> PointerSlots;
  std::vector<std::pair<uint64_t, uint64_t>> Storage;
  auto RegisterStorage = [&](uint64_t RVA, uint64_t Size) -> llvm::Error {
    if (!ReadOnly(RVA, Size))
      return invalid("CFG metadata requires read-only nonexecutable storage");
    if (RVA < ConfigRVA + DeclaredSize && RVA + Size > ConfigRVA)
      return invalid("CFG storage overlaps load configuration");
    for (unsigned Index = 0; Index != 16; ++Index) {
      const auto *Other = Object.getDataDirectory(Index);
      if (!Other || !Other->Size || Index == llvm::COFF::CERTIFICATE_TABLE)
        continue;
      const uint64_t Start = Other->RelativeVirtualAddress;
      if (RVA < Start + Other->Size && RVA + Size > Start)
        return invalid("CFG storage overlaps loader metadata");
    }
    for (const auto &Import : Imports)
      if (RVA < Import.Slot - Base + 8 && RVA + Size > Import.Slot - Base)
        return invalid("CFG storage overlaps import binding storage");
    for (auto [Start, Length] : Storage)
      if (RVA < Start + Length && RVA + Size > Start)
        return invalid("overlapping CFG metadata storage");
    Storage.emplace_back(RVA, Size);
    return llvm::Error::success();
  };
  auto PointerRVA = [&](size_t Offset) -> llvm::Expected<uint64_t> {
    const uint64_t Address = Read64(Offset);
    if (!Address)
      return uint64_t(0);
    if (Address <= Base || Address - Base >= Header->SizeOfImage)
      return invalid("CFG pointer is outside image storage");
    PointerFields.insert(ConfigRVA + Offset);
    return Address - Base;
  };
  auto Slot = [&](size_t Offset, bool ZeroOnly) -> llvm::Expected<uint64_t> {
    auto RVA = PointerRVA(Offset);
    if (!RVA || !*RVA)
      return RVA;
    if (*RVA & 7)
      return invalid("unaligned CFG pointer slot");
    auto Contents = fileBytes(Object, Sections, *RVA, 8);
    if (!Contents)
      return Contents.takeError();
    if (auto Error = RegisterStorage(*RVA, 8))
      return std::move(Error);
    const uint64_t Target = llvm::support::endian::read64le(Contents->data());
    if (ZeroOnly) {
      if (Target)
        return invalid("unsupported active load configuration guard extension");
    } else {
      if (Target < Base || !Executable(Target - Base))
        return invalid("CFG fallback target is not file-backed image code");
      PointerSlots.insert(*RVA);
    }
    return *RVA;
  };
  auto Check =
      Slot(offsetof(coff_load_configuration64, GuardCFCheckFunction), false);
  if (!Check)
    return Check.takeError();
  auto Dispatch =
      Slot(offsetof(coff_load_configuration64, GuardCFCheckDispatch), false);
  if (!Dispatch)
    return Dispatch.takeError();
  if ((*Check || *Dispatch) && !(Flags & guard::Instrumented))
    return invalid("CFG slots require the instrumented flag");
  if (Guard.Enabled && !*Check)
    return invalid("CFG image requires a check pointer slot");
  Guard.CheckPointerAddress = *Check ? ActualBase + *Check : 0;
  Guard.DispatchPointerAddress = *Dispatch ? ActualBase + *Dispatch : 0;

  // No XFG policy is installed. Unadvertised XFG fallbacks remain ordinary
  // guest code; active XFG is rejected above. New optional extension slots
  // are admitted only as inert zero storage, never synthesized by the host.
  for (size_t Offset :
       {offsetof(coff_load_configuration64, GuardXFGCheckFunctionPointer),
        offsetof(coff_load_configuration64, GuardXFGDispatchFunctionPointer),
        offsetof(coff_load_configuration64,
                 GuardXFGTableDispatchFunctionPointer)}) {
    auto Result = Slot(Offset, false);
    if (!Result)
      return Result.takeError();
  }
  for (size_t Offset :
       {offsetof(coff_load_configuration64, CastGuardOsDeterminedFailureMode),
        size_t(guard::GuardMemcpyPointerOffset),
        size_t(guard::UmaPointersOffset)}) {
    auto Result = Slot(Offset, true);
    if (!Result)
      return Result.takeError();
  }
  const uint64_t Stride = 4 + ExtraBytes;
  auto Table = [&](size_t PointerOffset, size_t CountOffset,
                   bool IAT) -> llvm::Error {
    auto RVA = PointerRVA(PointerOffset);
    if (!RVA)
      return RVA.takeError();
    const uint64_t Count = Read64(CountOffset);
    if (bool(*RVA) != bool(Count))
      return invalid("inconsistent CFG table pointer and count");
    if (!IAT && (Count || ExtraBytes) && !(Flags & guard::FunctionTablePresent))
      return invalid("CFG function table is missing its presence flag");
    if (!IAT && Guard.Enabled && !Count)
      return invalid("CFG image requires declared function targets");
    if (!Count)
      return llvm::Error::success();
    if (Count > guard::MaximumTargets)
      return invalid("CFG target count exceeds execution profile");
    // GFIDS and address-taken IAT tables are packed 4+n-byte records. PE
    // defines no DWORD alignment for the table base (LLD uses byte alignment).
    // The executable targets' alignment is independent of this storage.
    auto Contents = fileBytes(Object, Sections, *RVA, Count * Stride);
    if (!Contents)
      return Contents.takeError();
    if (auto Error = RegisterStorage(*RVA, Count * Stride))
      return Error;
    uint32_t Previous = 0;
    for (uint64_t Index = 0; Index != Count; ++Index) {
      const uint8_t *Entry = Contents->data() + Index * Stride;
      const uint32_t Target = llvm::support::endian::read32le(Entry);
      const uint8_t Metadata = ExtraBytes ? Entry[4] : 0;
      if (!Target || (Index && Target <= Previous))
        return invalid("CFG targets must be unique and sorted");
      Previous = Target;
      if (IAT) {
        if (Metadata || std::none_of(Imports.begin(), Imports.end(),
                                     [&](const auto &Import) {
                                       return Import.Slot - Base == Target;
                                     }))
          return invalid(
              "CFG address-taken IAT target is not a declared import slot");
      } else {
        if (!Executable(Target))
          return invalid(
              "CFG function target is not file-backed executable code");
        if (Metadata & ~guard::FunctionSuppressed)
          return invalid("unsupported CFG function target metadata");
        // The linker may explicitly include compatibility fallbacks when
        // inferring targets from non-CFG objects. Honor its declared GFIDS;
        // recommending that toolchains suppress a helper is not a PE validity
        // condition and does not permit inventing a different target set.
        if (!(Metadata & guard::FunctionSuppressed))
          Guard.ValidTargets.push_back(ActualBase + Target);
      }
    }
    return llvm::Error::success();
  };
  if (auto Error = Table(
          offsetof(coff_load_configuration64, GuardCFFunctionTable),
          offsetof(coff_load_configuration64, GuardCFFunctionCount), false))
    return std::move(Error);
  if (auto Error = Table(
          offsetof(coff_load_configuration64, GuardAddressTakenIatEntryTable),
          offsetof(coff_load_configuration64, GuardAddressTakenIatEntryCount),
          true))
    return std::move(Error);
  if (Guard.Enabled &&
      !std::binary_search(Guard.ValidTargets.begin(), Guard.ValidTargets.end(),
                          ActualBase + Header->AddressOfEntryPoint))
    return invalid("CFG entry point is missing from declared valid targets");

  // DIR64 must cover each surviving absolute pointer on a requested rebase.
  // It may not rewrite counts, flags, RVAs or partial guard records.
  const auto CookieOffset = offsetof(coff_load_configuration64, SecurityCookie);
  if (Read64(CookieOffset))
    PointerFields.insert(ConfigRVA + CookieOffset);
  for (uint64_t RVA : Relocations) {
    if (RVA < ConfigRVA + DeclaredSize && RVA + 8 > ConfigRVA &&
        !PointerFields.count(RVA))
      return invalid(
          "DIR64 relocation overlaps nonpointer load configuration metadata");
    for (auto [Start, Size] : Storage)
      if (RVA < Start + Size && RVA + 8 > Start && !PointerSlots.count(RVA))
        return invalid("DIR64 relocation overlaps CFG metadata");
  }
  if (ActualBase != Base) {
    PointerFields.insert(PointerSlots.begin(), PointerSlots.end());
    for (uint64_t RVA : PointerFields)
      if (!Relocations.count(RVA))
        return invalid(
            "rebasing CFG metadata requires complete DIR64 relocations");
  }
  return Guard;
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
  if (Header->NumberOfRvaAndSize > MaxPEDirectoryEntries ||
      Header->LoaderFlags || Header->Win32VersionValue)
    return invalid("unsupported optional-header fields");
  const uint64_t Base = Header->ImageBase;
  const uint64_t ActualBase = LoadAddress ? LoadAddress : Base;
  const uint64_t Size = Header->SizeOfImage;
  const uint64_t HeadersSize = Header->SizeOfHeaders;
  const uint64_t SectionAlignment = Header->SectionAlignment;
  const uint64_t FileAlignment = Header->FileAlignment;
  if (!powerOfTwo(SectionAlignment) || SectionAlignment < PageSize ||
      !powerOfTwo(FileAlignment) || FileAlignment < MinPEFileAlignment ||
      FileAlignment > MaxPEFileAlignment || SectionAlignment < FileAlignment ||
      Base < PEImageBaseAlignment || (Base % PEImageBaseAlignment) || !Size ||
      (Size % SectionAlignment) || Size > MemoryLimit ||
      Size > UINT64_MAX - Base || !HeadersSize || HeadersSize % FileAlignment ||
      HeadersSize > (*Buffer)->getBufferSize() || pages(HeadersSize) > Size)
    return invalid("invalid image size, alignment, base, or memory limit");
  auto CanonicalImageRange = [&](uint64_t Address) {
    if (Address < PEImageBaseAlignment || (Address % PEImageBaseAlignment) ||
        Size > UINT64_MAX - Address)
      return false;
    return (Address <= profile::CanonicalUserMax &&
            Address + Size - 1 <= profile::CanonicalUserMax) ||
           Address >= profile::CanonicalKernelMin;
  };
  if (!CanonicalImageRange(Base) || !CanonicalImageRange(ActualBase))
    return invalid(
        "image base has invalid alignment or noncanonical x64 range");
  const uint64_t End = ActualBase + Size;
  if (ActualBase < profile::UserProbeLimit)
    return invalid("driver image overlaps the modeled user address space");
  for (auto [Start, Limit] :
       {std::pair{profile::KernelArenaBase,
                  profile::KernelArenaBase + profile::KernelArenaSize},
        std::pair{profile::StackBase, profile::StackBase + profile::StackSize},
        std::pair{profile::ThunkBase, profile::ThunkBase + profile::ThunkSize},
        std::pair{profile::CallbackStackBase,
                  profile::CallbackStackBase +
                      profile::MaxConcurrentCallbacks *
                          profile::CallbackStackStride},
        std::pair{profile::GuardThunkBase,
                  profile::GuardThunkBase + profile::PageSize},
        std::pair{profile::UserAliasBase,
                  profile::UserAliasBase + profile::UserAliasSize},
        std::pair{profile::MMIOBase, profile::MMIOBase + profile::MMIOSize}})
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

  auto Guard =
      validateGuard(Object, Sections, *Imports, Relocations, ActualBase);
  if (!Guard)
    return Guard.takeError();

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
  Image.Guard = std::move(*Guard);
  Image.Imports = std::move(*Imports);
  Image.Exceptions = std::move(Loaded->ExceptionMetadata);
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
