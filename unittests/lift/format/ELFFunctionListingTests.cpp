//===- ELFFunctionListingTests.cpp - ELF function listing tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/loader/ELF/ELFLoader.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr uint64_t ImageBase = 0x400000;
constexpr uint32_t TextOff = 0x100;
constexpr uint32_t DataOff = 0x180;
constexpr uint32_t StringOff = 0x200;
constexpr uint32_t SymbolOff = 0x300;
constexpr uint32_t DynamicSymbolOff = 0x500;
constexpr uint32_t SectionNameOff = 0x700;
constexpr uint32_t SectionOff = 0x800;

// Real ELF layouts, including locals-first .symtab ordering, without invoking
// a cross compiler or executing the sample. The dynamic table repeats defined
// functions, an IFUNC resolver and data, and also contains an undefined import.
template <typename ELFT>
std::vector<uint8_t> makeELFFunctionFixture(bool DynamicFirst,
                                            bool DynamicSizeIsZero,
                                            bool ExerciseIdentity) {
  using namespace llvm::ELF;
  using Ehdr = typename ELFT::Ehdr;
  using Phdr = typename ELFT::Phdr;
  using Shdr = typename ELFT::Shdr;
  using Sym = typename ELFT::Sym;
  std::vector<uint8_t> Bytes(SectionOff + 7 * sizeof(Shdr), 0);
  auto Put = [&](size_t Offset, const auto &Record) {
    std::memcpy(Bytes.data() + Offset, &Record, sizeof(Record));
  };

  Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, 4);
  Header.e_ident[EI_CLASS] = ELFT::Is64Bits ? ELFCLASS64 : ELFCLASS32;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_DYN;
  Header.e_machine = ELFT::Is64Bits ? EM_X86_64 : EM_386;
  Header.e_version = EV_CURRENT;
  Header.e_phoff = sizeof(Ehdr);
  Header.e_shoff = SectionOff;
  Header.e_ehsize = sizeof(Ehdr);
  Header.e_phentsize = sizeof(Phdr);
  Header.e_phnum = 1;
  Header.e_shentsize = sizeof(Shdr);
  Header.e_shnum = 7;
  Header.e_shstrndx = 6;
  Put(0, Header);

  Phdr Load{};
  Load.p_type = PT_LOAD;
  Load.p_flags = PF_R | PF_X;
  Load.p_vaddr = ImageBase;
  Load.p_paddr = ImageBase;
  Load.p_filesz = Bytes.size();
  Load.p_memsz = Bytes.size();
  Load.p_align = 0x1000;
  Put(sizeof(Ehdr), Load);
  std::fill_n(Bytes.begin() + TextOff, 0x40, 0xc3); // x86 ret

  std::string Strings(1, '\0');
  auto SetSymbol = [&](Sym &Symbol, llvm::StringRef Name, uint8_t Binding,
                       uint8_t Type, uint16_t Section, uint32_t Offset,
                       uint32_t Size) {
    Symbol.st_name = Strings.size();
    Strings.append(Name.data(), Name.size());
    Strings.push_back('\0');
    Symbol.setBindingAndType(Binding, Type);
    Symbol.st_shndx = Section;
    Symbol.st_value = ImageBase + Offset;
    Symbol.st_size = Size;
  };
  std::array<Sym, 10> Symbols{};
  SetSymbol(Symbols[1], "local", STB_LOCAL, STT_FUNC, 1, TextOff + 0x10, 4);
  SetSymbol(Symbols[2], "local", STB_LOCAL, STT_FUNC, 1, TextOff + 0x18, 4);
  SetSymbol(Symbols[3], "primary", ExerciseIdentity ? STB_LOCAL : STB_GLOBAL,
            STT_FUNC, 1, TextOff, 4);
  SetSymbol(Symbols[4], "alias", STB_WEAK, STT_FUNC, 1, TextOff, 4);
  SetSymbol(Symbols[5], "resolver", STB_GLOBAL, STT_GNU_IFUNC, 1, TextOff + 8,
            4);
  SetSymbol(Symbols[6], "object", STB_GLOBAL, STT_OBJECT, 2, DataOff, 8);
  // Even a nonzero address cannot turn an undefined symbol into a definition.
  SetSymbol(Symbols[7], "imported", STB_GLOBAL, STT_FUNC, SHN_UNDEF, TextOff,
            0);
  SetSymbol(Symbols[8], "label", STB_GLOBAL, STT_NOTYPE, 1, TextOff + 0x20, 0);
  SetSymbol(Symbols[9], "code_data", STB_GLOBAL, STT_OBJECT, 1, TextOff + 0x28,
            4);
  if (ExerciseIdentity) {
    // Distinct raw types may share a name and address. Keep OBJECT and NOTYPE
    // separate too: both become non-functions in the normalized symbol model.
    // The function is local here and global in .dynsym, with locals first.
    SetSymbol(Symbols[8], "primary", STB_GLOBAL, STT_NOTYPE, 1, TextOff, 0);
    SetSymbol(Symbols[9], "primary", STB_GLOBAL, STT_OBJECT, 1, TextOff, 4);
  }
  std::array<Sym, 5> DynamicSymbols{};
  SetSymbol(DynamicSymbols[1], "primary", STB_GLOBAL, STT_FUNC, 1, TextOff,
            DynamicSizeIsZero ? 0 : 4);
  SetSymbol(DynamicSymbols[2], "resolver", STB_GLOBAL, STT_GNU_IFUNC, 1,
            TextOff + 8, DynamicSizeIsZero ? 0 : 4);
  SetSymbol(DynamicSymbols[3], "object", STB_GLOBAL, STT_OBJECT, 2, DataOff, 8);
  SetSymbol(DynamicSymbols[4], "imported", STB_GLOBAL, STT_FUNC, SHN_UNDEF,
            TextOff, 0);
  Put(SymbolOff, Symbols);
  Put(DynamicSymbolOff, DynamicSymbols);
  std::memcpy(Bytes.data() + StringOff, Strings.data(), Strings.size());

  std::array<Shdr, 7> Sections{};
  std::string SectionNames(1, '\0');
  auto SetSection = [&](uint32_t Index, llvm::StringRef Name, uint32_t Type,
                        uint64_t Flags, uint64_t Address, uint32_t Offset,
                        uint64_t Size, uint32_t Alignment) {
    Shdr &Section = Sections[Index];
    Section.sh_name = SectionNames.size();
    SectionNames.append(Name.data(), Name.size());
    SectionNames.push_back('\0');
    Section.sh_type = Type;
    Section.sh_flags = Flags;
    Section.sh_addr = Address;
    Section.sh_offset = Offset;
    Section.sh_size = Size;
    Section.sh_addralign = Alignment;
  };
  SetSection(1, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
             ImageBase + TextOff, TextOff, 0x40, 16);
  SetSection(2, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
             ImageBase + DataOff, DataOff, 8, 8);
  SetSection(3, ".strtab", SHT_STRTAB, 0, 0, StringOff, Strings.size(), 1);
  SetSection(4, ".symtab", SHT_SYMTAB, 0, 0, SymbolOff, sizeof(Symbols),
             alignof(Sym));
  SetSection(5, ".dynsym", SHT_DYNSYM, 0, 0, DynamicSymbolOff,
             sizeof(DynamicSymbols), alignof(Sym));
  for (uint32_t Index : {4u, 5u}) {
    Sections[Index].sh_link = 3;
    Sections[Index].sh_info = Index == 4 ? 3 : 1;
    Sections[Index].sh_entsize = sizeof(Sym);
  }
  if (ExerciseIdentity)
    Sections[4].sh_info = 4;
  SetSection(6, ".shstrtab", SHT_STRTAB, 0, 0, SectionNameOff, 0, 1);
  Sections[6].sh_size = SectionNames.size();
  if (DynamicFirst)
    std::swap(Sections[4], Sections[5]);
  Put(SectionOff, Sections);
  std::memcpy(Bytes.data() + SectionNameOff, SectionNames.data(),
              SectionNames.size());
  return Bytes;
}

