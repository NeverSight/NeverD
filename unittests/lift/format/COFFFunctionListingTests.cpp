//===- COFFFunctionListingTests.cpp - PE function ownership ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace neverd;

constexpr uint32_t TextRVA = 0x1000;
constexpr uint32_t TextOffset = 0x200;
constexpr uint32_t PDataOffset = 0x400;
constexpr uint32_t XDataOffset = 0x600;
constexpr uint32_t SymbolOffset = 0x800;
constexpr std::array<Arch, 3> Architectures = {Arch::X64, Arch::ARM,
                                               Arch::AArch64};

uint64_t imageBase(Arch A) { return A == Arch::ARM ? 0x400000 : 0x140000000; }
uint32_t functionSize(Arch A) {
  return A == Arch::X64 ? 11 : A == Arch::ARM ? 4 : 12;
}
uint16_t machine(Arch A) {
  return A == Arch::X64   ? llvm::COFF::IMAGE_FILE_MACHINE_AMD64
         : A == Arch::ARM ? llvm::COFF::IMAGE_FILE_MACHINE_ARMNT
                          : llvm::COFF::IMAGE_FILE_MACHINE_ARM64;
}

void put16(std::vector<uint8_t> &Bytes, size_t Offset, uint16_t Value) {
  llvm::support::endian::write16le(Bytes.data() + Offset, Value);
}
void put32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}
void put64(std::vector<uint8_t> &Bytes, size_t Offset, uint64_t Value) {
  llvm::support::endian::write64le(Bytes.data() + Offset, Value);
}
void putName(std::vector<uint8_t> &Bytes, size_t Offset, llvm::StringRef Name) {
  std::memcpy(Bytes.data() + Offset, Name.data(), Name.size());
}
void putCode(std::vector<uint8_t> &Bytes, size_t Offset, Arch A) {
  if (A == Arch::X64) {
    // push rbp; mov rbp,rsp; mov eax,42; pop rbp; ret.
    constexpr uint8_t Code[] = {0x55, 0x48, 0x89, 0xe5, 0xb8, 0x2a,
                                0,    0,    0,    0x5d, 0xc3};
    std::memcpy(Bytes.data() + Offset, Code, sizeof(Code));
  } else if (A == Arch::ARM) {
    // push {r4,lr}; pop {r4,pc}.
    put16(Bytes, Offset, 0xb510);
    put16(Bytes, Offset + 2, 0xbd10);
  } else {
    // sub sp,sp,#16; add sp,sp,#16; ret.
    put32(Bytes, Offset, 0xd10043ff);
    put32(Bytes, Offset + 4, 0x910043ff);
    put32(Bytes, Offset + 8, 0xd65f03c0);
  }
}

// A genuine function-definition auxiliary record follows foo. LLVM must skip
// those 18 bytes as a record, rather than interpreting them as another symbol.
void appendSymbols(std::vector<uint8_t> &Bytes, uint32_t Offset, Arch A,
                   bool Alias) {
  const size_t Records = Alias ? 3 : 2;
  Bytes.resize(Offset + Records * 18 + 4, 0);
  auto Write = [&](size_t Record, llvm::StringRef Name, bool Auxiliary) {
    const size_t At = Offset + Record * 18;
    putName(Bytes, At, Name);
    put32(Bytes, At + 8, A == Arch::ARM ? 1 : 0);
    put16(Bytes, At + 12, 1);    // .text section, one-based.
    put16(Bytes, At + 14, 0x20); // Function derived type.
    Bytes[At + 16] = llvm::COFF::IMAGE_SYM_CLASS_EXTERNAL;
    Bytes[At + 17] = Auxiliary ? 1 : 0;
  };
  Write(0, "foo", true);
  put32(Bytes, Offset + 18 + 4, functionSize(A)); // Aux TotalSize.
  if (Alias)
    Write(2, "alias", false);
  put32(Bytes, Offset + Records * 18, 4); // Empty COFF string table.
}

