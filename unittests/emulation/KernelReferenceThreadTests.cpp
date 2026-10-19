//===- KernelReferenceThreadTests.cpp - Native construction and lifetime
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native construction and lifetime.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelReferenceImage.h"
#include "os/windows/kernel/KernelReferenceThread.h"

#include "llvm/Support/Endian.h"

#include <cstdlib>

namespace neverd::emulation {
namespace {
std::optional<DriverOptions> nativeThreadOptions() {
  const auto *Image = std::getenv("NEVERD_THREAD_REFERENCE_IMAGE");
  const auto *PDB = std::getenv("NEVERD_THREAD_REFERENCE_PDB");
  if (!Image || !PDB)
    return std::nullopt;
  DriverOptions Options;
  Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
  Options.KernelImage = Image;
  Options.KernelPDB = PDB;
  return Options;
}
TEST(KernelReferenceThread, ScalarCompanionDoesNotInventNativeConstruction) {
  const auto Root = std::filesystem::path(NEVERD_DRIVER_FIXTURES);
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Root / "driver_kernel_debugger_reference.sys", 4 * 1024 * 1024,
      Root / "driver_kernel_debugger_reference.pdb"));
  EXPECT_FALSE(hasKernelThreadConstruction(Reference));
  EXPECT_TRUE(Reference.threadInitializationSymbols().empty());
  auto Result = constructKernelReferenceThread(
      Reference, {0x70044040, 0x70000400, 8, 0x140001000, profile::StackBase,
                  profile::StackSize, 0x70045000});
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("authenticated"),
            std::string::npos);
}
TEST(KernelReferenceThread, MatchedNativeConstructorRejectsMalformedOwners) {
  const auto Options = nativeThreadOptions();
  if (!Options)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  auto Reference = KernelReferenceImage::load(
      Options->KernelImage, Options->MemoryLimit, Options->KernelPDB);
  ASSERT_TRUE(bool(Reference)) << llvm::toString(Reference.takeError());
  ASSERT_TRUE(hasKernelThreadConstruction(*Reference));
  const KernelReferenceThreadInput Input{
      0x70044040,         0x70000400,         8,         0x140001000,
      profile::StackBase, profile::StackSize, 0x70045000};
  auto Constructed = constructKernelReferenceThread(*Reference, Input);
  ASSERT_TRUE(bool(Constructed)) << llvm::toString(Constructed.takeError());
  const auto &Body = Constructed->Body;
  ASSERT_EQ(Body.size(), 0x898u);
  EXPECT_EQ(Body[0], 6u);
  EXPECT_EQ(Body[0x184], 2u);
  EXPECT_EQ(llvm::support::endian::read64le(Body.data() + 0x480),
            Input.ThreadID);
  EXPECT_EQ(llvm::support::endian::read64le(Body.data() + 0x478), 4u);
  EXPECT_EQ(llvm::support::endian::read64le(Body.data() + 0x450),
            Input.StartAddress);
  EXPECT_GT(Constructed->Instructions, 1000u);
  for (unsigned Case = 0; Case < 7; ++Case) {
    auto Bad = Input;
    switch (Case) {
    case 0:
      ++Bad.Object;
      break;
    case 1:
      Bad.ThreadID = 0;
      break;
    case 2:
      Bad.StackSize = 4096;
      break;
    case 3:
      Bad.StackBase = UINT64_MAX - 4095;
      break;
    case 4:
      Bad.AuxiliaryBase = Bad.StackBase;
      break;
    case 5:
      Bad.Process = Bad.Object;
      break;
    case 6:
      Bad.AuxiliaryBase = Reference->base();
      break;
    }
    auto Rejected = constructKernelReferenceThread(*Reference, Bad);
    EXPECT_FALSE(bool(Rejected)) << "case " << Case;
    llvm::consumeError(Rejected.takeError());
  }
}
TEST(KernelReferenceThread, OriginalClientScansBodyAndCannotWriteIt) {
  auto Options = nativeThreadOptions();
  if (!Options)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  const auto Root = std::filesystem::path(NEVERD_DRIVER_FIXTURES);
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
    Options->Contract = Contract;
    Options->Backend = ExecutionBackendKind::Unicorn;
    for (bool Write : {false, true}) {
      const auto Path = Root / (Write ? "driver_native_thread_scan_write.sys"
                                      : "driver_native_thread_scan.sys");
      auto Image = llvm::cantFail(loadDriverImage(Path, Options->MemoryLimit));
      for (const auto &Import : Image.Imports)
        Options->KernelExports[Import.Name] = true;
      auto Result = emulateDriver(Path, *Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      if (Write) {
        EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
            << Result->Diagnostic;
        EXPECT_FALSE(Result->NTStatus);
      } else {
        EXPECT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_EQ(Result->NTStatus, 0u);
      }
    }
  }
}

