//===- ELFProgramMetadata.cpp - Decode original PT_DYNAMIC records -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/loader/ELF/ELFProgramMetadata.h"

#include "neverd/loader/BinaryImageModel.h"

#include "llvm/Object/ELFTypes.h"

#include <cstring>

namespace neverd {
namespace {
#define NEVERD_ELF_PROGRAM_DIAGNOSTIC(Name, Text) constexpr char Name[] = Text;
#include "ELFProgramMetadata.def"
#undef NEVERD_ELF_PROGRAM_DIAGNOSTIC
llvm::Error failure(const char *Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
template <typename ELFT>
llvm::Expected<std::vector<ELFDynamicEntry>>
readTable(const BinaryImage &Image, const ELFProgramHeader &Header) {
  using Dyn = typename ELFT::Dyn;
  if (!Header.FileSize || Header.FileSize % sizeof(Dyn) ||
      Header.FileSize > Header.MemorySize ||
      Header.FileOffset > Image.Raw.size() ||
      Header.FileSize > Image.Raw.size() - Header.FileOffset)
    return failure(TableDiagnostic);
  std::vector<ELFDynamicEntry> Entries;
  for (uint64_t Offset = 0; Offset < Header.FileSize; Offset += sizeof(Dyn)) {
    Dyn Entry;
    std::memcpy(&Entry, Image.Raw.data() + Header.FileOffset + Offset,
                sizeof(Entry));
    if (Entry.d_tag == llvm::ELF::DT_NULL)
      return Entries;
    Entries.push_back({Entry.d_tag, Entry.d_un.d_val});
  }
  return failure(TableDiagnostic);
}
} // namespace

llvm::Expected<std::vector<ELFDynamicEntry>>
readELFProgramDynamicTable(const BinaryImage &Image) {
  if (!Image.isELF() || !Image.ELFMetadata ||
      (Image.Bits != Bitness::Bits32 && Image.Bits != Bitness::Bits64) ||
      Image.Raw.size() < llvm::ELF::EI_NIDENT ||
      (Image.Raw[llvm::ELF::EI_DATA] != llvm::ELF::ELFDATA2LSB &&
       Image.Raw[llvm::ELF::EI_DATA] != llvm::ELF::ELFDATA2MSB))
    return failure(ImageDiagnostic);
  const ELFProgramHeader *Table = nullptr;
  for (const auto &Header : Image.ELFMetadata->ProgramHeaders) {
    if (Header.Type != llvm::ELF::PT_DYNAMIC)
      continue;
    if (Table)
      return failure(TableDiagnostic);
    Table = &Header;
  }
  if (!Table)
    return std::vector<ELFDynamicEntry>();
  if (Image.Raw[llvm::ELF::EI_DATA] == llvm::ELF::ELFDATA2MSB)
    return Image.Bits == Bitness::Bits64
               ? readTable<llvm::object::ELF64BE>(Image, *Table)
               : readTable<llvm::object::ELF32BE>(Image, *Table);
  return Image.Bits == Bitness::Bits64
             ? readTable<llvm::object::ELF64LE>(Image, *Table)
             : readTable<llvm::object::ELF32LE>(Image, *Table);
}
} // namespace neverd
