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
#include "neverd/Limits.h"
#include "neverd/debug/CompressedSections.h"
#include "neverd/debug/DWARFLoader.h"
#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/debug/MapFile.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <tuple>
#include <vector>

using namespace neverd;

TEST(DebugInfoDiscovery, SymbolStoreKeyUsesUniqueCodeViewIdentity) {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.DynInfo.PDBPath = "C:\\build\\Private\\NTKERNEL.PDB";
  Img.DynInfo.CodeViewPDBIdentityState = PDBIdentityState::Unique;
  PDBBuildIdentity Identity;
  Identity.Guid = {0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a, 0xf0, 0xde,
                   0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
  Identity.Age = 0x2a;
  Img.DynInfo.CodeViewPDBIdentity = Identity;
  auto Request = pdbSymbolRequest(Img);
  ASSERT_TRUE(Request);
  EXPECT_EQ(Request->FileName, "ntkernel.pdb");
  EXPECT_EQ(Request->Key, "123456789ABCDEF0123456789ABCDEF02a");
  Identity.Kind = PDBIdentityKind::NB10;
  Identity.Signature = 0x1234abcd;
  Img.DynInfo.CodeViewPDBIdentity = Identity;
  Request = pdbSymbolRequest(Img);
  ASSERT_TRUE(Request);
  EXPECT_EQ(Request->Key, "1234ABCD2a");
  Img.Format = BinaryFormat::ELF;
  EXPECT_FALSE(pdbSymbolRequest(Img));
}

TEST(DebugInfoDiscovery, MissingAmbiguousOrUnsafePdbCannotRequestSymbols) {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.DynInfo.PDBPath = "kernel.pdb";
  EXPECT_FALSE(pdbSymbolRequest(Img));
  PDBBuildIdentity Identity;
  Identity.Guid[0] = 1;
  Identity.Age = 1;
  Img.DynInfo.CodeViewPDBIdentity = Identity;
  Img.DynInfo.CodeViewPDBIdentityState = PDBIdentityState::Ambiguous;
  EXPECT_FALSE(pdbSymbolRequest(Img));
  Img.DynInfo.CodeViewPDBIdentityState = PDBIdentityState::Unique;
  for (const auto *Path : {"", ".pdb", "kernel.dll", "bad:stream.pdb",
                           "bad?query.pdb", "bad\nname.pdb"}) {
    Img.DynInfo.PDBPath = Path;
    EXPECT_FALSE(pdbSymbolRequest(Img)) << Path;
  }
  Img.DynInfo.PDBPath = "kernel.pdb";
  Img.DynInfo.CodeViewPDBIdentity->Age = 0;
  EXPECT_FALSE(pdbSymbolRequest(Img));
}

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
    " 0002:00000000 00001000H .data                   DATA\n"
    "\n"
    "  Address         Publics by Value        Rva+Base       Lib:Object\n"
    "\n"
    " 0001:00001000       parse_header            0000140001000 f   probe.obj\n"
    " 0001:00001040       emit_record             0000140001040 f   probe.obj\n"
    " 0002:00000000       ProbeSink               0000140002000     probe.obj\n"
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
  RecordReference,
  RecordRvalueReference,
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

std::vector<uint8_t> makeELFDWARFReturnFixture(DWARFReturnFixtureKind Kind,
                                               unsigned CompileUnits = 1) {
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
  const bool RecordReference =
      Kind == DWARFReturnFixtureKind::RecordReference ||
      Kind == DWARFReturnFixtureKind::RecordRvalueReference;

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
  appendULEB(Abbrev, Kind == DWARFReturnFixtureKind::RecordReference
                         ? DW_TAG_reference_type
                     : Kind == DWARFReturnFixtureKind::RecordRvalueReference
                         ? DW_TAG_rvalue_reference_type
                     : BaseType    ? DW_TAG_base_type
                     : WrapperType ? DW_TAG_typedef
                                   : DW_TAG_pointer_type);
  Abbrev.push_back(DW_CHILDREN_no);
  if (RecordReference) {
    appendAttributeSpec(Abbrev, DW_AT_type, DW_FORM_ref4);
  } else if (BaseType) {
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
  if (RecordReference) {
    appendULEB(Abbrev, 6);
    appendULEB(Abbrev, DW_TAG_namespace);
    Abbrev.push_back(DW_CHILDREN_yes);
    appendAttributeSpec(Abbrev, DW_AT_name, DW_FORM_string);
    appendULEB(Abbrev, 0);
    appendULEB(Abbrev, 0);
    appendULEB(Abbrev, 7);
    appendULEB(Abbrev, DW_TAG_class_type);
    Abbrev.push_back(DW_CHILDREN_no);
    appendAttributeSpec(Abbrev, DW_AT_name, DW_FORM_string);
    appendAttributeSpec(Abbrev, DW_AT_byte_size, DW_FORM_data1);
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
  if (RecordReference) {
    const size_t ReferenceOffset = Info.size();
    appendU32LE(Info, 0);
    appendULEB(Info, 6);
    constexpr llvm::StringLiteral Namespace = "Demo";
    Info.insert(Info.end(), Namespace.begin(), Namespace.end());
    Info.push_back(0);
    patchU32LE(Info, ReferenceOffset, static_cast<uint32_t>(Info.size()));
    appendULEB(Info, 7);
    constexpr llvm::StringLiteral Record = "Widget<int>";
    Info.insert(Info.end(), Record.begin(), Record.end());
    Info.push_back(0);
    Info.push_back(16);
    Info.push_back(0);
  } else if (BaseType) {
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

  const auto Unit = Info;
  for (unsigned I = 1; I < CompileUnits; ++I)
    Info.insert(Info.end(), Unit.begin(), Unit.end());

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
  if (CompileUnits > 1)
    Bytes.resize(std::max<size_t>(Bytes.size(), 256 * 1024), 0);
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
// isSynthesizedFuncName recognizes spelling; Symbol::Origin owns precedence.
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

// Similar spellings remain ordinary names. Exact placeholder spellings still
// require the producer's origin before a signature may replace them.
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
  Img.Symbols.push_back(Symbol::makeFunc(0x1000));

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

TEST(ApplyDebugSymbolsTest, StatedHexPlaceholderNameRemainsAuthoritative) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "sub_1000", 32));
  FakeDebugContext Dbg({makeDebugFunc(0x1000, "stale_name", 64)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);
  EXPECT_EQ(Img.Symbols.front().Name, "sub_1000");
  EXPECT_EQ(Img.Symbols.front().Origin, NameOrigin::Stated);
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
  Img.Symbols.push_back(Symbol::makeFunc(0x1000));
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

TEST(ApplyDebugSymbolsTest, PublishesNamesOnlyDataObjectsWithoutExtents) {
  BinaryImage Img;
  FakeDebugContext Dbg({}, {{"__security_cookie", 0x1400050E0, 0, false}},
                       false);
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);
  EXPECT_TRUE(Img.ExactDataObjects.empty());
  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols[0].Name, "__security_cookie");
  EXPECT_EQ(Img.Symbols[0].Addr, 0x1400050E0u);
  EXPECT_FALSE(Img.Symbols[0].IsFunc);
}

// The loader guesses a function after padding wherever the next bytes read
// as a prologue; MSVC keeps string literals in .text, and `push rdi` is a
// wide string's first letter.  A data object the debug information names
// there refutes the guess, but not code metadata.
TEST(ApplyDebugSymbolsTest, DataRefutesAFunctionGuessedAfterPadding) {
  BinaryImage Img;
  for (const va_t Addr : {0x1400u, 0x1500u, 0x1600u}) {
    Symbol Guess = Symbol::makeFunc(Addr);
    Guess.IsBoundaryGuess = true;
    Img.Symbols.push_back(Guess);
  }
  Img.KnownCodeRanges.push_back({0x1600, 0x1610});
  FakeDebugContext Dbg(
      {}, {{"??_C@_1BE@BDEKEKBI@?$AAW?$AAI?$AAN", 0x1400, 0, false},
           {"table_or_code", 0x1600, 0, false}});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);
  EXPECT_EQ(findFunc(Img, 0x1400), nullptr);
  EXPECT_NE(findFunc(Img, 0x1500), nullptr);
  EXPECT_NE(findFunc(Img, 0x1600), nullptr);
}

// A pointer .data holds to such a literal points to data: the guess the
// literal refutes claims no pointer to it as one to code.
TEST(ApplyDebugSymbolsTest, PointerToALiteralWhereAGuessWasPointsToData) {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  const SegmentFlags Code = SegmentFlags::Readable | SegmentFlags::Executable;
  const SegmentFlags Writable = SegmentFlags::Readable | SegmentFlags::Writable;
  for (const auto &[VA, Name, Flags] :
       {std::tuple{va_t{0x1000}, "PAGE", Code},
        std::tuple{va_t{0x3000}, ".data", Writable}}) {
    Segment Seg;
    Seg.VA = VA;
    Seg.Size = Seg.FileSz = 0x100;
    Seg.Flags = Flags;
    Seg.Data.assign(0x100, 0);
    Img.Segments.push_back(Seg);
    Section Sec;
    Sec.Name = Name;
    Sec.VA = VA;
    Sec.Size = Sec.FileSz = 0x100;
    Sec.Flags = Flags;
    Sec.Data.assign(0x100, 0);
    Img.Sections.push_back(std::move(Sec));
  }
  Symbol Guess = Symbol::makeFunc(0x1040);
  Guess.IsBoundaryGuess = true;
  Img.Symbols.push_back(Guess);
  const uint64_t Literal = 0x1040;
  std::memcpy(Img.Segments[1].Data.data(), &Literal, sizeof(Literal));
  recordAbsolutePointerRelocations(Img, {{0x3000, Literal, InvalidVA}});
  ASSERT_TRUE(Img.CodePtrRelocSlots.count(0x3000));

  FakeDebugContext Dbg(
      {}, {{"??_C@_1BE@BDEKEKBI@?$AAW?$AAI?$AAN", Literal, 0, false}});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);
  EXPECT_EQ(findFunc(Img, Literal), nullptr);
  EXPECT_FALSE(Img.CodePtrRelocSlots.count(0x3000));
  EXPECT_TRUE(Img.DataPtrRelocSlots.count(0x3000));
}

