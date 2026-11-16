//===- StaticSymbolTableTests.cpp - Images read without symbol tables -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A load that declines the static symbol table reads the image exactly as it
// reads a copy that no longer has one, keeps what dynamic linking needs, and
// refuses an object file, whose relocations need the table.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/StaticSymbolTable.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/Process.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace neverd;

namespace {

using ELFT = llvm::object::ELF64LE;

std::filesystem::path safetyFixture(const char *Name) {
  return std::filesystem::path(__FILE__).parent_path().parent_path() /
         "safety" / "fixtures" / "binaries" / Name;
}

std::vector<char> readBytes(const std::filesystem::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Input), {}};
}

/// A copy of an ELF64 image whose .symtab no loader reads: its section
/// becomes plain bytes, as if strip had taken the table out.
std::vector<char> withoutSymbolTable(std::vector<char> Bytes) {
  ELFT::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  for (unsigned Index = 0; Index < Header.e_shnum; ++Index) {
    ELFT::Shdr Section;
    const size_t At = Header.e_shoff + Index * Header.e_shentsize;
    std::memcpy(&Section, Bytes.data() + At, sizeof(Section));
    if (Section.sh_type != llvm::ELF::SHT_SYMTAB)
      continue;
    Section.sh_type = llvm::ELF::SHT_PROGBITS;
    std::memcpy(Bytes.data() + At, &Section, sizeof(Section));
  }
  return Bytes;
}

/// A copy of a 64-bit Mach-O image in which the exported symbol \p Name is
/// local, one of the symbols strip removes; its export trie still exports it.
std::vector<char> withLocalSymbol(std::vector<char> Bytes,
                                  llvm::StringRef Name) {
  llvm::MachO::mach_header_64 Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  size_t At = sizeof(Header);
  for (uint32_t Index = 0; Index < Header.ncmds; ++Index) {
    llvm::MachO::load_command Command;
    std::memcpy(&Command, Bytes.data() + At, sizeof(Command));
    if (Command.cmd == llvm::MachO::LC_SYMTAB) {
      llvm::MachO::symtab_command Table;
      std::memcpy(&Table, Bytes.data() + At, sizeof(Table));
      for (uint32_t Symbol = 0; Symbol < Table.nsyms; ++Symbol) {
        const size_t Entry =
            Table.symoff + Symbol * sizeof(llvm::MachO::nlist_64);
        llvm::MachO::nlist_64 List;
        std::memcpy(&List, Bytes.data() + Entry, sizeof(List));
        if (Name == Bytes.data() + Table.stroff + List.n_strx) {
          List.n_type &= ~llvm::MachO::N_EXT;
          std::memcpy(Bytes.data() + Entry, &List, sizeof(List));
        }
      }
    }
    At += Command.cmdsize;
  }
  return Bytes;
}

BinaryImage load(const std::vector<char> &Bytes, bool ReadStaticSymbols) {
  BinaryLoadOptions Options;
  Options.ReadStaticSymbols = ReadStaticSymbols;
  auto Image = loadBinaryBuffer(
      llvm::MemoryBufferRef(llvm::StringRef(Bytes.data(), Bytes.size()),
                            "image"),
      Options);
  EXPECT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  return Image ? std::move(*Image) : BinaryImage();
}

std::set<std::string> symbolNames(const BinaryImage &Image) {
  std::set<std::string> Names;
  for (const auto &Symbol : Image.Symbols)
    Names.insert(Symbol.Name);
  return Names;
}

std::set<std::string> exportNames(const BinaryImage &Image) {
  std::set<std::string> Names;
  for (const auto &Export : Image.Exports)
    Names.insert(Export.Name);
  return Names;
}

