//===- DebugInfoDiscoveryTests.cpp - Debug symbol precedence -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pins the order in which NeverD believes competing sources of a function
/// name, and the search that decides which debug file an image is analyzed
/// with.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/debug/DWARFLoader.h"
#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Loader.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace neverd;

namespace {

/// A DebugContext holding exactly the functions a test hands it, so the
/// precedence rules can be exercised without a real DWARF/PDB/MAP file.
class FakeDebugContext : public DebugContext {
public:
  explicit FakeDebugContext(std::vector<FunctionSym> Funcs,
                            std::vector<DataObjectSym> Objects = {},
                            bool AuthenticatedObjects = false)
      : Functions(std::move(Funcs)), Objects(std::move(Objects)),
        AuthenticatedObjects(AuthenticatedObjects) {}

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
    for (const FunctionSym &FS : Functions)
      if (FS.Addr == Addr)
        return FS;
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return Functions; }
  std::vector<DataObjectSym> allDataObjects() const override { return Objects; }
  bool hasAuthenticatedObjectExtents() const override {
    return AuthenticatedObjects;
  }
  bool hasInfo() const override {
    return !Functions.empty() || !Objects.empty();
  }

private:
  std::vector<FunctionSym> Functions;
  std::vector<DataObjectSym> Objects;
  bool AuthenticatedObjects = false;
};

FunctionSym makeDebugFunc(va_t Addr, llvm::StringRef Name, uint64_t Size = 0) {
  FunctionSym FS;
  FS.Addr = Addr;
  FS.Name = Name.str();
  FS.Size = Size;
  return FS;
}

Symbol makeNamedFunc(va_t Addr, llvm::StringRef Name, uint64_t Size = 0) {
  Symbol S;
  S.Addr = Addr;
  S.Name = Name.str();
  S.Size = Size;
  S.IsFunc = true;
  return S;
}

const Symbol *findFunc(const BinaryImage &Img, va_t Addr) {
  for (const Symbol &S : Img.Symbols)
    if (S.IsFunc && S.Addr == Addr)
      return &S;
  return nullptr;
}

/// A scratch directory that removes itself, so a discovery test can lay out
/// the companion files an image is expected to be found next to.
class ScratchDir {
public:
  ScratchDir() {
    llvm::SmallString<128> Path;
    llvm::sys::fs::createUniqueDirectory("neverd-debug-discovery", Path);
    Root = std::filesystem::path(Path.str().str());
  }
  ~ScratchDir() {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }

  std::filesystem::path write(llvm::StringRef Name, llvm::StringRef Content) {
    std::filesystem::path P = Root / Name.str();
    std::ofstream OS(P);
    OS << Content.str();
    return P;
  }

  std::filesystem::path writeBytes(llvm::StringRef Name,
                                   llvm::ArrayRef<uint8_t> Bytes) {
    std::filesystem::path P = Root / Name.str();
    std::ofstream OS(P, std::ios::binary);
    OS.write(reinterpret_cast<const char *>(Bytes.data()),
             static_cast<std::streamsize>(Bytes.size()));
    return P;
  }

  std::filesystem::path path(llvm::StringRef Name) const {
    return Root / Name.str();
  }

private:
  std::filesystem::path Root;
};

/// A link.exe /MAP naming two functions in a PE whose image base is
/// 0x140000000.
constexpr llvm::StringLiteral kMSVCMap =
    " probe\n"
    "\n"
    " Preferred load address is 0000000140000000\n"
    "\n"
    " Start         Length     Name                   Class\n"
    " 0001:00000000 00001000H .text                   CODE\n"
    "\n"
    "  Address         Publics by Value        Rva+Base       Lib:Object\n"
    "\n"
    " 0001:00001000       parse_header            0000140001000 f   probe.obj\n"
    " 0001:00001040       emit_record             0000140001040 f   probe.obj\n"
    "\n"
    " entry point at        0001:00001000\n";

/// An lld-link /lldmap whose object file sits in a dot-prefixed build
/// directory, with one symbol in code and one in read-only data.
constexpr llvm::StringLiteral kCOFFLLDMap =
    "Address  Size     Align Out     In      Symbol\n"
    "00001000 00000080  4096 .text\n"
    "00001000 00000080    16         .build/probe.obj:(.text)\n"
    "00001000 00000000     0                 parse_header\n"
    "00001040 00000000     0                 emit_record\n"
    "00002000 00000200  4096 .rdata\n"
    "00002000 00000008     1         .build/probe.obj:(.rdata)\n"
    "00002000 00000000     0                 lookup_table\n";

constexpr llvm::StringLiteral kELFLLDMap =
    "VMA LMA Size Align Out In Symbol\n"
    "00001000 00001000 00000080 16 .text\n"
    "00001000 00001000 00000080 16 .build/probe.o:(.text)\n"
    "00001000 00001000 00000020 1 parse_header\n"
    "00001040 00001040 00000020 1 emit_record\n"
    "00002000 00002000 00000200 16 .rodata\n"
    "00002000 00002000 00000008 1 .build/probe.o:(.rodata)\n"
    "00002000 00002000 00000008 1 lookup_table\n";

BinaryImage makePEImage() {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Base = 0x140000000;
  return Img;
}

std::filesystem::path safetyFixture(llvm::StringRef Name) {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "safety" / "fixtures" / "binaries" / Name.str();
}