// A kernel keeps tables among its code, where the debug information names
// them as data and its PDB marks assembly labels the same way.  An address
// the image's code metadata covers, or where that code ends (a routine's end
// label), stays code, a literal names no object, and data outside the code
// needs no record.
TEST(ApplyDebugSymbolsTest, DataAmongCodeIsWhereNoCodeIs) {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  for (const auto &[VA, Flags] :
       {std::pair{va_t{0x1000},
                  SegmentFlags::Readable | SegmentFlags::Executable},
        std::pair{va_t{0x3000},
                  SegmentFlags::Readable | SegmentFlags::Writable}}) {
    Segment Seg;
    Seg.VA = VA;
    Seg.Size = Seg.FileSz = 0x100;
    Seg.Flags = Flags;
    Seg.Data.assign(0x100, 0);
    Img.Segments.push_back(std::move(Seg));
  }
  Img.KnownCodeRanges.push_back({0x1000, 0x1020});
  FakeDebugContext Dbg({}, {{"KiServiceLabel", 0x1010, 0, false},
                            {"KiServiceEnd", 0x1020, 0, false},
                            {"NlsRegKeyName", 0x1040, 0, false},
                            {"??_C@_15OHIPBLFN@?$AAS?$AAA@", 0x1060, 0, false},
                            {"NlsSectionLock", 0x3000, 0, false}});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);
  EXPECT_EQ(Img.DataInCode, std::set<va_t>{0x1040});
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

TEST(LoadDebugInfoTest, MSVCMapPublishesDataPublicsWithoutExtents) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);
  ASSERT_TRUE(static_cast<bool>(R));
  ASSERT_NE(R.Context, nullptr);

  const std::vector<DataObjectSym> Objects = R.Context->allDataObjects();
  ASSERT_EQ(Objects.size(), 1u);
  EXPECT_EQ(Objects[0].Name, "ProbeSink");
  EXPECT_EQ(Objects[0].Addr, Img.Base + 0x2000);
  EXPECT_EQ(Objects[0].Size, 0u);
  EXPECT_FALSE(Objects[0].IsBuffer);
  EXPECT_FALSE(R.Context->hasAuthenticatedObjectExtents());
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

