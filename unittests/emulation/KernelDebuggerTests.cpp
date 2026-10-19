//===- KernelDebuggerTests.cpp - Matched PE/PDB guest debugger facts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Matched PE/PDB guest debugger facts.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/driver/GuardFunctionTable.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelObjectTypes.h"
#include "os/windows/kernel/KernelReferenceImage.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <fstream>

namespace neverd::emulation {
namespace {
std::filesystem::path kdFixture(const char *Name,
                                const char *Extension = ".sys") {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + Extension);
}
DriverOptions kdOptions() {
  DriverOptions Options;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.PoolTagPolicy = DriverPoolTagPolicy::Windows19041;
  Options.KernelImage = kdFixture("driver_kernel_debugger_reference");
  Options.KernelPDB = kdFixture("driver_kernel_debugger_reference", ".pdb");
  for (const char *Name : {"driver_kd_client", "driver_kd_unknown_global"}) {
    auto Image = loadDriverImage(kdFixture(Name), Options.MemoryLimit);
    if (!Image) {
      ADD_FAILURE() << llvm::toString(Image.takeError());
      continue;
    }
    for (const auto &Import : Image->Imports)
      Options.KernelExports[Import.Name] = true;
  }
  return Options;
}
const DriverAPIEvent *kdCall(const DriverResult &Result, const char *Name) {
  for (const auto &Call : Result.Calls)
    if (Call.Name == Name)
      return &Call;
  return nullptr;
}
TEST(KernelDebuggerReference,
     ExactCompanionAuthenticatesRelocatableLayoutFacts) {
  const auto Options = kdOptions();
  auto Reference = KernelReferenceImage::load(
      Options.KernelImage, 4 * 1024 * 1024, Options.KernelPDB);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  ASSERT_TRUE(Reference->debugger());
  const auto &Facts = *Reference->debugger();
  EXPECT_EQ(Reference->pdbDigest().size(), 64u);
  EXPECT_EQ(Facts.Template.size(), 0x380u);
  EXPECT_EQ(Facts.ProcessSize, 0xa40u);
  EXPECT_EQ(Facts.ThreadSize, 0x898u);
  EXPECT_GE(Facts.Address, Reference->base());
  EXPECT_GE(Facts.ListHead, Reference->base());
  EXPECT_GE(Facts.LoaderBlock, Reference->base());
  EXPECT_GE(Facts.PagedPoolCommit, Reference->base());
  EXPECT_GE(llvm::support::endian::read64le(Facts.Template.data() + 0x48),
            Reference->base());
  EXPECT_EQ(llvm::support::endian::read64le(Facts.Template.data() + 0x138),
            4096u);
  EXPECT_FALSE(Reference->opaqueData().empty());
}

TEST(KernelDebuggerReference,
     ThreadFieldsUseAuthenticatedAccessorsAndStayReadOnly) {
  const auto Options = kdOptions();
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
  const auto &Facts = *Reference.debugger();
  EXPECT_EQ(Facts.ThreadIDOffset, 0x198u);
  EXPECT_EQ(Facts.ThreadProcessIDOffset, 0x190u);
  EXPECT_EQ(Facts.ThreadProcessOffset, 0x1c0u);
  EXPECT_EQ(Facts.ThreadWin32StartOffset, 0x1e0u);
  EXPECT_EQ(Facts.ThreadWin32ThreadOffset, 0x1d0u);
  auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
  llvm::cantFail(Reference.map(*Backend));
  DriverResult Result;
  KernelModel Model(*Backend, Result);
  llvm::cantFail(Model.setDebuggerReference(Reference));
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, Options));
  Model.enterExecution(profile::StackBase);
  const auto Thread = llvm::cantFail(Model.call("PsGetCurrentThread", {}));
  const auto ID = llvm::cantFail(Model.call("PsGetCurrentThreadId", {}));
  const auto Process =
      llvm::cantFail(Model.call("PsGetThreadProcess", {Thread}));
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(Thread + 0x198, 8)), ID);
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(Thread + 0x190, 8)), 4u);
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(Thread + 0x1c0, 8)), Process);
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(Thread + 0x1e0, 8)),
            Image.Entry);
  EXPECT_EQ(llvm::cantFail(Model.call("PsGetCurrentThreadWin32Thread", {})),
            0u);
  llvm::cantFail(
      Model.call("PsSetThreadWin32Thread", {Thread, Image.Entry, 0}));
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(Thread + 0x1d0, 8)),
            Image.Entry);
  EXPECT_EQ(llvm::cantFail(Model.call("PsGetThreadWin32Thread", {Thread})),
            Image.Entry);
  for (auto [Offset, Size] : {std::pair<uint64_t, uint32_t>{0x190, 16},
                              {0x195, 7},
                              {0x1c0, 8},
                              {0x1d0, 8},
                              {0x1e0, 8}})
    llvm::cantFail(Model.validateGuestAccess(Thread + Offset, Size, false));
  for (auto [Offset, Size] : {std::pair<uint64_t, uint32_t>{0, 8},
                              {0x18f, 2},
                              {0x19f, 2},
                              {0x1c7, 2},
                              {0x1cf, 2},
                              {0x1d7, 2},
                              {0x1df, 2},
                              {0x1e7, 2},
                              {0x478, 16}}) {
    auto Denied = Model.validateGuestAccess(Thread + Offset, Size, false);
    EXPECT_TRUE(bool(Denied));
    llvm::consumeError(std::move(Denied));
  }
  for (auto Offset : {0x190u, 0x198u, 0x1c0u, 0x1d0u, 0x1e0u}) {
    auto Denied = Model.validateGuestAccess(Thread + Offset, 1, true);
    EXPECT_TRUE(bool(Denied));
    llvm::consumeError(std::move(Denied));
  }
  llvm::cantFail(Model.setUserRequestContext(true, 0x3456));
  const auto User = llvm::cantFail(Model.call("PsGetCurrentThread", {}));
  EXPECT_NE(User, Thread);
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(User + 0x190, 8)), 0x3456u);
  llvm::cantFail(Model.validateGuestAccess(User + 0x190, 16, false));
  auto MissingStart = Model.validateGuestAccess(User + 0x1e0, 8, false);
  EXPECT_TRUE(bool(MissingStart));
  llvm::consumeError(std::move(MissingStart));
  auto MissingWin32 = Model.validateGuestAccess(User + 0x1d0, 8, false);
  EXPECT_TRUE(bool(MissingWin32));
  llvm::consumeError(std::move(MissingWin32));
  llvm::cantFail(Model.call("PsSetThreadWin32Thread", {User, Image.Entry, 0}));
  llvm::cantFail(Model.validateGuestAccess(User + 0x1d0, 8, false));
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(User + 0x1d0, 8)), Image.Entry);
  llvm::cantFail(Model.call("PsSetThreadWin32Thread", {User, 0, Image.Entry}));
  EXPECT_EQ(llvm::cantFail(Model.call("PsGetCurrentThreadWin32Thread", {})),
            0u);
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(User + 0x1d0, 8)), 0u);
}