std::vector<uint8_t> makeELFObjectExtentFixture() {
  using ELFT = llvm::object::ELF64LE;
  using Elf_Ehdr = ELFT::Ehdr;
  using Elf_Shdr = ELFT::Shdr;
  using Elf_Sym = ELFT::Sym;
  using namespace llvm::ELF;

  std::string SectionNames(1, '\0');
  const auto appendName = [](std::string &Table, llvm::StringRef Name) {
    const uint32_t Offset = static_cast<uint32_t>(Table.size());
    Table.append(Name.data(), Name.size());
    Table.push_back('\0');
    return Offset;
  };
  const uint32_t DataName = appendName(SectionNames, ".data");
  const uint32_t StrtabName = appendName(SectionNames, ".strtab");
  const uint32_t SymtabName = appendName(SectionNames, ".symtab");
  const uint32_t ShstrtabName = appendName(SectionNames, ".shstrtab");

  std::string SymbolNames(1, '\0');
  const uint32_t ObjectName = appendName(SymbolNames, "object");
  const uint32_t NotypeName = appendName(SymbolNames, "notype");
  const uint32_t ZeroName = appendName(SymbolNames, "zero");
  const uint32_t OutsideName = appendName(SymbolNames, "outside");

  constexpr size_t SectionCount = 5;
  constexpr size_t SymbolCount = 5;
  constexpr size_t DataSection = 1;
  constexpr size_t StringTableSection = 2;
  constexpr size_t SymbolTableSection = 3;
  constexpr size_t SectionNameTableSection = 4;
  constexpr size_t DataSize = 16;

  const size_t DataOffset = llvm::alignTo(sizeof(Elf_Ehdr), 8);
  const size_t StringTableOffset = DataOffset + DataSize;
  const size_t SymbolTableOffset =
      llvm::alignTo(StringTableOffset + SymbolNames.size(), alignof(Elf_Sym));
  const size_t SectionNameTableOffset =
      SymbolTableOffset + SymbolCount * sizeof(Elf_Sym);
  const size_t SectionTableOffset = llvm::alignTo(
      SectionNameTableOffset + SectionNames.size(), alignof(Elf_Shdr));
  std::vector<uint8_t> Bytes(
      SectionTableOffset + SectionCount * sizeof(Elf_Shdr), 0);

  Elf_Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_REL;
  Header.e_machine = EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_ehsize = sizeof(Elf_Ehdr);
  Header.e_shoff = SectionTableOffset;
  Header.e_shentsize = sizeof(Elf_Shdr);
  Header.e_shnum = SectionCount;
  Header.e_shstrndx = SectionNameTableSection;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + StringTableOffset, SymbolNames.data(),
              SymbolNames.size());
  std::memcpy(Bytes.data() + SectionNameTableOffset, SectionNames.data(),
              SectionNames.size());

  std::array<Elf_Sym, SymbolCount> Symbols{};
  Symbols[1].st_name = ObjectName;
  Symbols[1].setBindingAndType(STB_GLOBAL, STT_OBJECT);
  Symbols[1].st_shndx = DataSection;
  Symbols[1].st_size = 8;
  Symbols[2].st_name = NotypeName;
  Symbols[2].setBindingAndType(STB_GLOBAL, STT_NOTYPE);
  Symbols[2].st_shndx = DataSection;
  Symbols[2].st_value = 8;
  Symbols[2].st_size = 4;
  Symbols[3].st_name = ZeroName;
  Symbols[3].setBindingAndType(STB_GLOBAL, STT_OBJECT);
  Symbols[3].st_shndx = DataSection;
  Symbols[3].st_value = 8;
  Symbols[4].st_name = OutsideName;
  Symbols[4].setBindingAndType(STB_GLOBAL, STT_OBJECT);
  Symbols[4].st_shndx = DataSection;
  Symbols[4].st_value = 12;
  Symbols[4].st_size = 8;
  std::memcpy(Bytes.data() + SymbolTableOffset, Symbols.data(),
              sizeof(Symbols));

  std::array<Elf_Shdr, SectionCount> Sections{};
  Sections[DataSection].sh_name = DataName;
  Sections[DataSection].sh_type = SHT_PROGBITS;
  Sections[DataSection].sh_flags = SHF_ALLOC | SHF_WRITE;
  Sections[DataSection].sh_offset = DataOffset;
  Sections[DataSection].sh_size = DataSize;
  Sections[DataSection].sh_addralign = 8;
  Sections[StringTableSection].sh_name = StrtabName;
  Sections[StringTableSection].sh_type = SHT_STRTAB;
  Sections[StringTableSection].sh_offset = StringTableOffset;
  Sections[StringTableSection].sh_size = SymbolNames.size();
  Sections[StringTableSection].sh_addralign = 1;
  Sections[SymbolTableSection].sh_name = SymtabName;
  Sections[SymbolTableSection].sh_type = SHT_SYMTAB;
  Sections[SymbolTableSection].sh_offset = SymbolTableOffset;
  Sections[SymbolTableSection].sh_size = sizeof(Symbols);
  Sections[SymbolTableSection].sh_link = StringTableSection;
  Sections[SymbolTableSection].sh_info = 1;
  Sections[SymbolTableSection].sh_addralign = alignof(Elf_Sym);
  Sections[SymbolTableSection].sh_entsize = sizeof(Elf_Sym);
  Sections[SectionNameTableSection].sh_name = ShstrtabName;
  Sections[SectionNameTableSection].sh_type = SHT_STRTAB;
  Sections[SectionNameTableSection].sh_offset = SectionNameTableOffset;
  Sections[SectionNameTableSection].sh_size = SectionNames.size();
  Sections[SectionNameTableSection].sh_addralign = 1;
  std::memcpy(Bytes.data() + SectionTableOffset, Sections.data(),
              sizeof(Sections));
  return Bytes;
}

enum class DWARFReturnFixtureKind {
  PointerSizeAbsent,
  PointerSizeExplicit,
  PointerSizeZero,
  PointerSizeMalformed,
  PointerSizeOverflow,
  PointerSizeDuplicate,
  PointerTrailingTruncatedAttribute,
  DuplicateType,
  DuplicateSpecification,
  DuplicateAbstractOrigin,
  DuplicateBaseEncoding,
  DuplicateWrapperType,
};

void appendULEB(std::vector<uint8_t> &Bytes, uint64_t Value) {
  do {
    uint8_t Byte = static_cast<uint8_t>(Value & 0x7f);
    Value >>= 7;
    if (Value != 0)
      Byte |= 0x80;
    Bytes.push_back(Byte);
  } while (Value != 0);
}

void appendU16LE(std::vector<uint8_t> &Bytes, uint16_t Value) {
  Bytes.push_back(static_cast<uint8_t>(Value));
  Bytes.push_back(static_cast<uint8_t>(Value >> 8));
}

void appendU32LE(std::vector<uint8_t> &Bytes, uint32_t Value) {
  for (unsigned Shift = 0; Shift != 32; Shift += 8)
    Bytes.push_back(static_cast<uint8_t>(Value >> Shift));
}

void appendU64LE(std::vector<uint8_t> &Bytes, uint64_t Value) {
  for (unsigned Shift = 0; Shift != 64; Shift += 8)
    Bytes.push_back(static_cast<uint8_t>(Value >> Shift));
}

void patchU32LE(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  ASSERT_LE(Offset + sizeof(Value), Bytes.size());
  for (unsigned Shift = 0; Shift != 32; Shift += 8)
    Bytes[Offset + Shift / 8] = static_cast<uint8_t>(Value >> Shift);
}

void appendAttributeSpec(std::vector<uint8_t> &Abbrev,
                         llvm::dwarf::Attribute Attribute,
                         llvm::dwarf::Form Form) {
  appendULEB(Abbrev, Attribute);
  appendULEB(Abbrev, Form);
}