TEST(LoadDebugInfoTest, ELFMapPreservesCompleteDemangledSymbolNames) {
  const llvm::StringRef Maps[] = {
      "     VMA      LMA     Size Align Out     In      Symbol\n"
      "    1000     1000       a0     1 .text\n"
      "    1000     1000       a0     1         .build with "
      "spaces/probe.o:(.text)\n"
      "    1000     1000       10     1                 demo::mix(int, long)\n"
      "    1020     1020       11     1                 operator new(unsigned "
      "long)\n"
      "    1040     1040       12     1                 "
      "demo::Box::operator=(demo::Box const&)\n"
      "    1060     1060       13     1                 (anonymous "
      "namespace)::work(int, long)\n"
      "    1080     1080       14     1                 demo::(anonymous "
      "namespace)::work(int, long)\n"
      "    3000     3000       20     1 .rodata\n"
      "    3000     3000       20     1         probe.o:(.rodata)\n"
      "    3000     3000        8     1                 demo::table\n",
      "             VMA              LMA     Size Align Out     In      "
      "Symbol\n"
      "            1000             1000 100000080     1 .text\n"
      "            1000             1000 100000080     1         .build with "
      "spaces/probe.o:(.text)\n"
      "            1000             1000       10     1                 "
      "demo::mix(int, long)\n"
      "            1020             1020       11     1                 "
      "operator new(unsigned long)\n"
      "            1040             1040       12     1                 "
      "demo::Box::operator=(demo::Box const&)\n"
      "            1060             1060       13     1                 "
      "(anonymous namespace)::work(int, long)\n"
      "            1080             1080       14     1                 "
      "demo::(anonymous namespace)::work(int, long)\n"
      "       200000000        200000000       20     1 .rodata\n"
      "       200000000        200000000       20     1         "
      "probe.o:(.rodata)\n"
      "       200000000        200000000        8     1                 "
      "demo::table\n"};
  const llvm::StringRef Names[] = {
      "demo::mix(int, long)", "operator new(unsigned long)",
      "demo::Box::operator=(demo::Box const&)",
      "(anonymous namespace)::work(int, long)",
      "demo::(anonymous namespace)::work(int, long)"};
  for (unsigned Layout = 0; Layout < 2; ++Layout) {
    SCOPED_TRACE(Layout);
    ScratchDir Dir;
    DebugInfoRequest Req;
    Req.MapPath = Dir.write("names.elf.map", Maps[Layout]);
    BinaryImage Img;
    Img.Format = BinaryFormat::ELF;
    Img.Arch = Layout == 0 ? Arch::X86 : Arch::X64;
    Img.Bits = Layout == 0 ? Bitness::Bits32 : Bitness::Bits64;
    DebugInfoResult R = loadDebugInfo(Dir.path("names.elf"), Img, Req);
    ASSERT_TRUE(static_cast<bool>(R));
    EXPECT_EQ(R.Kind, DebugInfoKind::Map);
    std::vector<FunctionSym> Funcs = R.Context->allFunctions();
    ASSERT_EQ(Funcs.size(), 5u);
    for (unsigned I = 0; I < 5; ++I) {
      EXPECT_EQ(Funcs[I].Name, Names[I].str());
      EXPECT_EQ(Funcs[I].Addr, 0x1000u + I * 0x20u);
      EXPECT_EQ(Funcs[I].Size, 0x10u + I);
    }
  }
}

TEST(LoadDebugInfoTest, ELFMapDistinguishesSymbolsFromScriptAndInputRows) {
  constexpr llvm::StringLiteral Map =
      "             VMA              LMA     Size Align Out     In      "
      "Symbol\n"
      "            1000             1000       32    16 .text\n"
      "            1000             1000       10    16         .build with "
      "spaces/first.o:(.text)\n"
      "            1000             1000       10     1                 "
      "demo::Box::operator=(demo::Box const&)\n"
      "            1010             1010        0     1         inner=.\n"
      "            1010             1010        0     1         . = ALIGN(16)\n"
      "            1010             1010        1     1         BYTE(0)\n"
      "            1020             1020       12    16         "
      "second.o:(.text)\n"
      "            1020             1020       12     1                 "
      "demo::mix(int, long)\n"
      "            1032             1032        0     1 alias=.\n"
      "            1032             1032        e     1 . = ALIGN(16)\n"
      "            2000             2000       20    16 my_data\n"
      "            2000             2000       20     1         "
      "probe.o:(my_data)\n"
      "            2000             2000        8     1                 "
      "data_symbol\n";
  ScratchDir Dir;
  DebugInfoRequest Req;
  Req.MapPath = Dir.write("rows.elf.map", Map);
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  DebugInfoResult R = loadDebugInfo(Dir.path("rows.elf"), Img, Req);
  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::Map);
  std::vector<FunctionSym> Funcs = R.Context->allFunctions();
  ASSERT_EQ(Funcs.size(), 2u);
  EXPECT_EQ(Funcs[0].Name, "demo::Box::operator=(demo::Box const&)");
  EXPECT_EQ(Funcs[0].Addr, 0x1000u);
  EXPECT_EQ(Funcs[0].Size, 0x10u);
  EXPECT_EQ(Funcs[1].Name, "demo::mix(int, long)");
  EXPECT_EQ(Funcs[1].Addr, 0x1020u);
  EXPECT_EQ(Funcs[1].Size, 0x12u);
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

TEST(DWARFSubprogramExtentRegistry, SweepMatchesOverlapPolicyAfterMerging) {
  using dwarf_loader_detail::SubprogramExtentRegistry;
  using Range = dwarf_loader_detail::SubprogramRange;
  struct Die {
    va_t Entry;
    std::vector<Range> Ranges;
  };
  std::mt19937 Random(42);
  std::vector<Die> Dies;
  SubprogramExtentRegistry Registry, Other;
  for (unsigned I = 0; I < 256; ++I) {
    Die D{Random() % 1024, {}};
    for (unsigned J = 0, N = Random() % 4; J < N; ++J) {
      va_t Begin = Random() % 4096;
      D.Ranges.emplace_back(Begin, Begin + Random() % 32);
    }
    (I < 128 ? Registry : Other).insert(D.Entry, D.Ranges);
    Dies.push_back(D);
  }
  Registry.append(std::move(Other));
  Registry.finalize();
  for (const Die &D : Dies) {
    bool Expected = false;
    // All DIEs at an entry share its ambiguity. Compare the external policy,
    // not the interval index's component algorithm.
    for (const Die &Owner : Dies) {
      if (Owner.Entry != D.Entry)
        continue;
      Expected |= Owner.Ranges.empty();
      for (const Range &R : Owner.Ranges)
        Expected |= R.first >= R.second;
      for (const Die &Candidate : Dies) {
        if (&Owner == &Candidate)
          continue;
        Expected |= Owner.Entry == Candidate.Entry;
        for (const Range &A : Owner.Ranges)
          for (const Range &B : Candidate.Ranges)
            Expected |= A.first < B.second && B.first < A.second;
      }
    }
    EXPECT_EQ(Registry.isAmbiguous(D.Entry), Expected) << D.Entry;
  }
  const std::array<Range, 1> Fresh = {{{0x100000, 0x100010}}};
  Registry.insert(0x100000, Fresh);
  EXPECT_FALSE(Registry.isAmbiguous(0x100000));
  Registry.insert(0x100000, Fresh);
  EXPECT_TRUE(Registry.isAmbiguous(0x100000));
}

