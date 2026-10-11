//===- PERuntime.cpp - Append a linked native runtime to a PE image
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "PEImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <cstring>

namespace neverd::unpack::pe {
namespace {
using namespace llvm::object;
using namespace llvm::support::endian;

template <class T> llvm::Expected<T> record(const Image &I, uint64_t RVA) {
  const auto Bytes = I.fileBytesFrom(RVA);
  if (Bytes.size() < sizeof(T))
    return failure("runtime metadata is not file backed");
  T Value;
  std::memcpy(&Value, Bytes.data(), sizeof(T));
  return Value;
}
template <class T> void put(std::vector<uint8_t> &B, uint64_t At, const T &V) {
  std::memcpy(B.data() + At, &V, sizeof(V));
}
llvm::Expected<std::vector<coff_import_directory_table_entry>>
imports(const Image &I) {
  std::vector<coff_import_directory_table_entry> Result;
  const auto D = I.directory(llvm::COFF::IMPORT_TABLE);
  if (!D.RelativeVirtualAddress && !D.Size)
    return Result;
  for (uint64_t At = 0;
       At + sizeof(coff_import_directory_table_entry) <= D.Size;
       At += sizeof(coff_import_directory_table_entry)) {
    auto R = record<coff_import_directory_table_entry>(
        I, uint64_t(D.RelativeVirtualAddress) + At);
    if (!R)
      return R.takeError();
    if (!R->ImportLookupTableRVA && !R->TimeDateStamp && !R->ForwarderChain &&
        !R->NameRVA && !R->ImportAddressTableRVA)
      return Result;
    if (!R->ImportLookupTableRVA || !R->ImportAddressTableRVA || !R->NameRVA)
      return failure("runtime import descriptor has no complete ownership");
    Result.push_back(*R);
  }
  return failure("runtime import descriptors are unterminated");
}
llvm::Expected<uint64_t> exported(const Image &I, llvm::StringRef Name) {
  llvm::MemoryBufferRef Buffer(
      llvm::StringRef(reinterpret_cast<const char *>(I.file().data()),
                      I.file().size()),
      "native runtime");
  auto Object = COFFObjectFile::create(Buffer);
  if (!Object)
    return Object.takeError();
  for (const auto &Export : (*Object)->export_directories()) {
    llvm::StringRef Candidate;
    if (auto E = Export.getSymbolName(Candidate))
      return std::move(E);
    if (Candidate != Name)
      continue;
    uint32_t RVA;
    if (auto E = Export.getExportRVA(RVA))
      return std::move(E);
    if (!I.regionAt(RVA))
      return failure("native runtime entry is outside its sections");
    return RVA;
  }
  return failure("native runtime entry export is missing");
}
} // namespace

llvm::Expected<RebuiltImage> appendRuntime(RebuiltImage Out,
                                           llvm::ArrayRef<uint8_t> Runtime) {
  auto Parsed = Image::read(Out.File), Linked = Image::read(Runtime);
  if (!Parsed)
    return Parsed.takeError();
  if (!Linked)
    return Linked.takeError();
  const auto &Main = **Parsed, &Code = **Linked;
  if (Main.architecture() != emulation::GuestArchitecture::X64 ||
      Code.architecture() != Main.architecture() ||
      Code.preferredBase() < Main.preferredBase())
    return failure("native runtime requires matching x64 PE images");
  const uint64_t Bias = Code.preferredBase() - Main.preferredBase();
  if (Bias < Main.extent() || Bias > UINT32_MAX ||
      Code.extent() > UINT32_MAX - Bias)
    return failure("native runtime placement overlaps or exceeds the image");
  const auto &H = Main.headers();
  if (H.Directories.size() <= llvm::COFF::IAT ||
      Out.Sections.size() != Main.regions().size())
    return failure("native runtime requires complete rebuilt PE metadata");
  if (Code.headers().SectionAlignment != H.SectionAlignment ||
      Bias % H.SectionAlignment)
    return failure("native runtime requires matching PE section alignment");
  // The separately linked image needs a 64 KiB image base and reserves its
  // own header page. Cover that gap explicitly: Windows rejects an image
  // whose section RVAs are not adjacent, even though Wine can map it.
  const uint64_t Padding = Bias + Code.regions().front().RVA - Main.extent();
  const uint64_t Count =
      Main.regions().size() + Code.regions().size() + 1 + (Padding != 0);
  // Existing headers and file-only debug payloads keep their file offsets.
  // Refuse an image without room rather than overwrite a source section.
  if (Count > value::MaxSections || H.SectionTableOffset > H.SizeOfHeaders ||
      Count > (H.SizeOfHeaders - H.SectionTableOffset) / sizeof(coff_section))
    return failure("native runtime needs more PE section-header capacity");
  auto Initializer = exported(Code, "restore");
  coff_file_header OriginalHeader;
  std::memcpy(&OriginalHeader, Main.file().data() + H.FileHeaderOffset,
              sizeof(OriginalHeader));
  auto Entry = exported(
      Code, (OriginalHeader.Characteristics & llvm::COFF::IMAGE_FILE_DLL)
                ? "library_entry"
                : "program_entry");
  if (!Initializer)
    return Initializer.takeError();
  if (!Entry)
    return Entry.takeError();
  auto MainImports = imports(Main), CodeImports = imports(Code);
  if (!MainImports)
    return MainImports.takeError();
  if (!CodeImports)
    return CodeImports.takeError();
  std::vector<uint8_t> Mapped(Code.extent(), 0);
  for (const auto &R : Code.regions())
    std::copy_n(Code.file().data() + R.FileOffset, R.FileSize,
                Mapped.data() + R.RVA);
  for (auto R : *CodeImports) {
    const uint64_t ILT = R.ImportLookupTableRVA, IAT = R.ImportAddressTableRVA;
    uint64_t I = 0;
    for (;; ++I) {
      auto Item = record<llvm::support::ulittle64_t>(Code, ILT + I * 8);
      if (!Item)
        return Item.takeError();
      if (IAT > Mapped.size() || I * 8 + 8 > Mapped.size() - IAT)
        return failure("native runtime IAT exceeds its backing");
      // The original image keeps its own IAT protection range. The runtime
      // uses writable import cells so the native loader can bind both tables
      // without making intervening code/data pages part of that range.
      const auto *Region = Code.regionAt(IAT + I * 8);
      if (!Region || IAT + I * 8 + 8 > Region->RVA + Region->MemorySize)
        return failure("native runtime IAT crosses its section");
      const auto Access = Code.headers()
                              .Sections[Region - Code.regions().data()]
                              .Characteristics;
      if (!(Access & llvm::COFF::IMAGE_SCN_MEM_WRITE) ||
          (Access & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
        return failure("native runtime IAT requires writable data");
      uint64_t Value = *Item;
      if (!Value)
        break;
      if (!(Value & (1ull << 63))) {
        if (Value > UINT32_MAX - Bias)
          return failure("native runtime import name exceeds RVA width");
        Value += Bias;
      }
      write64le(Mapped.data() + ILT + I * 8, Value);
      write64le(Mapped.data() + IAT + I * 8, Value);
    }
    R.ImportLookupTableRVA = uint64_t(R.ImportLookupTableRVA) + Bias;
    R.NameRVA = uint64_t(R.NameRVA) + Bias;
    R.ImportAddressTableRVA = uint64_t(R.ImportAddressTableRVA) + Bias;
    MainImports->push_back(R);
  }
  MainImports->emplace_back();
  const uint64_t MetadataRVA = Bias + Code.extent();
  std::vector<uint8_t> Metadata;
  auto Append = [&](llvm::ArrayRef<uint8_t> Bytes, uint64_t Alignment = 8) {
    const uint64_t At = llvm::alignTo(Metadata.size(), Alignment);
    Metadata.resize(At + Bytes.size(), 0);
    std::copy(Bytes.begin(), Bytes.end(), Metadata.begin() + At);
    return MetadataRVA + At;
  };
  auto BytesOf = [](const auto &V) {
    return llvm::ArrayRef(reinterpret_cast<const uint8_t *>(V.data()),
                          V.size() * sizeof(V.front()));
  };
  const uint64_t ImportRVA = Append(BytesOf(*MainImports));
  coff_tls_directory64 TLS{};
  const auto ExistingTLS = Main.directory(llvm::COFF::TLS_TABLE);
  if (ExistingTLS.RelativeVirtualAddress) {
    auto Value =
        record<coff_tls_directory64>(Main, ExistingTLS.RelativeVirtualAddress);
    if (!Value)
      return Value.takeError();
    TLS = *Value;
  } else {
    const uint8_t Zero[8]{};
    TLS.AddressOfIndex = Main.preferredBase() + Append(Zero);
  }
  std::vector<llvm::support::ulittle64_t> Callbacks;
  Callbacks.emplace_back(Code.preferredBase() + *Initializer);
  if (TLS.AddressOfCallBacks) {
    if (TLS.AddressOfCallBacks < Main.preferredBase())
      return failure("runtime TLS callbacks precede the image");
    uint64_t At = TLS.AddressOfCallBacks - Main.preferredBase();
    for (uint64_t I = 0;; ++I) {
      if (I == 4096)
        return failure("runtime TLS callback limit exceeded");
      auto Value = record<llvm::support::ulittle64_t>(Main, At + I * 8);
      if (!Value)
        return Value.takeError();
      if (!*Value)
        break;
      Callbacks.push_back(*Value);
    }
  }
  Callbacks.emplace_back(0);
  TLS.AddressOfCallBacks = Main.preferredBase() + Append(BytesOf(Callbacks));
  const uint64_t TLSRVA = Append(
      llvm::ArrayRef(reinterpret_cast<const uint8_t *>(&TLS), sizeof(TLS)));
  struct RuntimeFunction {
    llvm::support::ulittle32_t Begin, End, Unwind;
  };
  std::vector<RuntimeFunction> Functions;
  for (auto [I, Delta] : {std::pair{&Main, uint64_t(0)}, {&Code, Bias}}) {
    const auto D = I->directory(llvm::COFF::EXCEPTION_TABLE);
    if (D.Size % sizeof(RuntimeFunction))
      return failure("runtime exception table has a partial record");
    for (uint64_t At = 0; At < D.Size; At += sizeof(RuntimeFunction)) {
      auto F =
          record<RuntimeFunction>(*I, uint64_t(D.RelativeVirtualAddress) + At);
      if (!F)
        return F.takeError();
      if (Delta) {
        const auto Info = I->fileBytesFrom(F->Unwind);
        if (Info.empty() || Info[0] != 1)
          return failure("compiler runtime has unsupported chained or handler "
                         "unwind data");
        if (F->Begin >= F->End || F->End > I->extent() ||
            F->Unwind >= I->extent())
          return failure("compiler runtime unwind range is invalid");
        F->Begin = uint64_t(F->Begin) + Delta;
        F->End = uint64_t(F->End) + Delta;
        F->Unwind = uint64_t(F->Unwind) + Delta;
      }
      Functions.push_back(*F);
    }
  }
  llvm::sort(Functions,
             [](const auto &A, const auto &B) { return A.Begin < B.Begin; });
  const uint64_t ExceptionRVA =
      Functions.empty() ? 0 : Append(BytesOf(Functions), 4);
  if (Metadata.size() > UINT32_MAX - MetadataRVA)
    return failure("native runtime metadata exceeds RVA width");
  std::vector<uint8_t> Output = Out.File;
  auto AddSection = [&](llvm::StringRef Name, uint64_t RVA, uint64_t Size,
                        uint32_t Flags, llvm::ArrayRef<uint8_t> Bytes) {
    const uint64_t Offset =
        llvm::alignTo(Output.size(), uint64_t(H.FileAlignment));
    const uint64_t RawSize =
        llvm::alignTo(Bytes.size(), uint64_t(H.FileAlignment));
    if (RawSize > UINT32_MAX || Offset > UINT32_MAX - RawSize)
      return false;
    coff_section S{};
    std::copy_n(Name.data(), std::min<size_t>(Name.size(), 8), S.Name);
    S.VirtualSize = Size;
    S.VirtualAddress = RVA;
    S.SizeOfRawData = RawSize;
    S.PointerToRawData = RawSize ? Offset : 0;
    S.Characteristics = Flags;
    put(Output, H.SectionTableOffset + Out.Sections.size() * sizeof(S), S);
    Output.resize(Offset + RawSize, 0);
    std::copy(Bytes.begin(), Bytes.end(), Output.begin() + Offset);
    Out.Sections.push_back({Name.take_front(8).str(), RVA, Size, RawSize, 0});
    return true;
  };
  if (Padding && !AddSection(".ndpad", Main.extent(), Padding,
                             llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA, {}))
    return failure("native runtime padding exceeds file-offset width");
  for (size_t I = 0; I < Code.regions().size(); ++I) {
    const auto &R = Code.regions()[I];
    const auto Flags = Code.headers().Sections[I].Characteristics;
    if (!AddSection(".nd" + R.Name, Bias + R.RVA, R.MemorySize, Flags,
                    llvm::ArrayRef(Mapped).slice(R.RVA, R.MemorySize)))
      return failure("native runtime file exceeds file-offset width");
  }
  if (!AddSection(".ndstate", MetadataRVA, Metadata.size(),
                  llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                      llvm::COFF::IMAGE_SCN_MEM_READ |
                      llvm::COFF::IMAGE_SCN_MEM_WRITE,
                  Metadata))
    return failure("native runtime metadata exceeds file-offset width");
  coff_file_header COFF;
  pe32plus_header PE;
  std::memcpy(&COFF, Output.data() + H.FileHeaderOffset, sizeof(COFF));
  std::memcpy(&PE, Output.data() + H.OptionalHeaderOffset, sizeof(PE));
  COFF.NumberOfSections = Count;
  PE.AddressOfEntryPoint = Bias + *Entry;
  Out.LoaderEntryRVA = PE.AddressOfEntryPoint;
  const uint64_t ImageSize = llvm::alignTo(MetadataRVA + Metadata.size(),
                                           uint64_t(H.SectionAlignment));
  if (ImageSize > UINT32_MAX)
    return failure("native runtime image exceeds PE32+ RVA width");
  PE.SizeOfImage = ImageSize;
  uint64_t CodeSize = 0, DataSize = 0, ZeroSize = 0;
  for (uint64_t I = 0; I < Count; ++I) {
    coff_section S;
    std::memcpy(&S, Output.data() + H.SectionTableOffset + I * sizeof(S),
                sizeof(S));
    if (S.Characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE)
      CodeSize += S.SizeOfRawData;
    if (S.Characteristics & llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA)
      DataSize += S.SizeOfRawData;
    if (S.Characteristics & llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA)
      ZeroSize += S.VirtualSize;
  }
  if (CodeSize > UINT32_MAX || DataSize > UINT32_MAX || ZeroSize > UINT32_MAX)
    return failure("native runtime section totals exceed PE32+ widths");
  PE.SizeOfCode = CodeSize;
  PE.SizeOfInitializedData = DataSize;
  PE.SizeOfUninitializedData = ZeroSize;
  PE.CheckSum = 0;
  put(Output, H.FileHeaderOffset, COFF);
  put(Output, H.OptionalHeaderOffset, PE);
  auto Directory = [&](unsigned Index, uint64_t RVA, uint64_t Size) {
    data_directory D{};
    D.RelativeVirtualAddress = RVA;
    D.Size = Size;
    put(Output, H.OptionalHeaderOffset + sizeof(PE) + Index * sizeof(D), D);
  };
  Directory(llvm::COFF::IMPORT_TABLE, ImportRVA,
            MainImports->size() * sizeof(MainImports->front()));
  Directory(llvm::COFF::EXCEPTION_TABLE, ExceptionRVA,
            Functions.size() * sizeof(RuntimeFunction));
  Directory(llvm::COFF::TLS_TABLE, TLSRVA, sizeof(TLS));
  Out.File = std::move(Output);
  auto Checked = Image::read(Out.File);
  if (!Checked)
    return Checked.takeError();
  return Out;
}
} // namespace neverd::unpack::pe