class ELFFunctionListingTest : public NeverDLiftTest {
protected:
  llvm::Expected<BinaryImage> loadFixture(bool Is64, bool DynamicFirst,
                                          bool DynamicSizeIsZero = false,
                                          bool ExerciseIdentity = false) {
    const auto Bytes =
        Is64 ? makeELFFunctionFixture<llvm::object::ELF64LE>(
                   DynamicFirst, DynamicSizeIsZero, ExerciseIdentity)
             : makeELFFunctionFixture<llvm::object::ELF32LE>(
                   DynamicFirst, DynamicSizeIsZero, ExerciseIdentity);
    const fs::path Path = tmpFile("function-listing.elf");
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()),
                 static_cast<std::streamsize>(Bytes.size()));
    Output.close();
    EXPECT_TRUE(Output.good());
    return ELFLoader().load(Path);
  }
};

TEST_F(ELFFunctionListingTest,
       MergesRepeatedDefinitionsAndKeepsAliasesInBothLayoutsAndTableOrders) {
  for (bool Is64 : {false, true}) {
    for (bool DynamicFirst : {false, true}) {
      for (bool DynamicSizeIsZero : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "ELF" << (Is64 ? 64 : 32)
                     << " dynamic first=" << DynamicFirst
                     << " dynamic size zero=" << DynamicSizeIsZero);
        auto ImageOrErr = loadFixture(Is64, DynamicFirst, DynamicSizeIsZero);
        ASSERT_TRUE(static_cast<bool>(ImageOrErr))
            << llvm::toString(ImageOrErr.takeError());
        const auto Functions = ImageOrErr->getFunctionSymbols();
        ASSERT_EQ(Functions.size(), 5u);
        std::vector<std::pair<std::string, va_t>> Actual;
        for (const Symbol *Function : Functions) {
          EXPECT_EQ(Function->Size, 4u);
          Actual.emplace_back(Function->Name, Function->Addr);
        }
        std::sort(Actual.begin(), Actual.end());
        const std::vector<std::pair<std::string, va_t>> Expected = {
            {"alias", ImageBase + TextOff},
            {"local", ImageBase + TextOff + 0x10},
            {"local", ImageBase + TextOff + 0x18},
            {"primary", ImageBase + TextOff},
            {"resolver", ImageBase + TextOff + 8}};
        EXPECT_EQ(Actual, Expected);
      }
    }
  }
}