TEST(DWARFSourceTypes, ReferencesPreserveQualifiedRecordIdentity) {
  ScratchDir Dir;
  for (auto Kind : {DWARFReturnFixtureKind::RecordReference,
                    DWARFReturnFixtureKind::RecordRvalueReference}) {
    const auto Bytes = makeELFDWARFReturnFixture(Kind);
    const auto Path = Dir.writeBytes("reference.elf", Bytes);
    auto Context = DWARFDebugContext::load(Path, BinaryFormat::ELF,
                                           DWARFLoadTrust::InImage, Bytes);
    ASSERT_TRUE(Context);
    auto Function = Context->resolveFunction(0x1000);
    ASSERT_TRUE(Function && Function->ReturnType);
    EXPECT_EQ(Function->ReturnType->Kind, NdTypeKind::Ptr);
    const auto &Record = Function->ReturnType->Pointee;
    ASSERT_TRUE(Record);
    EXPECT_EQ(Record->Kind, NdTypeKind::Struct);
    EXPECT_EQ(Record->SourceName, "Demo::Widget<int>");
    EXPECT_EQ(Record->Size, 16);
  }
}

TEST(DWARFParallelLoad, DuplicateDIEsAcrossWorkersCannotAuthenticateAType) {
  ScratchDir Dir;
  auto Bytes = makeELFDWARFReturnFixture(
      DWARFReturnFixtureKind::PointerSizeExplicit, 16);
  const auto Path = Dir.writeBytes("parallel.elf", Bytes);
  const unsigned Saved = workerThreadOverride().load();
  llvm::scope_exit Restore([&] { setWorkerThreadCount(Saved); });
  std::vector<FunctionSym> Serial;
  for (unsigned Threads : {1, 4}) {
    setWorkerThreadCount(Threads);
    auto Context = DWARFDebugContext::load(Path, BinaryFormat::ELF,
                                           DWARFLoadTrust::InImage, Bytes);
    ASSERT_TRUE(Context && Context->hasInfo());
    auto Functions = Context->allFunctions();
    ASSERT_EQ(Functions.size(), 16U);
    if (Threads == 1)
      Serial = Functions;
    for (size_t I = 0; I < Functions.size(); ++I) {
      EXPECT_EQ(Functions[I].Addr, Serial[I].Addr);
      EXPECT_EQ(Functions[I].Name, Serial[I].Name);
      EXPECT_EQ(Functions[I].Size, Serial[I].Size);
      EXPECT_EQ(
          Context->resolveAuthenticatedReturnValueState(Functions[I].Addr).Kind,
          AuthenticatedReturnKind::Unknown);
    }
  }
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

/// An x86-64 ELF image as the loader leaves it for debug discovery.
BinaryImage makeELFImage(std::vector<uint8_t> Bytes) {
  BinaryImage Img;
  Img.Format = BinaryFormat::ELF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Raw = std::move(Bytes);
  return Img;
}

TEST(DebugInfoDiscovery, FindsDWARFInTheInputBeforeASplitFile) {
  ScratchDir Dir;
  const auto Bytes =
      makeELFDWARFReturnFixture(DWARFReturnFixtureKind::PointerSizeAbsent);
  const auto Image = Dir.writeBytes("image", Bytes);
  Dir.writeBytes("image.debug", Bytes);
  const auto Found = findDWARF(Image, BinaryFormat::ELF);
  ASSERT_TRUE(Found);
  EXPECT_EQ(Found->Location, DWARFLocation::Input);
  EXPECT_EQ(Found->Path, Image);
  EXPECT_STREQ(dwarfLocationName(Found->Location), "input");
}

TEST(DebugInfoDiscovery, FindsDWARFInASplitFileBesideAStrippedImage) {
  ScratchDir Dir;
  const auto Image = Dir.writeBytes("image", makeELFObjectExtentFixture());
  const auto Companion = Dir.writeBytes(
      "image.debug",
      makeELFDWARFReturnFixture(DWARFReturnFixtureKind::PointerSizeAbsent));
  const auto Found = findDWARF(Image, BinaryFormat::ELF);
  ASSERT_TRUE(Found);
  EXPECT_EQ(Found->Location, DWARFLocation::Beside);
  EXPECT_EQ(Found->Path, Companion);
  EXPECT_STREQ(dwarfLocationName(Found->Location), "beside");
}

TEST(DebugInfoDiscovery, FindsNoDWARFWithoutSectionsCompanionOrFormat) {
  ScratchDir Dir;
  const auto Image = Dir.writeBytes("image", makeELFObjectExtentFixture());
  // A companion without compile units holds no DWARF either.
  Dir.writeBytes("image.debug", makeELFObjectExtentFixture());
  EXPECT_FALSE(findDWARF(Image, BinaryFormat::ELF));
  // A PE carries its debug information in a PDB, which findDWARF never
  // reports, whatever the file holds.
  const auto PE = Dir.writeBytes(
      "probe.exe",
      makeELFDWARFReturnFixture(DWARFReturnFixtureKind::PointerSizeAbsent));
  EXPECT_FALSE(findDWARF(PE, BinaryFormat::COFF));
  EXPECT_FALSE(findDWARF(Dir.path("missing"), BinaryFormat::ELF));
}

TEST(DebugInfoDiscovery, DeclinedDWARFLeavesTheImageWithoutIt) {
  ScratchDir Dir;
  const auto Bytes =
      makeELFDWARFReturnFixture(DWARFReturnFixtureKind::PointerSizeAbsent);
  const auto Image = Dir.writeBytes("image", Bytes);
  const BinaryImage Img = makeELFImage(Bytes);

  const DebugInfoResult Read = loadDebugInfo(Image, Img);
  EXPECT_EQ(Read.Kind, DebugInfoKind::DWARF);
  EXPECT_TRUE(Read);

  DebugInfoRequest Declined;
  Declined.SearchDWARF = false;
  const DebugInfoResult Skipped = loadDebugInfo(Image, Img, Declined);
  EXPECT_EQ(Skipped.Kind, DebugInfoKind::None);
  EXPECT_FALSE(Skipped);
  EXPECT_TRUE(Skipped.Error.empty());
}

TEST(DebugInfoDiscovery, ImagesThatDescribeThemselvesReadNoDebugInfo) {
  EXPECT_TRUE(searchesDebugInfo(Arch::X64));
  EXPECT_TRUE(searchesDebugInfo(Arch::AArch64));
  EXPECT_FALSE(searchesDebugInfo(Arch::EVM));
  EXPECT_FALSE(searchesDebugInfo(Arch::SBF));
}

//===----------------------------------------------------------------------===//
// Linker MAPs held against their images, and the rank of their names
//===----------------------------------------------------------------------===//

/// The layout of a safety fixture image, as a MAP is held against it.
MapImageLayout fixtureLayout(llvm::StringRef Name) {
  auto Buffer = llvm::MemoryBuffer::getFile(safetyFixture(Name).string());
  if (!Buffer) {
    ADD_FAILURE() << "cannot read " << Name.str();
    return {};
  }
  auto Layout = readMapImageLayout((*Buffer)->getMemBufferRef());
  if (!Layout) {
    ADD_FAILURE() << llvm::toString(Layout.takeError());
    return {};
  }
  return std::move(*Layout);
}

std::string fixtureText(llvm::StringRef Name) {
  std::ifstream Input(safetyFixture(Name), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

std::vector<uint8_t> fixtureBytes(const std::filesystem::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(Input), {});
}

/// The separate-debug-file fixtures beside this test.
std::filesystem::path companionFixture(llvm::StringRef Name) {
  return std::filesystem::path(__FILE__).parent_path() / "fixtures" /
         Name.str();
}

/// Sets an environment variable for the life of the object.
class ScopedEnvironment {
public:
  ScopedEnvironment(const char *Name, const std::string &Value) : Name(Name) {
    if (const char *Old = std::getenv(Name))
      Previous = Old;
    set(Value.c_str());
  }
  ~ScopedEnvironment() { set(Previous ? Previous->c_str() : nullptr); }

private:
  void set(const char *Value) {
#ifdef _WIN32
    _putenv_s(Name, Value ? Value : "");
#else
    if (Value)
      ::setenv(Name, Value, 1);
    else
      ::unsetenv(Name);
#endif
  }

  const char *Name;
  std::optional<std::string> Previous;
};

TEST(MapFileTest, LinkerMapsHoldAgainstTheImagesTheyWereWrittenFor) {
  const std::pair<llvm::StringRef, llvm::StringRef> Pairs[] = {
      {"safety_cases_elf_x64", "safety_cases_elf_x64.map"},
      {"safety_cases_elf_arm64", "safety_cases_elf_arm64.map"},
      {"safety_cases_macho_x64", "safety_cases_macho_x64.map"},
      {"safety_cases_macho_arm64", "safety_cases_macho_arm64.map"},
      {"safety_cases_pe_x64.exe", "safety_cases_pe_x64.exe.map"},
      {"safety_cases_pe_arm64.exe", "safety_cases_pe_arm64.exe.map"}};
  for (const auto &[Image, Map] : Pairs) {
    SCOPED_TRACE(Map.str());
    const MapRead Read = readMapFile(safetyFixture(Map), fixtureLayout(Image));
    ASSERT_TRUE(Read.Context);
    EXPECT_EQ(Read.Check.Match, MapMatch::Consistent);
    EXPECT_EQ(Read.Check.Reason, MapMismatch::None);
    EXPECT_GT(Read.Check.Functions, 0u);
    EXPECT_EQ(Read.Check.Outside, 0u);
  }
}

TEST(MapFileTest, AMapThatPlacesASectionElsewhereDescribesAnotherBuild) {
  ScratchDir Dir;
  std::string Text = fixtureText("safety_cases_elf_x64.map");
  // The same link with .text one byte longer.
  const std::string Row = "1a70             1a70      351    16 .text";
  const size_t At = Text.find(Row);
  ASSERT_NE(At, std::string::npos);
  Text.replace(At, Row.size(), "1a70             1a70      352    16 .text");
  const MapRead Read = readMapFile(Dir.write("other.map", Text),
                                   fixtureLayout("safety_cases_elf_x64"));
  ASSERT_TRUE(Read.Context);
  EXPECT_EQ(Read.Check.Match, MapMatch::Mismatch);
  EXPECT_EQ(Read.Check.Reason, MapMismatch::Section);
  EXPECT_EQ(Read.Check.MapPlaced.Name, ".text");
  EXPECT_EQ(Read.Check.MapPlaced.Size, 0x352u);
  EXPECT_EQ(Read.Check.ImagePlaced.Size, 0x351u);
  EXPECT_EQ(describeMapMismatch(Read.Check),
            "it places .text at 0x1A70 (0x352 bytes), the image has .text at "
            "0x1A70 (0x351 bytes)");
}

// GNU ld lists a PE section at the size it takes in the file, rounded up to
// the file alignment, where lld lists its virtual size.
TEST(MapFileTest, AGNUMapSizesAPESectionAsTheFileHoldsIt) {
  MapImageLayout Layout;
  Layout.Format = BinaryFormat::COFF;
  Layout.Base = 0x140000000;
  Layout.Sections.push_back(
      {{}, ".text", 0x140001000, 0x1890, /*FileSize=*/0x1a00, true});
  Layout.Sections.push_back(
      {{}, ".bss", 0x140007000, 0x180, /*FileSize=*/0, false});
  const auto map = [](llvm::StringRef TextSize) {
    return "Linker script and memory map\n\n"
           ".text           0x0000000140001000     " +
           TextSize.str() +
           "\n"
           " .text          0x0000000140001480       0xc0 demo.o\n"
           "                0x0000000140001480                main\n"
           ".bss            0x0000000140007000      0x180\n";
  };
  ScratchDir Dir;
  for (llvm::StringRef Size : {"0x1a00", "0x1890"}) {
    SCOPED_TRACE(Size.str());
    const MapRead Read = readMapFile(Dir.write("demo.map", map(Size)), Layout);
    ASSERT_TRUE(Read.Context);
    EXPECT_EQ(Read.Facts.Dialect, MapDialect::GNU);
    EXPECT_EQ(Read.Check.Match, MapMatch::Consistent);
  }
  const MapRead Other =
      readMapFile(Dir.write("other.map", map("0x1c00")), Layout);
  EXPECT_EQ(Other.Check.Match, MapMatch::Mismatch);
  EXPECT_EQ(Other.Check.Reason, MapMismatch::Section);
}

TEST(MapFileTest, ALinkExeMapIsVerifiedByTheBuildStampItRecords) {
  const auto map = [](llvm::StringRef Stamp) {
    return " safety_cases_pe_x64\n\n Timestamp is " + Stamp.str() +
           " (Thu Oct 16 00:00:00 2026)\n\n"
           " Preferred load address is 0000000140000000\n\n"
           " Start         Length     Name                   Class\n"
           " 0001:00000000 000003d6H .text                   CODE\n\n"
           "  Address         Publics by Value              Rva+Base"
           "       Lib:Object\n\n"
           " 0001:00000250       main                       0000000140001250 "
           "f   safety_cases_pe_x64.obj\n";
  };
  ScratchDir Dir;
  const MapImageLayout Layout = fixtureLayout("safety_cases_pe_x64.exe");
  ASSERT_TRUE(Layout.Stamp);
  EXPECT_EQ(*Layout.Stamp, 0xb2a0cc38u);

  const MapRead Same =
      readMapFile(Dir.write("same.map", map("b2a0cc38")), Layout);
  ASSERT_TRUE(Same.Context);
  EXPECT_EQ(Same.Check.Match, MapMatch::Verified);
  ASSERT_TRUE(Same.Facts.Stamp);
  EXPECT_EQ(Same.Context->allFunctions().front().Addr, 0x140001250u);

  const MapRead Other =
      readMapFile(Dir.write("other.map", map("b2a0cc39")), Layout);
  EXPECT_EQ(Other.Check.Match, MapMatch::Mismatch);
  EXPECT_EQ(Other.Check.Reason, MapMismatch::Stamp);
  EXPECT_EQ(Other.Check.MapStamp, 0xb2a0cc39u);
  EXPECT_EQ(Other.Check.ImageStamp, 0xb2a0cc38u);
  EXPECT_EQ(describeMapMismatch(Other.Check),
            "it records the build stamp b2a0cc39, the image b2a0cc38");
}

// IDA's "Create MAP file" writes segment:offset publics without link.exe's
// Rva+Base column, numbers a PE's segments as the PE numbers its sections
// and states the address itself for an ELF image, and lists the names it made
// up from addresses beside the program's.
TEST(MapFileTest, IDAMapsPlaceTheirNamesWhereTheImageHasThem) {
  ScratchDir Dir;
  const MapRead PE = readMapFile(
      Dir.write("pe.map",
                " Start         Length     Name                   Class\n"
                " 0002:0000000000000000 000000000000003D6H .text"
                "                  CODE\n"
                " 0003:0000000000000000 000000000000002C0H .rdata"
                "                 DATA\n\n"
                "  Address         Publics by Value\n\n"
                " 00000001:0000000000000000       sub_140001000\n"
                " 00000001:0000000000000034       algn_140001034\n"
                " 00000001:0000000000000250       start\n"
                " 00000001:00000000000002B0       interproc_budget_entry\n"
                " 00000002:0000000000000000       VarName\n\n"
                "Program entry point at 0001:0000000000000250\n"),
      fixtureLayout("safety_cases_pe_x64.exe"));
  ASSERT_TRUE(PE.Context);
  const std::vector<FunctionSym> Functions = PE.Context->allFunctions();
  ASSERT_EQ(Functions.size(), 2u);
  EXPECT_EQ(Functions[0].Name, "start");
  EXPECT_EQ(Functions[0].Addr, 0x140001250u);
  EXPECT_EQ(Functions[1].Name, "interproc_budget_entry");
  EXPECT_EQ(Functions[1].Addr, 0x1400012b0u);
  const std::vector<DataObjectSym> Data = PE.Context->allDataObjects();
  ASSERT_EQ(Data.size(), 1u);
  EXPECT_EQ(Data[0].Name, "VarName");
  EXPECT_EQ(Data[0].Addr, 0x140002000u);
  EXPECT_EQ(PE.Check.Outside, 0u);

  const MapRead ELF = readMapFile(
      Dir.write("elf.map",
                " Start         Length     Name                   Class\n"
                " 0008:0000000000000000 00000000000000351H .text"
                "                  CODE\n\n"
                "  Address         Publics by Value\n\n"
                " 00000008:0000000000001A70       tainted_stack_overflow\n"
                " 00000008:0000000000001CE0       main\n"),
      fixtureLayout("safety_cases_elf_x64"));
  ASSERT_TRUE(ELF.Context);
  const std::vector<FunctionSym> ELFFunctions = ELF.Context->allFunctions();
  ASSERT_EQ(ELFFunctions.size(), 2u);
  EXPECT_EQ(ELFFunctions[0].Addr, 0x1a70u);
  EXPECT_EQ(ELFFunctions[1].Name, "main");
  EXPECT_EQ(ELFFunctions[1].Addr, 0x1ce0u);
}

/// A PE with its PDB, and a MAP named \p MapName beside it that calls main
/// otherwise, in \p Dir.
std::filesystem::path layOutPEWithPDBAndMap(ScratchDir &Dir,
                                            llvm::StringRef MapName) {
  const auto Image =
      Dir.writeBytes("safety_cases_pe_x64.exe",
                     fixtureBytes(safetyFixture("safety_cases_pe_x64.exe")));
  Dir.writeBytes("safety_cases_pe_x64.pdb",
                 fixtureBytes(safetyFixture("safety_cases_pe_x64.pdb")));
  std::string Map = fixtureText("safety_cases_pe_x64.exe.map");
  const std::string Main = "                 main";
  const size_t At = Map.find(Main);
  EXPECT_NE(At, std::string::npos);
  if (At != std::string::npos)
    Map.replace(At, Main.size(), "                 map_says_main");
  Dir.write(MapName, Map);
  return Image;
}

TEST(LoadDebugInfoTest, TheMapBesideAnImageItsPDBDescribesStaysUnread) {
  ScratchDir Dir;
  const auto Path = layOutPEWithPDBAndMap(Dir, "safety_cases_pe_x64.exe.map");
  auto Img = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());

  DebugInfoResult Found = loadDebugInfo(Path, *Img);
  ASSERT_TRUE(static_cast<bool>(Found)) << Found.Error;
  EXPECT_EQ(Found.Kind, DebugInfoKind::PDB);
  EXPECT_EQ(Found.Supplement, nullptr);
  ASSERT_EQ(Found.Files.size(), 1u);

  // Declined, the PDB leaves the MAP to name the image; declined too, the
  // MAP leaves it unnamed.
  DebugInfoRequest NoPDB;
  NoPDB.SearchPDB = false;
  const DebugInfoResult MapOnly = loadDebugInfo(Path, *Img, NoPDB);
  EXPECT_EQ(MapOnly.Kind, DebugInfoKind::Map);
  ASSERT_EQ(MapOnly.Files.size(), 1u);
  EXPECT_EQ(MapOnly.Files[0].Dialect, MapDialect::LLDCOFF);
  EXPECT_EQ(MapOnly.Files[0].Check.Match, MapMatch::Consistent);
  NoPDB.SearchMap = false;
  EXPECT_FALSE(loadDebugInfo(Path, *Img, NoPDB));
}

TEST(LoadDebugInfoTest, ANamedMapFillsOnlyWhatThePDBLeavesUnnamed) {
  ScratchDir Dir;
  const auto Path = layOutPEWithPDBAndMap(Dir, "named.map");
  auto Img = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());

  DebugInfoRequest Req;
  Req.MapPath = Dir.path("named.map");
  DebugInfoResult Found = loadDebugInfo(Path, *Img, Req);
  ASSERT_TRUE(static_cast<bool>(Found)) << Found.Error;
  EXPECT_EQ(Found.Kind, DebugInfoKind::PDB);
  ASSERT_NE(Found.Supplement, nullptr);
  ASSERT_EQ(Found.Files.size(), 2u);
  EXPECT_EQ(Found.Files[0].Kind, DebugInfoKind::PDB);
  EXPECT_EQ(Found.Files[1].Kind, DebugInfoKind::Map);

  applyDebugInfo(*Img, Found);
  const Symbol *Main = findFunc(*Img, 0x140001250);
  ASSERT_NE(Main, nullptr);
  EXPECT_EQ(Main->Name, "main");
  EXPECT_GT(Found.Files[0].Applied, 0u);
}