std::vector<uint8_t> makeELFDWARFReturnFixture(DWARFReturnFixtureKind Kind) {
  using ELFT = llvm::object::ELF64LE;
  using Elf_Ehdr = ELFT::Ehdr;
  using Elf_Shdr = ELFT::Shdr;
  using namespace llvm::ELF;
  using namespace llvm::dwarf;

  const bool DuplicateType = Kind == DWARFReturnFixtureKind::DuplicateType;
  const bool DuplicateSpecification =
      Kind == DWARFReturnFixtureKind::DuplicateSpecification;
  const bool DuplicateOrigin =
      Kind == DWARFReturnFixtureKind::DuplicateAbstractOrigin;
  const bool UsesDeclaration = DuplicateSpecification || DuplicateOrigin;
  const bool BaseType = Kind == DWARFReturnFixtureKind::DuplicateBaseEncoding;
  const bool WrapperType = Kind == DWARFReturnFixtureKind::DuplicateWrapperType;

  std::vector<uint8_t> Abbrev;
  appendULEB(Abbrev, 1);
  appendULEB(Abbrev, DW_TAG_compile_unit);
  Abbrev.push_back(DW_CHILDREN_yes);
  appendULEB(Abbrev, 0);
  appendULEB(Abbrev, 0);

  appendULEB(Abbrev, 2);
  appendULEB(Abbrev, DW_TAG_subprogram);
  Abbrev.push_back(DW_CHILDREN_no);
  appendAttributeSpec(Abbrev, DW_AT_name, DW_FORM_string);
  appendAttributeSpec(Abbrev, DW_AT_low_pc, DW_FORM_addr);
  appendAttributeSpec(Abbrev, DW_AT_high_pc, DW_FORM_data4);
  if (DuplicateSpecification) {
    appendAttributeSpec(Abbrev, DW_AT_specification, DW_FORM_ref4);
    appendAttributeSpec(Abbrev, DW_AT_specification, DW_FORM_ref4);
  } else if (DuplicateOrigin) {
    appendAttributeSpec(Abbrev, DW_AT_abstract_origin, DW_FORM_ref4);
    appendAttributeSpec(Abbrev, DW_AT_abstract_origin, DW_FORM_ref4);
  } else {
    appendAttributeSpec(Abbrev, DW_AT_type, DW_FORM_ref4);
    if (DuplicateType)
      appendAttributeSpec(Abbrev, DW_AT_type, DW_FORM_ref4);
  }
  appendULEB(Abbrev, 0);
  appendULEB(Abbrev, 0);

  appendULEB(Abbrev, 3);
  appendULEB(Abbrev, BaseType      ? DW_TAG_base_type
                     : WrapperType ? DW_TAG_typedef
                                   : DW_TAG_pointer_type);
  Abbrev.push_back(DW_CHILDREN_no);
  if (BaseType) {
    appendAttributeSpec(Abbrev, DW_AT_byte_size, DW_FORM_data1);
    appendAttributeSpec(Abbrev, DW_AT_encoding, DW_FORM_data1);
    appendAttributeSpec(Abbrev, DW_AT_encoding, DW_FORM_data1);
  } else if (WrapperType) {
    appendAttributeSpec(Abbrev, DW_AT_type, DW_FORM_ref4);
    appendAttributeSpec(Abbrev, DW_AT_type, DW_FORM_ref4);
  } else {
    switch (Kind) {
    case DWARFReturnFixtureKind::PointerSizeExplicit:
    case DWARFReturnFixtureKind::PointerSizeZero:
    case DWARFReturnFixtureKind::PointerSizeDuplicate:
      appendAttributeSpec(Abbrev, DW_AT_byte_size, DW_FORM_data1);
      if (Kind == DWARFReturnFixtureKind::PointerSizeDuplicate)
        appendAttributeSpec(Abbrev, DW_AT_byte_size, DW_FORM_data1);
      break;
    case DWARFReturnFixtureKind::PointerSizeMalformed:
      appendAttributeSpec(Abbrev, DW_AT_byte_size, DW_FORM_string);
      break;
    case DWARFReturnFixtureKind::PointerSizeOverflow:
      appendAttributeSpec(Abbrev, DW_AT_byte_size, DW_FORM_data4);
      break;
    case DWARFReturnFixtureKind::PointerTrailingTruncatedAttribute:
      appendAttributeSpec(Abbrev, DW_AT_byte_size, DW_FORM_data1);
      appendAttributeSpec(Abbrev, DW_AT_decl_line, DW_FORM_data8);
      break;
    default:
      break;
    }
  }
  appendULEB(Abbrev, 0);
  appendULEB(Abbrev, 0);

  if (UsesDeclaration) {
    appendULEB(Abbrev, 4);
    appendULEB(Abbrev, DW_TAG_subprogram);
    Abbrev.push_back(DW_CHILDREN_no);
    appendAttributeSpec(Abbrev, DW_AT_type, DW_FORM_ref4);
    appendULEB(Abbrev, 0);
    appendULEB(Abbrev, 0);
  }
  if (WrapperType) {
    appendULEB(Abbrev, 5);
    appendULEB(Abbrev, DW_TAG_pointer_type);
    Abbrev.push_back(DW_CHILDREN_no);
    appendULEB(Abbrev, 0);
    appendULEB(Abbrev, 0);
  }
  appendULEB(Abbrev, 0);

  std::vector<uint8_t> Info;
  appendU32LE(Info, 0);
  appendU16LE(Info, 4);
  appendU32LE(Info, 0);
  Info.push_back(8);
  appendULEB(Info, 1);

  appendULEB(Info, 2);
  constexpr llvm::StringLiteral FunctionName = "return_probe";
  Info.insert(Info.end(), FunctionName.begin(), FunctionName.end());
  Info.push_back(0);
  appendU64LE(Info, 0x1000);
  appendU32LE(Info, 0x10);
  std::vector<size_t> FunctionReferenceOffsets;
  FunctionReferenceOffsets.push_back(Info.size());
  appendU32LE(Info, 0);
  if (DuplicateType || UsesDeclaration) {
    FunctionReferenceOffsets.push_back(Info.size());
    appendU32LE(Info, 0);
  }

  const uint32_t ReturnTypeOffset = static_cast<uint32_t>(Info.size());
  appendULEB(Info, 3);
  std::vector<size_t> WrapperReferenceOffsets;
  if (BaseType) {
    Info.push_back(4);
    Info.push_back(DW_ATE_unsigned);
    Info.push_back(DW_ATE_unsigned);
  } else if (WrapperType) {
    WrapperReferenceOffsets.push_back(Info.size());
    appendU32LE(Info, 0);
    WrapperReferenceOffsets.push_back(Info.size());
    appendU32LE(Info, 0);
  } else {
    switch (Kind) {
    case DWARFReturnFixtureKind::PointerSizeExplicit:
      Info.push_back(8);
      break;
    case DWARFReturnFixtureKind::PointerSizeZero:
      Info.push_back(0);
      break;
    case DWARFReturnFixtureKind::PointerSizeMalformed:
      Info.insert(Info.end(), {'b', 'a', 'd', 0});
      break;
    case DWARFReturnFixtureKind::PointerSizeOverflow:
      appendU32LE(Info, 0x10000);
      break;
    case DWARFReturnFixtureKind::PointerSizeDuplicate:
      Info.push_back(8);
      Info.push_back(8);
      break;
    case DWARFReturnFixtureKind::PointerTrailingTruncatedAttribute:
      // Keep the requested byte size readable while leaving the following
      // fixed-width form structurally truncated at the end of the CU.
      Info.push_back(8);
      break;
    default:
      break;
    }
  }

  uint32_t WrappedPointerOffset = 0;
  if (WrapperType) {
    WrappedPointerOffset = static_cast<uint32_t>(Info.size());
    appendULEB(Info, 5);
  }

  uint32_t DeclarationOffset = 0;
  size_t DeclarationTypeOffset = 0;
  if (UsesDeclaration) {
    DeclarationOffset = static_cast<uint32_t>(Info.size());
    appendULEB(Info, 4);
    DeclarationTypeOffset = Info.size();
    appendU32LE(Info, 0);
  }
  Info.push_back(0);
  patchU32LE(Info, 0, static_cast<uint32_t>(Info.size() - 4));
  for (const size_t Offset : FunctionReferenceOffsets)
    patchU32LE(Info, Offset,
               UsesDeclaration ? DeclarationOffset : ReturnTypeOffset);
  if (UsesDeclaration)
    patchU32LE(Info, DeclarationTypeOffset, ReturnTypeOffset);
  for (const size_t Offset : WrapperReferenceOffsets)
    patchU32LE(Info, Offset, WrappedPointerOffset);

  std::string SectionNames(1, '\0');
  const auto appendName = [](std::string &Table, llvm::StringRef Name) {
    const uint32_t Offset = static_cast<uint32_t>(Table.size());
    Table.append(Name.data(), Name.size());
    Table.push_back('\0');
    return Offset;
  };
  const uint32_t TextName = appendName(SectionNames, ".text");
  const uint32_t AbbrevName = appendName(SectionNames, ".debug_abbrev");
  const uint32_t InfoName = appendName(SectionNames, ".debug_info");
  const uint32_t ShstrtabName = appendName(SectionNames, ".shstrtab");

  constexpr size_t SectionCount = 5;
  constexpr size_t TextSection = 1;
  constexpr size_t AbbrevSection = 2;
  constexpr size_t InfoSection = 3;
  constexpr size_t SectionNameTableSection = 4;
  constexpr size_t TextSize = 16;
  const size_t TextOffset = llvm::alignTo(sizeof(Elf_Ehdr), 16);
  const size_t AbbrevOffset = TextOffset + TextSize;
  const size_t InfoOffset = AbbrevOffset + Abbrev.size();
  const size_t SectionNameTableOffset = InfoOffset + Info.size();
  const size_t SectionTableOffset = llvm::alignTo(
      SectionNameTableOffset + SectionNames.size(), alignof(Elf_Shdr));
  std::vector<uint8_t> Bytes(
      SectionTableOffset + SectionCount * sizeof(Elf_Shdr), 0);

  Elf_Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_EXEC;
  Header.e_machine = EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_entry = 0x1000;
  Header.e_ehsize = sizeof(Elf_Ehdr);
  Header.e_shoff = SectionTableOffset;
  Header.e_shentsize = sizeof(Elf_Shdr);
  Header.e_shnum = SectionCount;
  Header.e_shstrndx = SectionNameTableSection;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + AbbrevOffset, Abbrev.data(), Abbrev.size());
  std::memcpy(Bytes.data() + InfoOffset, Info.data(), Info.size());
  std::memcpy(Bytes.data() + SectionNameTableOffset, SectionNames.data(),
              SectionNames.size());

  std::array<Elf_Shdr, SectionCount> Sections{};
  Sections[TextSection].sh_name = TextName;
  Sections[TextSection].sh_type = SHT_PROGBITS;
  Sections[TextSection].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
  Sections[TextSection].sh_addr = 0x1000;
  Sections[TextSection].sh_offset = TextOffset;
  Sections[TextSection].sh_size = TextSize;
  Sections[TextSection].sh_addralign = 16;
  Sections[AbbrevSection].sh_name = AbbrevName;
  Sections[AbbrevSection].sh_type = SHT_PROGBITS;
  Sections[AbbrevSection].sh_offset = AbbrevOffset;
  Sections[AbbrevSection].sh_size = Abbrev.size();
  Sections[AbbrevSection].sh_addralign = 1;
  Sections[InfoSection].sh_name = InfoName;
  Sections[InfoSection].sh_type = SHT_PROGBITS;
  Sections[InfoSection].sh_offset = InfoOffset;
  Sections[InfoSection].sh_size = Info.size();
  Sections[InfoSection].sh_addralign = 1;
  Sections[SectionNameTableSection].sh_name = ShstrtabName;
  Sections[SectionNameTableSection].sh_type = SHT_STRTAB;
  Sections[SectionNameTableSection].sh_offset = SectionNameTableOffset;
  Sections[SectionNameTableSection].sh_size = SectionNames.size();
  Sections[SectionNameTableSection].sh_addralign = 1;
  std::memcpy(Bytes.data() + SectionTableOffset, Sections.data(),
              sizeof(Sections));
  return Bytes;
}

