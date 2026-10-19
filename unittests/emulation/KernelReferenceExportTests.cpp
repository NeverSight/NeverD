//===- KernelReferenceExportTests.cpp - Original PE export observations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original PE export observations.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceExports.h"
#include "os/windows/kernel/KernelReferenceImage.h"

#include "neverd/loader/Loader.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>

namespace neverd::emulation {
namespace {
std::filesystem::path exportFixture(const char *Name) {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + ".sys");
}
class KernelReferenceExportsTest : public testing::Test {
protected:
  BinaryImage Image;
  uint64_t Base = 0xfffff80000000000;
  uint32_t DirectoryRVA = 0;
  size_t DirectoryOffset = 0, EATOffset = 0, OrdinalOffset = 0;
  size_t NameOffset = 0, SectionOffset = 0, DataDirectoryOffset = 0;
  void SetUp() override {
    auto Loaded = Loader::create(BinaryFormat::COFF)
                      ->load(exportFixture("driver_kernel_reference"));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    Image = std::move(*Loaded);
    auto Object = object();
    ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
    const auto *D = (*Object)->getDataDirectory(llvm::COFF::EXPORT_TABLE);
    DirectoryRVA = D->RelativeVirtualAddress;
    DataDirectoryOffset =
        reinterpret_cast<const uint8_t *>(D) - Image.Raw.data();
    auto Position = [&](uint32_t RVA) {
      uintptr_t Pointer = 0;
      llvm::cantFail((*Object)->getRvaPtr(RVA, Pointer));
      return reinterpret_cast<const uint8_t *>(Pointer) - Image.Raw.data();
    };
    DirectoryOffset = Position(DirectoryRVA);
    auto Field = [&](unsigned Offset) {
      return llvm::support::endian::read32le(Image.Raw.data() +
                                             DirectoryOffset + Offset);
    };
    EATOffset = Position(Field(28));
    OrdinalOffset = Position(Field(36));
    NameOffset = Position(llvm::support::endian::read32le(Image.Raw.data() +
                                                          Position(Field(32))));
    for (const auto &Section : (*Object)->sections()) {
      const auto *Raw = (*Object)->getCOFFSection(Section);
      if (DirectoryRVA >= Raw->VirtualAddress &&
          DirectoryRVA - Raw->VirtualAddress < Raw->SizeOfRawData)
        SectionOffset =
            reinterpret_cast<const uint8_t *>(Raw) - Image.Raw.data();
    }
    ASSERT_NE(SectionOffset, 0u);
  }
  llvm::Expected<std::unique_ptr<llvm::object::COFFObjectFile>> object() {
    return llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(Image.Raw.data()),
                        Image.Raw.size()),
        "export-test"));
  }
  llvm::Expected<KernelReferenceExports> metadata(uint64_t Limit = 4 * 1024 *
                                                                   1024) {
    auto Object = object();
    if (!Object)
      return Object.takeError();
    return KernelReferenceExports::load(Image, **Object, Base, Limit);
  }
  void put32(size_t Offset, uint32_t Value) {
    llvm::support::endian::write32le(Image.Raw.data() + Offset, Value);
  }
};