TEST(KernelDebuggerReference,
     ThreadFieldsNeedCompanionAndPreserveRetainedObjects) {
  for (bool WithPDB : {false, true}) {
    auto Options = kdOptions();
    if (!WithPDB)
      Options.KernelPDB.clear();
    auto Reference = llvm::cantFail(KernelReferenceImage::load(
        Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
    auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
    llvm::cantFail(Reference.map(*Backend));
    llvm::cantFail(Backend->map(0x60000000, 4096, Read | Write));
    DriverResult Result;
    KernelModel Model(*Backend, Result);
    llvm::cantFail(Model.setDebuggerReference(Reference));
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    llvm::cantFail(Model.initialize(Image, Options));
    Model.enterExecution(profile::StackBase);
    llvm::cantFail(
        Model.call("PsCreateSystemThread",
                   {0x60000000, 0x1fffff, 0, 0, 0, Image.Entry + 0x100, 0}));
    const auto Handle = llvm::cantFail(Backend->readInteger(0x60000000, 8));
    llvm::cantFail(Model.call("ObReferenceObjectByHandle",
                              {Handle, 0, 0, 0, 0x60000008, 0}));
    const auto Thread = llvm::cantFail(Backend->readInteger(0x60000008, 8));
    const auto Query = [&] {
      auto Access =
          Model.validateGuestAccess(Thread + (WithPDB ? 0x198 : 0), 8, false);
      EXPECT_EQ(bool(Access), !WithPDB);
      llvm::consumeError(std::move(Access));
      if (WithPDB) {
        EXPECT_EQ(llvm::cantFail(Backend->readInteger(Thread + 0x198, 8)),
                  llvm::cantFail(Model.call("PsGetThreadId", {Thread})));
        EXPECT_EQ(llvm::cantFail(Backend->readInteger(Thread + 0x1e0, 8)),
                  Image.Entry + 0x100);
      }
    };
    Query();
    auto Call = llvm::cantFail(Model.nextScheduled(false));
    ASSERT_TRUE(Call);
    Model.enterExecution(Call->ID);
    llvm::cantFail(Model.call("PsTerminateSystemThread", {0}));
    ASSERT_TRUE(Model.takeThreadTermination());
    llvm::cantFail(Model.finishScheduled(Call->ID));
    Model.enterExecution(profile::StackBase);
    llvm::cantFail(Model.call("ZwClose", {Handle}));
    Query();
    llvm::cantFail(Model.call("ObfDereferenceObject", {Thread}));
    auto Freed =
        Model.validateGuestAccess(Thread + (WithPDB ? 0x198 : 0), 8, false);
    ASSERT_TRUE(bool(Freed));
    EXPECT_NE(llvm::toString(std::move(Freed)).find("freed"),
              std::string::npos);
  }
}

TEST(KernelDebuggerReference, ThreadAccessorsRejectMalformedFieldsAndShapes) {
  const auto Options = kdOptions();
  auto Buffer = llvm::cantFail(llvm::errorOrToExpected(
      llvm::MemoryBuffer::getFile(Options.KernelImage.string())));
  const auto Data = Buffer->getBuffer();
  std::vector<uint8_t> Original(Data.bytes_begin(), Data.bytes_end());
  auto Object =
      llvm::cantFail(llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Original.data()),
                          Original.size()),
          "thread-fields")));
  std::map<std::string, size_t> Positions;
  for (const auto &Export : Object->export_directories()) {
    llvm::StringRef Name;
    uint32_t RVA = 0;
    uintptr_t Address = 0;
    llvm::cantFail(Export.getSymbolName(Name));
    if (Name != "PsGetThreadId" && Name != "PsQueryThreadStartAddress" &&
        Name != "PsGetThreadWin32Thread" &&
        Name != "PsGetCurrentThreadWin32Thread")
      continue;
    llvm::cantFail(Export.getExportRVA(RVA));
    llvm::cantFail(Object->getRvaPtr(RVA, Address));
    Positions[Name.str()] =
        reinterpret_cast<const uint8_t *>(Address) - Original.data();
  }
  ASSERT_EQ(Positions.size(), 4u);
  llvm::SmallString<128> Directory;
  llvm::cantFail(llvm::errorCodeToError(
      llvm::sys::fs::createUniqueDirectory("neverd-thread-fields", Directory)));
  const auto Path =
      std::filesystem::path(Directory.str().str()) / "reference.sys";
  for (unsigned Case = 0; Case < 13; ++Case) {
    auto Bytes = Original;
    auto *ID = Bytes.data() + Positions.at("PsGetThreadId");
    auto *Start = Bytes.data() + Positions.at("PsQueryThreadStartAddress");
    if (Case < 4) {
      const uint32_t Offsets[] = {0, 0x199, 0x898, 0x190};
      llvm::support::endian::write32le(ID + 3, Offsets[Case]);
    } else if (Case == 4)
      ID[7] = 0x90;
    else if (Case == 5)
      Start[18] = 8;
    else if (Case == 6)
      llvm::support::endian::write32le(Start + 22, 0x198);
    else if (Case < 11) {
      const uint32_t Offsets[] = {0, 0x1d1, 0x898, 0x198};
      llvm::support::endian::write32le(
          Bytes.data() + Positions.at("PsGetThreadWin32Thread") + 3,
          Offsets[Case - 7]);
    } else if (Case == 11)
      Bytes[Positions.at("PsGetCurrentThreadWin32Thread") + 5] = 0x90;
    else
      Bytes[Positions.at("PsGetCurrentThreadWin32Thread") + 16] = 0x90;
    {
      std::ofstream Output(Path, std::ios::binary);
      Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
      ASSERT_TRUE(Output);
    }
    auto Reference = KernelReferenceImage::load(Path, Options.MemoryLimit,
                                                Options.KernelPDB);
    ASSERT_FALSE(bool(Reference)) << "case " << Case;
    EXPECT_NE(llvm::toString(Reference.takeError()).find("thread"),
              std::string::npos)
        << "case " << Case;
  }
  std::error_code Ignored;
  std::filesystem::remove(Path, Ignored);
  std::filesystem::remove(Path.parent_path(), Ignored);
}