AuthenticatedReturnValueState
loadDWARFReturnFixture(DWARFReturnFixtureKind Kind) {
  ScratchDir Dir;
  const std::vector<uint8_t> Bytes = makeELFDWARFReturnFixture(Kind);
  const std::filesystem::path Binary =
      Dir.writeBytes("return-contract.elf", Bytes);
  std::unique_ptr<DWARFDebugContext> Context = DWARFDebugContext::load(
      Binary, BinaryFormat::ELF, DWARFLoadTrust::InImage, Bytes);
  EXPECT_NE(Context, nullptr);
  if (!Context)
    return {};
  EXPECT_TRUE(Context->hasAuthenticatedFunctionSignatures());
  const std::vector<FunctionSym> Functions = Context->allFunctions();
  const auto Probe = std::find_if(Functions.begin(), Functions.end(),
                                  [](const FunctionSym &Function) {
                                    return Function.Name == "return_probe";
                                  });
  EXPECT_NE(Probe, Functions.end());
  return Probe == Functions.end()
             ? AuthenticatedReturnValueState{}
             : Context->resolveAuthenticatedReturnValueState(Probe->Addr);
}

//===----------------------------------------------------------------------===//
// isSynthesizedFuncName — the predicate the whole precedence chain rests on
//===----------------------------------------------------------------------===//