/// A relocatable object with one function its symbol table names.
std::vector<char> relocatableObject() {
  using namespace llvm::ELF;
  static const char SectionTable[] = "\0.text\0.symtab\0.strtab\0.shstrtab";
  static const char SymbolTable[] = "\0function";
  const std::string SectionNames(SectionTable, sizeof(SectionTable));
  const std::string SymbolNames(SymbolTable, sizeof(SymbolTable));
  constexpr unsigned TextName = 1, SymtabName = 7, StrtabName = 15,
                     ShstrtabName = 23, FunctionName = 1;
  const char Text[] = {'\xC3'};
  ELFT::Sym Symbols[2] = {};
  Symbols[1].st_name = FunctionName;
  Symbols[1].setBindingAndType(STB_GLOBAL, STT_FUNC);
  Symbols[1].st_shndx = 1;
  Symbols[1].st_size = sizeof(Text);

  std::vector<char> Bytes(sizeof(ELFT::Ehdr));
  const auto append = [&](const void *Data, size_t Size) {
    const size_t At = Bytes.size();
    Bytes.insert(Bytes.end(), static_cast<const char *>(Data),
                 static_cast<const char *>(Data) + Size);
    return At;
  };
  const size_t TextAt = append(Text, sizeof(Text));
  Bytes.resize(llvm::alignTo(Bytes.size(), 8));
  const size_t SymtabAt = append(Symbols, sizeof(Symbols));
  const size_t StrtabAt = append(SymbolNames.data(), SymbolNames.size());
  const size_t ShstrtabAt = append(SectionNames.data(), SectionNames.size());
  Bytes.resize(llvm::alignTo(Bytes.size(), 8));

  ELFT::Shdr Sections[5] = {};
  Sections[1].sh_name = TextName;
  Sections[1].sh_type = SHT_PROGBITS;
  Sections[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
  Sections[1].sh_offset = TextAt;
  Sections[1].sh_size = sizeof(Text);
  Sections[1].sh_addralign = 1;
  Sections[2].sh_name = SymtabName;
  Sections[2].sh_type = SHT_SYMTAB;
  Sections[2].sh_offset = SymtabAt;
  Sections[2].sh_size = sizeof(Symbols);
  Sections[2].sh_link = 3;
  Sections[2].sh_info = 1;
  Sections[2].sh_entsize = sizeof(ELFT::Sym);
  Sections[2].sh_addralign = 8;
  Sections[3].sh_name = StrtabName;
  Sections[3].sh_type = SHT_STRTAB;
  Sections[3].sh_offset = StrtabAt;
  Sections[3].sh_size = SymbolNames.size();
  Sections[3].sh_addralign = 1;
  Sections[4].sh_name = ShstrtabName;
  Sections[4].sh_type = SHT_STRTAB;
  Sections[4].sh_offset = ShstrtabAt;
  Sections[4].sh_size = SectionNames.size();
  Sections[4].sh_addralign = 1;
  const size_t SectionsAt = append(Sections, sizeof(Sections));

  ELFT::Ehdr Header = {};
  std::memcpy(Header.e_ident, ElfMagic, std::strlen(ElfMagic));
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_REL;
  Header.e_machine = EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_ehsize = sizeof(ELFT::Ehdr);
  Header.e_shoff = SectionsAt;
  Header.e_shentsize = sizeof(ELFT::Shdr);
  Header.e_shnum = 5;
  Header.e_shstrndx = 4;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  return Bytes;
}

class ScratchFile {
public:
  ScratchFile(const char *Name, const std::vector<char> &Bytes)
      : Path(std::filesystem::temp_directory_path() /
             ("neverd-static-symbols-" +
              std::to_string(llvm::sys::Process::getProcessId()) + "-" +
              Name)) {
    std::ofstream(Path, std::ios::binary)
        .write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
  }
  ~ScratchFile() {
    std::error_code Ignored;
    std::filesystem::remove(Path, Ignored);
  }
  const std::filesystem::path Path;
};

TEST(StaticSymbolTableTest, ReferenceNamesAnELFImagesSymtab) {
  auto Found = findStaticSymbolTable(safetyFixture("safety_cases_elf_x64"), 4);
  ASSERT_TRUE(static_cast<bool>(Found)) << llvm::toString(Found.takeError());
  ASSERT_TRUE(Found->has_value());
  EXPECT_EQ((*Found)->Table, ".symtab");
  EXPECT_GT((*Found)->Count, 0u);
  EXPECT_FALSE((*Found)->Names.empty());
  EXPECT_LE((*Found)->Names.size(), 4u);
  // Every name it offers loads when the table is read.
  const auto Read =
      symbolNames(load(readBytes(safetyFixture("safety_cases_elf_x64")),
                       /*ReadStaticSymbols=*/true));
  for (const auto &Name : (*Found)->Names)
    EXPECT_TRUE(Read.count(Name)) << Name;
}

TEST(StaticSymbolTableTest, DeclinedELFReadsAsTheImageWithoutItsTable) {
  const auto Bytes = readBytes(safetyFixture("safety_cases_elf_x64"));
  const auto Read = load(Bytes, /*ReadStaticSymbols=*/true);
  const auto Declined = load(Bytes, /*ReadStaticSymbols=*/false);
  const auto Stripped = load(withoutSymbolTable(Bytes), true);
  EXPECT_TRUE(Read.ReadsStaticSymbols);
  EXPECT_FALSE(Declined.ReadsStaticSymbols);
  EXPECT_EQ(symbolNames(Declined), symbolNames(Stripped));
  EXPECT_EQ(exportNames(Declined), exportNames(Stripped));
  EXPECT_EQ(Declined.ExactDataObjects.size(), Stripped.ExactDataObjects.size());
  // The table held names the dynamic symbols do not.
  EXPECT_LT(symbolNames(Declined).size(), symbolNames(Read).size());
  // Dynamic linking still has what it needs.
  EXPECT_EQ(Declined.Imports.size(), Read.Imports.size());
}

TEST(StaticSymbolTableTest, ObjectFileCannotDeclineItsTable) {
  const auto Object = relocatableObject();
  BinaryLoadOptions Options;
  Options.ReadStaticSymbols = false;
  auto Declined = loadBinaryBuffer(
      llvm::MemoryBufferRef(llvm::StringRef(Object.data(), Object.size()),
                            "object"),
      Options);
  ASSERT_FALSE(static_cast<bool>(Declined));
  EXPECT_NE(llvm::toString(Declined.takeError()).find("object file"),
            std::string::npos);
  EXPECT_TRUE(symbolNames(load(Object, true)).count("function"));
  // Nor does a frontend ask about it.
  const ScratchFile File("object.o", Object);
  auto Found = findStaticSymbolTable(File.Path, 8);
  ASSERT_TRUE(static_cast<bool>(Found)) << llvm::toString(Found.takeError());
  EXPECT_FALSE(Found->has_value());
}

TEST(StaticSymbolTableTest, DeclinedMachOKeepsWhatItExports) {
  constexpr const char *Local = "_interproc_budget_entry";
  const auto Original = readBytes(safetyFixture("safety_cases_macho_x64"));
  const auto Bytes = withLocalSymbol(Original, Local);
  const ScratchFile File("macho", Bytes);
  auto Found = findStaticSymbolTable(File.Path, 8);
  ASSERT_TRUE(static_cast<bool>(Found)) << llvm::toString(Found.takeError());
  ASSERT_TRUE(Found->has_value());
  EXPECT_EQ((*Found)->Table, "LC_SYMTAB");
  EXPECT_EQ((*Found)->Count, 1u);
  EXPECT_EQ((*Found)->Names, std::vector<std::string>{Local});
  // Declined, the local symbol is gone, while the export trie, which strip
  // keeps, still names the function.
  EXPECT_TRUE(symbolNames(load(Bytes, true)).count(Local));
  const auto Declined = load(Bytes, false);
  EXPECT_FALSE(symbolNames(Declined).count(Local));
  EXPECT_TRUE(exportNames(Declined).count(Local));
  // The fixture exports all it defines: no table to ask about, and nothing
  // a declining load leaves out.
  auto Exported =
      findStaticSymbolTable(safetyFixture("safety_cases_macho_x64"), 8);
  ASSERT_TRUE(static_cast<bool>(Exported))
      << llvm::toString(Exported.takeError());
  EXPECT_FALSE(Exported->has_value());
  EXPECT_EQ(symbolNames(load(Original, false)),
            symbolNames(load(Original, true)));
}

TEST(StaticSymbolTableTest, ImageWithoutOneHasNone) {
  // An MSVC image carries no COFF symbol table, and a stripped ELF no .symtab.
  auto PE = findStaticSymbolTable(safetyFixture("safety_cases_pe_x64.exe"), 8);
  ASSERT_TRUE(static_cast<bool>(PE)) << llvm::toString(PE.takeError());
  EXPECT_FALSE(PE->has_value());
  const ScratchFile Stripped(
      "stripped",
      withoutSymbolTable(readBytes(safetyFixture("safety_cases_elf_x64"))));
  auto ELF = findStaticSymbolTable(Stripped.Path, 8);
  ASSERT_TRUE(static_cast<bool>(ELF)) << llvm::toString(ELF.takeError());
  EXPECT_FALSE(ELF->has_value());
  auto Missing = findStaticSymbolTable(Stripped.Path.string() + ".missing", 8);
  EXPECT_FALSE(static_cast<bool>(Missing));
  llvm::consumeError(Missing.takeError());
}

TEST(StaticSymbolTableTest, MinGWImageNamesNoSectionOrFileRecord) {
  // GNU ld keeps a static symbol for each input section, at the address of
  // the object's first function or variable, and the object's .file record.
  // A section's symbol still bounds what the object contributed but names
  // nothing; the .file record is neither a name nor a place.  A static
  // function's own record follows its auxiliary function definition as a
  // section's follows a section definition, and names that function.
  const auto Path = std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                    "mingw_sections.exe";
  const auto Image = load(readBytes(Path), /*ReadStaticSymbols=*/true);
  std::set<std::string> Names, Sections;
  for (const auto &Symbol : Image.Symbols)
    (Symbol.IsSection ? Sections : Names).insert(Symbol.Name);
  for (const char *Name : {"twice", "bump", "entry", "counter", "hidden_total"})
    EXPECT_TRUE(Names.count(Name)) << Name;
  for (const char *Name : {".text", ".data", ".bss"})
    EXPECT_TRUE(Sections.count(Name)) << Name;
  for (const auto &Name : Names)
    EXPECT_NE(Name.front(), '.') << Name;
  EXPECT_FALSE(Names.count("mingw_sections.c"));
  EXPECT_FALSE(Sections.count("mingw_sections.c"));
  // What names an address passes over its section's symbol.
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsSection)
      if (const auto *Named = Image.findSymbolAt(Symbol.Addr))
        EXPECT_FALSE(Named->IsSection) << Named->Name;
  const auto *Bump = Image.findSymbol("bump");
  ASSERT_NE(Bump, nullptr);
  const auto *Twice = Image.findSymbol("twice");
  ASSERT_NE(Twice, nullptr);
  EXPECT_EQ(Image.getFunctionNameAt(Twice->Addr), "twice");

  auto Found = findStaticSymbolTable(Path, 16);
  ASSERT_TRUE(static_cast<bool>(Found)) << llvm::toString(Found.takeError());
  ASSERT_TRUE(Found->has_value());
  EXPECT_EQ((*Found)->Table, "COFF symbol table");
  // The prompt counts the names the load reads, the image's exports aside.
  const auto Declined = symbolNames(load(readBytes(Path), false));
  size_t Read = 0;
  for (const auto &Symbol : Image.Symbols)
    Read += !Symbol.IsSection && !Declined.count(Symbol.Name);
  EXPECT_EQ((*Found)->Count, Read);
  for (const auto &Name : (*Found)->Names)
    EXPECT_TRUE(Names.count(Name)) << Name;
}

