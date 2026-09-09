//===- MachOFunctionListingTests.cpp - Mach-O function listing tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOLoader.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace neverd;

constexpr uint64_t kImageBase = 0x100000000ULL;
constexpr uint32_t kTextOff = 0x400;
constexpr uint32_t kFunctionSize = 8;
constexpr uint32_t kConstOff = 0x500;
constexpr uint32_t kUnwindOff = 0x600;
constexpr uint32_t kLinkeditOff = 0x800;
constexpr uint32_t kSymtabOff = kLinkeditOff;
constexpr uint32_t kStringTableOff = kLinkeditOff + 0x40;
constexpr uint32_t kFileSize = 0x900;

void setMachOName(char (&Destination)[16], llvm::StringRef Name) {
  ASSERT_LE(Name.size(), sizeof(Destination));
  std::memset(Destination, 0, sizeof(Destination));
  std::memcpy(Destination, Name.data(), Name.size());
}

std::vector<uint8_t> makeCompactUnwind() {
  constexpr uint32_t IndexOff = 28;
  constexpr uint32_t PageOff = IndexOff + 24;
  constexpr uint32_t EntriesOff = PageOff + 8;
  constexpr uint32_t EntryCount = 2;
  constexpr uint32_t EndOff = kTextOff + EntryCount * kFunctionSize;
  constexpr uint32_t UnwindSize = EntriesOff + EntryCount * 8;

  std::vector<uint8_t> Bytes(UnwindSize, 0);
  auto Write32 = [&](uint32_t Off, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + Off, Value);
  };
  auto Write16 = [&](uint32_t Off, uint16_t Value) {
    llvm::support::endian::write16le(Bytes.data() + Off, Value);
  };

  Write32(0, 1);         // version
  Write32(4, IndexOff);  // common encodings
  Write32(8, 0);         // common encoding count
  Write32(12, IndexOff); // personality array
  Write32(16, 0);        // personality count
  Write32(20, IndexOff); // top-level index
  Write32(24, 2);        // one page plus sentinel
  Write32(IndexOff, kTextOff);
  Write32(IndexOff + 4, PageOff);
  Write32(IndexOff + 8, UnwindSize);
  Write32(IndexOff + 12, EndOff);
  Write32(IndexOff + 16, 0);
  Write32(IndexOff + 20, UnwindSize);
  Write32(PageOff, 2);     // UNWIND_SECOND_LEVEL_REGULAR
  Write16(PageOff + 4, 8); // entries start after the page header
  Write16(PageOff + 6, EntryCount);
  for (uint32_t I = 0; I < EntryCount; ++I) {
    Write32(EntriesOff + I * 8, kTextOff + I * kFunctionSize);
    Write32(EntriesOff + I * 8 + 4, macho_unwind::kARM64ModeFrameless);
  }
  return Bytes;
}

