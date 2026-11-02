//===- PEBrowseCompatibilityTests.cpp - Bounded PE browsing metadata ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/sdk/NeverDCAPISession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>

using namespace neverd;
using namespace llvm::COFF;

namespace {
struct Fixture {
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(0x600, 0);
  bool Wide;
  uint32_t Width;
  va_t Base;
  size_t Directories;
  static constexpr size_t Optional = 0x98;
  static constexpr size_t IData = 0x400;

  void put16(size_t At, uint16_t Value) {
    llvm::support::endian::write16le(Bytes.data() + At, Value);
  }
  void put32(size_t At, uint32_t Value) {
    llvm::support::endian::write32le(Bytes.data() + At, Value);
  }
  void put64(size_t At, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + At, Value);
  }
  void word(size_t At, uint64_t Value) {
    if (Wide)
      put64(At, Value);
    else
      put32(At, Value);
  }
  void name(size_t At, llvm::StringRef Value) {
    std::copy(Value.begin(), Value.end(), Bytes.begin() + At);
    Bytes[At + Value.size()] = 0;
  }
  void directory(unsigned Index, uint32_t RVA, uint32_t Size) {
    put32(Directories + Index * 8, RVA);
    put32(Directories + Index * 8 + 4, Size);
  }
  uint64_t ordinal(unsigned Value) const {
    return (uint64_t{1} << (Width * 8 - 1)) | Value;
  }
  explicit Fixture(bool Is64 = true)
      : Wide(Is64), Width(Is64 ? 8 : 4), Base(Is64 ? 0x140000000ULL : 0x400000),
        Directories(Optional + (Is64 ? 112 : 96)) {
    name(0, "MZ");
    put32(0x3c, 0x80);
    name(0x80, llvm::StringRef("PE\0\0", 4));
    put16(0x84, Wide ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386);
    put16(0x86, 2);
    const uint16_t OptionalSize = Wide ? 240 : 224;
    put16(0x94, OptionalSize);
    put16(0x96, IMAGE_FILE_EXECUTABLE_IMAGE);
    put16(Optional, Wide ? 0x20b : 0x10b);
    put32(Optional + 16, 0x1000);
    if (Wide)
      put64(Optional + 24, Base);
    else
      put32(Optional + 28, Base);
    put32(Optional + 32, 0x1000);
    put32(Optional + 36, 0x200);
    put32(Optional + 56, 0x3000);
    put32(Optional + 60, 0x200);
    put16(Optional + 68, IMAGE_SUBSYSTEM_WINDOWS_CUI);
    put32(Directories - 4, 16);
    for (unsigned I = 0; I != 2; ++I) {
      const size_t S = Optional + OptionalSize + I * 40;
      name(S, I ? ".idata" : ".text");
      put32(S + 8, 0x200);
      put32(S + 12, 0x1000 * (I + 1));
      put32(S + 16, 0x200);
      put32(S + 20, 0x200 * (I + 1));
      put32(S + 36,
            IMAGE_SCN_MEM_READ |
                (I ? IMAGE_SCN_MEM_WRITE | IMAGE_SCN_CNT_INITIALIZED_DATA
                   : IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE));
    }
    Bytes[0x200] = 0x31; // xor eax,eax; ret
    Bytes[0x201] = 0xc0;
    Bytes[0x202] = 0xc3;
    directory(IMPORT_TABLE, 0x2000, 60);
    put32(IData, 0x2100);
    put32(IData + 12, 0x2080);
    put32(IData + 16, 0x2180);
    name(IData + 0x80, "example.dll");
    name(IData + 0x90, "other.dll");
    word(IData + 0x100, ordinal(1));
    word(IData + 0x180, ordinal(1));
  }
  void secondDescriptor(uint32_t IAT = 0x2180, uint32_t Module = 0x2090) {
    put32(IData + 20, 0x2100);
    put32(IData + 20 + 12, Module);
    put32(IData + 20 + 16, IAT);
  }
};

struct InputFile {
  llvm::SmallString<128> Path;
  explicit InputFile(llvm::ArrayRef<uint8_t> Bytes) {
    int FD = -1;
    const auto Error =
        llvm::sys::fs::createTemporaryFile("neverd-pe-browse", "exe", FD, Path);
    if (Error)
      return;
    llvm::raw_fd_ostream File(FD, true);
    File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  }
  ~InputFile() { llvm::sys::fs::remove(Path); }
};