TEST(StaticSymbolTableTest, MinGWDataAmongCodeIsData) {
  // GNU ld keeps __CTOR_LIST__, a table of constructors headed by -1, in
  // .text; the fixtures, built for x86-64 and i386, keep such a table there
  // and point at it from .data.  The base relocations, read before the symbol
  // table, take the pointer to the table and the pointer in it for pointers
  // in code.  The table's symbol, of no function type, starting no
  // instruction, says both are data.
  for (const auto &[File, Prefix] :
       {std::pair<const char *, std::string>{"mingw_text_data.exe", ""},
        std::pair<const char *, std::string>{"mingw_text_data32.exe", "_"}}) {
    SCOPED_TRACE(File);
    const auto Path =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / File;
    const auto Image = load(readBytes(Path), /*ReadStaticSymbols=*/true);
    const auto *Table = Image.findSymbol(Prefix + "text_table");
    const auto *Ref = Image.findSymbol(Prefix + "table_ref");
    const auto *Twice = Image.findSymbol(Prefix + "twice");
    ASSERT_NE(Table, nullptr);
    ASSERT_NE(Ref, nullptr);
    ASSERT_NE(Twice, nullptr);
    EXPECT_TRUE(Image.DataInCode.count(Table->Addr));
    EXPECT_FALSE(Image.DataInCode.count(Twice->Addr));
    // The pointer to the table points to data.
    EXPECT_TRUE(Image.DataPtrRelocSlots.count(Ref->Addr));
    EXPECT_FALSE(Image.CodePtrRelocSlots.count(Ref->Addr));
    // The table's entry is a slot that holds code, not an operand.
    const va_t Entry = Table->Addr + Image.getPointerSize();
    EXPECT_TRUE(Image.CodePtrRelocSlots.count(Entry));
    EXPECT_FALSE(Image.CodeAddressRelocOperands.count(Entry));

    // Declined, the symbol table says nothing of the table.
    const auto Declined = load(readBytes(Path), /*ReadStaticSymbols=*/false);
    EXPECT_TRUE(Declined.DataInCode.empty());
    EXPECT_TRUE(Declined.CodePtrRelocSlots.count(Ref->Addr));
    EXPECT_TRUE(Declined.CodeAddressRelocOperands.count(Entry));
  }
}