TEST(NameOriginTest, PlaceholdersAreSynthesized) {
  EXPECT_TRUE(isSynthesizedFuncName(""));
  EXPECT_TRUE(isSynthesizedFuncName("sub_140001000"));
  EXPECT_TRUE(isSynthesizedFuncName("sub_1A2B"));
  EXPECT_TRUE(isSynthesizedFuncName("func_a9059cbb"));
}

TEST(NameOriginTest, RealNamesAreNotSynthesized) {
  EXPECT_FALSE(isSynthesizedFuncName("main"));
  EXPECT_FALSE(isSynthesizedFuncName("_ZN3foo3barEv"));
  EXPECT_FALSE(isSynthesizedFuncName("?bar@foo@@QEAAXXZ"));
}

// A binary is free to export something spelled like a placeholder.  Requiring
// the exact `<prefix><hex>` shape is what keeps such a name from being treated
// as up for grabs by a signature match.
TEST(NameOriginTest, PlaceholderLookalikesKeepTheirName) {
  EXPECT_FALSE(isSynthesizedFuncName("sub_total"));
  EXPECT_FALSE(isSynthesizedFuncName("sub_"));
  EXPECT_FALSE(isSynthesizedFuncName("func_ptr"));
  EXPECT_FALSE(isSynthesizedFuncName("sub_1000_thunk"));
  EXPECT_FALSE(isSynthesizedFuncName("subtract"));
}

//===----------------------------------------------------------------------===//
// applyDebugSymbols — debug info is level with the image, above a placeholder
//===----------------------------------------------------------------------===//

TEST(ApplyDebugSymbolsTest, ReplacesPlaceholderAndFillsSize) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "sub_1000"));

  FakeDebugContext Dbg({makeDebugFunc(0x1000, "parse_header", 64)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 1u);

  const Symbol *S = findFunc(Img, 0x1000);
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->Name, "parse_header");
  EXPECT_EQ(S->Size, 64u);
}

TEST(ApplyDebugSymbolsTest, ImageNameOutranksDebugName) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "shipped_name", 32));

  FakeDebugContext Dbg({makeDebugFunc(0x1000, "stale_name", 64)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);

  const Symbol *S = findFunc(Img, 0x1000);
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->Name, "shipped_name");
  EXPECT_EQ(S->Size, 32u);
}

TEST(ApplyDebugSymbolsTest, AddsFunctionsTheImageDoesNotDescribe) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "main", 16));

  FakeDebugContext Dbg({makeDebugFunc(0x2000, "static_helper", 48)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 1u);

  const Symbol *S = findFunc(Img, 0x2000);
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->Name, "static_helper");
  EXPECT_EQ(S->Size, 48u);
  EXPECT_TRUE(S->IsFunc);
}

// Discovery passes mint a placeholder for code the symbol table also names, so
// an address can carry two entries.  The stated one is what decides.
TEST(ApplyDebugSymbolsTest, StatedEntryDecidesWhenAnAddressCarriesBoth) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "sub_1000"));
  Img.Symbols.push_back(makeNamedFunc(0x1000, "shipped_name", 32));

  FakeDebugContext Dbg({makeDebugFunc(0x1000, "stale_name", 64)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);

  for (const Symbol &S : Img.Symbols)
    EXPECT_NE(S.Name, "stale_name");
}

TEST(ApplyDebugSymbolsTest, IgnoresUnusableDebugEntries) {
  BinaryImage Img;
  FakeDebugContext Dbg(
      {makeDebugFunc(0x1000, ""), makeDebugFunc(0, "at_zero")});

  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);
  EXPECT_TRUE(Img.Symbols.empty());
}

TEST(ApplyDebugSymbolsTest, PublishesOnlyAuthenticatedTypedObjectExtents) {
  const std::vector<DataObjectSym> Objects = {
      {"array", 0x2000, 8, true},
      {"record", 0x2010, 16, false},
  };

  BinaryImage AuthenticatedImage;
  FakeDebugContext Authenticated({}, Objects, true);
  EXPECT_EQ(applyDebugSymbols(AuthenticatedImage, Authenticated), 0u);
  ASSERT_EQ(AuthenticatedImage.ExactDataObjects.size(), 2u);
  EXPECT_EQ(AuthenticatedImage.ExactDataObjects[0].Precision,
            ExactDataObjectPrecision::TypedBuffer);
  EXPECT_EQ(AuthenticatedImage.ExactDataObjects[1].Precision,
            ExactDataObjectPrecision::TypedNonBuffer);

  BinaryImage UntrustedImage;
  FakeDebugContext Untrusted({}, Objects, false);
  EXPECT_EQ(applyDebugSymbols(UntrustedImage, Untrusted), 0u);
  EXPECT_TRUE(UntrustedImage.ExactDataObjects.empty());
}

TEST(ApplyDebugSymbolsTest, RejectsMalformedAuthenticatedObjectExtents) {
  FakeDebugContext Dbg({},
                       {{"missing", InvalidVA, 8, true},
                        {"zero", 0x2000, 0, true},
                        {"overflow", InvalidVA - 3, 8, true}},
                       true);
  BinaryImage Img;
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);
  EXPECT_TRUE(Img.ExactDataObjects.empty());
}

//===----------------------------------------------------------------------===//
// loadDebugInfo — which file an image is analyzed with
//===----------------------------------------------------------------------===//

TEST(LoadDebugInfoTest, FindsMapBesideTheBinary) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::Map);
  EXPECT_EQ(R.Path.filename().string(), "probe.map");

  auto Funcs = R.Context->allFunctions();
  ASSERT_EQ(Funcs.size(), 2u);
  EXPECT_EQ(Funcs[0].Name, "parse_header");
  EXPECT_EQ(Funcs[0].Addr, Img.Base + 0x1000);
  EXPECT_EQ(Funcs[1].Name, "emit_record");
  EXPECT_EQ(Funcs[1].Addr, Img.Base + 0x1040);
}