llvm::Expected<BinaryImage> load(const Fixture &F) {
  InputFile Input(F.Bytes);
  COFFLoader Loader;
  return Loader.load(std::string(Input.Path));
}

void expectNoBindings(const BinaryImage &Image) {
  EXPECT_TRUE(Image.Imports.empty());
  EXPECT_TRUE(Image.ImportStorageSlots.empty());
  EXPECT_TRUE(Image.ImportPtrSlots.empty());
}

TEST(PEBrowseCompatibility, PE32AndPE32PlusPublishCompleteOrdinalImports) {
  for (const bool Wide : {false, true}) {
    Fixture F(Wide);
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_EQ(Image->Imports.size(), 1u);
    EXPECT_EQ(Image->Imports[0].IATAddr, F.Base + 0x2180);
    EXPECT_EQ(Image->Imports[0].Ordinal, 1);
    EXPECT_EQ(Image->Imports[0].Module, "example.dll");
    EXPECT_TRUE(Image->LoadDiagnostics.empty());
  }
}

TEST(PEBrowseCompatibility, NamesComeFromLookupEvenWhenIATIsAlreadyBound) {
  for (const bool Wide : {false, true}) {
    Fixture F(Wide);
    F.word(F.IData + 0x100, 0x20c0);
    F.name(F.IData + 0xc2, "TestFunction");
    F.word(F.IData + 0x180, 0x774c1234);
    // A separate lookup table owns the import count. A following program
    // cell can be nonzero, including an unresolved delay-load thunk.
    F.word(F.IData + 0x180 + F.Width, F.Base + 0x1020);
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_EQ(Image->Imports.size(), 1u);
    EXPECT_EQ(Image->Imports[0].Name, "TestFunction");
    EXPECT_EQ(Image->Imports[0].IATAddr, F.Base + 0x2180);
    EXPECT_EQ(Image->findImportAt(F.Base + 0x2180 + F.Width), nullptr);
    EXPECT_TRUE(Image->LoadDiagnostics.empty());
  }
}

TEST(PEBrowseCompatibility, MissingLookupUsesACompleteUnboundIAT) {
  Fixture F;
  F.put32(F.IData, 0);
  auto Image = load(F);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Imports.size(), 1u);
  EXPECT_EQ(Image->Imports[0].Ordinal, 1);
}

TEST(PEBrowseCompatibility, BoundIATWithoutLookupNeverGuessesAnIdentity) {
  for (const uint32_t Timestamp : {1u, UINT32_MAX}) {
    Fixture F;
    F.put32(F.IData, 0);
    F.put32(F.IData + 4, Timestamp);
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    expectNoBindings(*Image);
    EXPECT_FALSE(Image->LoadDiagnostics.empty());
  }
}

TEST(PEBrowseCompatibility, NoncanonicalIATRetainsOnlyDescriptorIdentities) {
  for (const bool Wide : {false, true})
    for (const uint32_t RVA : {0x2181u, 0x1080u}) {
      Fixture F(Wide);
      SCOPED_TRACE(::testing::Message()
                   << "width=" << F.Width << " IAT RVA=" << RVA);
      F.put32(F.IData + 16, RVA);
      const size_t Offset = RVA == 0x1080 ? 0x280 : F.IData + 0x181;
      F.word(Offset, F.ordinal(1));
      F.word(Offset + F.Width, 0);
      auto Image = load(F);
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      ASSERT_EQ(Image->Imports.size(), 1u);
      EXPECT_EQ(Image->Imports[0].IATAddr, F.Base + RVA);
      EXPECT_EQ(Image->Imports[0].Ordinal, 1u);
      EXPECT_EQ(Image->Imports[0].Module, "example.dll");
      EXPECT_NE(Image->findImportAt(F.Base + RVA), nullptr);
      EXPECT_TRUE(Image->ImportStorageSlots.empty());
      EXPECT_TRUE(Image->ImportPtrSlots.empty());
      EXPECT_TRUE(Image->collectImportStorageSlots().Slots.empty());
      if (RVA % F.Width) {
        // Browsing can retain the descriptor identity without proving the
        // aligned loader writes needed to initialize a mapped header page.
        ASSERT_EQ(Image->LoadDiagnostics.size(), 1u);
        EXPECT_EQ(Image->LoadDiagnostics[0].Code, "pe.headers_unmapped");
      } else {
        EXPECT_TRUE(Image->LoadDiagnostics.empty());
      }
    }
}