TEST(StaticSymbolTableTest, MinGWImportVariableNoCodeChangesIsConstant) {
  // The fixture calls `add_one` and `twice` through `__imp_X` variables, as
  // MinGW's runtime defines them for the functions it supplies in place of an
  // import (__MINGW_IMP_SYMBOL).  `__imp_twice` starts at a lazy binding that
  // rewrites it; nothing changes `__imp_add_one`.
  const auto Path = std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                    "mingw_imp_slots.exe";
  const auto Image = load(readBytes(Path), /*ReadStaticSymbols=*/true);
  const auto *Constant = Image.findSymbol("__imp_add_one");
  const auto *Lazy = Image.findSymbol("__imp_twice");
  const auto *AddOne = Image.findSymbol("add_one");
  ASSERT_NE(Constant, nullptr);
  ASSERT_NE(Lazy, nullptr);
  ASSERT_NE(AddOne, nullptr);
  ASSERT_EQ(Image.ConstantCodeSlots.size(), 1u);
  ASSERT_TRUE(Image.ConstantCodeSlots.count(Constant->Addr));
  EXPECT_EQ(Image.ConstantCodeSlots.at(Constant->Addr), AddOne->Addr);
  EXPECT_FALSE(Image.ConstantCodeSlots.count(Lazy->Addr));
  // Declined, the symbol table names no such variable.
  EXPECT_TRUE(load(readBytes(Path), /*ReadStaticSymbols=*/false)
                  .ConstantCodeSlots.empty());
}

