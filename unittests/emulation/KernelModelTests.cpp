//===- KernelModelTests.cpp - Windows initialization API contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows initialization API contracts.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace neverd::emulation {
namespace {

class DriverKernelModel : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(static_cast<bool>(Backend))
        << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    success(Model->initialize(Image, DriverOptions{}));
  }

  void success(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }

  uint64_t invoke(const char *Name, std::initializer_list<uint64_t> Arguments) {
    auto Value = Model->call(Name, Arguments);
    if (!Value) {
      ADD_FAILURE() << Name << ": " << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  std::string failure(const char *Name,
                      std::initializer_list<uint64_t> Arguments) {
    auto Value = Model->call(Name, Arguments);
    if (Value) {
      ADD_FAILURE() << Name << " unexpectedly succeeded";
      return {};
    }
    return llvm::toString(Value.takeError());
  }

  std::string denied(uint64_t Address, uint32_t Size, bool IsWrite = false) {
    auto Error = Model->validateGuestAccess(Address, Size, IsWrite);
    if (!Error) {
      ADD_FAILURE() << "guest access unexpectedly allowed";
      return {};
    }
    return llvm::toString(std::move(Error));
  }

  uint64_t integer(uint64_t Address, unsigned Size = 8) {
    auto Value = Memory->readInteger(Address, Size);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  std::vector<uint8_t> bytes(uint64_t Address, size_t Size) {
    std::vector<uint8_t> Bytes(Size);
    success(Memory->read(Address, Bytes));
    return Bytes;
  }

  void ascii(uint64_t Address, std::string_view Text) {
    std::vector<uint8_t> Bytes(Text.begin(), Text.end());
    Bytes.push_back(0);
    success(Memory->write(Address, Bytes));
  }

  void utf16(uint64_t Address, std::u16string_view Text) {
    std::vector<uint8_t> Bytes((Text.size() + 1) * 2);
    for (size_t I = 0; I < Text.size(); ++I) {
      Bytes[I * 2] = static_cast<uint8_t>(Text[I]);
      Bytes[I * 2 + 1] = static_cast<uint8_t>(Text[I] >> 8);
    }
    success(Memory->write(Address, Bytes));
  }

  void unicode(uint64_t Record, std::u16string_view Text) {
    // Construct the public x64 UNICODE_STRING ABI independently of the API
    // under test: byte lengths, then an aligned guest buffer pointer.
    utf16(Record + 32, Text);
    success(Memory->writeInteger(Record, Text.size() * 2, 2));
    success(Memory->writeInteger(Record + 2, (Text.size() + 1) * 2, 2));
    success(Memory->writeInteger(Record + 8, Record + 32, 8));
  }
};

TEST_F(DriverKernelModel, PoolOwnershipSurvivesBadTagAndRejectsUseAfterFree) {
  constexpr uint32_t Tag = 0x74736554;
  const uint64_t Pointer = invoke("ExAllocatePoolWithTag", {512, 48, Tag});
  ASSERT_NE(Pointer, 0u);
  EXPECT_EQ(Pointer % 16, 0u);
  success(Model->validateGuestAccess(Pointer, 48, true));
  EXPECT_NE(failure("ExFreePoolWithTag", {Pointer, Tag + 1}).find("tag"),
            std::string::npos);
  success(Model->validateGuestAccess(Pointer, 48, false));
  invoke("ExFreePoolWithTag", {Pointer, Tag});
  EXPECT_NE(denied(Pointer, 1).find("freed"), std::string::npos);
  EXPECT_NE(denied(Pointer + 47, 1, true).find("freed"), std::string::npos);
  EXPECT_NE(failure("ExFreePoolWithTag", {Pointer, Tag}).find("freed"),
            std::string::npos);
}

TEST_F(DriverKernelModel, MDLMappingIgnoresUnspecifiedNarrowArgumentHighBits) {
  using namespace windows;
  invoke("IoCreateDevice",
         {Model->driverObject(), 0, 0, UnknownDeviceType, 0, 0, Scratch});
  for (unsigned Major : {0u, 14u}) {
    const uint64_t Slot =
        Model->driverObject() + DriverDispatchOffset + Major * 8;
    success(Model->validateGuestAccess(Slot, 8, true));
    success(Memory->writeInteger(Slot, Entry, 8));
  }
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  auto Call = Model->beginRequest(Create);
  ASSERT_TRUE(static_cast<bool>(Call)) << llvm::toString(Call.takeError());
  const uint64_t IRP = Call->Argument1;
  success(Model->validateGuestAccess(IRP + IRPStatusOffset, 16, true));
  success(Memory->writeInteger(IRP + IRPStatusOffset, 0, 4));
  success(Memory->writeInteger(IRP + IRPInformationOffset, 0, 8));
  invoke("IofCompleteRequest", {IRP, 0});
  success(Model->finishRequest(0));
  DriverRequest IO;
  IO.ControlCode = 0x222002;
  IO.OutputSize = 2;
  IO.DirectInput = {0xab, 0xcd};
  Call = Model->beginRequest(IO);
  ASSERT_TRUE(static_cast<bool>(Call)) << llvm::toString(Call.takeError());
  const uint64_t MDL = integer(Call->Argument1 + IRPMdlOffset);
  constexpr uint64_t Upper = 0xaabbccdd00000000;
  const uint64_t Pointer = invoke("MmMapLockedPagesSpecifyCache",
                                  {MDL, Upper, Upper | MmCached, 0,
                                   Upper | 0x100, Upper | NormalPagePriority});
  ASSERT_NE(Pointer, 0u);
  EXPECT_EQ(bytes(Pointer, 2), (std::vector<uint8_t>{0xab, 0xcd}));
  EXPECT_EQ(
      invoke("MmGetSystemAddressForMdlSafe", {MDL, Upper | NormalPagePriority}),
      Pointer);
}

TEST_F(DriverKernelModel, UnicodeInitializationUsesByteLengthsAndNullSource) {
  success(Memory->write(Scratch, std::vector<uint8_t>(16, 0xaa)));
  invoke("RtlInitUnicodeString", {Scratch, 0});
  EXPECT_EQ(integer(Scratch, 2), 0u);
  EXPECT_EQ(integer(Scratch + 2, 2), 0u);
  EXPECT_EQ(integer(Scratch + 8), 0u);
  EXPECT_EQ(integer(Scratch + 4, 4), 0xaaaaaaaau); // ABI padding is untouched.

  const uint64_t Source = Scratch + 0x100;
  utf16(Source, u"A\U0001f600"); // One ASCII unit and a surrogate pair.
  invoke("RtlInitUnicodeString", {Scratch, Source});
  EXPECT_EQ(integer(Scratch, 2), 6u);
  EXPECT_EQ(integer(Scratch + 2, 2), 8u);
  EXPECT_EQ(integer(Scratch + 8), Source);
}

TEST_F(DriverKernelModel, DevicesLinkCollideAndDeleteWithoutLosingLiveState) {
  const uint64_t NameA = Scratch + 0x100;
  const uint64_t NameB = Scratch + 0x200;
  const uint64_t AliasA = Scratch + 0x300;
  unicode(NameA, u"\\Device\\First");
  unicode(NameB, u"\\Device\\Second");
  unicode(AliasA, u"\\device\\FIRST");
  auto Create = [&](uint64_t Name) {
    return invoke("IoCreateDevice",
                  {Model->driverObject(), 32, Name, 0x22, 0x100, 0, Scratch});
  };
  ASSERT_EQ(Create(NameA), 0u);
  const uint64_t First = integer(Scratch);
  ASSERT_EQ(Create(NameB), 0u);
  const uint64_t Second = integer(Scratch);
  EXPECT_NE(First, Second);
  EXPECT_EQ(Create(AliasA), 0xc0000035u); // STATUS_OBJECT_NAME_COLLISION.
  EXPECT_EQ(integer(Model->driverObject() + 8), Second);
  EXPECT_EQ(integer(Second + 0x10), First);
  const auto Extension = bytes(integer(Second + 0x40), 32);
  EXPECT_TRUE(std::all_of(Extension.begin(), Extension.end(),
                          [](uint8_t Byte) { return Byte == 0; }));
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 2u);
  EXPECT_EQ(Result.Devices[0].Name, "\\Device\\Second");
  EXPECT_EQ(Result.Devices[1].Address, First);

  invoke("IoDeleteDevice", {First});
  EXPECT_EQ(integer(Second + 0x10), 0u);
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(Result.Devices[0].Address, Second);
  EXPECT_NE(denied(First, 1).find("freed"), std::string::npos);
  invoke("IoDeleteDevice", {Second});
  EXPECT_EQ(integer(Model->driverObject() + 8), 0u);
  success(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(DriverKernelModel,
       WorkItemKeepsDeletePendingDeviceUntilCallbackReturns) {
  invoke("IoCreateDevice", {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch});
  const auto Device = integer(Scratch);
  const auto Item = invoke("IoAllocateWorkItem", {Device});
  ASSERT_NE(Item, 0u);
  invoke("IoQueueWorkItem", {Item, Entry, profile::DelayedWorkQueue, Scratch});
  invoke("IoDeleteDevice", {Device});
  success(Model->snapshot());
  ASSERT_EQ(Result.Devices.size(), 1u);
  EXPECT_EQ(integer(Device + windows::DeviceReferenceCount, 4), 1u);
  auto Next = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
  ASSERT_TRUE(Next->has_value());
  EXPECT_EQ((**Next).Arguments, (std::vector<uint64_t>{Device, Scratch}));
  invoke("IoFreeWorkItem", {Item});
  success(Model->validateGuestAccess(Device, 2, false));
  success(Model->finishScheduled((**Next).ID));
  success(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
  EXPECT_NE(denied(Device, 1).find("freed"), std::string::npos);
}

TEST_F(DriverKernelModel, WorkItemAllocationExhaustionReturnsNull) {
  invoke("IoCreateDevice", {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch});
  const auto Device = integer(Scratch);
  ASSERT_NE(invoke("ExAllocatePoolWithTag",
                   {0, profile::KernelArenaSize - profile::PageSize, 0x1234}),
            0u);
  bool Exhausted = false;
  for (size_t I = 0; I < profile::PageSize / profile::WorkItemTokenSize; ++I) {
    auto Item = Model->call("IoAllocateWorkItem", {Device});
    ASSERT_TRUE(bool(Item)) << llvm::toString(Item.takeError());
    if (!*Item) {
      Exhausted = true;
      break;
    }
    invoke("IoFreeWorkItem", {*Item});
  }
  EXPECT_TRUE(Exhausted);
}

TEST_F(DriverKernelModel, SymbolicLinksShareTheSessionDosNamespace) {
  const uint64_t Name = Scratch + 0x100;
  const uint64_t Alias = Scratch + 0x200;
  const uint64_t Target = Scratch + 0x300;
  unicode(Name, u"\\DosDevices\\Example");
  unicode(Alias, u"\\??\\EXAMPLE");
  unicode(Target, u"\\Device\\Example");
  EXPECT_EQ(invoke("IoCreateSymbolicLink", {Name, Target}), 0u);
  EXPECT_EQ(invoke("IoCreateSymbolicLink", {Alias, Target}), 0xc0000035u);
  EXPECT_EQ(invoke("IoDeleteSymbolicLink", {Alias}), 0u);
  EXPECT_EQ(invoke("IoDeleteSymbolicLink", {Name}), 0xc0000034u);
  EXPECT_EQ(invoke("IoCreateSymbolicLink", {Name, Target}), 0u);
}

TEST_F(DriverKernelModel,
       MemoryOperationsRespectOverlapAndComparisonContracts) {
  ascii(Scratch, "abcdef");
  EXPECT_EQ(invoke("memmove", {Scratch + 2, Scratch, 4}), Scratch + 2);
  EXPECT_EQ(bytes(Scratch, 6),
            (std::vector<uint8_t>{'a', 'b', 'a', 'b', 'c', 'd'}));
  EXPECT_NE(failure("memcpy", {Scratch + 1, Scratch, 4}).find("overlapping"),
            std::string::npos);
  ascii(Scratch + 0x100, "abazcd");
  const auto Comparison = static_cast<int32_t>(
      static_cast<uint32_t>(invoke("memcmp", {Scratch, Scratch + 0x100, 6})));
  EXPECT_LT(Comparison, 0);
  EXPECT_EQ(invoke("RtlCompareMemory", {Scratch, Scratch + 0x100, 6}), 3u);
  EXPECT_EQ(invoke("memcmp", {Scratch, Scratch, 6}), 0u);
  EXPECT_EQ(invoke("RtlCompareMemory", {Scratch, Scratch, 6}), 6u);
}

TEST_F(DriverKernelModel, DebugLiteralsAreCapturedButVariadicFormatsFail) {
  ascii(Scratch, "driver 100%%\n");
  EXPECT_EQ(invoke("DbgPrint", {Scratch}), 0u);
  EXPECT_EQ(invoke("DbgPrintEx", {77, 2, Scratch}), 0u);
  ASSERT_EQ(Result.Messages.size(), 2u);
  EXPECT_EQ(Result.Messages[0], "driver 100%\n");
  EXPECT_EQ(Result.Messages[1], Result.Messages[0]);
  ascii(Scratch, "count=%u");
  EXPECT_NE(failure("DbgPrint", {Scratch}).find("variadic"), std::string::npos);
  EXPECT_EQ(Result.Messages.size(), 2u);
}

TEST_F(DriverKernelModel, MemoryAPIsCannotBypassOpaqueObjectAccessChecks) {
  const uint64_t DriverSection = Model->driverObject() + 0x28;
  EXPECT_NE(failure("memcpy", {Scratch, DriverSection, 8}).find("unmodeled"),
            std::string::npos);
  EXPECT_NE(failure("RtlZeroMemory", {DriverSection, 8}).find("opaque"),
            std::string::npos);
  EXPECT_NE(failure("RtlInitUnicodeString", {DriverSection, 0}).find("opaque"),
            std::string::npos);
  // A thunk is an execution token, never an image of ntoskrnl code/data.
  success(Memory->map(0x72000000, 0x1000, Read | Write | Execute));
  success(Memory->writeInteger(0x72000000, 0xcccccccc, 4));
  EXPECT_NE(failure("memcpy", {Scratch, 0x72000000, 4}).find("thunks"),
            std::string::npos);
  EXPECT_NE(failure("RtlCopyMemory", {Scratch, 0x72000000, 4}).find("thunks"),
            std::string::npos);
}

TEST_F(DriverKernelModel,
       UnknownArenaAndDefaultDispatchAddressesStayUnobservable) {
  EXPECT_NE(denied(0x700ff000, 8).find("unallocated"), std::string::npos);
  const uint64_t Dispatch = Model->driverObject() + 0x70;
  EXPECT_NE(denied(Dispatch, 8).find("unmodeled"), std::string::npos);
  EXPECT_NE(failure("memcpy", {Scratch, Dispatch, 8}).find("unmodeled"),
            std::string::npos);
  // Mirror a CPU write notification before storing the guest callback.
  success(Model->validateGuestAccess(Dispatch, 8, true));
  success(Memory->writeInteger(Dispatch, Entry + 0x20, 8));
  success(Model->validateGuestAccess(Dispatch, 8, false));
  success(Model->snapshot());
  EXPECT_EQ(Result.MajorFunctions[0], Entry + 0x20);
  EXPECT_EQ(Result.MajorFunctions[1], 0u); // No guest registration observed.
}

} // namespace
} // namespace neverd::emulation