// A complete tiny PE32/PE32+ image with one bounded function, standard section
// alignment, typed COFF definitions, and matching x64 or ARM packed unwind.
std::vector<uint8_t> makePE(Arch A, bool Symbols, bool Alias, bool Unwind,
                            bool Exports = false) {
  const bool Is32 = A == Arch::ARM;
  const uint32_t Optional = 0x98;
  const uint16_t OptionalSize = Is32 ? 224 : 240;
  const uint32_t Directories = Optional + (Is32 ? 96 : 112);
  const uint32_t Sections = Optional + OptionalSize;
  std::vector<uint8_t> Bytes(SymbolOffset, 0);
  putName(Bytes, 0, "MZ");
  put32(Bytes, 0x3c, 0x80);
  putName(Bytes, 0x80, llvm::StringRef("PE\0\0", 4));
  put16(Bytes, 0x84, machine(A));
  put16(Bytes, 0x86, 3);
  put32(Bytes, 0x8c, Symbols ? SymbolOffset : 0);
  put32(Bytes, 0x90, Symbols ? (Alias ? 3 : 2) : 0);
  put16(Bytes, 0x94, OptionalSize);
  put16(Bytes, 0x96, Is32 ? 0x102 : 0x22);
  put16(Bytes, Optional, Is32 ? 0x10b : 0x20b);
  put32(Bytes, Optional + 4, 0x200);
  put32(Bytes, Optional + 8, 0x400);
  put32(Bytes, Optional + 16, TextRVA | (Is32 ? 1 : 0));
  put32(Bytes, Optional + 20, TextRVA);
  if (Is32) {
    put32(Bytes, Optional + 24, 0x2000);
    put32(Bytes, Optional + 28, static_cast<uint32_t>(imageBase(A)));
    put32(Bytes, Optional + 72, 0x100000);
    put32(Bytes, Optional + 76, 0x1000);
    put32(Bytes, Optional + 80, 0x100000);
    put32(Bytes, Optional + 84, 0x1000);
    put32(Bytes, Optional + 92, 16);
  } else {
    put64(Bytes, Optional + 24, imageBase(A));
    put64(Bytes, Optional + 72, 0x100000);
    put64(Bytes, Optional + 80, 0x1000);
    put64(Bytes, Optional + 88, 0x100000);
    put64(Bytes, Optional + 96, 0x1000);
    put32(Bytes, Optional + 108, 16);
  }
  put32(Bytes, Optional + 32, 0x1000);
  put32(Bytes, Optional + 36, 0x200);
  put16(Bytes, Optional + 40, 6);
  put16(Bytes, Optional + 48, 6);
  put32(Bytes, Optional + 56, 0x4000);
  put32(Bytes, Optional + 60, 0x200);
  put16(Bytes, Optional + 68, 3); // Console subsystem.
  const uint32_t PDataSize = A == Arch::X64 ? 12 : 8;
  if (Unwind) {
    put32(Bytes, Directories + 3 * 8, 0x2000);
    put32(Bytes, Directories + 3 * 8 + 4, PDataSize);
  }
  auto Section = [&](unsigned I, llvm::StringRef Name, uint32_t RVA,
                     uint32_t Size, uint32_t FileOffset, uint32_t Flags) {
    const size_t At = Sections + I * 40;
    putName(Bytes, At, Name);
    put32(Bytes, At + 8, Size);
    put32(Bytes, At + 12, RVA);
    put32(Bytes, At + 16, 0x200);
    put32(Bytes, At + 20, FileOffset);
    put32(Bytes, At + 36, Flags);
  };
  Section(0, ".text", TextRVA, functionSize(A), TextOffset, 0x60000020);
  Section(1, ".pdata", 0x2000, PDataSize, PDataOffset, 0x40000040);
  Section(2, ".xdata", 0x3000, Exports ? 0x100 : 8, XDataOffset, 0x40000040);
  putCode(Bytes, TextOffset, A);
  put32(Bytes, PDataOffset, TextRVA);
  if (A == Arch::X64) {
    put32(Bytes, PDataOffset + 4, TextRVA + functionSize(A));
    put32(Bytes, PDataOffset + 8, 0x3000);
    constexpr uint8_t Codes[] = {1, 4, 2, 5, 4, 3, 1, 0x50};
    std::memcpy(Bytes.data() + XDataOffset, Codes, sizeof(Codes));
  } else {
    const uint32_t Packed = A == Arch::ARM ? 1 | (2 << 2) | (1 << 20)
                                           : 1 | functionSize(A) | (1 << 23);
    put32(Bytes, PDataOffset + 4, Packed);
  }
  if (Exports) {
    put32(Bytes, Directories, 0x3020);
    put32(Bytes, Directories + 4, 0x80);
    const size_t Table = XDataOffset + 0x20;
    put32(Bytes, Table + 12, 0x3060); // Image name.
    put32(Bytes, Table + 16, 1);      // Ordinal base.
    put32(Bytes, Table + 20, 2);      // EAT entries, two legitimate aliases.
    put32(Bytes, Table + 24, 2);
    put32(Bytes, Table + 28, 0x3048);
    put32(Bytes, Table + 32, 0x3050);
    put32(Bytes, Table + 36, 0x3058);
    put32(Bytes, XDataOffset + 0x48, TextRVA | (Is32 ? 1 : 0));
    put32(Bytes, XDataOffset + 0x4c, TextRVA | (Is32 ? 1 : 0));
    put32(Bytes, XDataOffset + 0x50, 0x3070); // Names sorted: alias, foo.
    put32(Bytes, XDataOffset + 0x54, 0x3078);
    put16(Bytes, XDataOffset + 0x58, 1);
    put16(Bytes, XDataOffset + 0x5a, 0);
    putName(Bytes, XDataOffset + 0x60, "fixture.exe");
    putName(Bytes, XDataOffset + 0x70, "alias");
    putName(Bytes, XDataOffset + 0x78, "foo");
  }
  if (Symbols)
    appendSymbols(Bytes, SymbolOffset, A, Alias);
  return Bytes;
}