/// A linked image whose load segment covers `.text` and whose debug section
/// sits outside that segment.
std::vector<char> linkedImageWithUnmappedDebug() {
  using namespace llvm::ELF;
  static const char SectionTable[] = "\0.text\0.debug_info\0.shstrtab";
  const std::string SectionNames(SectionTable, sizeof(SectionTable));
  constexpr unsigned TextName = 1;
  constexpr unsigned DebugName = 7;
  constexpr unsigned ShstrtabName = 19;
  const char Text[] = {'\xAB'};
  const char Debug[] = {'\x01', '\x02', '\x03', '\x04'};

  std::vector<char> Bytes(sizeof(ELFT::Ehdr) + sizeof(ELFT::Phdr), 0);
  const auto append = [&](const void *Data, size_t Size) {
    const size_t At = Bytes.size();
    Bytes.insert(Bytes.end(), static_cast<const char *>(Data),
                 static_cast<const char *>(Data) + Size);
    return At;
  };
  Bytes.resize(0x100, 0);
  const size_t TextAt = append(Text, sizeof(Text));
  Bytes.resize(0x200, 0);
  const size_t DebugAt = append(Debug, sizeof(Debug));
  const size_t ShstrtabAt = append(SectionNames.data(), SectionNames.size());
  Bytes.resize(llvm::alignTo(Bytes.size(), 8));

  ELFT::Shdr Sections[4] = {};
  Sections[1].sh_name = TextName;
  Sections[1].sh_type = SHT_PROGBITS;
  Sections[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
  Sections[1].sh_addr = 0x1000;
  Sections[1].sh_offset = TextAt;
  Sections[1].sh_size = sizeof(Text);
  Sections[1].sh_addralign = 1;
  Sections[2].sh_name = DebugName;
  Sections[2].sh_type = SHT_PROGBITS;
  Sections[2].sh_offset = DebugAt;
  Sections[2].sh_size = sizeof(Debug);
  Sections[2].sh_addralign = 1;
  Sections[3].sh_name = ShstrtabName;
  Sections[3].sh_type = SHT_STRTAB;
  Sections[3].sh_offset = ShstrtabAt;
  Sections[3].sh_size = SectionNames.size();
  Sections[3].sh_addralign = 1;
  const size_t SectionsAt = append(Sections, sizeof(Sections));

  ELFT::Phdr Load = {};
  Load.p_type = PT_LOAD;
  Load.p_flags = PF_R | PF_X;
  Load.p_offset = TextAt;
  Load.p_vaddr = 0x1000;
  Load.p_paddr = 0x1000;
  Load.p_filesz = sizeof(Text);
  Load.p_memsz = sizeof(Text);
  Load.p_align = 1;

  ELFT::Ehdr Header = {};
  std::memcpy(Header.e_ident, ElfMagic, std::strlen(ElfMagic));
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_DYN;
  Header.e_machine = EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_ehsize = sizeof(ELFT::Ehdr);
  Header.e_phoff = sizeof(ELFT::Ehdr);
  Header.e_shoff = SectionsAt;
  Header.e_phentsize = sizeof(ELFT::Phdr);
  Header.e_phnum = 1;
  Header.e_shentsize = sizeof(ELFT::Shdr);
  Header.e_shnum = 4;
  Header.e_shstrndx = 3;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), &Load, sizeof(Load));
  return Bytes;
}