TEST(LoadDebugInfoTest, FindsMapNamedAfterTheWholeFileName) {
  ScratchDir Dir;
  Dir.write("probe.exe.map", kMSVCMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Path.filename().string(), "probe.exe.map");
}

TEST(LoadDebugInfoTest, ReportsNothingWhenNoCompanionFileExists) {
  ScratchDir Dir;

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::None);
  // An absent companion file is an ordinary outcome, not a failure.
  EXPECT_TRUE(R.Error.empty());
}

TEST(LoadDebugInfoTest, DisabledSearchIgnoresAnAvailableMap) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  DebugInfoRequest Req;
  Req.Enabled = false;

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::None);
}

TEST(LoadDebugInfoTest, ExplicitMapIsUsedOverTheOneBesideTheBinary) {
  ScratchDir Dir;
  Dir.write("probe.map", " Address         Publics by Value\n");
  std::filesystem::path Chosen = Dir.write("elsewhere.map", kMSVCMap);

  DebugInfoRequest Req;
  Req.MapPath = Chosen;

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Path.filename().string(), "elsewhere.map");
}

// Someone who names a file wants to hear that it was the wrong one, rather
// than get a silent fallback to whatever happened to be lying around.
TEST(LoadDebugInfoTest, MissingExplicitMapIsAnErrorNotAFallback) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  DebugInfoRequest Req;
  Req.MapPath = Dir.path("absent.map");

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_FALSE(R.Error.empty());
}

TEST(LoadDebugInfoTest, ExplicitMapWithoutFunctionsIsAnError) {
  ScratchDir Dir;
  std::filesystem::path Empty = Dir.write("empty.map", "nothing here\n");

  DebugInfoRequest Req;
  Req.MapPath = Empty;

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_FALSE(R.Error.empty());
}

// lld-link states each input chunk as `<object>:(<section>)`, and that object
// path is free to start with a dot.  Mistaking it for the enclosing output
// section files every symbol under it as non-code, which loses the whole map
// while still looking like a successful parse.
TEST(LoadDebugInfoTest, LLDMapKeepsSymbolsUnderADotPrefixedObjectPath) {
  ScratchDir Dir;

  DebugInfoRequest Req;
  Req.MapPath = Dir.write("probe.exe.map", kCOFFLLDMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::Map);

  // The data symbol stays out, so tracking the output section still works.
  std::vector<FunctionSym> Funcs = R.Context->allFunctions();
  ASSERT_EQ(Funcs.size(), 2u);
  EXPECT_EQ(Funcs[0].Name, "parse_header");
  EXPECT_EQ(Funcs[1].Name, "emit_record");
}

TEST(LoadDebugInfoTest, ELFMapKeepsSymbolsUnderADotPrefixedObjectPath) {
  ScratchDir Dir;

  DebugInfoRequest Req;
  Req.MapPath = Dir.write("probe.elf.map", kELFLLDMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.elf"), Img, Req);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::Map);
  std::vector<FunctionSym> Funcs = R.Context->allFunctions();
  ASSERT_EQ(Funcs.size(), 2u);
  EXPECT_EQ(Funcs[0].Name, "parse_header");
  EXPECT_EQ(Funcs[1].Name, "emit_record");
}

TEST(LoadDebugInfoTest, MissingExplicitPDBIsAnError) {
  ScratchDir Dir;

  DebugInfoRequest Req;
  Req.PDBPath = Dir.path("absent.pdb");

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_FALSE(R.Error.empty());
}

// EVM and SBF carry their own metadata formats; none of DWARF, PDB, or a
// linker MAP describes either, so discovery does not go looking.
TEST(LoadDebugInfoTest, SkipsFormatsWithNoNativeDebugFile) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  BinaryImage Img = makePEImage();
  Img.Arch = Arch::EVM;

  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);
  EXPECT_FALSE(static_cast<bool>(R));
}

TEST(LoadDebugInfoTest, AuthenticatesInImageDWARFAgainstLoadedSnapshot) {
  const std::filesystem::path Binary = safetyFixture("safety_cases_elf_x64");
  std::unique_ptr<Loader> Loader = Loader::create(BinaryFormat::ELF);
  ASSERT_NE(Loader, nullptr);
  llvm::Expected<BinaryImage> ImgOr = Loader->load(Binary);
  ASSERT_TRUE(static_cast<bool>(ImgOr)) << llvm::toString(ImgOr.takeError());

  DebugInfoResult R = loadDebugInfo(Binary, *ImgOr);
  ASSERT_TRUE(static_cast<bool>(R)) << R.Error;
  ASSERT_NE(R.Context, nullptr);
  EXPECT_EQ(R.Kind, DebugInfoKind::DWARF);
  EXPECT_TRUE(R.Context->hasAuthenticatedFunctionSignatures());
  EXPECT_TRUE(R.Context->hasAuthenticatedObjectExtents());
  const std::vector<FunctionSym> Functions = R.Context->allFunctions();
  const auto functionNamed = [&](llvm::StringRef Name) {
    return std::find_if(
        Functions.begin(), Functions.end(),
        [&](const FunctionSym &Function) { return Function.Name == Name; });
  };
  const auto Leaks = functionNamed("leaks_memory");
  const auto Main = functionNamed("main");
  ASSERT_NE(Leaks, Functions.end());
  ASSERT_NE(Main, Functions.end());
  EXPECT_EQ(R.Context->resolveAuthenticatedReturnValueState(Leaks->Addr).Kind,
            AuthenticatedReturnKind::NoValue);
  const AuthenticatedReturnValueState MainReturn =
      R.Context->resolveAuthenticatedReturnValueState(Main->Addr);
  EXPECT_EQ(MainReturn.Kind, AuthenticatedReturnKind::Integer);
  EXPECT_EQ(MainReturn.Size, 4u);
}

TEST(DWARFReturnContractIntegration,
     RepeatedTypeAndProvenanceAttributesFailClosed) {
  for (const DWARFReturnFixtureKind Kind :
       {DWARFReturnFixtureKind::DuplicateType,
        DWARFReturnFixtureKind::DuplicateSpecification,
        DWARFReturnFixtureKind::DuplicateAbstractOrigin}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    EXPECT_EQ(loadDWARFReturnFixture(Kind).Kind,
              AuthenticatedReturnKind::Unknown);
  }
}