std::vector<uint8_t> makeObject() {
  std::vector<uint8_t> Bytes(0x60, 0);
  put16(Bytes, 0, llvm::COFF::IMAGE_FILE_MACHINE_AMD64);
  put16(Bytes, 2, 1);
  put32(Bytes, 8, 0x60);
  put32(Bytes, 12, 3); // foo + its auxiliary record + alias.
  putName(Bytes, 20, ".text");
  put32(Bytes, 20 + 16, functionSize(Arch::X64));
  put32(Bytes, 20 + 20, 0x40);
  put32(Bytes, 20 + 36, 0x60500020); // Code, RX, 16-byte alignment.
  putCode(Bytes, 0x40, Arch::X64);
  appendSymbols(Bytes, 0x60, Arch::X64, true);
  return Bytes;
}

std::vector<std::string> functionNames(const BinaryImage &Img) {
  std::vector<std::string> Names;
  for (const Symbol *S : Img.getFunctionSymbols())
    Names.push_back(S->Name);
  std::sort(Names.begin(), Names.end());
  return Names;
}

class COFFFunctionListingTest : public NeverDLiftTest {
protected:
  fs::path writeFixture(const std::vector<uint8_t> &Bytes) {
    const fs::path Path = tmpFile("function-listing.bin");
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()),
                 static_cast<std::streamsize>(Bytes.size()));
    EXPECT_TRUE(Output.good());
    return Path;
  }
};

TEST_F(COFFFunctionListingTest,
       TypedDefinitionsOwnUnwindEntriesForEveryPEArch) {
  for (Arch A : Architectures) {
    SCOPED_TRACE(static_cast<int>(A));
    COFFLoader Loader;
    auto Image = Loader.load(writeFixture(makePE(A, true, false, true)));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(functionNames(*Image), (std::vector<std::string>{"foo"}));
    ASSERT_EQ(Image->getFunctionSymbols().size(), 1u);
    EXPECT_EQ(Image->getFunctionSymbols()[0]->Addr, imageBase(A) + TextRVA);
    EXPECT_EQ(Image->getFunctionSymbols()[0]->Size, functionSize(A));
    ASSERT_EQ(Image->ExceptionMetadata.Functions.size(), 1u);
    EXPECT_EQ(Image->ExceptionMetadata.Functions[0].ParseStatus,
              ExceptionParseStatus::Complete);
  }
}