TEST(KernelDebuggerReference, CompiledThreadFieldClientAgreesWithModeledAPIs) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    auto Options = kdOptions();
    Options.Contract = Contract;
    const auto Client = kdFixture("driver_thread_fields");
    auto Image = llvm::cantFail(loadDriverImage(Client, Options.MemoryLimit));
    for (const auto &Import : Image.Imports)
      Options.KernelExports[Import.Name] = true;
    auto Result = llvm::cantFail(emulateDriver(Client, Options));
    EXPECT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
    EXPECT_EQ(Result.NTStatus, 0u);
  }
}

TEST(KernelDebuggerReference, LStarMatchesUnifiedKernelAndRequiresCompanion) {
  for (bool WithPDB : {false, true}) {
    auto Options = kdOptions();
    if (!WithPDB)
      Options.KernelPDB.clear();
    auto Reference = llvm::cantFail(KernelReferenceImage::load(
        Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
    auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
    llvm::cantFail(Reference.map(*Backend));
    llvm::cantFail(Backend->map(0x60000000, 4096, Read | Write));
    DriverResult Result;
    KernelModel Model(*Backend, Result);
    llvm::cantFail(Model.setDebuggerReference(Reference));
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    llvm::cantFail(Model.initialize(Image, Options));
    EXPECT_TRUE(Model.hasModelSpecificRegisters()); // Fixed CET state is owned.
    auto LStar = Model.readModelSpecificRegister(0xc0000082);
    if (WithPDB) {
      ASSERT_TRUE(bool(LStar)) << llvm::toString(LStar.takeError());
      EXPECT_EQ(*LStar, Reference.debugger()->UnifiedSystemCallEntry);
      EXPECT_EQ(*LStar, Reference.exports().at("KiSystemCall64"));
      EXPECT_EQ((llvm::cantFail(Backend->readInteger(*LStar, 4)) & 0xffffff),
                0xf8010fu);
    } else {
      ASSERT_FALSE(bool(LStar));
      llvm::consumeError(LStar.takeError());
    }
    EXPECT_EQ(llvm::cantFail(Model.call("ZwQuerySystemInformation",
                                        {196, 0x60000000, 4, 0})),
              0u);
    EXPECT_EQ(llvm::cantFail(Backend->readInteger(0x60000000, 4)), 0u);
    for (uint32_t Index : {0u, 0xc0000080u, 0xc0000081u, 0xc0000083u}) {
      auto Unknown = Model.readModelSpecificRegister(Index);
      EXPECT_FALSE(bool(Unknown));
      llvm::consumeError(Unknown.takeError());
    }
  }
}

TEST(KernelDebuggerReference,
     OriginalMSRClientsKeepWidthsFlagsAndStrictWrites) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    auto Options = kdOptions();
    Options.Contract = Contract;
    auto R = llvm::cantFail(emulateDriver(kdFixture("driver_msr"), Options));
    EXPECT_EQ(R.Stop, DriverStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.NTStatus, 0u);
    for (const auto *Name : {"driver_msr_unknown", "driver_msr_write"}) {
      auto Denied = llvm::cantFail(emulateDriver(kdFixture(Name), Options));
      EXPECT_EQ(Denied.Stop, DriverStopReason::UnsupportedInstruction)
          << Denied.Diagnostic;
      EXPECT_NE(Denied.Diagnostic.find(
                    Name == std::string_view("driver_msr_unknown") ? "C0000081"
                                                                   : "wrmsr"),
                std::string::npos);
    }
    Options.KernelPDB.clear();
    auto Missing =
        llvm::cantFail(emulateDriver(kdFixture("driver_msr"), Options));
    EXPECT_EQ(Missing.Stop, DriverStopReason::UnsupportedInstruction);
    EXPECT_NE(Missing.Diagnostic.find("C0000082"), std::string::npos);
  }
}

TEST(KernelDebuggerReference, RejectsShadowPrefixAsUnifiedLStar) {
  const auto Options = kdOptions();
  auto Buffer = llvm::cantFail(llvm::errorOrToExpected(
      llvm::MemoryBuffer::getFile(Options.KernelImage.string())));
  auto Data = Buffer->getBuffer();
  std::vector<uint8_t> Bytes(Data.bytes_begin(), Data.bytes_end());
  auto Object =
      llvm::cantFail(llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                          Bytes.size()),
          "lstar-prefix")));
  size_t Position = 0;
  for (const auto &Export : Object->export_directories()) {
    llvm::StringRef Name;
    llvm::cantFail(Export.getSymbolName(Name));
    if (Name != "KiSystemCall64")
      continue;
    uint32_t RVA = 0;
    uintptr_t Address = 0;
    llvm::cantFail(Export.getExportRVA(RVA));
    llvm::cantFail(Object->getRvaPtr(RVA, Address));
    Position = reinterpret_cast<const uint8_t *>(Address) - Bytes.data();
  }
  ASSERT_NE(Position, 0u);
  ASSERT_LE(Position + 12, Bytes.size());
  Bytes[Position + 9] = 0x90; // GS:[0x9010] belongs to the shadow entry.
  llvm::SmallString<128> Directory;
  llvm::cantFail(llvm::errorCodeToError(
      llvm::sys::fs::createUniqueDirectory("neverd-lstar-prefix", Directory)));
  const auto Path =
      std::filesystem::path(Directory.str().str()) / "reference.sys";
  {
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    ASSERT_TRUE(Output);
  }
  auto Reference =
      KernelReferenceImage::load(Path, Options.MemoryLimit, Options.KernelPDB);
  std::error_code Ignored;
  std::filesystem::remove(Path, Ignored);
  std::filesystem::remove(Path.parent_path(), Ignored);
  ASSERT_FALSE(bool(Reference));
  EXPECT_NE(llvm::toString(Reference.takeError()).find("system-call entry"),
            std::string::npos);
}

