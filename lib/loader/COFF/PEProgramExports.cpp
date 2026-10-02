//===- PEProgramExports.cpp - Bounded original PE export decoding --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/loader/COFF/PEProgramExports.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>

namespace neverd {
namespace {
using namespace llvm::object;
namespace text {
#define NEVERD_PE_EXPORT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/loader/COFF/PEProgramExports.def"
#undef NEVERD_PE_EXPORT_TEXT
} // namespace text
llvm::Error invalid(llvm::StringRef Reason) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 llvm::Twine(text::Prefix) + Reason);
}
struct Section {
  uint64_t RVA, VirtualSize, Offset, FileSize;
};
class Reader {
  llvm::ArrayRef<uint8_t> File;
  const PEExportLimits &Limits;
  uint64_t Bytes = 0, Records = 0;
  uint64_t Headers = 0;
  std::vector<Section> Sections;

  template <class T> llvm::Expected<T> fileRecord(uint64_t Offset) {
    if (Offset > File.size() || sizeof(T) > File.size() - Offset)
      return invalid(text::Header);
    if (auto E = charge(sizeof(T), 1))
      return std::move(E);
    T V;
    std::memcpy(&V, File.data() + Offset, sizeof(V));
    return V;
  }
  template <class T>
  llvm::Expected<uint64_t> optionalHeader(uint64_t Offset, uint64_t Size) {
    if (Size < sizeof(T))
      return invalid(text::Header);
    auto Header = fileRecord<T>(Offset);
    if (!Header)
      return Header.takeError();
    Headers = Header->SizeOfHeaders;
    ImageSize = Header->SizeOfImage;
    if (Header->NumberOfRvaAndSize >
        (Size - sizeof(T)) / sizeof(data_directory))
      return invalid(text::Header);
    if (Header->NumberOfRvaAndSize > llvm::COFF::EXPORT_TABLE) {
      auto D = fileRecord<data_directory>(Offset + sizeof(T) +
                                          llvm::COFF::EXPORT_TABLE *
                                              sizeof(data_directory));
      if (!D)
        return D.takeError();
      Directory = *D;
    }
    return Offset + Size;
  }

public:
  std::vector<PEMetadataRange> Metadata;
  uint64_t ImageSize = 0;
  void finish(PEProgramExports &Out) {
    Out.Metadata = std::move(Metadata);
    Out.BytesRead = Bytes;
    Out.RecordsRead = Records;
  }
  data_directory Directory{};
  Reader(llvm::ArrayRef<uint8_t> File, const PEExportLimits &Limits)
      : File(File), Limits(Limits) {}

