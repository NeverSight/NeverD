//===- KernelSymbolicLinkTests.cpp - Object namespace handle contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Object namespace handle contracts.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
class KernelSymbolicLink : public ::testing::Test {
protected:
  static constexpr uint64_t S = 0x20000000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void initialize(bool Inventory = true) {
    Model.reset();
    auto M = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(M)) << llvm::toString(M.takeError());
    Memory = std::move(*M);
    ok(Memory->map(S, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    if (Inventory)
      O.FiniteFileSystem = std::vector<DriverFiniteFileSystemVolume>{
          {"C", "\\Device\\NeverDVolumeC", {}}};
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 0x1000;
    I.Size = 0x3000;
    ok(Model->initialize(I, O));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void SetUp() override { initialize(); }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    auto V = Memory->readInteger(Address, Width);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
    auto V = Model->call(Name.str(), A);
    if (!V) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(V.takeError());
      return UINT64_MAX;
    }
    return *V;
  }
  void error(llvm::StringRef Name, std::initializer_list<uint64_t> A,
             llvm::StringRef Text) {
    auto V = Model->call(Name.str(), A);
    ASSERT_FALSE(bool(V));
    auto Message = llvm::toString(V.takeError());
    EXPECT_NE(Message.find(Text.str()), std::string::npos);
  }
  void string(uint64_t Record, uint64_t Buffer, llvm::StringRef Text) {
    std::vector<uint8_t> Bytes((Text.size() + 1) * 2);
    for (size_t I = 0; I < Text.size(); ++I)
      Bytes[I * 2] = uint8_t(Text[I]);
    ok(Memory->write(Buffer, Bytes));
    put(Record, Text.size() * 2, 2);
    put(Record + 2, Bytes.size(), 2);
    put(Record + 8, Buffer);
  }
  void name(llvm::StringRef Text, uint32_t Attributes = 0x240) {
    string(S + 0x100, S + 0x200, Text);
    put(S + 0x300, 48, 4);
    put(S + 0x308, 0);
    put(S + 0x310, S + 0x100);
    put(S + 0x318, Attributes, 4);
    put(S + 0x320, 0);
    put(S + 0x328, 0);
  }
  uint64_t open(llvm::StringRef Text = "\\??\\C:", uint32_t Attributes = 0x240,
                uint32_t Access = 0x80000000) {
    name(Text, Attributes);
    return call("ZwOpenSymbolicLinkObject", {S, Access, S + 0x300});
  }
  void output(uint16_t Maximum, uint16_t Length = 0) {
    put(S + 0x400, Length, 2);
    put(S + 0x402, Maximum, 2);
    put(S + 0x404, 0xa5a5a5a5, 4);
    put(S + 0x408, S + 0x1000);
    ok(Memory->write(S + 0x1000, std::vector<uint8_t>(1024, 0xcc)));
    put(S + 0x800, 0xaaaaaaaa, 4);
  }
  void expectTarget(llvm::StringRef Target) {
    EXPECT_EQ(get(S + 0x400, 2), Target.size() * 2);
    for (size_t I = 0; I < Target.size(); ++I)
      EXPECT_EQ(get(S + 0x1000 + I * 2, 2), uint8_t(Target[I]));
  }
};

TEST_F(KernelSymbolicLink, MissingOpenZerosOutputAndAliasesShareOneObject) {
  put(S, 0x77777777);
  EXPECT_EQ(open("\\??\\A:"), 0xc0000034u);
  EXPECT_EQ(get(S), 0u);
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  output(1024);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, S + 0x800}), 0u);
  expectTarget("\\Device\\NeverDVolumeC");
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  ASSERT_EQ(open("\\DosDevices\\c:"), 0u);
  H = get(S);
  output(1024);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, 0}), 0u);
  expectTarget("\\Device\\NeverDVolumeC");
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0xc0000008u);
}