const Section *sectionNamed(const BinaryImage &Image, llvm::StringRef Name) {
  for (const Section &Sec : Image.Sections)
    if (Sec.Name == Name)
      return &Sec;
  return nullptr;
}

TEST(StaticSymbolTableTest, LinkedImageDoesNotRepeatMappedOrDebugBytes) {
  const auto Image =
      load(linkedImageWithUnmappedDebug(), /*ReadStaticSymbols=*/true);
  const Section *Text = sectionNamed(Image, ".text");
  const Section *Debug = sectionNamed(Image, ".debug_info");
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(Debug, nullptr);
  EXPECT_TRUE(Text->Data.empty());
  EXPECT_TRUE(Debug->Data.empty());
  const uint8_t *Byte = Image.readVA(Text->VA, 1);
  ASSERT_NE(Byte, nullptr);
  EXPECT_EQ(*Byte, 0xAB);

  const auto Object = load(relocatableObject(), /*ReadStaticSymbols=*/true);
  const Section *ObjectText = sectionNamed(Object, ".text");
  ASSERT_NE(ObjectText, nullptr);
  ASSERT_EQ(ObjectText->Data.size(), 1u);
  EXPECT_EQ(ObjectText->Data[0], static_cast<uint8_t>('\xC3'));
}

} // namespace