std::vector<uint8_t> makeMachOFunctionFixture() {
  using namespace llvm::MachO;

  constexpr uint32_t SectionCount = 3;
  constexpr uint32_t TextCommandSize =
      sizeof(segment_command_64) + SectionCount * sizeof(section_64);
  constexpr uint32_t LinkeditCommandSize = sizeof(segment_command_64);
  constexpr uint32_t CommandSize =
      TextCommandSize + LinkeditCommandSize + sizeof(symtab_command);

  std::vector<uint8_t> Binary(kFileSize, 0);
  auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
  Header->magic = MH_MAGIC_64;
  Header->cputype = CPU_TYPE_ARM64;
  Header->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
  Header->filetype = MH_EXECUTE;
  Header->ncmds = 3;
  Header->sizeofcmds = CommandSize;
  Header->flags = MH_NOUNDEFS;

  uint8_t *Command = Binary.data() + sizeof(mach_header_64);
  auto *TextSegment = reinterpret_cast<segment_command_64 *>(Command);
  TextSegment->cmd = LC_SEGMENT_64;
  TextSegment->cmdsize = TextCommandSize;
  setMachOName(TextSegment->segname, "__TEXT");
  TextSegment->vmaddr = kImageBase;
  TextSegment->vmsize = kLinkeditOff;
  TextSegment->fileoff = 0;
  TextSegment->filesize = kLinkeditOff;
  TextSegment->maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
  TextSegment->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  TextSegment->nsects = SectionCount;

  auto *Sections = reinterpret_cast<section_64 *>(TextSegment + 1);
  setMachOName(Sections[0].sectname, "__text");
  setMachOName(Sections[0].segname, "__TEXT");
  Sections[0].addr = kImageBase + kTextOff;
  Sections[0].size = 2 * kFunctionSize;
  Sections[0].offset = kTextOff;
  Sections[0].align = 2;
  Sections[0].flags = static_cast<uint32_t>(S_REGULAR) |
                      static_cast<uint32_t>(S_ATTR_PURE_INSTRUCTIONS);

  setMachOName(Sections[1].sectname, "__const");
  setMachOName(Sections[1].segname, "__TEXT");
  Sections[1].addr = kImageBase + kConstOff;
  Sections[1].size = sizeof(uint32_t);
  Sections[1].offset = kConstOff;
  Sections[1].align = 2;
  Sections[1].flags = S_REGULAR;

  const std::vector<uint8_t> Unwind = makeCompactUnwind();
  setMachOName(Sections[2].sectname, "__unwind_info");
  setMachOName(Sections[2].segname, "__TEXT");
  Sections[2].addr = kImageBase + kUnwindOff;
  Sections[2].size = Unwind.size();
  Sections[2].offset = kUnwindOff;
  Sections[2].align = 2;
  Sections[2].flags = S_REGULAR;

  Command += TextCommandSize;
  auto *LinkeditSegment = reinterpret_cast<segment_command_64 *>(Command);
  LinkeditSegment->cmd = LC_SEGMENT_64;
  LinkeditSegment->cmdsize = LinkeditCommandSize;
  setMachOName(LinkeditSegment->segname, "__LINKEDIT");
  LinkeditSegment->vmaddr = kImageBase + kLinkeditOff;
  LinkeditSegment->vmsize = kFileSize - kLinkeditOff;
  LinkeditSegment->fileoff = kLinkeditOff;
  LinkeditSegment->filesize = kFileSize - kLinkeditOff;
  LinkeditSegment->maxprot = VM_PROT_READ;
  LinkeditSegment->initprot = VM_PROT_READ;

  Command += LinkeditCommandSize;
  auto *Symtab = reinterpret_cast<symtab_command *>(Command);
  Symtab->cmd = LC_SYMTAB;
  Symtab->cmdsize = sizeof(symtab_command);
  Symtab->symoff = kSymtabOff;
  Symtab->nsyms = 4;
  Symtab->stroff = kStringTableOff;

  std::string Strings(1, '\0');
  auto AddName = [&](llvm::StringRef Name) {
    const uint32_t Offset = static_cast<uint32_t>(Strings.size());
    Strings.append(Name.data(), Name.size());
    Strings.push_back('\0');
    return Offset;
  };
  const uint32_t HeaderName = AddName("__mh_execute_header");
  const uint32_t MainName = AddName("_main");
  const uint32_t FunctionName = AddName("_neverd_fixture_fn");
  const uint32_t ConstName = AddName("_neverd_const_bl");
  Symtab->strsize = static_cast<uint32_t>(Strings.size());
  if (kStringTableOff + Strings.size() > Binary.size()) {
    ADD_FAILURE() << "fixture string table exceeds its backing buffer";
    return {};
  }

  auto *Symbols = reinterpret_cast<nlist_64 *>(Binary.data() + kSymtabOff);
  auto SetSymbol = [&](uint32_t Index, uint32_t Name, uint8_t Section,
                       uint64_t Address) {
    Symbols[Index].n_strx = Name;
    Symbols[Index].n_type =
        static_cast<uint8_t>(static_cast<uint8_t>(N_SECT) | N_EXT);
    Symbols[Index].n_sect = Section;
    Symbols[Index].n_value = Address;
  };
  SetSymbol(0, HeaderName, 1, kImageBase);
  SetSymbol(1, MainName, 1, kImageBase + kTextOff);
  SetSymbol(2, FunctionName, 1, kImageBase + kTextOff + kFunctionSize);
  SetSymbol(3, ConstName, 2, kImageBase + kConstOff);
  std::memcpy(Binary.data() + kStringTableOff, Strings.data(), Strings.size());

  const uint32_t Instructions[] = {0x52800000, 0xd65f03c0, 0x52800540,
                                   0xd65f03c0};
  for (uint32_t I = 0; I < 4; ++I)
    llvm::support::endian::write32le(Binary.data() + kTextOff + I * 4,
                                     Instructions[I]);
  llvm::support::endian::write32le(Binary.data() + kConstOff, 0x94000000);
  std::copy(Unwind.begin(), Unwind.end(), Binary.begin() + kUnwindOff);
  return Binary;
}