TEST_F(KernelReferenceExportsTest, OriginalTablesAndNamespaceShareRVAIdentity) {
  auto Data = metadata();
  ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
  auto Reference = KernelReferenceImage::load(
      exportFixture("driver_kernel_reference"), 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  EXPECT_TRUE(Data->permitsRead(Base + DirectoryRVA, 40));
  EXPECT_FALSE(Data->permitsRead(Base + DirectoryRVA - 1, 41));
  EXPECT_FALSE(Data->permitsRead(UINT64_MAX, 2));
  EXPECT_EQ(Data->names().size(), 3u);
  DriverOptions Options;
  KernelExportRegistry Registry;
  llvm::cantFail(Registry.initialize(Options));
  auto E = Registry.attachReference(*Reference, Options);
  ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  for (const auto &[Name, RVA] : Data->names()) {
    if (Name == "ReferenceData")
      EXPECT_EQ(Reference->exports().at(Name), 0u);
    else {
      EXPECT_EQ(Reference->exports().at(Name), Base + RVA);
      EXPECT_EQ(llvm::cantFail(Registry.resolve(Name)), Base + RVA);
    }
  }
  auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
  llvm::cantFail(Reference->map(*Backend));
  for (const auto &Page : Data->pages()) {
    std::vector<uint8_t> ReadBack(Page.Bytes.size());
    llvm::cantFail(Backend->read(Page.Address, ReadBack));
    EXPECT_EQ(ReadBack, Page.Bytes);
    EXPECT_FALSE(llvm::cantFail(Backend->canAccess(Page.Address, 1, Write)));
    EXPECT_FALSE(llvm::cantFail(Backend->canAccess(Page.Address, 1, Execute)));
  }
}

TEST_F(KernelReferenceExportsTest,
       MalformedTablesStringsAndOrdinalsFailClearly) {
  const auto Original = Image.Raw;
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    Image.Raw = Original;
    switch (Case) {
    case 0:
      put32(DataDirectoryOffset + 4, 39);
      break;
    case 1:
      put32(DirectoryOffset + 20, 65537);
      break;
    case 2:
      put32(DirectoryOffset + 24, 65537);
      break;
    case 3:
      put32(DirectoryOffset + 28, 0xfffffffc);
      break;
    case 4:
      llvm::support::endian::write16le(Image.Raw.data() + OrdinalOffset,
                                       0xffff);
      break;
    case 5:
      put32(DirectoryOffset + 12, 0);
      break;
    case 6:
      put32(EATOffset, 0xffffffff);
      break;
    case 7:
      put32(DataDirectoryOffset + 4, 0xffffffff);
      break;
    case 8: {
      auto Flags = llvm::support::endian::read32le(Image.Raw.data() +
                                                   SectionOffset + 36);
      put32(SectionOffset + 36, Flags | llvm::COFF::IMAGE_SCN_MEM_WRITE);
      break;
    }
    case 9: {
      auto Flags = llvm::support::endian::read32le(Image.Raw.data() +
                                                   SectionOffset + 36);
      put32(SectionOffset + 36, Flags | llvm::COFF::IMAGE_SCN_MEM_EXECUTE);
      break;
    }
    case 10:
      std::fill_n(Image.Raw.begin() + NameOffset,
                  std::min<size_t>(profile::MaxKernelExportNameSize + 1,
                                   Image.Raw.size() - NameOffset),
                  'A');
      break;
    case 11:
      put32(DirectoryOffset, 1);
      break;
    }
    auto Data = metadata();
    ASSERT_FALSE(bool(Data));
    EXPECT_FALSE(llvm::toString(Data.takeError()).empty());
  }
  Image.Raw = Original;
  auto TooSmall = metadata(39);
  ASSERT_FALSE(bool(TooSmall));
  EXPECT_NE(llvm::toString(TooSmall.takeError()).find("budget"),
            std::string::npos);
}

TEST_F(KernelReferenceExportsTest, ForwardersAndRelocationSpansRemainExplicit) {
  auto Data = metadata();
  ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
  const auto DLLName =
      llvm::support::endian::read32le(Image.Raw.data() + DirectoryOffset + 12);
  const auto Original = Image.Raw;
  put32(EATOffset, DLLName);
  auto Forwarded = metadata();
  ASSERT_TRUE(bool(Forwarded)) << llvm::toString(Forwarded.takeError());
  // The DLL's own name is a finite module.symbol spelling in this fixture;
  // observing a forwarder does not resolve or execute it.
  Image.Raw = Original;
  Image.BaseRelocations.emplace_back();
  Image.BaseRelocations.back().Address = Image.Base + DirectoryRVA - 1;
  Image.BaseRelocations.back().Type = llvm::COFF::IMAGE_REL_BASED_DIR64;
  auto Overlap = metadata();
  ASSERT_FALSE(bool(Overlap));
  EXPECT_NE(llvm::toString(Overlap.takeError()).find("relocation"),
            std::string::npos);
  Image.BaseRelocations.back().Address = Image.Base + DirectoryRVA - 16;
  auto Outside = metadata();
  ASSERT_TRUE(bool(Outside)) << llvm::toString(Outside.takeError());
}

