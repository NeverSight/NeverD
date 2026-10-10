//===- KernelModuleImages.cpp - Readable modeled kernel PE exports --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModuleImages.h"

#include "KernelExportRegistry.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace neverd::emulation {
llvm::Expected<std::vector<KernelModuleImage>>
makeKernelModuleImages(const KernelExportRegistry &Registry) {
  using namespace llvm::object;
  using namespace llvm::support::endian;
  using namespace profile;
  using Export = KernelExportRegistry::Export;
  std::map<uint64_t, std::vector<const Export *>> Groups;
  for (const auto &[Address, Entry] : Registry.entries()) {
    if (Entry.Kind != KernelExportRegistry::ExportKind::ModuleExport)
      continue;
    const auto Base = KernelExportRegistry::moduleBase(Entry.Module);
    if (!Base || Address < *Base + KernelModuleCodeRVA ||
        Address >= *Base + KernelModuleExportRVA)
      return llvm::createStringError("kernel export is outside its module");
    Groups[*Base].push_back(&Entry);
  }
  std::vector<KernelModuleImage> Images;
  for (auto &[Base, Entries] : Groups) {
    std::sort(
        Entries.begin(), Entries.end(),
        [](const Export *A, const Export *B) { return A->Name < B->Name; });
    const uint64_t Count = Entries.size();
    uint64_t Size = KernelModuleExportRVA +
                    sizeof(export_directory_table_entry) + Count * 10 +
                    Entries.front()->Module.size() + 1;
    for (const auto *E : Entries)
      Size += E->Name.size() + 1;
    Size = llvm::alignTo(Size, PageSize);
    if (Size > KernelModuleStride || Count > UINT16_MAX)
      return llvm::createStringError("kernel module metadata exceeds its span");
    KernelModuleImage Image{{Entries.front()->Module, Base, Size},
                            std::vector<uint8_t>(Size)};
    auto &Bytes = Image.Bytes;
    auto Store = [&](uint64_t At, const auto &Record) {
      std::memcpy(Bytes.data() + At, &Record, sizeof(Record));
    };
    // A conventional mapped x64 PE. It describes the synthetic provider's
    // actual export identities, never host kernel addresses or native code.
    write16le(Bytes.data(), 0x5a4d);
    write32le(Bytes.data() + 0x3c, 0x80);
    write32le(Bytes.data() + 0x80, 0x4550);
    coff_file_header COFF{};
    COFF.Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
    COFF.NumberOfSections = 2;
    COFF.SizeOfOptionalHeader =
        sizeof(pe32plus_header) + 16 * sizeof(data_directory);
    COFF.Characteristics = llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE |
                           llvm::COFF::IMAGE_FILE_DLL |
                           llvm::COFF::IMAGE_FILE_LARGE_ADDRESS_AWARE;
    Store(0x84, COFF);
    pe32plus_header PE{};
    PE.Magic = llvm::COFF::PE32Header::PE32_PLUS;
    PE.ImageBase = Base;
    PE.SectionAlignment = PageSize;
    PE.FileAlignment = PageSize;
    PE.SizeOfImage = Size;
    PE.SizeOfHeaders = PageSize;
    PE.SizeOfCode = ThunkSize;
    PE.SizeOfInitializedData = Size - KernelModuleExportRVA;
    PE.BaseOfCode = KernelModuleCodeRVA;
    PE.Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_NATIVE;
    PE.NumberOfRvaAndSize = 16;
    Store(0x98, PE);
    uint64_t Header = 0x98 + sizeof(PE);
    data_directory Directory{};
    Directory.RelativeVirtualAddress = KernelModuleExportRVA;
    Directory.Size = Size - KernelModuleExportRVA;
    Store(Header, Directory);
    Header += 16 * sizeof(Directory);
    for (unsigned I = 0; I < 2; ++I) {
      coff_section Section{};
      const char *Name = I ? ".edata" : ".text";
      std::memcpy(Section.Name, Name, std::strlen(Name));
      Section.VirtualAddress = I ? KernelModuleExportRVA : KernelModuleCodeRVA;
      Section.VirtualSize = I ? Size - KernelModuleExportRVA : ThunkSize;
      Section.PointerToRawData = Section.VirtualAddress;
      Section.SizeOfRawData = Section.VirtualSize;
      Section.Characteristics = llvm::COFF::IMAGE_SCN_MEM_READ |
                                (I ? llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA
                                   : llvm::COFF::IMAGE_SCN_CNT_CODE |
                                         llvm::COFF::IMAGE_SCN_MEM_EXECUTE);
      Store(Header, Section);
      Header += sizeof(Section);
    }
    std::fill(Bytes.begin() + KernelModuleCodeRVA,
              Bytes.begin() + KernelModuleExportRVA, ThunkTrapByte);
    export_directory_table_entry Exports{};
    Exports.OrdinalBase = 1;
    Exports.AddressTableEntries = Count;
    Exports.NumberOfNamePointers = Count;
    uint64_t Cursor = KernelModuleExportRVA + sizeof(Exports);
    Exports.ExportAddressTableRVA = Cursor;
    Cursor += Count * 4;
    Exports.NamePointerRVA = Cursor;
    Cursor += Count * 4;
    Exports.OrdinalTableRVA = Cursor;
    Cursor += Count * 2;
    const auto String = [&](llvm::StringRef Text) {
      const uint32_t At = Cursor;
      std::copy(Text.begin(), Text.end(), Bytes.begin() + Cursor);
      Cursor += Text.size() + 1;
      return At;
    };
    Exports.NameRVA = String(Image.Identity.Name);
    for (uint64_t I = 0; I < Count; ++I) {
      write32le(Bytes.data() + Exports.ExportAddressTableRVA + I * 4,
                Entries[I]->Address - Base);
      write32le(Bytes.data() + Exports.NamePointerRVA + I * 4,
                String(Entries[I]->Name));
      write16le(Bytes.data() + Exports.OrdinalTableRVA + I * 2, I);
    }
    Store(KernelModuleExportRVA, Exports);
    Images.push_back(std::move(Image));
  }
  return Images;
}
} // namespace neverd::emulation