TEST(KernelDebuggerReference, PublicObjectTypeDataNeedsNoPrivatePDBLayout) {
  for (const bool WithPDB : {false, true}) {
    auto Options = kdOptions();
    if (!WithPDB) {
      Options.KernelPDB.clear();
      Options.Version.reset();
      Options.PoolTagPolicy = DriverPoolTagPolicy::Strict;
    }
    auto Reference = KernelReferenceImage::load(
        Options.KernelImage, 4 * 1024 * 1024, Options.KernelPDB);
    ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
    ASSERT_EQ(Reference->objectTypeExports().size(), 4u);
    KernelExportRegistry Exports;
    llvm::cantFail(Exports.initialize(Options));
    auto E = Exports.attachReference(*Reference, Options);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    auto Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
    llvm::cantFail(Reference->map(*Memory));
    DriverResult Result;
    KernelModel Model(*Memory, Result, &Exports);
    llvm::cantFail(Model.setDebuggerReference(*Reference));
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    llvm::cantFail(Model.initialize(Image, Options));
    for (const auto &Entry : object_type::Entries) {
      const uint64_t Slot = llvm::cantFail(Exports.resolve(Entry.Name));
      EXPECT_EQ(Slot, Reference->objectTypeExports().at(Entry.Name.str()));
      EXPECT_EQ(llvm::cantFail(Memory->readInteger(Slot, 8)),
                object_type::identity(Entry.Type));
      llvm::cantFail(Model.validateGuestAccess(Slot, 8, false));
      auto Write = Model.validateGuestAccess(Slot, 8, true);
      EXPECT_TRUE(bool(Write));
      llvm::consumeError(std::move(Write));
      auto Crossed = Model.validateGuestAccess(Slot + 7, 2, false);
      EXPECT_TRUE(bool(Crossed));
      llvm::consumeError(std::move(Crossed));
      auto Opaque = Model.validateGuestAccess(object_type::identity(Entry.Type),
                                              1, false);
      EXPECT_TRUE(bool(Opaque));
      llvm::consumeError(std::move(Opaque));
    }
  }
}

TEST(KernelDebuggerReference, CompiledThreadClientUsesNativeTypeSlots) {
  for (bool WithPDB : {false, true})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      auto Options = kdOptions();
      Options.Contract = Contract;
      Options.InstructionLimit = 10000;
      if (!WithPDB) {
        Options.KernelPDB.clear();
        Options.PoolTagPolicy = DriverPoolTagPolicy::Strict;
      }
      const auto Client = kdFixture("driver_thread_identity");
      auto Image = llvm::cantFail(loadDriverImage(Client, Options.MemoryLimit));
      for (const auto &Import : Image.Imports)
        Options.KernelExports[Import.Name] = true;
      for (const auto *Name : {"PsGetCurrentThread", "PsGetCurrentThreadId",
                               "PsGetThreadId", "PsLookupThreadByThreadId"})
        Options.KernelExports[Name] = true;
      auto Result = emulateDriver(Client, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

      EXPECT_EQ(Result->NTStatus, 0u);
      EXPECT_NE(kdCall(*Result, "ObReferenceObjectByHandle"), nullptr);
    }
}

TEST(KernelDebuggerReference, ObjectTypeExportsRejectCodeAndMisalignedSlots) {
  auto Buffer =
      llvm::cantFail(llvm::errorOrToExpected(llvm::MemoryBuffer::getFile(
          kdFixture("driver_kernel_debugger_reference").string())));
  const auto Data = Buffer->getBuffer();
  std::vector<uint8_t> Bytes(Data.bytes_begin(), Data.bytes_end());
  auto Object =
      llvm::cantFail(llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                          Bytes.size()),
          "types")));
  uint32_t TypeRVA = 0, TypeIndex = 0;
  for (const auto &Export : Object->export_directories()) {
    llvm::StringRef Name;
    llvm::cantFail(Export.getSymbolName(Name));
    if (Name != "PsThreadType")
      continue;
    uint32_t Ordinal = 0, Base = 0;
    llvm::cantFail(Export.getExportRVA(TypeRVA));
    llvm::cantFail(Export.getOrdinal(Ordinal));
    llvm::cantFail(Export.getOrdinalBase(Base));
    TypeIndex = Ordinal - Base;
  }
  ASSERT_NE(TypeRVA, 0u);
  const auto *Directory = Object->getDataDirectory(llvm::COFF::EXPORT_TABLE);
  ASSERT_NE(Directory, nullptr);
  uintptr_t DirectoryAddress = 0, TableAddress = 0;
  llvm::cantFail(
      Object->getRvaPtr(Directory->RelativeVirtualAddress, DirectoryAddress));
  const auto *Table =
      reinterpret_cast<const llvm::object::export_directory_table_entry *>(
          DirectoryAddress);
  llvm::cantFail(Object->getRvaPtr(Table->ExportAddressTableRVA, TableAddress));
  const auto Offset = reinterpret_cast<const uint8_t *>(TableAddress) -
                      Bytes.data() + TypeIndex * 4;
  ASSERT_LE(Offset + 4, Bytes.size());
  llvm::SmallString<128> Temporary;
  llvm::cantFail(llvm::errorCodeToError(
      llvm::sys::fs::createUniqueDirectory("neverd-object-type", Temporary)));
  const auto Path =
      std::filesystem::path(Temporary.str().str()) / "reference.sys";
  for (uint32_t Replacement :
       {uint32_t(Object->getPE32PlusHeader()->AddressOfEntryPoint),
        TypeRVA + 1}) {
    llvm::support::endian::write32le(Bytes.data() + Offset, Replacement);
    {
      std::ofstream Output(Path, std::ios::binary);
      Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
      ASSERT_TRUE(Output);
    }
    auto Reference = KernelReferenceImage::load(Path, 4 * 1024 * 1024);
    ASSERT_FALSE(bool(Reference));
    EXPECT_NE(llvm::toString(Reference.takeError()).find("object-type"),
              std::string::npos);
  }
  std::error_code Ignored;
  std::filesystem::remove_all(Path.parent_path(), Ignored);
}

TEST(KernelDebuggerReference, RejectsDifferentPDBIdentityAndMissingCompanion) {
  const auto Options = kdOptions();
  for (const auto &Path : {kdFixture("driver_kernel_debugger_other", ".pdb"),
                           kdFixture("missing_debugger_companion", ".pdb")}) {
    auto Reference =
        KernelReferenceImage::load(Options.KernelImage, 4 * 1024 * 1024, Path);
    ASSERT_FALSE(bool(Reference));
    EXPECT_FALSE(llvm::toString(Reference.takeError()).empty());
  }
}

TEST(KernelDebuggerReference,
     MatchingPDBDoesNotAuthorizeWritingIntoReferenceCode) {
  auto Reference = KernelReferenceImage::load(
      kdFixture("driver_kernel_debugger_code_data"), 4 * 1024 * 1024,
      kdFixture("driver_kernel_debugger_code_data", ".pdb"));
  ASSERT_FALSE(bool(Reference));
  EXPECT_NE(llvm::toString(Reference.takeError()).find("data symbol"),
            std::string::npos);
}