TEST(PEBrowseCompatibility, LongNamesAndRepeatedDescriptorsStayBounded) {
  for (const bool LongName : {true, false}) {
    Fixture F;
    F.Bytes.resize(0x2400, 0);
    const size_t IDataSection = F.Optional + 240 + 40;
    F.put32(IDataSection + 8, 0x2000);
    F.put32(IDataSection + 16, 0x2000);
    F.put32(F.Optional + 56, 0x4000);
    if (LongName) {
      std::fill_n(F.Bytes.begin() + F.IData + 0x80, 5000, 'x');
    } else {
      F.directory(IMPORT_TABLE, 0x2000, 201 * 20);
      std::fill(F.Bytes.begin() + F.IData, F.Bytes.end(), 0);
      F.name(F.IData + 0x1b00, "example.dll");
      for (unsigned I = 0; I != 32; ++I) {
        F.word(F.IData + 0x1c00 + I * F.Width, F.ordinal(I + 1));
        F.word(F.IData + 0x1d80 + I * F.Width, F.ordinal(I + 1));
      }
      for (unsigned I = 0; I != 200; ++I) {
        F.put32(F.IData + I * 20, 0x3c00);
        F.put32(F.IData + I * 20 + 12, 0x3b00);
        F.put32(F.IData + I * 20 + 16, 0x3d80);
      }
    }
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    expectNoBindings(*Image);
    EXPECT_FALSE(Image->LoadDiagnostics.empty());
    if (!LongName)
      EXPECT_EQ(Image->LoadDiagnostics.back().Code,
                "pe.import_budget_exhausted");
  }
}

TEST(PEBrowseCompatibility, ManyShortNamesDoNotSpendUnscannedTailBytes) {
  for (const bool Wide : {false, true}) {
    Fixture F(Wide);
    F.Bytes.resize(0x2400, 0);
    const size_t IDataSection = F.Optional + (Wide ? 240 : 224) + 40;
    F.put32(IDataSection + 8, 0x2000);
    F.put32(IDataSection + 16, 0x2000);
    F.put32(F.Optional + 56, 0x4000);
    std::fill(F.Bytes.begin() + F.IData + 0x100, F.Bytes.end(), 0);
    F.put32(F.IData, 0x3000);
    F.put32(F.IData + 16, 0x3400);
    for (unsigned I = 0; I != 80; ++I) {
      const uint32_t NameRVA = 0x2100 + I * 16;
      F.name(F.IData + 0x100 + I * 16 + 2, "function" + std::to_string(I));
      F.word(F.IData + 0x1000 + I * F.Width, NameRVA);
      F.word(F.IData + 0x1400 + I * F.Width, NameRVA);
    }
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Imports.size(), 80u);
    EXPECT_TRUE(Image->LoadDiagnostics.empty());
  }
}

TEST(PEBrowseCompatibility,
     OutputRejectsImportDiagnosticsWithoutExceptionTable) {
  Fixture F;
  ASSERT_FALSE(static_cast<bool>(validatePatchedCOFFImage(F.Bytes, Arch::X64)));
  F.put32(F.IData + 16, 0x90000000);
  auto Error = validatePatchedCOFFImage(F.Bytes, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error))
                .find("final PE import metadata is incomplete"),
            std::string::npos);
}

TEST(PEBrowseCompatibility,
     OutputRejectsIncompleteDelayDirectoryWithExceptionTable) {
  Fixture F;
  F.directory(EXCEPTION_TABLE, 0x2040, 12);
  F.put32(F.IData + 0x40, 0x1000);
  F.put32(F.IData + 0x44, 0x1003);
  F.put32(F.IData + 0x48, 0x2060);
  F.Bytes[F.IData + 0x60] = 1; // version 1, no unwind operations
  auto Clean = validatePatchedCOFFImage(F.Bytes, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(Clean)) << llvm::toString(std::move(Clean));
  F.directory(DELAY_IMPORT_DESCRIPTOR, 0x20a0, 1);
  auto Error = validatePatchedCOFFImage(F.Bytes, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error))
                .find("final PE import metadata is incomplete"),
            std::string::npos);
}