  llvm::Error charge(uint64_t Size, uint64_t Count = 0) {
    if (Size > Limits.Bytes - Bytes || Count > Limits.Records - Records)
      return invalid(text::Budget);
    Bytes += Size;
    Records += Count;
    return llvm::Error::success();
  }
  llvm::Error initialize() {
    auto DOS = fileRecord<dos_header>(0);
    if (!DOS)
      return DOS.takeError();
    if (std::memcmp(DOS->Magic, text::DOSMagic, sizeof(DOS->Magic)) ||
        DOS->AddressOfNewExeHeader < sizeof(dos_header))
      return invalid(text::Header);
    auto Signature = fileRecord<uint32_t>(DOS->AddressOfNewExeHeader);
    if (!Signature)
      return Signature.takeError();
    if (std::memcmp(&*Signature, llvm::COFF::PEMagic, sizeof(*Signature)))
      return invalid(text::Header);
    const uint64_t CoffOffset =
        uint64_t(DOS->AddressOfNewExeHeader) + sizeof(*Signature);
    auto COFF = fileRecord<coff_file_header>(CoffOffset);
    if (!COFF)
      return COFF.takeError();
    const uint64_t Offset = CoffOffset + sizeof(*COFF);
    const uint64_t Size = COFF->SizeOfOptionalHeader;
    if (!(COFF->Characteristics & llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE) ||
        Offset > File.size() || Size > File.size() - Offset)
      return invalid(text::Header);
    auto Magic = fileRecord<llvm::support::ulittle16_t>(Offset);
    if (!Magic)
      return Magic.takeError();
    auto End = [&]() -> llvm::Expected<uint64_t> {
      if (*Magic == llvm::COFF::PE32Header::PE32_PLUS)
        return optionalHeader<pe32plus_header>(Offset, Size);
      if (*Magic == llvm::COFF::PE32Header::PE32)
        return optionalHeader<pe32_header>(Offset, Size);
      return invalid(text::Header);
    }();
    if (!End)
      return End.takeError();
    if (!Headers || Headers > File.size() || Headers > ImageSize ||
        *End > Headers ||
        uint64_t(COFF->NumberOfSections) * sizeof(coff_section) >
            Headers - *End)
      return invalid(text::Header);
    std::vector<std::pair<uint64_t, uint64_t>> Virtual{{0, Headers}},
        Physical{{0, Headers}};
    for (unsigned I = 0; I < COFF->NumberOfSections; ++I) {
      auto H = fileRecord<coff_section>(*End + I * sizeof(coff_section));
      if (!H)
        return H.takeError();
      Section S{H->VirtualAddress, H->VirtualSize, H->PointerToRawData,
                H->SizeOfRawData};
      if (!S.VirtualSize)
        S.VirtualSize = S.FileSize;
      if (S.RVA > ImageSize || S.VirtualSize > ImageSize - S.RVA ||
          (S.FileSize &&
           (S.Offset > File.size() || S.FileSize > File.size() - S.Offset)))
        return invalid(text::Sections);
      if (S.VirtualSize)
        Virtual.emplace_back(S.RVA, S.RVA + S.VirtualSize);
      if (S.FileSize)
        Physical.emplace_back(S.Offset, S.Offset + S.FileSize);
      if (S.VirtualSize)
        Sections.push_back(S);
    }
    auto Disjoint = [](auto Ranges) {
      std::sort(Ranges.begin(), Ranges.end());
      for (size_t I = 1; I < Ranges.size(); ++I)
        if (Ranges[I].first < Ranges[I - 1].second)
          return false;
      return true;
    };
    if (!Disjoint(Virtual) || !Disjoint(Physical))
      return invalid(text::Sections);
    std::sort(Sections.begin(), Sections.end(),
              [](const auto &A, const auto &B) { return A.RVA < B.RVA; });
    return llvm::Error::success();
  }
  llvm::Expected<llvm::ArrayRef<uint8_t>> read(uint64_t RVA, uint64_t Size) {
    if (!Size || RVA >= ImageSize || Size > ImageSize - RVA)
      return invalid(text::Mapping);
    uint64_t Offset = RVA;
    if (RVA >= Headers || Size > Headers - RVA) {
      auto I = std::upper_bound(
          Sections.begin(), Sections.end(), RVA,
          [](uint64_t Address, const auto &S) { return Address < S.RVA; });
      if (I == Sections.begin())
        return invalid(text::Mapping);
      --I;
      const uint64_t Delta = RVA - I->RVA;
      if (Delta >= I->VirtualSize || Size > I->VirtualSize - Delta ||
          Delta >= I->FileSize || Size > I->FileSize - Delta)
        return invalid(text::Mapping);
      Offset = I->Offset + Delta;
    }
    if (auto E = charge(Size))
      return std::move(E);
    if (!Metadata.empty() && RVA >= Metadata.back().RVA &&
        RVA <= Metadata.back().RVA + Metadata.back().Size)
      Metadata.back().Size =
          std::max(Metadata.back().Size, RVA + Size - Metadata.back().RVA);
    else
      Metadata.push_back({RVA, Size});
    return File.slice(Offset, Size);
  }
  llvm::Expected<std::string> string(uint64_t RVA, uint64_t End) {
    std::string Result;
    for (uint64_t I = 0; I < Limits.StringBytes && RVA + I < End; ++I) {
      auto B = read(RVA + I, 1);
      if (!B)
        return B.takeError();
      if (!B->front()) {
        if (Result.empty())
          return invalid(text::Name);
        return Result;
      }
      Result += static_cast<char>(B->front());
    }
    return invalid(text::Name);
  }
};
} // namespace