TEST_F(
    ELFFunctionListingTest,
    KeepsIFuncClassificationWithoutUndefinedOrDataFunctionsOrDuplicateExports) {
  for (bool Is64 : {false, true}) {
    for (bool DynamicFirst : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "ELF" << (Is64 ? 64 : 32)
                                        << " dynamic first=" << DynamicFirst);
      auto ImageOrErr = loadFixture(Is64, DynamicFirst);
      ASSERT_TRUE(static_cast<bool>(ImageOrErr))
          << llvm::toString(ImageOrErr.takeError());
      EXPECT_EQ(ImageOrErr->Symbols.size(), 8u);
      for (const Symbol &Symbol : ImageOrErr->Symbols) {
        EXPECT_NE(Symbol.Name, "imported");
        if (Symbol.Name == "object" || Symbol.Name == "code_data" ||
            Symbol.Name == "label")
          EXPECT_FALSE(Symbol.IsFunc) << Symbol.Name;
      }
      EXPECT_TRUE(ImageOrErr->isRuntimeFunctionAt(ImageBase + TextOff + 8));
      std::vector<std::pair<std::string, va_t>> Actual;
      for (const Export &Export : ImageOrErr->Exports)
        Actual.emplace_back(Export.Name, Export.Addr);
      std::sort(Actual.begin(), Actual.end());
      const std::vector<std::pair<std::string, va_t>> Expected = {
          {"alias", ImageBase + TextOff},
          {"primary", ImageBase + TextOff},
          {"resolver", ImageBase + TextOff + 8}};
      EXPECT_EQ(Actual, Expected);
    }
  }
}

TEST_F(ELFFunctionListingTest,
       KeepsDifferentTypesAndExportsRepeatedLocalGlobalDefinitions) {
  for (bool Is64 : {false, true}) {
    for (bool DynamicFirst : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "ELF" << (Is64 ? 64 : 32)
                                        << " dynamic first=" << DynamicFirst);
      auto ImageOrErr = loadFixture(Is64, DynamicFirst,
                                    /*DynamicSizeIsZero=*/false,
                                    /*ExerciseIdentity=*/true);
      ASSERT_TRUE(static_cast<bool>(ImageOrErr))
          << llvm::toString(ImageOrErr.takeError());
      size_t FunctionCount = 0;
      size_t DataCount = 0;
      for (const Symbol &Symbol : ImageOrErr->Symbols) {
        if (Symbol.Name != "primary" || Symbol.Addr != ImageBase + TextOff)
          continue;
        if (Symbol.IsFunc)
          ++FunctionCount;
        else
          ++DataCount;
      }
      EXPECT_EQ(FunctionCount, 1u);
      EXPECT_EQ(DataCount, 2u);
      EXPECT_EQ(std::count_if(ImageOrErr->Exports.begin(),
                              ImageOrErr->Exports.end(),
                              [](const Export &Export) {
                                return Export.Name == "primary" &&
                                       Export.Addr == ImageBase + TextOff;
                              }),
                1);
    }
  }
}

} // namespace