TEST_F(KernelReferenceExportsTest, FoldedAliasesRetainAmbiguousTrapIdentity) {
  auto Data = metadata();
  ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
  const uint32_t RVA = Data->names().at("KeGetCurrentIrql");
  const uint32_t Count =
      llvm::support::endian::read32le(Image.Raw.data() + DirectoryOffset + 20);
  for (uint32_t I = 0; I < Count; ++I)
    put32(EATOffset + I * 4, RVA);
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(bool(
      llvm::sys::fs::createUniqueDirectory("neverd-export-alias", Temporary)));
  const auto Directory = std::filesystem::path(Temporary.str().str());
  const auto Path = Directory / "reference.sys";
  {
    std::ofstream File(Path, std::ios::binary);
    File.write(reinterpret_cast<const char *>(Image.Raw.data()),
               Image.Raw.size());
    ASSERT_TRUE(File);
  }
  auto Reference = KernelReferenceImage::load(Path, 4 * 1024 * 1024);
  std::error_code Ignored;
  std::filesystem::remove(Path, Ignored);
  std::filesystem::remove(Directory, Ignored);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  EXPECT_EQ(Reference->exports().size(), Data->names().size());
  for (const auto &[Name, Address] : Reference->exports()) {
    EXPECT_EQ(Address, 0u) << Name;
    EXPECT_EQ(Reference->exportData().names().at(Name), RVA);
  }
}

TEST_F(KernelReferenceExportsTest,
       MetadataCannotBecomeMutableObjectTypeStorage) {
  auto Object = object();
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  const uint32_t PointersRVA =
      llvm::support::endian::read32le(Image.Raw.data() + DirectoryOffset + 32);
  uintptr_t Pointer = 0;
  llvm::cantFail((*Object)->getRvaPtr(PointersRVA, Pointer));
  const size_t PointersOffset =
      reinterpret_cast<const uint8_t *>(Pointer) - Image.Raw.data();
  const uint32_t Second =
      llvm::support::endian::read32le(Image.Raw.data() + PointersOffset + 4);
  const uint32_t Third =
      llvm::support::endian::read32le(Image.Raw.data() + PointersOffset + 8);
  llvm::cantFail((*Object)->getRvaPtr(Third, Pointer));
  std::copy_n("PsThreadType", 13, reinterpret_cast<uint8_t *>(Pointer));
  // Keep names sorted and their ordinal associations intact after renaming.
  put32(PointersOffset + 4, Third);
  put32(PointersOffset + 8, Second);
  const auto SecondOrdinal =
      llvm::support::endian::read16le(Image.Raw.data() + OrdinalOffset + 2);
  const auto ThirdOrdinal =
      llvm::support::endian::read16le(Image.Raw.data() + OrdinalOffset + 4);
  llvm::support::endian::write16le(Image.Raw.data() + OrdinalOffset + 2,
                                   ThirdOrdinal);
  llvm::support::endian::write16le(Image.Raw.data() + OrdinalOffset + 4,
                                   SecondOrdinal);
  ASSERT_EQ(PointersRVA % 8, 0u);
  put32(EATOffset + ThirdOrdinal * 4, PointersRVA);
  // The short directory excludes this RVA from the forwarder interval. The
  // independently declared table pointers still validate as PE metadata.
  put32(DataDirectoryOffset + 4, 40);
  auto Data = metadata();
  ASSERT_TRUE(bool(Data)) << llvm::toString(Data.takeError());
  EXPECT_EQ(Data->names().at("PsThreadType"), PointersRVA);
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(bool(
      llvm::sys::fs::createUniqueDirectory("neverd-export-owner", Temporary)));
  const auto Directory = std::filesystem::path(Temporary.str().str());
  const auto Path = Directory / "reference.sys";
  {
    std::ofstream File(Path, std::ios::binary);
    File.write(reinterpret_cast<const char *>(Image.Raw.data()),
               Image.Raw.size());
    ASSERT_TRUE(File);
  }
  auto Reference = KernelReferenceImage::load(Path, 4 * 1024 * 1024);
  std::error_code Ignored;
  std::filesystem::remove(Path, Ignored);
  std::filesystem::remove(Directory, Ignored);
  ASSERT_FALSE(bool(Reference));
  EXPECT_NE(
      llvm::toString(Reference.takeError()).find("object-type data overlaps"),
      std::string::npos);
}

