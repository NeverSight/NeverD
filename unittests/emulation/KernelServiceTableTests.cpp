//===- KernelServiceTableTests.cpp - Original boot service observations
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original boot service observations.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/KernelReferenceInitializer.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <fstream>

namespace neverd::emulation {
namespace {
std::filesystem::path serviceFixture(const char *Name,
                                     const char *Ext = ".sys") {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         (std::string(Name) + Ext);
}
DriverOptions serviceOptions() {
  DriverOptions Options;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.KernelImage = serviceFixture("driver_kernel_debugger_services");
  Options.KernelPDB = serviceFixture("driver_kernel_debugger_services", ".pdb");
  return Options;
}
uint32_t serviceExportRVA(const DriverOptions &Options, llvm::StringRef Name) {
  auto Buffer = llvm::cantFail(llvm::errorOrToExpected(
      llvm::MemoryBuffer::getFile(Options.KernelImage.string())));
  auto Object = llvm::cantFail(
      llvm::object::COFFObjectFile::create(Buffer->getMemBufferRef()));
  for (const auto &Export : Object->export_directories()) {
    llvm::StringRef ExportName;
    llvm::cantFail(Export.getSymbolName(ExportName));
    if (ExportName == Name) {
      uint32_t RVA = 0;
      llvm::cantFail(Export.getExportRVA(RVA));
      return RVA;
    }
  }
  ADD_FAILURE() << "missing fixture export " << Name.str();
  return 0;
}
TEST(KernelServiceTable, NativeCompactorAndBootWritesAgreeWithPublishedFacts) {
  const auto Options = serviceOptions();
  auto Reference = KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  const auto &Facts = *Reference->debugger();
  ASSERT_EQ(Facts.CompactedServices.size(), 8u);
  EXPECT_EQ(Facts.ServiceArgumentBytes, (std::vector<uint8_t>{0, 4}));
  KernelInitializerPlan Plan;
  Plan.StackBase = 0x60000000;
  Plan.StackSize = 0x10000;
  Plan.ReturnPC = 0x60010000;
  std::vector<uint8_t> Raw(8);
  for (unsigned I = 0; I < 2; ++I) {
    const auto Target =
        Reference->exports().at(I ? "FixtureService1" : "FixtureService0");
    llvm::support::endian::write32le(Raw.data() + I * 4,
                                     Target - Reference->base());
    const int32_t Encoded =
        llvm::support::endian::read32le(Facts.CompactedServices.data() + I * 4);
    EXPECT_LT(Encoded, 0); // Targets precede .rdata: exercise signed decoding.
    EXPECT_EQ(Facts.ServiceTable + int64_t(Encoded >> 4), Target);
    EXPECT_EQ(Encoded & 15, I);
  }
  Plan.Inputs = {
      {Facts.ServiceDescriptor, std::vector<uint8_t>(32)},
      {Facts.ServiceTable, Raw},
      {Facts.ServiceArguments, Facts.ServiceArgumentBytes},
      {Reference->base() + serviceExportRVA(Options, "KiServiceLimit"),
       {2, 0, 0, 0}}};
  Plan.Writable = {{Facts.ServiceDescriptor, 32}, {Facts.ServiceTable, 8}};
  Plan.Outputs = Plan.Writable;
  Plan.Calls = {
      {Reference->exports().at("KiInitSystem") + 0xe6, {}, false},
      {Reference->exports().at("KeCompactServiceTable"),
       {Facts.ServiceTable, Facts.ServiceArguments, 2, 0, Reference->base()},
       false}};
  auto Result = initializeFromKernelReference(*Reference, Plan);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Outputs.size(), 2u);
  EXPECT_EQ(Result->Outputs[1].Bytes, Facts.CompactedServices);
  const auto *Descriptor = Result->Outputs[0].Bytes.data();
  EXPECT_EQ(llvm::support::endian::read64le(Descriptor), Facts.ServiceTable);
  EXPECT_EQ(llvm::support::endian::read64le(Descriptor + 8), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(Descriptor + 16), 2u);
  EXPECT_EQ(llvm::support::endian::read64le(Descriptor + 24),
            Facts.ServiceArguments);
}
TEST(KernelServiceTable, OwnedReadOnlyRangesRemainBounded) {
  auto Options = serviceOptions();
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
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
  const auto &Facts = *Reference.debugger();
  auto Before = Model.validateGuestAccess(Facts.ServiceDescriptor, 8, false);
  ASSERT_TRUE(bool(Before));
  llvm::consumeError(std::move(Before));
  llvm::cantFail(Model.initializeDebuggerState());
  for (auto [Address, Size] :
       {std::pair<uint64_t, uint32_t>{Facts.ServiceDescriptor, 32},
        {Facts.ServiceTable, 8},
        {Facts.ServiceArguments, 2}}) {
    llvm::cantFail(Model.validateGuestAccess(Address, Size, false));
    auto Write = Model.validateGuestAccess(Address, 1, true);
    EXPECT_TRUE(bool(Write));
    llvm::consumeError(std::move(Write));
    auto Overrun = Model.validateGuestAccess(Address + Size - 1, 2, false);
    EXPECT_TRUE(bool(Overrun));
    llvm::consumeError(std::move(Overrun));
  }
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(Facts.ServiceDescriptor, 8)),
            Facts.ServiceTable);
  EXPECT_EQ(
      llvm::cantFail(Backend->readInteger(Facts.ServiceDescriptor + 16, 8)),
      2u);
  EXPECT_EQ(llvm::cantFail(Backend->readInteger(Facts.ServiceTable, 8)),
            llvm::support::endian::read64le(Facts.CompactedServices.data()));
}
TEST(KernelServiceTable, OriginalClientsUseLStarAndRejectGuestWrites) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    auto Options = serviceOptions();
    Options.Contract = Contract;
    auto Result = emulateDriver(serviceFixture("driver_services"), Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_EQ(Result->NTStatus, 0u);
    for (const char *Name :
         {"driver_services_descriptor_write", "driver_services_table_write"}) {
      auto Denied =
          llvm::cantFail(emulateDriver(serviceFixture(Name), Options));
      EXPECT_TRUE(Denied.Stop == DriverStopReason::ModelError ||
                  Denied.Stop == DriverStopReason::MemoryFault)
          << Denied.Diagnostic;
      EXPECT_FALSE(Denied.NTStatus);
    }
    Options.KernelPDB.clear();
    auto Missing = llvm::cantFail(
        emulateDriver(serviceFixture("driver_services"), Options));
    EXPECT_EQ(Missing.Stop, DriverStopReason::UnsupportedInstruction);
  }
}
TEST(KernelServiceTable, MalformedInputsAndChangedNativeSemanticsAreRejected) {
  const auto Options = serviceOptions();
  const auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options.KernelImage, Options.MemoryLimit, Options.KernelPDB));
  auto Buffer = llvm::cantFail(llvm::errorOrToExpected(
      llvm::MemoryBuffer::getFile(Options.KernelImage.string())));
  auto Data = Buffer->getBuffer();
  const std::vector<uint8_t> Original(Data.bytes_begin(), Data.bytes_end());
  auto Object = llvm::cantFail(
      llvm::object::COFFObjectFile::create(Buffer->getMemBufferRef()));
  auto Position = [&](uint64_t Address) {
    uintptr_t Ptr = 0;
    llvm::cantFail(Object->getRvaPtr(Address - Reference.base(), Ptr));
    return reinterpret_cast<const char *>(Ptr) - Data.data();
  };
  const auto &Facts = *Reference.debugger();
  const auto Descriptor = Position(Facts.ServiceDescriptor),
             Table = Position(Facts.ServiceTable);
  const auto Arguments = Position(Facts.ServiceArguments);
  const auto Limit =
      Position(Reference.base() + serviceExportRVA(Options, "KiServiceLimit"));
  const auto Compact =
      Position(Reference.exports().at("KeCompactServiceTable"));
  const auto Boot = Position(Reference.exports().at("KiInitSystem"));
  llvm::SmallString<128> Directory;
  llvm::cantFail(llvm::errorCodeToError(
      llvm::sys::fs::createUniqueDirectory("neverd-services", Directory)));
  const auto Path =
      std::filesystem::path(Directory.str().str()) / "reference.sys";
  for (unsigned Case = 0; Case < 10; ++Case) {
    auto Bytes = Original;
    switch (Case) {
    case 0:
      llvm::support::endian::write32le(Bytes.data() + Limit, 0);
      break;
    case 1:
      llvm::support::endian::write32le(Bytes.data() + Limit, 4097);
      break;
    case 2:
      Bytes[Arguments] = 1;
      break;
    case 3:
      Bytes[Arguments] = 64;
      break;
    case 4:
      llvm::support::endian::write32le(Bytes.data() + Table,
                                       Facts.ServiceTable - Reference.base());
      break;
    case 5:
      llvm::support::endian::write32le(Bytes.data() + Table, 0xffffffff);
      break;
    case 6:
      Bytes[Compact + 47] = 3;
      break; // SHL displacement encoding.
    case 7:
      Bytes[Compact + 71] ^= 1;
      break; // Unrelated post-compaction call.
    case 8:
      Bytes[Boot + 0xe6 + 10] ^= 8;
      break; // Wrong descriptor destination.
    case 9:
      Bytes[Descriptor + 8] = 1;
      break;
    }
    {
      std::ofstream Output(Path, std::ios::binary);
      Output.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
      ASSERT_TRUE(Output);
    }
    auto Invalid = KernelReferenceImage::load(Path, Options.MemoryLimit,
                                              Options.KernelPDB);
    EXPECT_FALSE(bool(Invalid)) << "case " << Case;
    llvm::consumeError(Invalid.takeError());
  }
  std::error_code Ignored;
  std::filesystem::remove(Path, Ignored);
  std::filesystem::remove(Path.parent_path(), Ignored);
}
} // namespace
} // namespace neverd::emulation