TEST(KernelDebuggerReference,
     GuestOwnedKDDataDoesNotAuthorizeOtherKernelState) {
  const auto Options = kdOptions();
  auto Reference = KernelReferenceImage::load(
      Options.KernelImage, 4 * 1024 * 1024, Options.KernelPDB);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  auto Backend = UnicornBackend::create(4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  llvm::cantFail(Reference->map(**Backend));
  DriverResult Result;
  KernelModel Model(**Backend, Result);
  llvm::cantFail(Model.setDebuggerReference(*Reference));
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, Options));
  Model.addReferenceModule(Reference->base(), Reference->size());
  llvm::cantFail(Model.initializeDebuggerState());
  const auto &Facts = *Reference->debugger();
  EXPECT_EQ(llvm::cantFail((*Backend)->readInteger(Facts.Address + 0x18, 8)),
            Reference->base());
  EXPECT_EQ(llvm::cantFail((*Backend)->readInteger(Facts.ListHead, 8)),
            Facts.Address);
  llvm::cantFail(Model.validateGuestAccess(Facts.Address, 0x380, false));
  auto Write = Model.validateGuestAccess(Facts.Address, 8, true);
  ASSERT_TRUE(bool(Write));
  llvm::consumeError(std::move(Write));
  const uint64_t ModuleHead =
      llvm::support::endian::read64le(Facts.Template.data() + 0x48);
  EXPECT_EQ(llvm::cantFail(Model.call("MmIsAddressValid", {ModuleHead})), 1u);
  auto Unknown = Model.validateGuestAccess(ModuleHead, 8, false);
  ASSERT_TRUE(bool(Unknown));
  EXPECT_NE(llvm::toString(std::move(Unknown)).find("reference kernel"),
            std::string::npos);
  Model.enterExecution(profile::StackBase);
  const auto Thread = llvm::cantFail(Model.currentThreadObject());
  auto Tail = Model.validateGuestAccess(Thread + 0x880, 8, false);
  ASSERT_TRUE(bool(Tail));
  EXPECT_NE(llvm::toString(std::move(Tail)).find("opaque thread"),
            std::string::npos);
}

TEST(KernelDebuggerReference, CompiledClientCopiesKDAndUsesNativePoolRelease) {
  auto Options = kdOptions();
  for (auto Contract : {ExecutionContract::CheckedX64}) {
    Options.Contract = Contract;
    auto Result = emulateDriver(kdFixture("driver_kd_client"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

    EXPECT_EQ(Result->NTStatus, 0u);
    EXPECT_EQ(Result->KernelPDBSHA256.size(), 64u);
    const auto *Copy = kdCall(*Result, "RtlCopyMemory");
    ASSERT_NE(Copy, nullptr);
    ASSERT_EQ(Copy->Arguments.size(), 3u);
    EXPECT_EQ(Copy->Arguments[2], 0x380u);
    EXPECT_TRUE(Copy->Result);
    const auto *Free = kdCall(*Result, "ExFreePoolWithTag");
    ASSERT_NE(Free, nullptr);
    EXPECT_EQ(Free->Result, 0u);
  }
}

TEST(KernelDebuggerReference, PagingOwnsKDBaseAndOnlyKnownSystemVAAssignments) {
  auto Options = kdOptions();
  Options.PageTables = true;
  auto Reference = KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  ASSERT_TRUE(Reference->debugger());
  const auto &Facts = *Reference->debugger();
  ASSERT_NE(Facts.SystemVaAssignments, 0u);
  auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
  llvm::cantFail(Reference->map(*Backend));
  DriverResult Result;
  KernelModel Model(*Backend, Result);
  llvm::cantFail(Model.setDebuggerReference(*Reference));
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, Options));
  auto Before = Model.validateGuestAccess(Facts.SystemVaAssignments, 8, false);
  ASSERT_TRUE(bool(Before));
  llvm::consumeError(std::move(Before));
  llvm::cantFail(Model.initializeDebuggerState());
  auto Root = llvm::cantFail(Model.guestCR3());
  auto RootVA = llvm::cantFail(Model.call("MmGetVirtualForPhysical", {Root}));
  unsigned Matches = 0;
  for (uint64_t I = 0; I < 512; ++I)
    if ((llvm::cantFail(Backend->readInteger(RootVA + I * 8, 8)) &
         0x000ffffffffff000ULL) == Root) {
      ++Matches;
      EXPECT_EQ(llvm::cantFail(Backend->readInteger(Facts.Address + 0x360, 8)),
                0xffff000000000000ULL | (I << 39));
    }
  EXPECT_EQ(Matches, 1u);
  llvm::cantFail(
      Model.validateGuestAccess(Facts.SystemVaAssignments, 64, false));
  EXPECT_EQ(
      llvm::cantFail(Backend->readInteger(Facts.SystemVaAssignments + 48, 8)),
      Reference->base());
  EXPECT_EQ(
      llvm::cantFail(Backend->readInteger(Facts.SystemVaAssignments + 56, 8)),
      Reference->size());
  for (const auto &[Offset, IsWrite] :
       {std::pair<uint64_t, bool>{0, true},
        {systemva::AssignmentBytes, false},
        {systemva::AssignmentBytes - 4, false}}) {
    auto Denied = Model.validateGuestAccess(Facts.SystemVaAssignments + Offset,
                                            8, IsWrite);
    ASSERT_TRUE(bool(Denied));
    llvm::consumeError(std::move(Denied));
  }
}

TEST(KernelDebuggerReference, StrictPolicyStillRejectsTheSameMismatchedTag) {
  auto Options = kdOptions();
  Options.PoolTagPolicy = DriverPoolTagPolicy::Strict;
  auto Result = emulateDriver(kdFixture("driver_kd_client"), Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("tag does not match"), std::string::npos);
  EXPECT_FALSE(Result->NTStatus);
}

TEST(KernelDebuggerReference, CompiledClientCannotReadUnmodeledGlobalContents) {
  auto Result =
      emulateDriver(kdFixture("driver_kd_unknown_global"), kdOptions());
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find("reference kernel data field"),
            std::string::npos);
  const auto *Valid = kdCall(*Result, "MmIsAddressValid");
  ASSERT_NE(Valid, nullptr);
  EXPECT_EQ(Valid->Result, 1u);
  EXPECT_FALSE(Result->NTStatus);
}