TEST_F(COFFFunctionListingTest,
       SameAddressTypedAndExportAliasesRemainDistinct) {
  for (Arch A : Architectures) {
    SCOPED_TRACE(static_cast<int>(A));
    COFFLoader Loader;
    auto Image = Loader.load(writeFixture(makePE(A, true, true, true, true)));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(functionNames(*Image),
              (std::vector<std::string>{"alias", "foo"}));
    ASSERT_EQ(Image->Exports.size(), 2u);
    std::vector<std::string> ExportNames;
    for (const Symbol *S : Image->getFunctionSymbols()) {
      EXPECT_EQ(S->Addr, imageBase(A) + TextRVA);
      EXPECT_EQ(S->Size, functionSize(A));
    }
    for (const Export &E : Image->Exports) {
      EXPECT_EQ(E.Addr, imageBase(A) + TextRVA);
      ExportNames.push_back(E.Name);
    }
    std::sort(ExportNames.begin(), ExportNames.end());
    EXPECT_EQ(ExportNames, (std::vector<std::string>{"alias", "foo"}));
  }
}

TEST_F(COFFFunctionListingTest, UnnamedUnwindEntriesStillProduceFunctions) {
  for (Arch A : Architectures) {
    SCOPED_TRACE(static_cast<int>(A));
    COFFLoader Loader;
    auto Image = Loader.load(writeFixture(makePE(A, false, false, true)));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    const auto Functions = Image->getFunctionSymbols();
    ASSERT_EQ(Functions.size(), 1u);
    EXPECT_EQ(Functions[0]->Name,
              Symbol::makeFunc(imageBase(A) + TextRVA).Name);
    EXPECT_EQ(Functions[0]->Size, functionSize(A));
  }
}

TEST_F(COFFFunctionListingTest, OrdinaryObjectAuxRecordsDoNotBecomeAliases) {
  COFFLoader Loader;
  auto Image = Loader.load(writeFixture(makeObject()));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_TRUE(Image->IsRelocatable);
  EXPECT_EQ(functionNames(*Image), (std::vector<std::string>{"alias", "foo"}));
  EXPECT_EQ(Image->Symbols.size(), 2u);
  for (const Symbol *S : Image->getFunctionSymbols())
    EXPECT_EQ(S->Addr, 0x1000u);
}

TEST_F(COFFFunctionListingTest, UnwindOnlyFillsMissingTypedSizes) {
  for (Arch A : Architectures) {
    SCOPED_TRACE(static_cast<int>(A));
    COFFLoader Loader;
    auto Image = Loader.load(writeFixture(makePE(A, true, true, false)));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_EQ(Image->Symbols.size(), 2u);
    Image->Symbols[0].Size = 1;
    const auto Bytes = makePE(A, true, true, true);
    auto Object = llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                        Bytes.size()),
        "fixture"));
    ASSERT_TRUE(static_cast<bool>(Object))
        << llvm::toString(Object.takeError());
    coff_loader::parseExceptions(**Object, *Image, imageBase(A));
    ASSERT_EQ(Image->Symbols.size(), 2u);
    EXPECT_EQ(Image->Symbols[0].Size, 1u);
    EXPECT_EQ(Image->Symbols[1].Size, functionSize(A));

    // The same address being present as data must not be promoted or resized.
    Image->Symbols[0].IsFunc = false;
    Image->Symbols[0].Size = 0;
    Image->Symbols.resize(1);
    Image->ExceptionMetadata = {};
    Image->KnownCodeRanges.clear();
    coff_loader::parseExceptions(**Object, *Image, imageBase(A));
    ASSERT_EQ(Image->Symbols.size(), 2u);
    EXPECT_FALSE(Image->Symbols[0].IsFunc);
    EXPECT_EQ(Image->Symbols[0].Size, 0u);
    EXPECT_EQ(Image->Symbols[1].Name,
              Symbol::makeFunc(imageBase(A) + TextRVA).Name);
    EXPECT_TRUE(Image->Symbols[1].IsFunc);
    EXPECT_EQ(Image->Symbols[1].Size, functionSize(A));
  }
}

