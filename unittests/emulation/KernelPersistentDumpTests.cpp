//===- KernelPersistentDumpTests.cpp - Observable capture boundaries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Observable capture boundaries.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
class KernelPersistentDump : public testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000, Size = 0x40000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  uint64_t Output = 0;
  std::array<uint64_t, 8> Arguments;
  void check(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    check(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions Options;
    Options.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    check(Model->initialize(Image, Options));
    Output = call("ExAllocatePoolWithTag", {0, Size + 16, 0x64756d70});
    ASSERT_NE(Output, 0u);
    check(Memory->write(Scratch, std::vector<uint8_t>(1232, 0xa5)));
    check(Memory->writeInteger(Scratch + 248, 0x180001234, 8));
    Arguments = {Scratch, 0, 0x123456789abcdef0, 1, 2, 3, 4, Output};
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    auto Value = Model->call(Name, A);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }
  uint64_t integer(uint64_t Address, unsigned Width = 8) {
    return llvm::cantFail(Memory->readInteger(Address, Width));
  }
  void capture() {
    auto Value = Model->call("KeCapturePersistentThreadState", Arguments);
    ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
    EXPECT_EQ(*Value, Size);
  }
  std::string rejected() {
    auto Value = Model->call("KeCapturePersistentThreadState", Arguments);
    if (Value) {
      ADD_FAILURE() << "capture unexpectedly succeeded";
      return {};
    }
    return llvm::toString(Value.takeError());
  }
  std::string denied(uint64_t Address, uint32_t Length, bool Write = false) {
    auto E = Model->validateGuestAccess(Address, Length, Write);
    if (!E) {
      ADD_FAILURE() << "unmodeled dump bytes unexpectedly accessible";
      return {};
    }
    return llvm::toString(std::move(E));
  }
};

TEST_F(KernelPersistentDump,
       PublishesVersionedHeaderContextAndBugcheckArguments) {
  capture();
  EXPECT_EQ(integer(Output), 0x3436554445474150ULL); // PAGEDU64
  EXPECT_EQ(integer(Output + 8, 4), 15u);
  EXPECT_EQ(integer(Output + 12, 4), 19041u);
  EXPECT_EQ(integer(Output + 0x30, 4), 0x8664u);
  EXPECT_EQ(integer(Output + 0x34, 4), 1u);
  EXPECT_EQ(integer(Output + 0x38, 4), 0x9abcdef0u);
  for (unsigned I = 0; I != 4; ++I)
    EXPECT_EQ(integer(Output + 0x40 + I * 8), I + 1u);
  std::vector<uint8_t> Source(1232), Saved(1232);
  check(Memory->read(Scratch, Source));
  check(Memory->read(Output + 0x348, Saved));
  EXPECT_EQ(Source, Saved);
  EXPECT_EQ(integer(Output + 0xf00, 4), 0x80000003u);
  EXPECT_EQ(integer(Output + 0xf10), 0x180001234u);
  EXPECT_EQ(integer(Output + 0xf18, 4), 0u);
  EXPECT_EQ(integer(Output + 0xf98, 4), 4u);
  EXPECT_EQ(integer(Output + 0xfa0), Size);
  EXPECT_EQ(integer(Output + 0x2004, 4), Size);
  EXPECT_EQ(integer(Output + 0x2008, 4), Size - 4);
  EXPECT_EQ(integer(Output + 0x200c, 4), 0x348u);
  EXPECT_EQ(integer(Output + 0x3fffc, 4), 0x44475254u);
  EXPECT_EQ(integer(Output + Size), 0xcdcdcdcdcdcdcdcdULL);
  check(Model->validateGuestAccess(Output + 0x348, 1232, false));
  check(Model->validateGuestAccess(Output + Size - 4, 12, false));
}

TEST_F(KernelPersistentDump,
       RejectsUnknownStateAndBulkCopiesBeforeOutputChanges) {
  capture();
  for (uint64_t Offset : {0x10u, 0x18u, 0x20u, 0x80u, 0x2080u}) {
    EXPECT_NE(denied(Output + Offset, 8).find("unmodeled persistent"),
              std::string::npos);
    EXPECT_NE(denied(Output + Offset, 8, true).find("unmodeled persistent"),
              std::string::npos);
  }
  EXPECT_EQ(call("MmIsAddressValid", {Output + 0x2080}), 1u);
  const auto Before = integer(Scratch + 0x2000);
  auto Copy = Model->call("RtlCopyMemory", {Scratch + 0x2000, Output, 32});
  ASSERT_FALSE(bool(Copy));
  EXPECT_NE(llvm::toString(Copy.takeError()).find("unmodeled persistent"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch + 0x2000), Before);
  EXPECT_FALSE(Memory->hasMemoryFault());
}

TEST_F(KernelPersistentDump, RecaptureReplacesMetadataAndFreeRetiresTheView) {
  capture();
  Arguments[3] = 0xffff800012345678;
  capture();
  EXPECT_EQ(integer(Output + 0x40), Arguments[3]);
  call("ExFreePoolWithTag", {Output, 0x64756d70});
  EXPECT_NE(denied(Output + 0x2080, 8).find("freed"), std::string::npos);
}

TEST_F(KernelPersistentDump, BadContextAndThreadDoNotPublishPartialOutput) {
  const auto Before = integer(Output);
  Arguments[0] = Scratch + 1;
  EXPECT_NE(rejected().find("alignment"), std::string::npos);
  Arguments[0] = Output;
  EXPECT_NE(rejected().find("overlaps"), std::string::npos);
  Arguments[0] = Scratch;
  Arguments[1] = 0x1234;
  EXPECT_NE(rejected().find("current thread"), std::string::npos);
  EXPECT_EQ(integer(Output), Before);
  check(Model->validateGuestAccess(Output + 0x2080, 8, false));
}