TEST(LoadDebugInfoTest, AMapOfAnotherBuildIsPassedOverOrRefused) {
  ScratchDir Dir;
  const auto Image =
      Dir.writeBytes("safety_cases_elf_x64",
                     fixtureBytes(safetyFixture("safety_cases_elf_x64")));
  std::string Text = fixtureText("safety_cases_elf_x64.map");
  const std::string Row = "1a70             1a70      351    16 .text";
  const size_t At = Text.find(Row);
  ASSERT_NE(At, std::string::npos);
  Text.replace(At, Row.size(), "1a70             1a70      352    16 .text");
  Dir.write("safety_cases_elf_x64.map", Text);
  auto Img = loadBinary(Image);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());

  // Beside the binary it is passed over.
  DebugInfoRequest NoDWARF;
  NoDWARF.SearchDWARF = false;
  const DebugInfoResult Beside = loadDebugInfo(Image, *Img, NoDWARF);
  EXPECT_FALSE(Beside);
  EXPECT_TRUE(Beside.Error.empty());

  // Named, it is refused, unless the request takes it unchecked.
  DebugInfoRequest Named = NoDWARF;
  Named.MapPath = Dir.path("safety_cases_elf_x64.map");
  const DebugInfoResult Refused = loadDebugInfo(Image, *Img, Named);
  EXPECT_FALSE(Refused);
  EXPECT_NE(Refused.Error.find("describes another build: it places .text"),
            std::string::npos)
      << Refused.Error;
  Named.UncheckedMap = true;
  const DebugInfoResult Taken = loadDebugInfo(Image, *Img, Named);
  ASSERT_TRUE(static_cast<bool>(Taken)) << Taken.Error;
  EXPECT_EQ(Taken.Kind, DebugInfoKind::Map);
  EXPECT_EQ(Taken.Files[0].Check.Match, MapMatch::Mismatch);
}

