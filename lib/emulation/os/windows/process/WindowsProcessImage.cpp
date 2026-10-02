//===- WindowsProcessImage.cpp - Bounded PE user image admission ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "neverd/loader/Loader.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace neverd::emulation::windows_process {
namespace {
using namespace llvm::object;
using namespace value;
bool powerOfTwo(uint64_t N) { return N && !(N & (N - 1)); }
uint64_t pages(uint64_t N) { return (N + PageSize - 1) & ~(PageSize - 1); }
struct Section {
  coff_section Header;
  llvm::ArrayRef<uint8_t> Raw;
};
class Reader {
public:
  llvm::ArrayRef<uint8_t> Raw;
  std::vector<Section> Sections;
  ImageReadBudget &Budget;
  mutable std::vector<PEMetadataRange> Metadata;
  bool record() const {
    if (!Budget.Records)
      return false;
    --Budget.Records;
    return true;
  }
  template <typename T> bool copy(uint64_t Offset, T &Out) const {
    if (Offset > Raw.size() || sizeof(T) > Raw.size() - Offset)
      return false;
    std::memcpy(&Out, Raw.data() + Offset, sizeof(T));
    return true;
  }
  llvm::Expected<llvm::ArrayRef<uint8_t>> bytes(uint64_t RVA,
                                                uint64_t Size) const {
    if (Size > Budget.MetadataBytes)
      return failure(text::ModuleBudget);
    Budget.MetadataBytes -= Size;
    if (!Metadata.empty() && RVA >= Metadata.back().RVA &&
        RVA <= Metadata.back().RVA + Metadata.back().Size)
      Metadata.back().Size =
          std::max(Metadata.back().Size, RVA + Size - Metadata.back().RVA);
    else
      Metadata.push_back({RVA, Size});
    for (const auto &S : Sections) {
      const uint64_t Start = S.Header.VirtualAddress;
      if (RVA < Start || RVA - Start >= S.Header.VirtualSize)
        continue;
      const uint64_t Offset = RVA - Start;
      if (Offset > S.Raw.size() || Size > S.Raw.size() - Offset ||
          Size > uint64_t(S.Header.VirtualSize) - Offset)
        return failure(text::Metadata);
      return S.Raw.slice(Offset, Size);
    }
    return failure(text::Metadata);
  }
  bool accessible(uint64_t RVA, uint64_t Size, uint32_t Rights) const {
    for (const auto &S : Sections) {
      const uint64_t Start = S.Header.VirtualAddress;
      if (RVA >= Start && RVA - Start < S.Header.VirtualSize &&
          Size <= uint64_t(S.Header.VirtualSize) - (RVA - Start) &&
          (S.Header.Characteristics & Rights) == Rights)
        return true;
    }
    return false;
  }
  llvm::Expected<std::string> name(uint64_t RVA) const {
    std::string Name;
    for (uint64_t I = 0; I < MaxName; ++I) {
      auto B = bytes(RVA + I, 1);
      if (!B)
        return B.takeError();
      if (!B->front()) {
        if (Name.empty())
          return failure(text::Imports);
        return Name;
      }
      if (!llvm::isPrint(B->front()))
        return failure(text::Imports);
      Name += static_cast<char>(B->front());
    }
    return failure(text::Imports);
  }
};
llvm::Error readImports(const Reader &R, const data_directory &D, Image &Out,
                        bool GuestImports) {
  if (!D.Size)
    return llvm::Error::success();
  auto Data = R.bytes(D.RelativeVirtualAddress, D.Size);
  if (!Data)
    return Data.takeError();
  if (D.Size / sizeof(coff_import_directory_table_entry) > MaxImportDescriptors)
    return failure(text::Imports);
  std::set<uint64_t> Slots;
  bool Terminated = false;
  for (size_t Offset = 0;
       Offset + sizeof(coff_import_directory_table_entry) <= Data->size();
       Offset += sizeof(coff_import_directory_table_entry)) {
    coff_import_directory_table_entry Entry;
    std::memcpy(&Entry, Data->data() + Offset, sizeof(Entry));
    if (Entry.isNull()) {
      Terminated = true;
      break;
    }
    if (Entry.TimeDateStamp || Entry.ForwarderChain ||
        !Entry.ImportAddressTableRVA || !Entry.NameRVA)
      return failure(text::Imports);
    auto Module = R.name(Entry.NameRVA);
    if (!Module)
      return Module.takeError();
    if (!R.record())
      return failure(text::ModuleBudget);
    if (!GuestImports && !findProvider(*Module))
      return failure(text::Import + *Module);
    Out.Dependencies.push_back(llvm::StringRef(*Module).lower());
    const uint64_t Lookup = Entry.ImportLookupTableRVA
                                ? uint32_t(Entry.ImportLookupTableRVA)
                                : uint32_t(Entry.ImportAddressTableRVA);
    if (Lookup % PointerSize || Entry.ImportAddressTableRVA % PointerSize)
      return failure(text::Imports);
    bool End = false;
    for (uint64_t I = 0; I <= MaxImports; ++I) {
      const uint64_t Slot =
          uint64_t(Entry.ImportAddressTableRVA) + I * PointerSize;
      auto Symbol = R.bytes(Lookup + I * PointerSize, PointerSize);
      if (!Symbol)
        return Symbol.takeError();
      auto IAT = R.bytes(Slot, PointerSize);
      if (!IAT)
        return IAT.takeError();
      const uint64_t NameRVA = llvm::support::endian::read64le(Symbol->data());
      if (NameRVA != llvm::support::endian::read64le(IAT->data()))
        return failure(text::Imports);
      if (!NameRVA) {
        End = true;
        break;
      }
      if (Out.Imports.size() == MaxImports || !Slots.insert(Slot).second)
        return failure(text::Imports);
      if (!R.record())
        return failure(text::ModuleBudget);
      std::string Name;
      std::optional<uint16_t> Ordinal;
      if (NameRVA & ImportOrdinalFlag) {
        if (NameRVA & ~(ImportOrdinalFlag | ImportOrdinalMask))
          return failure(text::Imports);
        Ordinal = NameRVA & ImportOrdinalMask;
      } else {
        if (NameRVA > UINT32_MAX)
          return failure(text::Imports);
        auto Hint = R.bytes(NameRVA, sizeof(uint16_t));
        if (!Hint)
          return Hint.takeError();
        auto SymbolName = R.name(NameRVA + sizeof(uint16_t));
        if (!SymbolName)
          return SymbolName.takeError();
        Name = std::move(*SymbolName);
      }
      const auto *Target = Ordinal ? nullptr : findService(*Module, Name);
      if ((!GuestImports || findProvider(*Module)) && !Target)
        return failure(text::Import + *Module +
                       llvm::Twine(text::ImportSeparator) + Name);
      Out.Imports.push_back({Out.Base + Slot, Target,
                             llvm::StringRef(*Module).lower(), 0,
                             std::move(Name), Ordinal});
    }
    if (!End)
      return failure(text::Imports);
  }
  return Terminated ? llvm::Error::success() : failure(text::Imports);
}
llvm::Error readTLS(const Reader &R, const data_directory &D, Image &Out) {
  if (!D.Size)
    return llvm::Error::success();
  if (D.Size != sizeof(coff_tls_directory64))
    return failure(text::TLS);
  auto Data = R.bytes(D.RelativeVirtualAddress, D.Size);
  if (!Data)
    return Data.takeError();
  coff_tls_directory64 TLS;
  std::memcpy(&TLS, Data->data(), sizeof(TLS));
  auto RVA = [&](uint64_t VA, uint64_t Size, uint32_t Rights) {
    return VA >= Out.Base && R.accessible(VA - Out.Base, Size, Rights);
  };
  const uint64_t Begin = TLS.StartAddressOfRawData;
  const uint64_t End = TLS.EndAddressOfRawData;
  const uint64_t Alignment =
      (TLS.Characteristics & TLSAlignmentMask) >> TLSAlignmentShift;
  if (End < Begin || End - Begin > TLSCapacity ||
      TLS.SizeOfZeroFill > TLSCapacity - (End - Begin) ||
      (TLS.Characteristics & ~TLSAlignmentMask) ||
      (Alignment && (uint64_t(1) << (Alignment - 1)) > PageSize) ||
      !RVA(TLS.AddressOfIndex, DWordSize, llvm::COFF::IMAGE_SCN_MEM_WRITE) ||
      TLS.AddressOfIndex % DWordSize)
    return failure(text::TLS);
  if (Begin && (Begin < Out.Base || Begin - Out.Base > Out.Size))
    return failure(text::TLS);
  if (End != Begin) {
    if (!RVA(Begin, End - Begin, llvm::COFF::IMAGE_SCN_MEM_READ))
      return failure(text::TLS);
    auto Bytes = R.bytes(Begin - Out.Base, End - Begin);
    if (!Bytes)
      return Bytes.takeError();
    Out.TLSBytes.assign(Bytes->begin(), Bytes->end());
  }
  Out.TLSCallbackPointer = Out.Base + D.RelativeVirtualAddress +
                           offsetof(coff_tls_directory64, AddressOfCallBacks);
  Out.TLSIndex = TLS.AddressOfIndex;
  Out.TLSSize = End - Begin + TLS.SizeOfZeroFill;
  for (const auto &I : Out.Imports)
    if (Out.TLSIndex < I.Slot + PointerSize &&
        I.Slot < Out.TLSIndex + DWordSize)
      return failure(text::TLS);
  if (!TLS.AddressOfCallBacks)
    return llvm::Error::success();
  if (TLS.AddressOfCallBacks < Out.Base || TLS.AddressOfCallBacks % PointerSize)
    return failure(text::TLS);
  for (uint64_t I = 0; I <= MaxCallbacks; ++I) {
    const uint64_t Offset = TLS.AddressOfCallBacks - Out.Base;
    if (Offset > Out.Size || I * PointerSize > Out.Size - Offset)
      return failure(text::TLS);
    auto Pointer = R.bytes(Offset + I * PointerSize, PointerSize);
    if (!Pointer)
      return Pointer.takeError();
    const uint64_t Target = llvm::support::endian::read64le(Pointer->data());
    if (!Target)
      return llvm::Error::success();
    if (I == MaxCallbacks ||
        (Out.Architecture == GuestArchitecture::AArch64 &&
         Target % DWordSize) ||
        !RVA(Target, 1, llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
      return failure(text::TLS);
    auto Code = R.bytes(Target - Out.Base, 1);
    if (!Code)
      return Code.takeError();
    Out.TLSCallbacks.push_back(Target);
  }
  return failure(text::TLS);
}
llvm::Expected<Image> readImage(const std::filesystem::path &Path,
                                ImageReadBudget &Budget, bool DLL,
                                bool GuestImports) {
  std::error_code EC;
  const uint64_t FileSize = std::filesystem::file_size(Path, EC);
  if (EC || FileSize > Budget.FileBytes)
    return failure(text::Image);
  auto Buffer = llvm::MemoryBuffer::getFile(Path.string());
  if (!Buffer)
    return llvm::errorCodeToError(Buffer.getError());
  if ((*Buffer)->getBufferSize() != FileSize)
    return failure(text::Image);
  Budget.FileBytes -= FileSize;
  Reader R{{reinterpret_cast<const uint8_t *>((*Buffer)->getBufferStart()),
            (*Buffer)->getBufferSize()},
           {},
           Budget,
           {}};
  dos_header DOS;
  coff_file_header COFF;
  pe32plus_header PE;
  uint32_t Signature;
  // Bound every record directly. No optional parser may follow load-config
  // or mixed-architecture pointers before execution policy has admitted them.
  if (!R.copy(0, DOS) ||
      std::memcmp(DOS.Magic, text::DOSMagic, sizeof(DOS.Magic)) ||
      !R.copy(DOS.AddressOfNewExeHeader, Signature) ||
      std::memcmp(&Signature, llvm::COFF::PEMagic, sizeof(Signature)))
    return failure(text::Headers);
  const uint64_t CoffOffset =
      uint64_t(DOS.AddressOfNewExeHeader) + sizeof(Signature);
  if (!R.copy(CoffOffset, COFF))
    return failure(text::Headers);
  const uint64_t OptionalOffset = CoffOffset + sizeof(COFF);
  if (!R.copy(OptionalOffset, PE) || COFF.SizeOfOptionalHeader < sizeof(PE) ||
      PE.Magic != llvm::COFF::PE32Header::PE32_PLUS ||
      PE.NumberOfRvaAndSize > MaxDirectories ||
      sizeof(PE) + uint64_t(PE.NumberOfRvaAndSize) * sizeof(data_directory) >
          COFF.SizeOfOptionalHeader)
    return failure(text::Headers);
  if ((COFF.Machine != llvm::COFF::IMAGE_FILE_MACHINE_AMD64 &&
       COFF.Machine != llvm::COFF::IMAGE_FILE_MACHINE_ARM64) ||
      !(COFF.Characteristics & llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE) ||
      bool(COFF.Characteristics & llvm::COFF::IMAGE_FILE_DLL) != DLL ||
      (COFF.Characteristics & llvm::COFF::IMAGE_FILE_SYSTEM) ||
      (PE.Subsystem != llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI &&
       (!DLL || PE.Subsystem != llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_GUI)) ||
      PE.LoaderFlags || PE.Win32VersionValue ||
      (PE.DLLCharacteristics & llvm::COFF::IMAGE_DLL_CHARACTERISTICS_GUARD_CF))
    return failure(text::Image);
  const uint64_t Base = PE.ImageBase, Size = PE.SizeOfImage;
  if (!powerOfTwo(PE.SectionAlignment) || PE.SectionAlignment < PageSize ||
      !powerOfTwo(PE.FileAlignment) || PE.FileAlignment < MinFileAlignment ||
      PE.FileAlignment > MaxFileAlignment ||
      PE.FileAlignment > PE.SectionAlignment || Base < ImageAlignment ||
      Base % ImageAlignment || Base >= UserLimit || !Size ||
      Size > UserLimit - Base || Size > Budget.MappedBytes ||
      Size % PE.SectionAlignment || !PE.SizeOfHeaders ||
      PE.SizeOfHeaders % PE.FileAlignment || PE.SizeOfHeaders > R.Raw.size() ||
      pages(PE.SizeOfHeaders) > Size || !COFF.NumberOfSections ||
      COFF.NumberOfSections > MaxSections)
    return failure(text::Headers);
  Image Out{COFF.Machine == llvm::COFF::IMAGE_FILE_MACHINE_AMD64
                ? GuestArchitecture::X64
                : GuestArchitecture::AArch64,
            Base, Size,
            PE.AddressOfEntryPoint ? Base + PE.AddressOfEntryPoint : 0};
  Budget.MappedBytes -= Size;
  if (DLL && PE.AddressOfEntryPoint)
    return failure(text::ModuleInit);
  uint64_t Table = OptionalOffset + COFF.SizeOfOptionalHeader;
  if (Table > PE.SizeOfHeaders ||
      uint64_t(COFF.NumberOfSections) * sizeof(coff_section) >
          PE.SizeOfHeaders - Table)
    return failure(text::Headers);
  uint64_t Previous = pages(PE.SizeOfHeaders);
  for (unsigned I = 0; I < COFF.NumberOfSections; ++I) {
    if (!R.record())
      return failure(text::ModuleBudget);
    coff_section S;
    if (!R.copy(Table + I * sizeof(S), S))
      return failure(text::Headers);
    const uint64_t Span =
        pages(std::max(uint32_t(S.VirtualSize), uint32_t(S.SizeOfRawData)));
    const uint64_t FileOffset = S.PointerToRawData, RawSize = S.SizeOfRawData;
    if (!S.VirtualSize || S.VirtualAddress % PE.SectionAlignment ||
        S.VirtualAddress < Previous || S.VirtualAddress >= Size ||
        Span > Size - S.VirtualAddress || S.NumberOfRelocations ||
        S.PointerToRelocations ||
        (RawSize &&
         (FileOffset < PE.SizeOfHeaders || FileOffset % PE.FileAlignment ||
          RawSize % PE.FileAlignment || FileOffset > R.Raw.size() ||
          RawSize > R.Raw.size() - FileOffset)))
      return failure(text::Sections);
    for (const auto &Other : R.Sections)
      if (RawSize && Other.Header.SizeOfRawData &&
          FileOffset <
              uint64_t(Other.Header.PointerToRawData) + Other.Raw.size() &&
          FileOffset + RawSize > Other.Header.PointerToRawData)
        return failure(text::Sections);
    unsigned Permissions = UserAccessible;
    if (S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ)
      Permissions |= Read;
    if (S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE)
      Permissions |= Write;
    if (S.Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE)
      Permissions |= Execute;
    if (!(Permissions & Read))
      return failure(text::Sections);
    auto Raw =
        RawSize ? R.Raw.slice(FileOffset, RawSize) : llvm::ArrayRef<uint8_t>{};
    R.Sections.push_back({S, Raw});
    ImageRegion Region{Base + S.VirtualAddress, Permissions,
                       std::vector<uint8_t>(Span)};
    std::copy(Raw.begin(), Raw.end(), Region.Bytes.begin());
    Out.Regions.push_back(std::move(Region));
    Previous = S.VirtualAddress + Span;
  }
  if (!DLL) {
    if ((Out.Architecture == GuestArchitecture::AArch64 &&
         PE.AddressOfEntryPoint % DWordSize) ||
        !R.accessible(PE.AddressOfEntryPoint, 1,
                      llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
      return failure(text::Image);
    auto Entry = R.bytes(PE.AddressOfEntryPoint, 1);
    if (!Entry)
      return Entry.takeError();
  }
  std::array<data_directory, MaxDirectories> Directories{};
  for (unsigned I = 0; I < PE.NumberOfRvaAndSize; ++I) {
    auto &D = Directories[I];
    if (!R.copy(OptionalOffset + sizeof(PE) + I * sizeof(D), D))
      return failure(text::Headers);
    if (!D.RelativeVirtualAddress && !D.Size)
      continue;
    if (!D.RelativeVirtualAddress || !D.Size ||
        I == llvm::COFF::LOAD_CONFIG_TABLE ||
        I == llvm::COFF::DELAY_IMPORT_DESCRIPTOR ||
        I == llvm::COFF::BOUND_IMPORT || I == llvm::COFF::CLR_RUNTIME_HEADER ||
        I == llvm::COFF::ARCHITECTURE || I == llvm::COFF::GLOBAL_PTR ||
        I == MaxDirectories - 1)
      return failure(text::Directory + llvm::Twine(I));
    if (I == llvm::COFF::CERTIFICATE_TABLE) {
      if (D.RelativeVirtualAddress > R.Raw.size() ||
          D.Size > R.Raw.size() - D.RelativeVirtualAddress)
        return failure(text::Metadata);
      continue;
    }
    if (DLL && I == llvm::COFF::TLS_TABLE)
      return failure(text::ModuleInit);
    auto Data = R.bytes(D.RelativeVirtualAddress, D.Size);
    if (!Data)
      return Data.takeError();
    if (I == llvm::COFF::DEBUG_DIRECTORY) {
      if (D.Size % sizeof(debug_directory))
        return failure(text::Metadata);
      for (uint64_t Offset = 0; Offset < Data->size();
           Offset += sizeof(debug_directory)) {
        debug_directory Debug;
        std::memcpy(&Debug, Data->data() + Offset, sizeof(Debug));
        if (Debug.Type == llvm::COFF::IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS) {
          if (Debug.SizeOfData != DWordSize ||
              Debug.PointerToRawData > R.Raw.size() ||
              Debug.SizeOfData > R.Raw.size() - Debug.PointerToRawData)
            return failure(text::Directory + llvm::Twine(I));
          const auto *Payload = R.Raw.data() + Debug.PointerToRawData;
          // An unmapped debug payload may have RVA zero. If both locations
          // exist, they must identify the same bytes, not merely equal flags.
          if (Debug.AddressOfRawData) {
            auto Mapped = R.bytes(Debug.AddressOfRawData, Debug.SizeOfData);
            if (!Mapped)
              return Mapped.takeError();
            if (Mapped->data() != Payload)
              return failure(text::Directory + llvm::Twine(I));
          }
          if (llvm::support::endian::read32le(Payload))
            return failure(text::Directory + llvm::Twine(I));
        }
      }
    }
    if (I == llvm::COFF::BASE_RELOCATION_TABLE) {
      Out.Relocatable =
          !(COFF.Characteristics & llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED);
      uint64_t Offset = 0;
      std::set<uint64_t> Targets;
      while (Offset < Data->size()) {
        coff_base_reloc_block_header B;
        if (Data->size() - Offset < sizeof(B))
          return failure(text::Metadata);
        std::memcpy(&B, Data->data() + Offset, sizeof(B));
        if (B.BlockSize < sizeof(B) || B.BlockSize % sizeof(uint32_t) ||
            B.BlockSize > Data->size() - Offset || B.PageRVA % PageSize)
          return failure(text::Metadata);
        for (uint64_t J = sizeof(B); J < B.BlockSize; J += sizeof(uint16_t)) {
          if (!R.record())
            return failure(text::ModuleBudget);
          const auto E =
              llvm::support::endian::read16le(Data->data() + Offset + J);
          const auto Type = E >> RelocationTypeShift;
          if (Type == llvm::COFF::IMAGE_REL_BASED_ABSOLUTE)
            continue;
          const uint64_t Target = uint64_t(B.PageRVA) + (E & (PageSize - 1));
          if (Type != llvm::COFF::IMAGE_REL_BASED_DIR64 ||
              !R.accessible(Target, PointerSize, 0))
            return failure(text::Metadata);
          auto Next = Targets.lower_bound(Target);
          if ((Next != Targets.end() && *Next < Target + PointerSize) ||
              (Next != Targets.begin() &&
               *std::prev(Next) + PointerSize > Target))
            return failure(text::Metadata);
          Targets.insert(Target);
          Out.Relocations.push_back(Target);
        }
        Offset += B.BlockSize;
      }
    }
  }
  if (auto E = readImports(R, Directories[llvm::COFF::IMPORT_TABLE], Out,
                           GuestImports))
    return std::move(E);
  if (auto E = readTLS(R, Directories[llvm::COFF::TLS_TABLE], Out))
    return std::move(E);
  auto Exports = readPEProgramExports(
      R.Raw, {Budget.MetadataBytes, Budget.Records, MaxName});
  if (!Exports)
    return Exports.takeError();
  Budget.MetadataBytes -= Exports->BytesRead;
  Budget.Records -= Exports->RecordsRead;
  Out.Exports = std::move(*Exports);
  for (const auto &Export : Out.Exports.Entries) {
    if (Export.Kind == PEExportKind::Forwarder)
      return failure(text::ModuleForwarder);
    if (Export.Kind == PEExportKind::Address && !R.accessible(Export.RVA, 1, 0))
      return failure(text::ModuleExport);
  }
  if (DLL) {
    R.Metadata.insert(R.Metadata.end(), Out.Exports.Metadata.begin(),
                      Out.Exports.Metadata.end());
    std::sort(R.Metadata.begin(), R.Metadata.end(),
              [](const auto &A, const auto &B) { return A.RVA < B.RVA; });
    std::sort(Out.Relocations.begin(), Out.Relocations.end());
    size_t Index = 0;
    for (uint64_t Target : Out.Relocations) {
      while (Index < R.Metadata.size() &&
             R.Metadata[Index].RVA + R.Metadata[Index].Size <= Target)
        ++Index;
      if (Index < R.Metadata.size() &&
          R.Metadata[Index].RVA < Target + PointerSize)
        return failure(text::ModuleFixup);
    }
  }
  // Execution admission is stricter than analysis. Keep the repository's
  // loader authoritative for the image/segment/import identities and use the
  // original bytes, never analysis-time pointer fixups, for guest execution.
  auto Loader = neverd::Loader::create(BinaryFormat::COFF);
  auto Decoded = Loader->load(Path);
  if (!Decoded)
    return Decoded.takeError();
  if (Decoded->Raw.size() != R.Raw.size() ||
      !std::equal(Decoded->Raw.begin(), Decoded->Raw.end(), R.Raw.begin()) ||
      Decoded->Base != Base || Decoded->Entry != Out.Entry ||
      Decoded->Arch != (Out.Architecture == GuestArchitecture::X64
                            ? Arch::X64
                            : Arch::AArch64) ||
      Decoded->Segments.size() != Out.Regions.size() ||
      Decoded->Imports.size() != Out.Imports.size())
    return failure(text::LoaderDisagreement);
  for (size_t I = 0; I < R.Sections.size(); ++I)
    if (Decoded->Segments[I].VA != Out.Regions[I].Address ||
        Decoded->Segments[I].Size != R.Sections[I].Header.VirtualSize)
      return failure(text::LoaderDisagreement);
  for (const auto &Import : Out.Imports)
    if (std::none_of(Decoded->Imports.begin(), Decoded->Imports.end(),
                     [&](const auto &I) {
                       return I.IATAddr == Import.Slot &&
                              (Import.Ordinal ? I.Ordinal == *Import.Ordinal
                                              : I.Name == Import.Name) &&
                              llvm::StringRef(I.Module).lower() ==
                                  Import.Module;
                     }))
      return failure(text::LoaderDisagreement);
  ImageRegion Headers{Base, Read | UserAccessible,
                      std::vector<uint8_t>(pages(PE.SizeOfHeaders))};
  std::copy_n(R.Raw.begin(), PE.SizeOfHeaders, Headers.Bytes.begin());
  Out.Regions.insert(Out.Regions.begin(), std::move(Headers));
  return Out;
}
} // namespace
llvm::Expected<Image> loadImage(const std::filesystem::path &Path,
                                uint64_t MemoryLimit) {
  ImageReadBudget Budget{MemoryLimit, MemoryLimit};
  return readImage(Path, Budget, false, false);
}
llvm::Expected<Image> loadProgramImage(const std::filesystem::path &Path,
                                       ImageReadBudget &Budget, bool DLL) {
  return readImage(Path, Budget, DLL, true);
}
} // namespace neverd::emulation::windows_process