TEST(DWARFReturnContractIntegration,
     PointerByteSizeDistinguishesAbsentFromInvalid) {
  const AuthenticatedReturnValueState Absent =
      loadDWARFReturnFixture(DWARFReturnFixtureKind::PointerSizeAbsent);
  EXPECT_EQ(Absent.Kind, AuthenticatedReturnKind::Pointer);
  EXPECT_EQ(Absent.Size, 8u);

  const AuthenticatedReturnValueState Explicit =
      loadDWARFReturnFixture(DWARFReturnFixtureKind::PointerSizeExplicit);
  EXPECT_EQ(Explicit.Kind, AuthenticatedReturnKind::Pointer);
  EXPECT_EQ(Explicit.Size, 8u);

  for (const DWARFReturnFixtureKind Kind :
       {DWARFReturnFixtureKind::PointerSizeZero,
        DWARFReturnFixtureKind::PointerSizeMalformed,
        DWARFReturnFixtureKind::PointerSizeOverflow,
        DWARFReturnFixtureKind::PointerSizeDuplicate}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    EXPECT_EQ(loadDWARFReturnFixture(Kind).Kind,
              AuthenticatedReturnKind::Unknown);
  }
}

TEST(DWARFReturnContractIntegration, RepeatedEncodingAndWrapperTypeFailClosed) {
  for (const DWARFReturnFixtureKind Kind :
       {DWARFReturnFixtureKind::DuplicateBaseEncoding,
        DWARFReturnFixtureKind::DuplicateWrapperType}) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    EXPECT_EQ(loadDWARFReturnFixture(Kind).Kind,
              AuthenticatedReturnKind::Unknown);
  }
}

TEST(DWARFReturnContractIntegration,
     ReadableAttributeCannotAuthenticateATruncatedDIE) {
  EXPECT_EQ(loadDWARFReturnFixture(
                DWARFReturnFixtureKind::PointerTrailingTruncatedAttribute)
                .Kind,
            AuthenticatedReturnKind::Unknown);
}

TEST(LoadDebugInfoTest,
     CheckedInNoUUIDDSYMDoesNotAuthenticateSemanticCapabilities) {
  // This reproducible fixture is intentionally linked with -no_uuid.  Its
  // adjacent dSYM still supplies useful names and lines, but cannot prove that
  // its types or object extents belong to the exact loaded image.
  const std::filesystem::path Binary = safetyFixture("safety_cases_macho_x64");
  std::unique_ptr<Loader> Loader = Loader::create(BinaryFormat::MachO);
  ASSERT_NE(Loader, nullptr);
  llvm::Expected<BinaryImage> ImgOr = Loader->load(Binary);
  ASSERT_TRUE(static_cast<bool>(ImgOr)) << llvm::toString(ImgOr.takeError());
  EXPECT_TRUE(ImgOr->DynInfo.UUID.empty());

  DebugInfoResult R = loadDebugInfo(Binary, *ImgOr);
  ASSERT_TRUE(static_cast<bool>(R)) << R.Error;
  ASSERT_NE(R.Context, nullptr);
  EXPECT_EQ(R.Kind, DebugInfoKind::DWARF);
  EXPECT_TRUE(R.Context->hasInfo());
  EXPECT_FALSE(R.Context->hasAuthenticatedFunctionSignatures());
  EXPECT_FALSE(R.Context->hasAuthenticatedObjectExtents());
  const std::vector<FunctionSym> Functions = R.Context->allFunctions();
  const auto Leaks = std::find_if(Functions.begin(), Functions.end(),
                                  [](const FunctionSym &Function) {
                                    return Function.Name == "leaks_memory";
                                  });
  ASSERT_NE(Leaks, Functions.end());
  EXPECT_EQ(R.Context->resolveAuthenticatedReturnValueState(Leaks->Addr).Kind,
            AuthenticatedReturnKind::Unknown);
}

TEST(LoadDebugInfoTest, FileReplacementCannotAuthenticateStaleImageSnapshot) {
  const std::filesystem::path Original = safetyFixture("safety_cases_elf_x64");
  const std::filesystem::path Replacement =
      safetyFixture("safety_cases_elf_arm64");
  ScratchDir Dir;
  const std::filesystem::path Probe = Dir.path("probe");
  ASSERT_TRUE(std::filesystem::copy_file(Original, Probe));

  std::unique_ptr<Loader> Loader = Loader::create(BinaryFormat::ELF);
  ASSERT_NE(Loader, nullptr);
  llvm::Expected<BinaryImage> ImgOr = Loader->load(Probe);
  ASSERT_TRUE(static_cast<bool>(ImgOr)) << llvm::toString(ImgOr.takeError());

  ASSERT_TRUE(std::filesystem::copy_file(
      Replacement, Probe, std::filesystem::copy_options::overwrite_existing));
  DebugInfoResult R = loadDebugInfo(Probe, *ImgOr);
  ASSERT_TRUE(static_cast<bool>(R)) << R.Error;
  ASSERT_NE(R.Context, nullptr);
  EXPECT_EQ(R.Kind, DebugInfoKind::DWARF);
  EXPECT_FALSE(R.Context->hasAuthenticatedFunctionSignatures());
  EXPECT_FALSE(R.Context->hasAuthenticatedObjectExtents());
  const std::vector<FunctionSym> Functions = R.Context->allFunctions();
  ASSERT_FALSE(Functions.empty());
  EXPECT_EQ(
      R.Context->resolveAuthenticatedReturnValueState(Functions.front().Addr)
          .Kind,
      AuthenticatedReturnKind::Unknown);
}

TEST(ELFObjectExtentProducer,
     PublishesOnlySizedObjectsWhollyInsideAllocatedSectionsAsStorage) {
  ScratchDir Dir;
  const std::filesystem::path Binary =
      Dir.writeBytes("objects.o", makeELFObjectExtentFixture());
  std::unique_ptr<Loader> Loader = Loader::create(BinaryFormat::ELF);
  ASSERT_NE(Loader, nullptr);
  llvm::Expected<BinaryImage> ImgOr = Loader->load(Binary);
  ASSERT_TRUE(static_cast<bool>(ImgOr)) << llvm::toString(ImgOr.takeError());

  ASSERT_EQ(ImgOr->ExactDataObjects.size(), 1u);
  const ExactDataObjectExtent &Extent = ImgOr->ExactDataObjects.front();
  EXPECT_EQ(Extent.Base, 0u);
  EXPECT_EQ(Extent.Size, 8u);
  EXPECT_EQ(Extent.Evidence, ExactDataObjectEvidence::ELFObjectSymbol);
  EXPECT_EQ(Extent.Precision, ExactDataObjectPrecision::Storage);
}