TEST(KernelReferenceExportAccess, ReadsAreExactAndWritesAndPaddingStayOpaque) {
  for (bool WithPDB : {false, true}) {
    DriverOptions Options;
    Options.KernelImage = exportFixture("driver_kernel_debugger_reference");
    Options.PageTables = true;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    if (WithPDB) {
      Options.KernelPDB = Options.KernelImage;
      Options.KernelPDB.replace_extension(".pdb");
    }
    auto Reference = KernelReferenceImage::load(
        Options.KernelImage, Options.MemoryLimit, Options.KernelPDB);
    ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
    auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
    llvm::cantFail(Reference->map(*Backend));
    constexpr uint64_t Scratch = 0x60000000;
    llvm::cantFail(Backend->map(Scratch, 4096, Read | Write));
    DriverResult Result;
    KernelModel Model(*Backend, Result);
    llvm::cantFail(Model.setDebuggerReference(*Reference));
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    llvm::cantFail(Model.initialize(Image, Options));
    Model.enterForeground();
    Model.enterExecution(profile::StackBase);
    const auto &Data = Reference->exportData();
    bool Padding = false, Observed = false;
    uint64_t ObservedAddress = 0;
    for (const auto &Page : Data.pages())
      for (uint64_t I = 0; I < Page.Bytes.size(); ++I) {
        const uint64_t Address = Page.Address + I;
        if (Data.permitsRead(Address, 1)) {
          if (!Observed) {
            llvm::cantFail(Model.validateGuestAccess(Address, 1, false));
            auto Write = Model.validateGuestAccess(Address, 1, true);
            ASSERT_TRUE(bool(Write));
            EXPECT_NE(llvm::toString(std::move(Write)).find("read-only"),
                      std::string::npos);
            Observed = true;
            ObservedAddress = Address;
          }
        } else if (!Padding) {
          auto Read = Model.validateGuestAccess(Address, 1, false);
          ASSERT_TRUE(bool(Read));
          llvm::consumeError(std::move(Read));
          Padding = true;
        }
        if (Padding && Observed)
          break;
      }
    EXPECT_TRUE(Padding);
    EXPECT_TRUE(Observed);
    ASSERT_NE(ObservedAddress, 0u);
    // Section aliases must use the canonical reference byte policy too. With
    // a PDB the page is shared with model-owned fields and may be backend RW;
    // the metadata itself remains guest read-only.
    const auto Call = [&](const char *Name, std::initializer_list<uint64_t> A) {
      return Model.call(Name, A);
    };
    const auto Put = [&](uint64_t Offset, uint64_t Value, unsigned Width = 8) {
      llvm::cantFail(Backend->writeInteger(Scratch + Offset, Value, Width));
    };
    const std::u16string Section = u"\\Device\\PhysicalMemory";
    for (size_t I = 0; I <= Section.size(); ++I)
      Put(0x200 + I * 2, I == Section.size() ? 0 : Section[I], 2);
    llvm::cantFail(
        Call("RtlInitUnicodeString", {Scratch + 0x100, Scratch + 0x200}));
    Put(0x300, 48, 4);
    Put(0x310, Scratch + 0x100);
    Put(0x318, 0x40, 4);
    ASSERT_EQ(llvm::cantFail(
                  Call("ZwOpenSection", {Scratch, 0xf001f, Scratch + 0x300})),
              0u);
    const auto Handle = llvm::cantFail(Backend->readInteger(Scratch, 8));
    const auto Physical =
        llvm::cantFail(Call("MmGetPhysicalAddress", {ObservedAddress}));
    Put(0x408, 4096);
    Put(0x410, Physical & ~uint64_t(4095));
    ASSERT_EQ(llvm::cantFail(Call("ZwMapViewOfSection",
                                  {Handle, UINT64_MAX, Scratch + 0x400, 0, 0,
                                   Scratch + 0x410, Scratch + 0x408, 2, 0,
                                   WithPDB ? 4u : 2u})),
              0u);
    const auto View = llvm::cantFail(Backend->readInteger(Scratch + 0x400, 8));
    const auto Alias = View + (Physical & 4095);
    llvm::cantFail(Model.validateGuestAccess(Alias, 1, false));
    EXPECT_EQ(llvm::cantFail(Backend->readInteger(Alias, 1)),
              llvm::cantFail(Backend->readInteger(ObservedAddress, 1)));
    if (WithPDB) {
      auto Write = Model.validateGuestAccess(Alias, 1, true);
      ASSERT_TRUE(bool(Write));
      EXPECT_NE(llvm::toString(std::move(Write)).find("read-only"),
                std::string::npos);
    } else
      EXPECT_FALSE(llvm::cantFail(Backend->canAccess(Alias, 1, Write)));
    EXPECT_EQ(llvm::cantFail(Call("ZwUnmapViewOfSection", {UINT64_MAX, View})),
              0u);
    EXPECT_EQ(llvm::cantFail(Call("ZwClose", {Handle})), 0u);
  }
}