TEST_F(COFFFunctionListingTest, ExistingSymbolsStillRequireDiscoveryGates) {
  for (Arch A : Architectures) {
    SCOPED_TRACE(static_cast<int>(A));
    for (bool Executable : {false, true}) {
      SCOPED_TRACE(Executable);
      COFFLoader Loader;
      auto Image = Loader.load(writeFixture(makePE(A, true, false, false)));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      ASSERT_EQ(Image->Symbols.size(), 1u);
      ASSERT_EQ(Image->Symbols[0].Size, 0u);
      auto &Text = Image->Segments[0];
      if (!Executable)
        Text.Flags = SegmentFlags::Readable;
      else if (A == Arch::X64)
        Text.Data[0] = 0x90; // NOP is not the required prologue.
      else if (A == Arch::ARM)
        put16(Text.Data, 0, 0xbf00);
      else
        put32(Text.Data, 0, 0x91000000); // add x0,x0,#0, not a prologue.
      const auto Bytes = makePE(A, true, false, true);
      auto Object = llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                          Bytes.size()),
          "fixture"));
      ASSERT_TRUE(static_cast<bool>(Object))
          << llvm::toString(Object.takeError());
      coff_loader::parseExceptions(**Object, *Image, imageBase(A));
      ASSERT_EQ(Image->Symbols.size(), 1u);
      EXPECT_EQ(Image->Symbols[0].Size, 0u);
    }
  }
}

TEST_F(COFFFunctionListingTest, PackedFragmentsDoNotResizeNamedDefinitions) {
  for (Arch A : {Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(A));
    auto Bytes = makePE(A, true, false, true);
    const auto Packed =
        llvm::support::endian::read32le(Bytes.data() + PDataOffset + 4);
    put32(Bytes, PDataOffset + 4, (Packed & ~3u) | 2u);
    COFFLoader Loader;
    auto Image = Loader.load(writeFixture(Bytes));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_EQ(Image->Symbols.size(), 1u);
    EXPECT_EQ(Image->Symbols[0].Size, 0u);
    ASSERT_EQ(Image->ExceptionMetadata.Functions.size(), 1u);
    EXPECT_EQ(Image->ExceptionMetadata.Functions[0].Kind,
              RuntimeFunctionKind::Fragment);
  }
}

TEST_F(COFFFunctionListingTest,
       SectionDefinitionsDoNotSuppressUnwindDiscovery) {
  for (Arch A : Architectures) {
    SCOPED_TRACE(static_cast<int>(A));
    auto Bytes = makePE(A, true, false, true);
    std::fill_n(Bytes.begin() + SymbolOffset, 8, 0);
    putName(Bytes, SymbolOffset, ".text");
    put32(Bytes, SymbolOffset + 8, 0);
    put16(Bytes, SymbolOffset + 14, 0);
    Bytes[SymbolOffset + 16] = llvm::COFF::IMAGE_SYM_CLASS_STATIC;
    // Replace the function aux with the standard section-definition record.
    std::fill_n(Bytes.begin() + SymbolOffset + 18, 18, 0);
    put32(Bytes, SymbolOffset + 18, functionSize(A));
    COFFLoader Loader;
    auto Image = Loader.load(writeFixture(Bytes));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_EQ(Image->Symbols.size(), 2u);
    EXPECT_EQ(Image->Symbols[0].Name, ".text");
    EXPECT_FALSE(Image->Symbols[0].IsFunc);
    EXPECT_EQ(Image->Symbols[0].Size, 0u);
    const auto Functions = Image->getFunctionSymbols();
    ASSERT_EQ(Functions.size(), 1u);
    EXPECT_EQ(Functions[0]->Name,
              Symbol::makeFunc(imageBase(A) + TextRVA).Name);
    EXPECT_EQ(Functions[0]->Size, functionSize(A));
  }
}

} // namespace