//===----------------------------------------------------------------------===//
// Separate DWARF files, as GDB finds them and as a user names them
//===----------------------------------------------------------------------===//

TEST(DebugInfoDiscovery, FindsTheCompanionADebugDirectoryKeepsByBuildID) {
  ScratchDir Dir;
  const auto Image = Dir.writeBytes(
      "companion.so", fixtureBytes(companionFixture("companion.so")));
  const auto Store = Dir.path("debug");
  const auto Kept = Store / ".build-id" / "76" /
                    "8168543f774654eb07639c4447a657c2bc40ae.debug";
  std::filesystem::create_directories(Kept.parent_path());
  std::filesystem::copy_file(companionFixture("companion.so.debug"), Kept);
  ScopedEnvironment Directories("NEVERD_DEBUG_FILE_DIRECTORY", Store.string());

  const auto Found = findDWARF(Image, BinaryFormat::ELF);
  ASSERT_TRUE(Found);
  EXPECT_EQ(Found->Location, DWARFLocation::BuildID);
  EXPECT_EQ(Found->Path, Kept);
  EXPECT_STREQ(dwarfLocationName(Found->Location), "build-id");

  auto Img = loadBinary(Image);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());
  const DebugInfoResult Read = loadDebugInfo(Image, *Img);
  ASSERT_TRUE(static_cast<bool>(Read)) << Read.Error;
  EXPECT_EQ(Read.Kind, DebugInfoKind::DWARF);
  EXPECT_EQ(Read.Path, Kept);
  EXPECT_FALSE(Read.Context->hasAuthenticatedObjectExtents());
}