llvm::Expected<PEProgramExports>
readPEProgramExports(llvm::ArrayRef<uint8_t> File,
                     const PEExportLimits &Limits) {
  Reader R(File, Limits);
  if (auto E = R.initialize())
    return std::move(E);
  const uint64_t Begin = R.Directory.RelativeVirtualAddress;
  const uint64_t Size = R.Directory.Size;
  if (!Begin && !Size) {
    PEProgramExports Out;
    R.finish(Out);
    return Out;
  }
  if (!Begin || Size < sizeof(export_directory_table_entry))
    return invalid(text::Directory);
  auto Bytes = R.read(Begin, Size);
  if (!Bytes)
    return Bytes.takeError();
  export_directory_table_entry D;
  std::memcpy(&D, Bytes->data(), sizeof(D));
  const uint64_t Count = D.AddressTableEntries, Names = D.NumberOfNamePointers;
  if (D.ExportFlags || !D.NameRVA ||
      Count > uint64_t(UINT32_MAX) + 1 - D.OrdinalBase || (!Count && Names) ||
      (Count && !D.ExportAddressTableRVA) ||
      (Names && (!D.NamePointerRVA || !D.OrdinalTableRVA)))
    return invalid(text::Directory);
  if (auto E = R.charge(0, Count + Names))
    return std::move(E);
  auto Module = R.string(D.NameRVA, R.ImageSize);
  if (!Module)
    return Module.takeError();
  PEProgramExports Out;
  Out.Module = std::move(*Module);
  if (!Count) {
    R.finish(Out);
    return Out;
  }
  auto Addresses = R.read(D.ExportAddressTableRVA, Count * sizeof(uint32_t));
  if (!Addresses)
    return Addresses.takeError();
  Out.Entries.reserve(Count);
  for (uint64_t I = 0; I < Count; ++I) {
    const uint32_t RVA = llvm::support::endian::read32le(Addresses->data() +
                                                         I * sizeof(uint32_t));
    const bool Forwarded = RVA >= Begin && RVA - Begin < Size;
    if (RVA >= R.ImageSize)
      return invalid(text::Target);
    PEProgramExport E{uint32_t(D.OrdinalBase + I),
                      RVA,
                      !RVA        ? PEExportKind::Hole
                      : Forwarded ? PEExportKind::Forwarder
                                  : PEExportKind::Address,
                      {},
                      {}};
    if (Forwarded) {
      auto Name = R.string(RVA, Begin + Size);
      if (!Name)
        return Name.takeError();
      E.Forwarder = std::move(*Name);
    }
    Out.Entries.push_back(std::move(E));
  }
  if (!Names) {
    R.finish(Out);
    return Out;
  }
  auto Pointers = R.read(D.NamePointerRVA, Names * sizeof(uint32_t));
  if (!Pointers)
    return Pointers.takeError();
  auto Ordinals = R.read(D.OrdinalTableRVA, Names * sizeof(uint16_t));
  if (!Ordinals)
    return Ordinals.takeError();
  std::string Previous;
  for (uint64_t I = 0; I < Names; ++I) {
    const auto Ordinal = llvm::support::endian::read16le(Ordinals->data() +
                                                         I * sizeof(uint16_t));
    if (Ordinal >= Count)
      return invalid(text::Ordinal);
    auto Name = R.string(llvm::support::endian::read32le(Pointers->data() +
                                                         I * sizeof(uint32_t)),
                         R.ImageSize);
    if (!Name)
      return Name.takeError();
    if (I && llvm::StringRef(Previous).compare(*Name) >= 0)
      return invalid(text::Order);
    Previous = *Name;
    Out.Entries[Ordinal].Names.push_back(std::move(*Name));
  }
  R.finish(Out);
  return Out;
}
} // namespace neverd