TEST(DebugInfoKindNameTest, NamesEveryKind) {
  EXPECT_STREQ(debugInfoKindName(DebugInfoKind::None), "none");
  EXPECT_STREQ(debugInfoKindName(DebugInfoKind::DWARF), "dwarf");
  EXPECT_STREQ(debugInfoKindName(DebugInfoKind::PDB), "pdb");
  EXPECT_STREQ(debugInfoKindName(DebugInfoKind::Map), "map");
}

TEST(DWARFSubprogramExtentRegistry,
     OverlapAcrossCompileUnitsIsStickyAndOrderIndependent) {
  using dwarf_loader_detail::SubprogramExtentRegistry;
  using dwarf_loader_detail::SubprogramRange;
  const std::array<SubprogramRange, 1> First = {{{0x1000, 0x1020}}};
  const std::array<SubprogramRange, 1> Second = {{{0x1010, 0x1030}}};
  for (const bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    SubprogramExtentRegistry Registry;
    if (Reverse) {
      Registry.insert(0x1010, Second);
      Registry.insert(0x1000, First);
    } else {
      Registry.insert(0x1000, First);
      Registry.insert(0x1010, Second);
    }
    EXPECT_TRUE(Registry.isAmbiguous(0x1000));
    EXPECT_TRUE(Registry.isAmbiguous(0x1010));
  }
}

TEST(DWARFSubprogramExtentRegistry, SameEntryDifferentExtentsIsAmbiguous) {
  using dwarf_loader_detail::SubprogramExtentRegistry;
  using dwarf_loader_detail::SubprogramRange;
  const std::array<SubprogramRange, 1> Short = {{{0x1000, 0x1010}}};
  const std::array<SubprogramRange, 1> Long = {{{0x1000, 0x1040}}};
  SubprogramExtentRegistry Registry;
  Registry.insert(0x1000, Short);
  Registry.insert(0x1000, Long);
  EXPECT_TRUE(Registry.isAmbiguous(0x1000));
}

TEST(DWARFSubprogramExtentRegistry, DuplicateSameExtentIsStickyAmbiguous) {
  using dwarf_loader_detail::SubprogramExtentRegistry;
  using dwarf_loader_detail::SubprogramRange;
  const std::array<SubprogramRange, 1> Range = {{{0x1000, 0x1010}}};
  SubprogramExtentRegistry Registry;
  Registry.insert(0x1000, Range);
  Registry.insert(0x1000, Range);
  EXPECT_TRUE(Registry.isAmbiguous(0x1000));
}

TEST(DWARFSubprogramExtentRegistry, DisjointSubprogramsRemainUnique) {
  using dwarf_loader_detail::SubprogramExtentRegistry;
  using dwarf_loader_detail::SubprogramRange;
  const std::array<SubprogramRange, 1> First = {{{0x1000, 0x1010}}};
  const std::array<SubprogramRange, 1> Second = {{{0x1020, 0x1030}}};
  SubprogramExtentRegistry Registry;
  Registry.insert(0x1000, First);
  Registry.insert(0x1020, Second);
  EXPECT_FALSE(Registry.isAmbiguous(0x1000));
  EXPECT_FALSE(Registry.isAmbiguous(0x1020));
}

TEST(DWARFReturnTypeProvenancePolicy,
     InheritedVoidConflictsWithConcreteValueInEitherOrder) {
  using Status = dwarf_loader_detail::ReturnTypeProvenanceStatus;
  using dwarf_loader_detail::mergeReturnTypeProvenance;
  EXPECT_EQ(mergeReturnTypeProvenance(Status::Value, Status::NoValue),
            Status::Malformed);
  EXPECT_EQ(mergeReturnTypeProvenance(Status::NoValue, Status::Value),
            Status::Malformed);
  EXPECT_EQ(mergeReturnTypeProvenance(Status::Value, Status::Value,
                                      /*SameConcreteType=*/false),
            Status::Malformed);
}

TEST(DWARFReturnTypeProvenancePolicy,
     UnspecifiedIsIdentityButMalformedIsSticky) {
  using Status = dwarf_loader_detail::ReturnTypeProvenanceStatus;
  using dwarf_loader_detail::mergeReturnTypeProvenance;
  EXPECT_EQ(mergeReturnTypeProvenance(Status::Unspecified, Status::Value),
            Status::Value);
  EXPECT_EQ(mergeReturnTypeProvenance(Status::NoValue, Status::Unspecified),
            Status::NoValue);
  EXPECT_EQ(mergeReturnTypeProvenance(Status::Malformed, Status::Value),
            Status::Malformed);
}

TEST(DWARFLocationExpressionPolicy,
     UnsupportedAndTrailingOperationsAreMalformed) {
  using dwarf_loader_detail::decodeLocationExpressionShape;
  using dwarf_loader_detail::LocationExpressionBase;

  const std::array<uint64_t, 0> NoOperands = {};
  EXPECT_EQ(decodeLocationExpressionShape(llvm::dwarf::DW_OP_deref, NoOperands,
                                          true, false, 7, 6)
                .Base,
            LocationExpressionBase::Malformed);

  const std::array<uint64_t, 1> Offset = {static_cast<uint64_t>(-16)};
  EXPECT_EQ(decodeLocationExpressionShape(llvm::dwarf::DW_OP_breg7, Offset,
                                          false, false, 7, 6)
                .Base,
            LocationExpressionBase::Malformed);
  EXPECT_EQ(decodeLocationExpressionShape(llvm::dwarf::DW_OP_breg5, Offset,
                                          true, false, 7, 6)
                .Base,
            LocationExpressionBase::Malformed);
}

TEST(DWARFLocationExpressionPolicy,
     OnlyCompleteRegisterValuesAreProvablyNonStack) {
  using dwarf_loader_detail::decodeLocationExpressionShape;
  using dwarf_loader_detail::LocationExpressionBase;

  const std::array<uint64_t, 0> NoOperands = {};
  EXPECT_EQ(decodeLocationExpressionShape(llvm::dwarf::DW_OP_reg5, NoOperands,
                                          true, false, 7, 6)
                .Base,
            LocationExpressionBase::NonStack);
  EXPECT_EQ(decodeLocationExpressionShape(llvm::dwarf::DW_OP_reg5, NoOperands,
                                          false, false, 7, 6)
                .Base,
            LocationExpressionBase::Malformed);
  EXPECT_EQ(decodeLocationExpressionShape(llvm::dwarf::DW_OP_reg5, NoOperands,
                                          true, true, 7, 6)
                .Base,
            LocationExpressionBase::Malformed);
}

} // anonymous namespace