// Build both nlist layouts without invoking a platform compiler. Debug records
// deliberately overlap a real function, while an ordinary alias shares its VA.
std::vector<uint8_t> makeMachOStabFixture(bool Is64) {
  using namespace llvm::MachO;
  auto Build = [&]<typename HeaderT, typename SegmentT, typename SectionT,
                   typename SymbolT>(HeaderT, SegmentT, SectionT, SymbolT) {
    constexpr uint32_t Base = 0x1000;
    constexpr uint32_t FunctionVA = Base + kTextOff;
    constexpr uint32_t StringOff = kLinkeditOff + 0x90;
    constexpr uint32_t SegmentSize = sizeof(SegmentT) + sizeof(SectionT);
    std::vector<uint8_t> Binary(kFileSize, 0);
    HeaderT Header{};
    Header.magic = Is64 ? MH_MAGIC_64 : MH_MAGIC;
    Header.cputype = Is64 ? CPU_TYPE_X86_64 : CPU_TYPE_X86;
    Header.cpusubtype = CPU_SUBTYPE_I386_ALL;
    Header.filetype = MH_EXECUTE;
    Header.ncmds = 2;
    Header.sizeofcmds = SegmentSize + sizeof(symtab_command);
    std::memcpy(Binary.data(), &Header, sizeof(Header));

    SegmentT Segment{};
    Segment.cmd = Is64 ? LC_SEGMENT_64 : LC_SEGMENT;
    Segment.cmdsize = SegmentSize;
    setMachOName(Segment.segname, "__TEXT");
    Segment.vmaddr = Base;
    Segment.vmsize = kFileSize;
    Segment.filesize = kFileSize;
    Segment.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    Segment.initprot = Segment.maxprot;
    Segment.nsects = 1;
    std::memcpy(Binary.data() + sizeof(Header), &Segment, sizeof(Segment));

    SectionT Section{};
    setMachOName(Section.sectname, "__text");
    setMachOName(Section.segname, "__TEXT");
    Section.addr = FunctionVA;
    Section.size = 1;
    Section.offset = kTextOff;
    Section.flags = S_ATTR_PURE_INSTRUCTIONS;
    std::memcpy(Binary.data() + sizeof(Header) + sizeof(Segment), &Section,
                sizeof(Section));
    Binary[kTextOff] = 0xc3; // ret, valid in both i386 and x86-64.

    std::array<SymbolT, 6> Symbols{};
    std::string Strings(1, '\0');
    auto AddSymbol = [&](size_t Index, llvm::StringRef Name, uint8_t Type,
                         uint8_t SectionID, uint32_t Value) {
      auto &Symbol = Symbols[Index];
      Symbol.n_strx = static_cast<uint32_t>(Strings.size());
      Strings.append(Name.data(), Name.size());
      Strings.push_back('\0');
      Symbol.n_type = Type;
      Symbol.n_sect = SectionID;
      Symbol.n_value = Value;
    };
    AddSymbol(0, "_main", N_SECT | N_EXT, 1, FunctionVA);
    AddSymbol(1, "_main", N_FUN, 1, FunctionVA);
    AddSymbol(2, "_main_alias", N_SECT | N_EXT, 1, FunctionVA);
    AddSymbol(3, "fixture-debug.o", N_OSO, 0, 0x65ab1234);
    AddSymbol(4, "_debug_only_static", N_STSYM, 1, FunctionVA);
    AddSymbol(5, "_debug_begin", N_BNSYM, 1, FunctionVA);
    symtab_command Symtab{};
    Symtab.cmd = LC_SYMTAB;
    Symtab.cmdsize = sizeof(Symtab);
    Symtab.symoff = kSymtabOff;
    Symtab.nsyms = static_cast<uint32_t>(Symbols.size());
    Symtab.stroff = StringOff;
    Symtab.strsize = static_cast<uint32_t>(Strings.size());
    std::memcpy(Binary.data() + sizeof(Header) + SegmentSize, &Symtab,
                sizeof(Symtab));
    std::memcpy(Binary.data() + kSymtabOff, Symbols.data(), sizeof(Symbols));
    std::memcpy(Binary.data() + StringOff, Strings.data(), Strings.size());
    return Binary;
  };
  if (Is64)
    return Build(mach_header_64{}, segment_command_64{}, section_64{},
                 nlist_64{});
  return Build(mach_header{}, segment_command{}, section{}, nlist{});
}

const Symbol *findSymbol(const BinaryImage &Image, llvm::StringRef Name) {
  auto It = std::find_if(Image.Symbols.begin(), Image.Symbols.end(),
                         [&](const Symbol &Item) { return Item.Name == Name; });
  return It == Image.Symbols.end() ? nullptr : &*It;
}

class MachOFunctionListingTest : public NeverDLiftTest {
protected:
  fs::path writeFixture(
      const std::vector<uint8_t> &Binary = makeMachOFunctionFixture()) {
    const fs::path Path = tmpFile("function-listing.macho");
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Binary.data()),
                 static_cast<std::streamsize>(Binary.size()));
    EXPECT_TRUE(Output.good());
    return Path;
  }
};