TEST(KernelReferenceThread, ActiveInitialViewUsesCurrentIdentityAndExpires) {
  const auto Options = nativeThreadOptions();
  if (!Options)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options->KernelImage, Options->MemoryLimit, Options->KernelPDB));
  auto CPU = llvm::cantFail(UnicornBackend::create(Options->MemoryLimit));
  llvm::cantFail(Reference.map(*CPU));
  DriverResult Result;
  KernelModel Model(*CPU, Result);
  llvm::cantFail(Model.setDebuggerReference(Reference));
  DriverImage Image;
  Image.Base = 0x180000000;
  Image.Entry = Image.Base + 0x1000;
  Image.Size = 0x3000;
  llvm::cantFail(Model.initialize(Image, *Options));
  llvm::cantFail(Model.initializeDebuggerState());
  Model.enterExecution(profile::StackBase);
  auto Thread = Model.call("PsGetCurrentThread", {});
  ASSERT_TRUE(bool(Thread)) << llvm::toString(Thread.takeError());
  auto Full = Model.validateGuestAccess(*Thread, 0x898, false);
  ASSERT_FALSE(bool(Full)) << llvm::toString(std::move(Full));
  std::vector<uint8_t> Body(0x898);
  llvm::cantFail(CPU->read(*Thread, Body));
  const auto Start = llvm::support::endian::read64le(Body.data() + 0x450);
  EXPECT_EQ(Start, Image.Entry);
  EXPECT_EQ(llvm::support::endian::read64le(Body.data() + 0x480),
            llvm::cantFail(Model.call("PsGetThreadId", {*Thread})));
  unsigned First = 0;
  for (unsigned Offset = 0; Offset < 0x800; ++Offset) {
    llvm::cantFail(Model.validateGuestAccess(*Thread + Offset, 8, false));
    if (llvm::support::endian::read64le(Body.data() + Offset) == Start) {
      First = Offset;
      break;
    }
  }
  EXPECT_EQ(First, 0x450u);
  const auto Process = llvm::cantFail(Model.call("IoGetCurrentProcess", {}));
  const auto Head =
      Process + KernelReferenceThreadInput::ProcessThreadListHeadOffset;
  llvm::cantFail(Model.validateGuestAccess(Head, 16, false));
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Head, 8)), *Thread + 0x2f8);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Head + 8, 8)), *Thread + 0x2f8);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(*Thread + 0x2f8, 8)), Head);
  for (auto [Address, Size, IsWrite] :
       {std::tuple<uint64_t, uint32_t, bool>{Head, 8, true},
        {Head + 12, 8, false},
        {Head - 1, 2, false}}) {
    auto Denied = Model.validateGuestAccess(Address, Size, IsWrite);
    EXPECT_TRUE(bool(Denied));
    llvm::consumeError(std::move(Denied));
  }
  auto Write = Model.validateGuestAccess(*Thread, 8, true);
  EXPECT_TRUE(bool(Write));
  llvm::consumeError(std::move(Write));
  const auto Auxiliary = (*Thread + 0x898 + 15) & ~uint64_t(15);
  auto Hidden = Model.validateGuestAccess(Auxiliary, 8, false);
  ASSERT_TRUE(bool(Hidden));
  EXPECT_NE(llvm::toString(std::move(Hidden)).find("construction resources"),
            std::string::npos);
  llvm::cantFail(Model.call("KeEnterCriticalRegion", {}));
  auto Critical = Model.validateGuestAccess(*Thread, 8, false);
  EXPECT_TRUE(bool(Critical));
  llvm::consumeError(std::move(Critical));
  llvm::cantFail(Model.validateGuestAccess(*Thread + 0x478, 16, false));
  llvm::cantFail(Model.call("KeLeaveCriticalRegion", {}));
  llvm::cantFail(Model.validateGuestAccess(*Thread, 0x898, false));
  llvm::cantFail(Model.call("KeSetPriorityThread", {*Thread, 9}));
  llvm::cantFail(Model.call("KeSetPriorityThread", {*Thread, 8}));
  auto Stale = Model.validateGuestAccess(*Thread, 8, false);
  EXPECT_TRUE(bool(Stale));
  llvm::consumeError(std::move(Stale));
  auto StaleHead = Model.validateGuestAccess(Head, 8, false);
  EXPECT_TRUE(bool(StaleHead));
  llvm::consumeError(std::move(StaleHead));
  llvm::cantFail(Model.validateGuestAccess(*Thread + 0x478, 16, false));
}
TEST(KernelReferenceThread, ClockOrNewThreadsCannotReuseTheInitialSnapshot) {
  const auto Options = nativeThreadOptions();
  if (!Options)
    GTEST_SKIP() << "requires explicit owned Windows 19041 PE/PDB";
  auto Reference = llvm::cantFail(KernelReferenceImage::load(
      Options->KernelImage, Options->MemoryLimit, Options->KernelPDB));
  for (bool CreateThread : {false, true}) {
    auto CPU = llvm::cantFail(UnicornBackend::create(Options->MemoryLimit));
    llvm::cantFail(Reference.map(*CPU));
    DriverResult Result;
    KernelModel Model(*CPU, Result);
    llvm::cantFail(Model.setDebuggerReference(Reference));
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    llvm::cantFail(Model.initialize(Image, *Options));
    llvm::cantFail(Model.initializeDebuggerState());
    Model.enterExecution(profile::StackBase);
    const auto Thread = llvm::cantFail(Model.call("PsGetCurrentThread", {}));
    llvm::cantFail(Model.validateGuestAccess(Thread, 0x898, false));
    if (CreateThread) {
      const auto Scratch =
          llvm::cantFail(Model.call("ExAllocatePoolWithTag", {0, 16, 1}));
      EXPECT_EQ(llvm::cantFail(Model.call(
                    "PsCreateSystemThread",
                    {Scratch, 0x1fffff, 0, 0, 0, Image.Entry + 0x100, 0})),
                0u);
    } else {
      llvm::cantFail(Model.advanceExecutionTo100ns(1));
    }
    auto Stale = Model.validateGuestAccess(Thread, 8, false);
    EXPECT_TRUE(bool(Stale));
    llvm::consumeError(std::move(Stale));
    llvm::cantFail(Model.validateGuestAccess(Thread + 0x478, 16, false));
  }
}
} // namespace
} // namespace neverd::emulation