TEST(DebugInfoDiscovery, FindsTheCompanionTheDebugLinkNamesByItsCRC) {
  ScratchDir Dir;
  ScopedEnvironment Directories("NEVERD_DEBUG_FILE_DIRECTORY",
                                Dir.path("none").string());
  const auto Image = Dir.writeBytes(
      "image.so", fixtureBytes(companionFixture("companion-nobuildid.so")));
  // The link names companion.debug, not a name the image's own suggests.
  const auto Linked = Dir.writeBytes(
      "companion.debug", fixtureBytes(companionFixture("companion.debug")));
  const auto Found = findDWARF(Image, BinaryFormat::ELF);
  ASSERT_TRUE(Found);
  EXPECT_EQ(Found->Location, DWARFLocation::DebugLink);
  EXPECT_EQ(Found->Path, Linked);

  // A file of that name with other contents is another build's.
  std::vector<uint8_t> Other =
      fixtureBytes(companionFixture("companion.debug"));
  Other.back() ^= 0xff;
  Dir.writeBytes("companion.debug", Other);
  EXPECT_FALSE(findDWARF(Image, BinaryFormat::ELF));
}

// Distribution debug packages ship their DWARF compressed; the reader sees
// the sections expanded whether or not the LLVM it uses has a decompressor.
TEST(LoadDebugInfoTest, CompressedDWARFSectionsReadExpanded) {
  ScratchDir Dir;
  ScopedEnvironment Directories("NEVERD_DEBUG_FILE_DIRECTORY",
                                Dir.path("none").string());
  const std::vector<uint8_t> Compressed =
      fixtureBytes(companionFixture("companion-zlib.debug"));
  const auto Expanded = expandCompressedSections(llvm::toStringRef(Compressed));
  ASSERT_TRUE(Expanded);
  EXPECT_GT(Expanded->size(), Compressed.size());
  const std::vector<uint8_t> Plain =
      fixtureBytes(companionFixture("companion.so.debug"));
  EXPECT_FALSE(expandCompressedSections(llvm::toStringRef(Plain)));
  EXPECT_TRUE(hasDWARFUnits(companionFixture("companion-zlib.debug")));

  const auto Image = Dir.writeBytes(
      "image.so", fixtureBytes(companionFixture("companion.so")));
  auto Img = loadBinary(Image);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());
  DebugInfoRequest Req;
  Req.DWARFPath = companionFixture("companion-zlib.debug");
  const DebugInfoResult Read = loadDebugInfo(Image, *Img, Req);
  ASSERT_TRUE(static_cast<bool>(Read)) << Read.Error;
  const std::vector<FunctionSym> Functions = Read.Context->allFunctions();
  EXPECT_TRUE(
      std::any_of(Functions.begin(), Functions.end(), [](const FunctionSym &F) {
        return F.Name == "also_named_by_dwarf";
      }));
}