TEST_F(MachOFunctionListingTest,
       IgnoresStabsAndPreservesSameAddressAliasesInBothNListLayouts) {
  for (bool Is64 : {false, true}) {
    SCOPED_TRACE(Is64 ? "nlist_64" : "nlist");
    MachOLoader Loader;
    auto ImageOrErr = Loader.load(writeFixture(makeMachOStabFixture(Is64)));
    ASSERT_TRUE(static_cast<bool>(ImageOrErr))
        << llvm::toString(ImageOrErr.takeError());
    EXPECT_EQ(ImageOrErr->Symbols.size(), 2u);
    EXPECT_EQ(findSymbol(*ImageOrErr, "fixture-debug.o"), nullptr);
    EXPECT_EQ(findSymbol(*ImageOrErr, "_debug_only_static"), nullptr);
    EXPECT_EQ(findSymbol(*ImageOrErr, "_debug_begin"), nullptr);
    EXPECT_EQ(
        std::count_if(ImageOrErr->Symbols.begin(), ImageOrErr->Symbols.end(),
                      [](const Symbol &Sym) { return Sym.Name == "_main"; }),
        1);
    const auto Functions = ImageOrErr->getFunctionSymbols();
    ASSERT_EQ(Functions.size(), 2u);
    std::vector<std::string> Names;
    for (const Symbol *Function : Functions) {
      EXPECT_EQ(Function->Addr, 0x1000u + kTextOff);
      Names.push_back(Function->Name);
    }
    std::sort(Names.begin(), Names.end());
    EXPECT_EQ(Names, (std::vector<std::string>{"_main", "_main_alias"}));
  }
}

TEST_F(MachOFunctionListingTest,
       CompactUnwindDoesNotPromoteDuplicateDebugFunctionRecords) {
  auto Binary = makeMachOFunctionFixture();
  auto *Symbols =
      reinterpret_cast<llvm::MachO::nlist_64 *>(Binary.data() + kSymtabOff);
  // Replace the unrelated const symbol with the duplicate debug function seen
  // beside the ordinary N_SECT record in a real clang -g Mach-O executable.
  Symbols[3] = Symbols[1];
  Symbols[3].n_type = llvm::MachO::N_FUN;
  MachOLoader Loader;
  auto ImageOrErr = Loader.load(writeFixture(Binary));
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const auto Functions = ImageOrErr->getFunctionSymbols();
  ASSERT_EQ(Functions.size(), 2u);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const Symbol *Sym) {
                            return Sym->Name == "_main" &&
                                   Sym->Addr == kImageBase + kTextOff;
                          }),
            1);
  for (const Symbol *Function : Functions)
    EXPECT_EQ(Function->Size, kFunctionSize);
}

TEST_F(MachOFunctionListingTest,
       LoaderClassifiesCodeSymbolsAndMergesExactUnwindSizes) {
  MachOLoader Loader;
  auto ImageOrErr = Loader.load(writeFixture());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());

  const Symbol *Header = findSymbol(*ImageOrErr, "__mh_execute_header");
  const Symbol *Main = findSymbol(*ImageOrErr, "_main");
  const Symbol *Function = findSymbol(*ImageOrErr, "_neverd_fixture_fn");
  const Symbol *Constant = findSymbol(*ImageOrErr, "_neverd_const_bl");
  ASSERT_NE(Header, nullptr);
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Constant, nullptr);
  EXPECT_FALSE(Header->IsFunc);
  EXPECT_TRUE(Main->IsFunc);
  EXPECT_EQ(Main->Size, kFunctionSize);
  EXPECT_TRUE(Function->IsFunc);
  EXPECT_EQ(Function->Size, kFunctionSize);
  EXPECT_FALSE(Constant->IsFunc);
}

TEST_F(MachOFunctionListingTest,
       FuncsJSONListsOnlyFunctionsWithStableNamesAndExactSizes) {
  const RunResult Result =
      exec(ndBin(), {"funcs", "--json", writeFixture().string()});
  ASSERT_EQ(Result.exitCode, 0) << Result.err;
  auto Parsed = llvm::json::parse(Result.out);
  ASSERT_TRUE(static_cast<bool>(Parsed))
      << llvm::toString(Parsed.takeError()) << "\n"
      << Result.out;
  const llvm::json::Array *Functions = Parsed->getAsArray();
  ASSERT_NE(Functions, nullptr) << Result.out;
  ASSERT_EQ(Functions->size(), 2u) << Result.out;

  const std::vector<std::string> ExpectedNames = {"_main",
                                                  "_neverd_fixture_fn"};
  for (size_t I = 0; I < Functions->size(); ++I) {
    const llvm::json::Object *Function = (*Functions)[I].getAsObject();
    ASSERT_NE(Function, nullptr);
    EXPECT_EQ(Function->getString("name").value_or(""), ExpectedNames[I]);
    EXPECT_EQ(Function->getInteger("size").value_or(0), kFunctionSize);
  }
}

} // namespace