TEST(PEBrowseCompatibility, UnmappedIATAndInvalidEntryStillExposeActualCode) {
  Fixture F(false);
  F.put32(F.Optional + 28, 0x774c0000);
  F.put32(F.Optional + 16, 0x88f41000);
  F.put32(F.IData + 16, 0x88fb7a93);
  auto Image = load(F);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Entry, 0u);
  expectNoBindings(*Image);
  EXPECT_FALSE(Image->isRuntimeFunctionAt(0x100401000ULL));
  ASSERT_NE(Image->readVA(0x774c1000, 3), nullptr);
  EXPECT_EQ(Image->readVA(0x774c1000, 3)[0], 0x31);
  EXPECT_EQ(Image->Raw, F.Bytes);
  ASSERT_EQ(Image->LoadDiagnostics.size(), 3u);
  EXPECT_EQ(Image->LoadDiagnostics[0].Code, "pe.entry_out_of_range");
  EXPECT_EQ(Image->LoadDiagnostics[1].Code, "pe.headers_unmapped");
  EXPECT_EQ(Image->LoadDiagnostics[2].Code, "pe.import_descriptor_invalid");
}

TEST(PEBrowseCompatibility, BadDescriptorDoesNotRemoveAnIndependentValidOne) {
  Fixture F;
  F.secondDescriptor(0x90000000);
  auto Image = load(F);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  ASSERT_EQ(Image->Imports.size(), 1u);
  EXPECT_EQ(Image->Imports[0].Module, "example.dll");
  ASSERT_EQ(Image->LoadDiagnostics.size(), 2u);
  EXPECT_EQ(Image->LoadDiagnostics[0].Code, "pe.headers_unmapped");
  EXPECT_EQ(Image->LoadDiagnostics[1].Code, "pe.import_descriptor_invalid");
}

TEST(PEBrowseCompatibility, TruncatedTablesPublishNoPrefixBindings) {
  for (const bool Wide : {false, true}) {
    Fixture F(Wide);
    F.word(F.IData + 0x100 + F.Width, F.ordinal(2));
    F.put32(F.IData + 16, 0x2200 - 2 * F.Width);
    F.word(F.IData + 0x200 - 2 * F.Width, F.ordinal(1));
    F.word(F.IData + 0x200 - F.Width, F.ordinal(2));
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    expectNoBindings(*Image);
    EXPECT_FALSE(Image->LoadDiagnostics.empty());
  }
}

TEST(PEBrowseCompatibility, InvalidNamesAndReservedOrdinalBitsAreDiagnosed) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    Fixture F;
    if (Case == 0)
      F.word(F.IData + 0x100, F.ordinal(1) | (uint64_t{1} << 32));
    else if (Case == 1) {
      F.word(F.IData + 0x100, 0x21fc);
      std::fill(F.Bytes.begin() + F.IData + 0x1fc, F.Bytes.end(), 'x');
    } else
      F.word(F.IData + 0x100, 0x90000000);
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    expectNoBindings(*Image);
    EXPECT_FALSE(Image->LoadDiagnostics.empty());
  }
}

TEST(PEBrowseCompatibility, UnterminatedDirectoryPublishesNoDescriptors) {
  Fixture F;
  F.directory(IMPORT_TABLE, 0x2000, 20);
  auto Image = load(F);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  expectNoBindings(*Image);
  EXPECT_EQ(Image->LoadDiagnostics.back().Code,
            "pe.import_directory_unterminated");
}

TEST(PEBrowseCompatibility, ConflictingModulesAndPartialSlotsDoNotBind) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    Fixture F;
    F.secondDescriptor(Case == 1 ? 0x2184 : 0x2180);
    if (Case == 2)
      std::swap_ranges(F.Bytes.begin() + F.IData,
                       F.Bytes.begin() + F.IData + 20,
                       F.Bytes.begin() + F.IData + 20);
    if (Case == 3) {
      F.directory(IMPORT_TABLE, 0x2000, 80);
      std::copy_n(F.Bytes.begin() + F.IData, 20,
                  F.Bytes.begin() + F.IData + 40);
    }
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    expectNoBindings(*Image);
    EXPECT_FALSE(Image->LoadDiagnostics.empty());
  }
}