TEST(LoadDebugInfoTest, CompressedDWARFExpansionBoundsAlignmentPadding) {
  std::vector<uint8_t> Compressed =
      fixtureBytes(companionFixture("companion-zlib.debug"));
  using ELF = llvm::object::ELF64LE;
  auto File = llvm::object::ELFFile<ELF>::create(llvm::toStringRef(Compressed));
  ASSERT_TRUE(static_cast<bool>(File)) << llvm::toString(File.takeError());
  auto Sections = File->sections();
  ASSERT_TRUE(static_cast<bool>(Sections))
      << llvm::toString(Sections.takeError());
  for (const auto &Section : *Sections) {
    if (!(Section.sh_flags & llvm::ELF::SHF_COMPRESSED))
      continue;
    const uint64_t Offset = Section.sh_offset;
    ASSERT_LE(Offset, Compressed.size());
    ASSERT_LE(sizeof(ELF::Chdr), Compressed.size() - Offset);
    ELF::Chdr Header;
    std::memcpy(&Header, Compressed.data() + Offset, sizeof(Header));
    Header.ch_addralign = 2 * limits::kMaxExpandedDebugSections;
    std::memcpy(Compressed.data() + Offset, &Header, sizeof(Header));
    EXPECT_FALSE(expandCompressedSections(llvm::toStringRef(Compressed)));
    return;
  }
  FAIL() << "fixture has no compressed section";
}

TEST(LoadDebugInfoTest, ANamedDWARFFileMustBeOfTheImagesBuildAndMachine) {
  ScratchDir Dir;
  ScopedEnvironment Directories("NEVERD_DEBUG_FILE_DIRECTORY",
                                Dir.path("none").string());
  const auto Image = Dir.writeBytes(
      "image.so", fixtureBytes(companionFixture("companion.so")));
  auto Img = loadBinary(Image);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());
  // Stripped and alone, the image has no DWARF to find.
  EXPECT_FALSE(loadDebugInfo(Image, *Img));

  DebugInfoRequest Req;
  Req.DWARFPath = companionFixture("companion.so.debug");
  const DebugInfoResult Read = loadDebugInfo(Image, *Img, Req);
  ASSERT_TRUE(static_cast<bool>(Read)) << Read.Error;
  EXPECT_EQ(Read.Kind, DebugInfoKind::DWARF);
  const std::vector<FunctionSym> Functions = Read.Context->allFunctions();
  EXPECT_TRUE(
      std::any_of(Functions.begin(), Functions.end(), [](const FunctionSym &F) {
        return F.Name == "named_by_dwarf";
      }));

  // Another build's: the companion's build ID with one byte changed.
  std::vector<uint8_t> Other =
      fixtureBytes(companionFixture("companion.so.debug"));
  const std::array<uint8_t, 4> BuildIDStart = {0x76, 0x81, 0x68, 0x54};
  auto At = std::search(Other.begin(), Other.end(), BuildIDStart.begin(),
                        BuildIDStart.end());
  ASSERT_NE(At, Other.end());
  *At ^= 0xff;
  Req.DWARFPath = Dir.writeBytes("other.debug", Other);
  const DebugInfoResult OtherBuild = loadDebugInfo(Image, *Img, Req);
  EXPECT_FALSE(OtherBuild);
  EXPECT_NE(OtherBuild.Error.find("another build"), std::string::npos)
      << OtherBuild.Error;

  // Another machine's.
  Req.DWARFPath = safetyFixture("safety_cases_elf_arm64");
  const DebugInfoResult OtherMachine = loadDebugInfo(Image, *Img, Req);
  EXPECT_FALSE(OtherMachine);
  EXPECT_NE(OtherMachine.Error.find("describes no x86_64 image"),
            std::string::npos)
      << OtherMachine.Error;

  // And a PE has no DWARF to name.
  Req.DWARFPath = companionFixture("companion.so.debug");
  BinaryImage PE = makePEImage();
  const DebugInfoResult NotELF = loadDebugInfo(Dir.path("probe.exe"), PE, Req);
  EXPECT_FALSE(NotELF);
  EXPECT_FALSE(NotELF.Error.empty());
}

} // anonymous namespace