TEST_F(KernelSymbolicLink,
       ProbeRequiresMaximumIncludingNulAndPreservesShortOutputs) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  const uint16_t Length = 42, Required = 44;
  for (uint16_t Maximum : {0, 1, 2, 41, 42, 43, 44, 1024}) {
    output(Maximum, 7);
    EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, S + 0x800}),
              Maximum < Required ? 0xc0000023u : 0u);
    EXPECT_EQ(get(S + 0x800, 4), Required);
    EXPECT_EQ(get(S + 0x402, 2), Maximum);
    EXPECT_EQ(get(S + 0x404, 4), 0xa5a5a5a5u);
    if (Maximum < Required) {
      EXPECT_EQ(get(S + 0x400, 2), 7u);
      EXPECT_EQ(get(S + 0x1000), 0xccccccccccccccccu);
    } else {
      expectTarget("\\Device\\NeverDVolumeC");
      EXPECT_EQ(get(S + 0x1000 + Length, 2), 0u);
      EXPECT_EQ(get(S + 0x1000 + Required, 2), 0xccccu);
    }
  }
  output(0);
  put(S + 0x408, 0);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, S + 0x800}),
            0xc0000023u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelSymbolicLink, OptionalReturnedLengthChangesTerminatorContract) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  output(42);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, 0}), 0u);
  expectTarget("\\Device\\NeverDVolumeC");
  EXPECT_EQ(get(S + 0x102a, 2), 0xccccu);
  output(41);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, 0}), 0xc0000023u);
  EXPECT_EQ(get(S + 0x800, 4), 0xaaaaaaaau);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelSymbolicLink,
       OutputAliasingFollowsCopyThenLengthThenRequiredOrder) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  for (uint64_t RequiredAt : {S + 0x400, S + 0x402, S + 0x408, S + 0x1000}) {
    output(1024);
    EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, RequiredAt}),
              0u);
    EXPECT_EQ(get(RequiredAt, 4), 44u);
    if (RequiredAt == S + 0x400)
      EXPECT_EQ(get(S + 0x402, 2), 0u);
    else
      EXPECT_EQ(get(S + 0x400, 2), 42u);
  }
  output(1024);
  put(S + 0x408, S + 0x400);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, S + 0x800}), 0u);
  EXPECT_EQ(get(S + 0x400, 2),
            42u); // Buffer overwrote the descriptor before Length.
  EXPECT_EQ(get(S + 0x402, 2), uint64_t('D'));
  EXPECT_EQ(get(S + 0x800, 4), 44u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelSymbolicLink, OpenHandlePinsUnlinkedObjectAcrossRecreation) {
  name("\\DosDevices\\Alias");
  string(S + 0x500, S + 0x600, "\\Device\\Old");
  EXPECT_EQ(call("IoCreateSymbolicLink", {S + 0x100, S + 0x500}), 0u);
  ASSERT_EQ(open("\\??\\Alias"), 0u);
  auto Old = get(S);
  EXPECT_EQ(call("IoDeleteSymbolicLink", {S + 0x100}), 0u);
  string(S + 0x500, S + 0x600, "\\Device\\New");
  EXPECT_EQ(call("IoCreateSymbolicLink", {S + 0x100, S + 0x500}), 0u);
  ASSERT_EQ(open("\\??\\Alias"), 0u);
  auto New = get(S);
  output(1024);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {Old, S + 0x400, S + 0x800}), 0u);
  expectTarget("\\Device\\Old");
  output(1024);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {New, S + 0x400, S + 0x800}), 0u);
  expectTarget("\\Device\\New");
  EXPECT_EQ(call("ZwClose", {Old}), 0u);
  EXPECT_EQ(call("ZwClose", {New}), 0u);
  EXPECT_EQ(call("IoDeleteSymbolicLink", {S + 0x100}), 0u);
}

TEST_F(KernelSymbolicLink, ExactLeafSpellingIsRetainedForCaseSensitiveOpen) {
  name("\\DosDevices\\Mixed");
  string(S + 0x500, S + 0x600, "\\Device\\Target");
  EXPECT_EQ(call("IoCreateSymbolicLink", {S + 0x100, S + 0x500}), 0u);
  EXPECT_EQ(open("\\??\\mixed", 0x200), 0xc0000034u);
  ASSERT_EQ(open("\\??\\Mixed", 0x200), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  ASSERT_EQ(open("\\??\\mixed"), 0u);
  EXPECT_EQ(call("ZwClose", {get(S)}), 0u);
  EXPECT_EQ(call("IoDeleteSymbolicLink", {S + 0x100}), 0u);
}

TEST_F(KernelSymbolicLink, SharedHandleScopeCoversOpenQueryAndClose) {
  ASSERT_EQ(open("\\??\\C:", 0x40), 0u);
  auto Scoped = get(S);
  ASSERT_EQ(open(), 0u);
  auto Kernel = get(S);
  output(1024);
  ok(Model->setUserRequestContext(true, 0x3456));
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {Scoped, S + 0x400, S + 0x800}),
            0xc0000008u);
  EXPECT_EQ(call("ZwClose", {Scoped}), 0xc0000008u);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {Kernel, S + 0x400, S + 0x800}),
            0u);
  EXPECT_EQ(call("ZwClose", {Kernel}), 0u);
  name("\\??\\C:", 0x40);
  error("ZwOpenSymbolicLinkObject", {S, 1, S + 0x300}, "OBJ_KERNEL_HANDLE");
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("ZwClose", {Scoped}), 0u);
}