TEST(KernelPoolTagProfile, NativeReleaseRetainsLifetimeIRQLAndPinChecks) {
  auto Backend = UnicornBackend::create(4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  DriverResult Result;
  KernelModel Model(**Backend, Result);
  DriverOptions Options;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.PoolTagPolicy = DriverPoolTagPolicy::Windows19041;
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, Options));
  Model.enterExecution(profile::StackBase);
  for (uint64_t Size : {16u, 0x40000u}) {
    const auto Pool =
        llvm::cantFail(Model.call("ExAllocatePoolWithTag", {0, Size, 1}));
    ASSERT_NE(Pool, 0u);
    EXPECT_EQ(llvm::cantFail(Model.call("ExFreePoolWithTag", {Pool, 2})), 0u);
    auto Again = Model.call("ExFreePoolWithTag", {Pool, 2});
    ASSERT_FALSE(bool(Again));
    EXPECT_NE(llvm::toString(Again.takeError()).find("already freed"),
              std::string::npos);
  }
  const auto Pool =
      llvm::cantFail(Model.call("ExAllocatePoolWithTag", {1, 64, 1}));
  const auto Old = llvm::cantFail(Model.call("KfRaiseIrql", {2}));
  auto High = Model.call("ExFreePoolWithTag", {Pool, 2});
  ASSERT_FALSE(bool(High));
  EXPECT_NE(llvm::toString(High.takeError()).find("APC_LEVEL"),
            std::string::npos);
  llvm::cantFail(Model.call("KeLowerIrql", {Old}));
  const auto MDL =
      llvm::cantFail(Model.call("IoAllocateMdl", {Pool, 64, 0, 0, 0}));
  llvm::cantFail(Model.call("MmProbeAndLockPages", {MDL, 0, 0}));
  auto Pinned = Model.call("ExFreePoolWithTag", {Pool, 2});
  ASSERT_FALSE(bool(Pinned));
  EXPECT_NE(llvm::toString(Pinned.takeError()).find("pinned"),
            std::string::npos);
  llvm::cantFail(Model.call("MmUnlockPages", {MDL}));
  llvm::cantFail(Model.call("IoFreeMdl", {MDL}));
  EXPECT_EQ(llvm::cantFail(Model.call("ExFreePoolWithTag", {Pool, 2})), 0u);
}
TEST(KernelDebuggerReference, CompiledClientClassifiesPoolsMDLAndBothImages) {
  auto Options = kdOptions();
  Options.PageTables = true;
  Options.MDLMappingPolicy = DriverMDLMappingPolicy::Windows19041;
  auto Client = llvm::cantFail(
      loadDriverImage(kdFixture("driver_system_va"), Options.MemoryLimit));
  for (const auto &Import : Client.Imports)
    Options.KernelExports[Import.Name] = true;
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    Options.Contract = Contract;
    auto Result = emulateDriver(kdFixture("driver_system_va"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;

    EXPECT_EQ(Result->NTStatus, 0u);
    auto Rejected = emulateDriver(kdFixture("driver_system_va_write"), Options);
    ASSERT_TRUE(bool(Rejected)) << llvm::toString(Rejected.takeError());
    EXPECT_EQ(Rejected->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Rejected->Diagnostic.find("reference kernel data field"),
              std::string::npos);
  }
}
TEST(KernelDebuggerReference,
     SecureCookieIsGuestOwnedAndOnlyItsWordIsReadable) {
  auto Options = kdOptions();
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
  ASSERT_TRUE(Reference.debugger());
  const auto Address = Reference.debugger()->SecureCookie;
  ASSERT_NE(Address, 0u);
  auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
  llvm::cantFail(Reference.map(*Backend));
  DriverResult Result;
  KernelModel Model(*Backend, Result);
  llvm::cantFail(Model.setDebuggerReference(Reference));
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, Options));
  auto Unpublished = Model.validateGuestAccess(Address, 8, false);
  ASSERT_TRUE(bool(Unpublished));
  llvm::consumeError(std::move(Unpublished));
  llvm::cantFail(Model.initializeDebuggerState());
  llvm::cantFail(Model.validateGuestAccess(Address, 8, false));
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(Address, 8)),
            0x4e44536563757265ULL);
  for (const auto &[Offset, Write] : {std::pair<uint64_t, bool>{0, true},
                                      {uint64_t(-1), false},
                                      {4, false}}) {
    auto Denied = Model.validateGuestAccess(Address + Offset, 8, Write);
    ASSERT_TRUE(bool(Denied));
    llvm::consumeError(std::move(Denied));
  }
}
TEST(KernelDebuggerReference,
     CFGStorageMutationsFailCompanionSectionAuthentication) {
  auto Options = kdOptions();
  auto Buffer = llvm::MemoryBuffer::getFile(Options.KernelImage.string());
  ASSERT_TRUE(bool(Buffer));
  auto Data = (*Buffer)->getBuffer();
  std::vector<uint8_t> Bytes(Data.begin(), Data.end());
  auto Object = llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                      Bytes.size()),
      "cfg-tail"));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  const auto *Config = (*Object)->getLoadConfig64();
  ASSERT_NE(Config, nullptr);
  const uint64_t TableRVA =
      Config->GuardCFFunctionTable - (*Object)->getImageBase();
  bool Changed = false;
  for (const auto &Ref : (*Object)->sections()) {
    const auto *Section = (*Object)->getCOFFSection(Ref);
    if (TableRVA < Section->VirtualAddress ||
        TableRVA - Section->VirtualAddress >= Section->SizeOfRawData)
      continue;
    auto Offset = [&](const void *Field) {
      return reinterpret_cast<const uint8_t *>(Field) - Bytes.data();
    };
    const uint64_t TableBytes = uint64_t(Config->GuardCFFunctionCount) * 4;
    ASSERT_LE(uint64_t(Section->VirtualSize) + TableBytes,
              uint64_t(Section->SizeOfRawData));
    const uint64_t Source =
        Section->PointerToRawData + TableRVA - Section->VirtualAddress;
    const uint64_t Destination =
        Section->PointerToRawData + Section->VirtualSize;
    std::copy_n(Bytes.data() + Source, TableBytes, Bytes.data() + Destination);
    llvm::support::endian::write64le(
        Bytes.data() + Offset(&Config->GuardCFFunctionTable),
        (*Object)->getImageBase() + Section->VirtualAddress +
            Section->VirtualSize);
    llvm::support::endian::write32le(
        Bytes.data() + Offset(&Section->Characteristics),
        Section->Characteristics | llvm::COFF::IMAGE_SCN_MEM_WRITE);
    Changed = true;
    break;
  }
  ASSERT_TRUE(Changed);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      bool(llvm::sys::fs::createUniqueDirectory("neverd-cfg-tail", Directory)));
  const auto Path =
      std::filesystem::path(Directory.str().str()) / "reference.sys";
  {
    std::ofstream Output(Path, std::ios::binary);
    Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    ASSERT_TRUE(Output);
  }
  auto Reference =
      KernelReferenceImage::load(Path, Options.MemoryLimit, Options.KernelPDB);
  std::error_code Ignored;
  std::filesystem::remove(Path, Ignored);
  std::filesystem::remove(Path.parent_path(), Ignored);
  ASSERT_FALSE(bool(Reference));
  const auto Message = llvm::toString(Reference.takeError());
  EXPECT_NE(Message.find("section table does not match"), std::string::npos)
      << Message;
}
TEST(GuardFunctionTable, BothConsumersShareOrderCodeAndMetadataValidation) {
  std::array<uint8_t, 15> Bytes{};
  for (unsigned I = 0; I != 3; ++I) {
    llvm::support::endian::write32le(Bytes.data() + I * 5, 0x1000 + I * 16);
    Bytes[I * 5 + 4] = I;
  }
  auto Code = [](uint32_t RVA) { return RVA >= 0x1000 && RVA < 0x2000; };
  auto Valid = guard::decodeFunctionTable(Bytes, 1, 3, Code);
  ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  ASSERT_EQ(Valid->size(), 3u);
  EXPECT_EQ((*Valid)[2].Flags, 2u);
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto Changed = Bytes;
    if (Mutation == 0)
      Changed[14] = 4;
    if (Mutation == 1)
      llvm::support::endian::write32le(Changed.data() + 5, 0x1000);
    if (Mutation == 2)
      llvm::support::endian::write32le(Changed.data(), 0);
    if (Mutation == 3)
      llvm::support::endian::write32le(Changed.data(), 0x500);
    if (Mutation == 4)
      llvm::support::endian::write32le(Changed.data() + 10, 0x1021);
    auto Rejected =
        guard::decodeFunctionTable(Changed, 1, Mutation == 5 ? 1 : 3, Code);
    ASSERT_FALSE(bool(Rejected));
    llvm::consumeError(Rejected.takeError());
  }
}
TEST(KernelDebuggerReference,
     OriginalClientReadsNativeCFGBitmapButCannotWriteIt) {
  auto Options = kdOptions();
  auto Client = llvm::cantFail(
      loadDriverImage(kdFixture("driver_kernel_cfg"), Options.MemoryLimit));
  for (const auto &Import : Client.Imports)
    Options.KernelExports[Import.Name] = true;
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    Options.Contract = Contract;
    auto Result =
        llvm::cantFail(emulateDriver(kdFixture("driver_kernel_cfg"), Options));
    EXPECT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
    EXPECT_EQ(Result.NTStatus, 0u);
    auto Denied = llvm::cantFail(
        emulateDriver(kdFixture("driver_kernel_cfg_write"), Options));
    EXPECT_TRUE(Denied.Stop == DriverStopReason::ModelError ||
                Denied.Stop == DriverStopReason::MemoryFault)
        << Denied.Diagnostic;
    EXPECT_FALSE(Denied.NTStatus);
  }
}
TEST(KernelDebuggerReference,
     ExportSuppressionRequiresResolutionAndFIDSuppressionSurvivesIt) {
  for (const auto *Fixture : {"driver_kernel_debugger_cfg_es",
                              "driver_kernel_debugger_cfg_suppressed"}) {
    auto Options = kdOptions();
    Options.KernelImage = kdFixture(Fixture);
    Options.KernelPDB = kdFixture(Fixture, ".pdb");
    auto Reference = llvm::cantFail(KernelReferenceImage::load(
        Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
    ASSERT_TRUE(Reference.debugger());
    const auto &Facts = *Reference.debugger();
    const uint64_t Target = Reference.exports().at("KeGetCurrentIrql");
    ASSERT_NE(Target, 0u);
    EXPECT_TRUE(Facts.GuardTargets.empty());
    auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
    llvm::cantFail(Reference.map(*Backend));
    KernelExportRegistry Exports;
    llvm::cantFail(Exports.initialize(Options));
    auto E = Exports.attachReference(Reference, Options);
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
    DriverResult Result;
    KernelModel Model(*Backend, Result, &Exports);
    llvm::cantFail(Model.setDebuggerReference(Reference));
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    llvm::cantFail(Model.initialize(Image, Options));
    auto Before = Model.validateGuestAccess(Facts.GuardBitmapPointer, 8, false);
    ASSERT_TRUE(bool(Before));
    llvm::consumeError(std::move(Before));
    llvm::cantFail(Model.initializeDebuggerState());
    llvm::cantFail(
        Model.validateGuestAccess(Facts.GuardBitmapPointer, 8, false));
    const auto Bias =
        llvm::cantFail(Backend->readInteger(Facts.GuardBitmapPointer, 8));
    const auto Word = Bias + (Target >> 9) * 8;
    const auto Mask = 1ULL << ((Target >> 3) & 63);
    EXPECT_EQ(llvm::cantFail(Backend->readInteger(Word, 8)) & Mask, 0u);
    const auto Storage =
        llvm::cantFail(Model.call("ExAllocatePoolWithTag", {0, 128, 1}));
    const char Text[] = "KeGetCurrentIrql";
    std::vector<uint8_t> UTF16(sizeof(Text) * 2);
    for (unsigned I = 0; I < sizeof(Text); ++I)
      UTF16[I * 2] = Text[I];
    llvm::cantFail(Backend->write(Storage, UTF16));
    llvm::cantFail(Model.call("RtlInitUnicodeString", {Storage + 64, Storage}));
    EXPECT_EQ(
        llvm::cantFail(Model.call("MmGetSystemRoutineAddress", {Storage + 64})),
        Target);
    const bool ExportSuppressed = !Facts.SuppressedExports.empty();
    EXPECT_EQ(bool(llvm::cantFail(Backend->readInteger(Word, 8)) & Mask),
              ExportSuppressed);
    for (auto [Address, Write] :
         {std::pair<uint64_t, bool>{Facts.GuardBitmapPointer, true},
          {Facts.GuardBitmapPointer + 4, false},
          {Word, true},
          {KernelGuardBitmap::StoreBase + Reference.size() / 64, false}}) {
      auto Error = Model.validateGuestAccess(Address, 8, Write);
      ASSERT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
  }
}
TEST(KernelDebuggerReference,
     BitmapMatchesPackedGFIDSAndRetainsReadOnlyNativeGranularity) {
  auto Options = kdOptions();
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
  const auto &Facts = *Reference.debugger();
  ASSERT_FALSE(Facts.GuardTargets.empty());
  auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
  KernelGuardBitmap Bitmap(*Backend);
  llvm::cantFail(Bitmap.initialize(Reference));
  unsigned Misaligned = 0;
  for (auto Target : Facts.GuardTargets) {
    const auto Word = Bitmap.pointer() + (Target >> 9) * 8;
    const auto Value = llvm::cantFail(Backend->readInteger(Word, 8));
    EXPECT_NE(Value & (1ULL << ((Target >> 3) & 63)), 0u);
    if (Target & 15) {
      ++Misaligned;
      EXPECT_NE(Value & (1ULL << (((Target >> 3) & 63) & ~1u)), 0u);
      EXPECT_NE(Value & (1ULL << ((((Target >> 3) & 63) & ~1u) + 1)), 0u);
    } else {
      EXPECT_EQ(Value & (1ULL << (((Target >> 3) & 63) + 1)), 0u);
    }
  }
  EXPECT_NE(Misaligned, 0u);
  EXPECT_EQ(
      llvm::cantFail(Backend->readInteger(KernelGuardBitmap::StoreBase, 8)),
      0u);
  auto Writable = llvm::cantFail(
      Backend->canAccess(KernelGuardBitmap::StoreBase, 8, Write));
  EXPECT_FALSE(Writable);
}
TEST(KernelDebuggerReference,
     ActiveProcessListSharesIdentityAndOnlyKnownFields) {
  auto Options = kdOptions();
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
  const auto &Facts = *Reference.debugger();
  ASSERT_NE(Facts.ActiveProcessHead, 0u);
  EXPECT_EQ(Facts.ActiveProcessLinksOffset, 0x180u);
  EXPECT_EQ(Facts.ProcessIDOffset, 0x120u);
  auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
  llvm::cantFail(Reference.map(*Backend));
  DriverResult Result;
  KernelModel Model(*Backend, Result);
  llvm::cantFail(Model.setDebuggerReference(Reference));
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, Options));
  auto Before = Model.validateGuestAccess(Facts.ActiveProcessHead, 16, false);
  ASSERT_TRUE(bool(Before));
  llvm::consumeError(std::move(Before));
  llvm::cantFail(Model.initializeDebuggerState());
  Model.enterExecution(profile::StackBase);
  const auto System = llvm::cantFail(Model.call("IoGetCurrentProcess", {}));
  const auto Head = Facts.ActiveProcessHead;
  const auto SystemLink = System + Facts.ActiveProcessLinksOffset;
  auto Read = [&](uint64_t Address) {
    return llvm::cantFail(Backend->readInteger(Address, 8));
  };
  EXPECT_EQ(Read(Head), SystemLink);
  EXPECT_EQ(Read(Head + 8), SystemLink);
  EXPECT_EQ(Read(SystemLink), Head);
  EXPECT_EQ(Read(SystemLink + 8), Head);
  EXPECT_EQ(Read(System + Facts.ProcessIDOffset), 4u);
  llvm::cantFail(Model.validateGuestAccess(SystemLink, 16, false));
  const auto Scratch =
      llvm::cantFail(Model.call("ExAllocatePoolWithTag", {0, 32, 1}));
  llvm::cantFail(Model.call("PsLookupProcessByProcessId", {4, Scratch}));
  EXPECT_EQ(Read(Scratch), System);
  llvm::cantFail(Model.call("ObfDereferenceObject", {System}));
  llvm::cantFail(Model.setUserRequestContext(true, 64));
  const auto User = llvm::cantFail(Model.call("IoGetCurrentProcess", {}));
  const auto UserLink = User + Facts.ActiveProcessLinksOffset;
  EXPECT_EQ(Read(User + Facts.ProcessIDOffset), 64u);
  EXPECT_EQ(Read(Head), SystemLink);
  EXPECT_EQ(Read(SystemLink), UserLink);
  EXPECT_EQ(Read(UserLink), Head);
  EXPECT_EQ(Read(Head + 8), UserLink);
  EXPECT_EQ(Read(UserLink + 8), SystemLink);
  llvm::cantFail(Model.call("PsLookupProcessByProcessId", {64, Scratch}));
  EXPECT_EQ(Read(Scratch), User);
  llvm::cantFail(Model.call("ObfDereferenceObject", {User}));
  for (auto [Address, Size, Write] :
       {std::tuple<uint64_t, uint32_t, bool>{Head, 16, true},
        {Head + 12, 8, false},
        {SystemLink, 8, true},
        {SystemLink + 12, 8, false},
        {User, 8, false},
        {User + Facts.ProcessIDOffset, 16, false},
        {User + Facts.ProcessIDOffset, 8, true}}) {
    auto Denied = Model.validateGuestAccess(Address, Size, Write);
    ASSERT_TRUE(bool(Denied));
    llvm::consumeError(std::move(Denied));
  }
  llvm::cantFail(Model.call("ExFreePoolWithTag", {Scratch, 1}));
}
TEST(KernelDebuggerReference, DeclaredRequestorsEnterTheListBeforeLazyLookup) {
  auto Options = kdOptions();
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Create;
  Request.RequestorProcessID = 64;
  Options.Requests.push_back(Request);
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
  const auto &Facts = *Reference.debugger();
  auto Backend = llvm::cantFail(UnicornBackend::create(8 * 1024 * 1024));
  llvm::cantFail(Reference.map(*Backend));
  DriverResult Result;
  KernelModel Model(*Backend, Result);
  llvm::cantFail(Model.setDebuggerReference(Reference));
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, Options));
  llvm::cantFail(Model.initializeDebuggerState());
  Model.enterExecution(profile::StackBase);
  auto Read = [&](uint64_t Address) {
    return llvm::cantFail(Backend->readInteger(Address, 8));
  };
  const auto Head = Facts.ActiveProcessHead;
  const auto SystemLink = Read(Head), UserLink = Read(SystemLink);
  ASSERT_NE(UserLink, Head);
  const auto User = UserLink - Facts.ActiveProcessLinksOffset;
  EXPECT_EQ(Read(User + Facts.ProcessIDOffset), 64u);
  EXPECT_EQ(Read(UserLink), Head);
  EXPECT_EQ(Read(Head + 8), UserLink);
  const auto Scratch =
      llvm::cantFail(Model.call("ExAllocatePoolWithTag", {0, 32, 1}));
  EXPECT_EQ(
      llvm::cantFail(Model.call("PsLookupProcessByProcessId", {64, Scratch})),
      0u);
  EXPECT_EQ(Read(Scratch), User);
  llvm::cantFail(Model.call("ObfDereferenceObject", {User}));
  EXPECT_EQ(Read(SystemLink), UserLink);
  llvm::cantFail(Model.call("ExFreePoolWithTag", {Scratch, 1}));
}
} // namespace
} // namespace neverd::emulation