TEST(PEBrowseCompatibility, IdenticalDescriptorsAreDeduplicated) {
  Fixture F;
  F.secondDescriptor(0x2180, 0x2080);
  auto Image = load(F);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  EXPECT_EQ(Image->Imports.size(), 1u);
  // Browsing deduplicates the identity, but repeated writes to its IAT slot
  // cannot establish the separate header-initialization contract.
  ASSERT_EQ(Image->LoadDiagnostics.size(), 1u);
  EXPECT_EQ(Image->LoadDiagnostics[0].Code, "pe.headers_unmapped");
  EXPECT_EQ(Image->readVA(F.Base, 1), nullptr);
}

TEST(PEBrowseCompatibility, DataAndUnmappedEntriesAreUnknown) {
  for (const uint32_t Entry : {0x2080u, 0x9000u}) {
    Fixture F;
    F.put32(F.Optional + 16, Entry);
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    EXPECT_EQ(Image->Entry, 0u);
    EXPECT_FALSE(Image->isRuntimeFunctionAt(F.Base + Entry));
    EXPECT_EQ(Image->LoadDiagnostics.back().Code, "pe.entry_unmapped");
  }
}

TEST(PEBrowseCompatibility, IncompleteDelayDirectoriesDoNotUnderflow) {
  for (const uint32_t Size : {0u, 1u, 31u}) {
    Fixture F;
    F.directory(DELAY_IMPORT_DESCRIPTOR, 0x2040, Size);
    auto Image = load(F);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    ASSERT_EQ(Image->LoadDiagnostics.size(), 2u);
    EXPECT_EQ(Image->LoadDiagnostics[0].Code, "pe.headers_unmapped");
    EXPECT_EQ(Image->LoadDiagnostics[1].Code,
              "pe.delay_import_directory_invalid");
  }
}

TEST(PEBrowseCompatibility, DiagnosticStorageIsBounded) {
  BinaryImage Image;
  for (unsigned I = 0; I != 1000; ++I)
    Image.addLoadDiagnostic("test.warning", "test message");
  EXPECT_EQ(Image.LoadDiagnostics.size(), 65u);
  EXPECT_EQ(Image.LoadDiagnostics.back().Code, "loader.diagnostics_truncated");
}

std::string owned(const char *Text) {
  const std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}

TEST(PEBrowseCompatibility, CAPIDiagnosticsDescribeOnlyTheLoadedImage) {
  EXPECT_EQ(owned(neverd_session_load_diagnostics_json(nullptr)), "[]");
  const auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  const auto Destroy =
      llvm::scope_exit([&] { neverd_session_destroy(Session); });
  EXPECT_EQ(owned(neverd_session_load_diagnostics_json(Session)), "[]");
  Fixture BadMetadata;
  BadMetadata.put32(BadMetadata.IData + 16, 0x90000000);
  InputFile Warned(BadMetadata.Bytes);
  ASSERT_TRUE(neverd_session_load(Session, Warned.Path.c_str()));
  auto JSON =
      llvm::json::parse(owned(neverd_session_load_diagnostics_json(Session)));
  ASSERT_TRUE(static_cast<bool>(JSON)) << llvm::toString(JSON.takeError());
  ASSERT_NE(JSON->getAsArray(), nullptr);
  ASSERT_EQ(JSON->getAsArray()->size(), 2u);
  EXPECT_EQ(JSON->getAsArray()->front().getAsObject()->getString("code"),
            std::optional<llvm::StringRef>("pe.headers_unmapped"));
  EXPECT_EQ(JSON->getAsArray()->back().getAsObject()->getString("code"),
            std::optional<llvm::StringRef>("pe.import_descriptor_invalid"));
  Fixture Good;
  InputFile Clean(Good.Bytes);
  ASSERT_TRUE(neverd_session_load(Session, Clean.Path.c_str()));
  EXPECT_EQ(owned(neverd_session_load_diagnostics_json(Session)), "[]");
}
} // namespace