TEST_F(KernelSymbolicLink, GenericMappingsAndKernelModeQueryContract) {
  for (uint32_t Access :
       {0u, 1u, 0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u}) {
    ASSERT_EQ(open("\\??\\C:", 0x240, Access), 0u);
    auto H = get(S);
    output(1024);
    EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, S + 0x800}), 0u);
    EXPECT_EQ(call("ZwClose", {H}), 0u);
  }
}

TEST_F(KernelSymbolicLink, TypeAndOpaqueObjectChecksDoNotInventFileObjects) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  output(1024);
  EXPECT_EQ(open("\\Device\\NeverDVolumeC"), 0xc0000024u);
  EXPECT_EQ(get(S), 0u);
  name("\\??\\C:");
  ASSERT_EQ(call("ZwOpenFile", {S, 0x100000, S + 0x300, S + 0x900, 1, 0x21}),
            0u);
  auto F = get(S);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {F, S + 0x400, S + 0x800}),
            0xc0000024u);
  EXPECT_EQ(call("ZwClose", {F}), 0u);
  EXPECT_EQ(call("ObReferenceObjectByHandle", {H, 0, 0, 0, S + 0x900, 0}), 0u);
  EXPECT_EQ(call("ObfDereferenceObject", {get(S + 0x900)}), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelSymbolicLink, InvalidOutputsDoNotPublishHandlesOrPartiallyCopy) {
  name("\\??\\C:");
  error("ZwOpenSymbolicLinkObject", {0xdead0000, 1, S + 0x300}, "permissions");
  EXPECT_EQ(call("ZwClose", {profile::SystemThreadHandleBase}), 0xc0000008u);
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  EXPECT_EQ(H, profile::SystemThreadHandleBase);
  output(1024);
  error("ZwQuerySymbolicLinkObject", {H, S + 0x400, 0xdead0000}, "permissions");
  EXPECT_EQ(get(S + 0x1000), 0xccccccccccccccccu);
  EXPECT_EQ(get(S + 0x400, 2), 0u);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelSymbolicLink,
       HandleLimitHasNativeFailureOutputAndCloseReleasesCapacity) {
  std::vector<uint64_t> Handles;
  for (unsigned I = 0; I < 256; ++I) {
    ASSERT_EQ(open(), 0u);
    Handles.push_back(get(S));
  }
  EXPECT_EQ(open(), 0xc000009au);
  EXPECT_EQ(get(S), 0u);
  EXPECT_EQ(call("ZwClose", {Handles.back()}), 0u);
  Handles.pop_back();
  ASSERT_EQ(open(), 0u);
  Handles.push_back(get(S));
  for (auto H : Handles)
    EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelSymbolicLink,
       GuestLinksWorkWithoutClaimingAnUnspecifiedHostNamespace) {
  initialize(false);
  name("\\??\\C:");
  EXPECT_EQ(call("ZwOpenSymbolicLinkObject", {S, 1, S + 0x300}), 0xc0000034u);
  name("\\??\\Owned");
  string(S + 0x500, S + 0x600, "\\Device\\Later");
  EXPECT_EQ(call("IoCreateSymbolicLink", {S + 0x100, S + 0x500}), 0u);
  ASSERT_EQ(open("\\??\\Owned"), 0u);
  auto H = get(S);
  output(1024);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {H, S + 0x400, S + 0x800}), 0u);
  expectTarget("\\Device\\Later");
  EXPECT_EQ(call("ZwClose", {H}), 0u);
}

TEST_F(KernelSymbolicLink, LiveHandlePreventsSuccessfulUnload) {
  ASSERT_EQ(open(), 0u);
  auto H = get(S);
  put(Model->driverObject() + windows::DriverUnloadOffset, 0x180001000);
  ok(Model->finishEntry());
  auto C = Model->beginUnload();
  ASSERT_TRUE(bool(C)) << llvm::toString(C.takeError());
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("symbolic-link handles"),
            std::string::npos);
  EXPECT_EQ(call("ZwClose", {H}), 0u);
  ok(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}
} // namespace
} // namespace neverd::emulation