TEST(KernelReferenceExportAccess, CompiledWalkerCallsTheSameModeledExport) {
  for (bool WithPDB : {false, true})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      DriverOptions Options;
      Options.Contract = Contract;
      Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
      Options.KernelImage = exportFixture("driver_kernel_debugger_reference");
      if (WithPDB) {
        Options.KernelPDB = Options.KernelImage;
        Options.KernelPDB.replace_extension(".pdb");
      }
      Options.KernelExports["ZwQuerySystemInformation"] = true;
      auto Result = emulateDriver(exportFixture("driver_export_walk"), Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_EQ(Result->NTStatus, 0u);
      ASSERT_EQ(Result->Calls.size(), 2u);
      EXPECT_EQ(Result->Calls.back().Name, "KeGetCurrentIrql");
      EXPECT_GE(Result->Calls.back().PC, 0xfffff80000000000);
    }
}

// Use bytes from the relocated reference as an explicit scanner input. The
// guest finds the address itself and calls it without walking the export table.
// The nonzero interior offset must not gain execution rights from that scan.
void checkSignatureClient(const DriverOptions &BaseOptions) {
  auto Reference = KernelReferenceImage::load(
      BaseOptions.KernelImage, BaseOptions.MemoryLimit, BaseOptions.KernelPDB);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  const auto Target = Reference->exports().at("KeGetCurrentIrql");
  ASSERT_GT(Target, Reference->base() + 16);
  auto Memory = UnicornBackend::create(BaseOptions.MemoryLimit);
  ASSERT_TRUE(bool(Memory)) << llvm::toString(Memory.takeError());
  auto Mapped = Reference->map(**Memory);
  ASSERT_FALSE(bool(Mapped)) << llvm::toString(std::move(Mapped));
  std::array<uint8_t, 8> Signature;
  auto Read = (*Memory)->read(Target, Signature);
  ASSERT_FALSE(bool(Read)) << llvm::toString(std::move(Read));

  auto Client = Loader::create(BinaryFormat::COFF)
                    ->load(exportFixture("driver_kernel_signature"));
  ASSERT_TRUE(bool(Client)) << llvm::toString(Client.takeError());
  // Unique initialized fixture data avoids requiring an export directory in
  // the executable driver profile. All four words are replaced before a run.
  const std::array<uint8_t, 16> Marker{0x5a, 0x5a, 0x5a, 0x5a, 0xfe, 0xca,
                                       0xad, 0x0b, 0xdf, 0x9b, 0x57, 0x13,
                                       0xe0, 0xac, 0x68, 0x24};
  const auto Found = std::search(Client->Raw.begin(), Client->Raw.end(),
                                 Marker.begin(), Marker.end());
  ASSERT_NE(Found, Client->Raw.end());
  ASSERT_EQ(
      std::search(Found + 1, Client->Raw.end(), Marker.begin(), Marker.end()),
      Client->Raw.end());
  const auto Offset = Found - Client->Raw.begin();
  ASSERT_LE(Offset + 32, Client->Raw.size());
  llvm::SmallString<128> Path;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-kernel-signature",
                                                  "sys", Path));
  llvm::FileRemover Cleanup(Path);
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (unsigned Mode = 0; Mode < 3; ++Mode) {
        SCOPED_TRACE(Mode);
        auto Bytes = Client->Raw;
        auto *Data = Bytes.data() + Offset;
        llvm::support::endian::write32le(Data, Target - Reference->base() - 16);
        llvm::support::endian::write32le(Data + 4, 32);
        llvm::support::endian::write32le(Data + 8, Signature.size());
        llvm::support::endian::write32le(Data + 12, Mode == 2 ? 1 : 0);
        std::copy(Signature.begin(), Signature.end(), Data + 16);
        if (Mode == 1)
          Data[16] ^= 0xff;
        std::ofstream Output(Path.c_str(), std::ios::binary);
        Output.write(reinterpret_cast<const char *>(Bytes.data()),
                     Bytes.size());
        Output.close();
        ASSERT_TRUE(Output);
        auto Options = BaseOptions;
        Options.Contract = Contract;
        if (Preemptive)
          Options.Scheduling = DriverScheduling{23, 1};
        auto Result =
            emulateDriver(std::filesystem::path(Path.c_str()), Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->KernelImageSHA256, Reference->digest());
        EXPECT_EQ(Result->KernelPDBSHA256, Reference->pdbDigest());
        if (Mode == 2) {
          EXPECT_EQ(Result->Stop, DriverStopReason::UnsupportedAPI)
              << Result->Diagnostic;
          EXPECT_EQ(Result->PC, Target + 1);
          EXPECT_FALSE(Result->NTStatus);
          EXPECT_NE(Result->Diagnostic.find("inside reference kernel"),
                    std::string::npos);
        } else {
          EXPECT_EQ(Result->Stop, DriverStopReason::Returned)
              << Result->Diagnostic;
          EXPECT_EQ(Result->NTStatus, Mode ? 0xc0000225u : 0u);
        }
        ASSERT_EQ(Result->Calls.size(), Mode ? 1u : 2u);
        if (!Mode) {
          EXPECT_EQ(Result->Calls.back().Name, "KeGetCurrentIrql");
          EXPECT_EQ(Result->Calls.back().PC, Target);
        }
      }
}
TEST(KernelReferenceExportAccess,
     SignatureScanCallsModelAndRejectsInteriorCode) {
  DriverOptions Options;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.KernelExports["ZwQuerySystemInformation"] = true;
  Options.KernelImage = exportFixture("driver_kernel_reference");
  checkSignatureClient(Options);
}
TEST(KernelReferenceExportAccess,
     MatchedNativeBundleSupportsTheSameSignatureClient) {
  const auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE");
  const auto *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  DriverOptions Options;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.KernelImage = Image;
  Options.KernelPDB = PDB;
  checkSignatureClient(Options);
}
} // namespace
} // namespace neverd::emulation