TEST_F(KernelPersistentDump, ShortOrPagedPoolDoesNotBecomeADump) {
  for (const auto [Type, Bytes] :
       {std::pair<uint64_t, uint64_t>{0, Size - 1}, {1, Size}}) {
    const auto Buffer = call("ExAllocatePoolWithTag", {Type, Bytes, 1});
    ASSERT_NE(Buffer, 0u);
    Arguments[7] = Buffer;
    EXPECT_NE(rejected().find("nonpaged pool"), std::string::npos);
    EXPECT_EQ(integer(Buffer), 0xcdcdcdcdcdcdcdcdULL);
    call("ExFreePool", {Buffer});
  }
}

TEST_F(KernelPersistentDump, ReadOnlyOutputFailsBeforePublishingAnyBytes) {
  check(Memory->protect(Output, Size, Read));
  EXPECT_NE(rejected().find("not writable"), std::string::npos);
  EXPECT_EQ(integer(Output), 0xcdcdcdcdcdcdcdcdULL);
  check(Model->validateGuestAccess(Output + 0x2080, 8, false));
}

TEST_F(KernelPersistentDump, NullOutputDoesNotReadTheContext) {
  Arguments[0] = 0;
  Arguments[7] = 0;
  auto Value = Model->call("KeCapturePersistentThreadState", Arguments);
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  EXPECT_EQ(*Value, 0u);
  EXPECT_FALSE(Memory->hasMemoryFault());
}

TEST_F(KernelPersistentDump, ExplicitCurrentThreadUsesTheExistingIdentity) {
  Model->enterExecution(profile::StackBase);
  auto Thread = Model->currentThreadObject();
  ASSERT_TRUE(bool(Thread)) << llvm::toString(Thread.takeError());
  Arguments[1] = *Thread;
  capture();
  EXPECT_EQ(llvm::cantFail(Model->currentThreadObject()), *Thread);
  Arguments[1] += 16;
  EXPECT_NE(rejected().find("other thread state"), std::string::npos);
  EXPECT_EQ(integer(Output + 0x40), 1u);
}

TEST_F(KernelPersistentDump, ExistingAndNewMDLsCannotExposeUnknownData) {
  const auto MDL = call("IoAllocateMdl", {Output, Size, 0, 0, 0});
  ASSERT_NE(MDL, 0u);
  call("MmBuildMdlForNonPagedPool", {MDL});
  EXPECT_NE(rejected().find("retained by an MDL"), std::string::npos);
  EXPECT_EQ(integer(Output), 0xcdcdcdcdcdcdcdcdULL);
  call("IoFreeMdl", {MDL});
  capture();
  const auto Next = call("IoAllocateMdl", {Output, Size, 0, 0, 0});
  auto Build = Model->call("MmBuildMdlForNonPagedPool", {Next});
  ASSERT_FALSE(bool(Build));
  EXPECT_NE(llvm::toString(Build.takeError()).find("unmodeled persistent"),
            std::string::npos);
  call("IoFreeMdl", {Next});
}

TEST_F(KernelPersistentDump, LockedAliasesRetainOnlyTheKnownByteRange) {
  capture();
  Model->enterExecution(profile::StackBase);
  const auto Unknown = call("IoAllocateMdl", {Output, Size, 0, 0, 0});
  auto Lock = Model->call("MmProbeAndLockPages", {Unknown, 0, 0});
  ASSERT_FALSE(bool(Lock));
  EXPECT_NE(llvm::toString(Lock.takeError()).find("unmodeled persistent"),
            std::string::npos);
  EXPECT_EQ(integer(Unknown + 0xa, 2), 0u);
  call("IoFreeMdl", {Unknown});
  const auto MDL = call("IoAllocateMdl", {Output + 0x30, 0x30, 0, 0, 0});
  call("MmProbeAndLockPages", {MDL, 0, 0});
  const auto Alias = call("MmGetSystemAddressForMdlSafe", {MDL, 16});
  ASSERT_NE(Alias, 0u);
  check(Model->validateGuestAccess(Alias, 0x30, false));
  EXPECT_EQ(integer(Alias), integer(Output + 0x30));
  EXPECT_TRUE(llvm::cantFail(Memory->canAccess(Alias - 0x18, 8, Read)));
  EXPECT_NE(denied(Alias - 0x18, 8).find("MDL byte range"), std::string::npos);
  EXPECT_NE(rejected().find("pinned"), std::string::npos);
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  capture();
}

TEST(KernelPersistentDumpProfile, RequiresAnExplicitSupportedWindowsVersion) {
  for (std::optional<DriverOptions::WindowsVersion> Version :
       {std::optional<DriverOptions::WindowsVersion>{},
        std::optional{DriverOptions::WindowsVersion{10, 0, 18362}},
        std::optional{DriverOptions::WindowsVersion{10, 0, 22621}}}) {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    DriverResult Result;
    KernelModel Model(**Backend, Result);
    DriverOptions Options;
    Options.Version = Version;
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    auto Initialized = Model.initialize(Image, Options);
    ASSERT_FALSE(bool(Initialized)) << llvm::toString(std::move(Initialized));
    auto Capture =
        Model.call("KeCapturePersistentThreadState", {0, 0, 0, 0, 0, 0, 0, 0});
    ASSERT_FALSE(bool(Capture));
    EXPECT_NE(llvm::toString(Capture.takeError()).find("10.0.19041"),
              std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation
