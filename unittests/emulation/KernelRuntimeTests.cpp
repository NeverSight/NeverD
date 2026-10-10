//===- KernelRuntimeTests.cpp - Windows runtime API contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent expectations for pool allocation, counted UTF-16 strings, and
/// guest debugger formatting. Guest pointers never reach host printf.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include <initializer_list>
#include <string_view>

namespace neverd::emulation {
namespace {
class KernelRuntime : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint32_t Tag = 0x74736554;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

  void check(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }

  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(static_cast<bool>(Backend))
        << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    check(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = 0x180001000;
    Image.Size = 0x3000;
    check(Model->initialize(Image, DriverOptions{}));
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

  uint64_t integer(uint64_t Address, unsigned Size = 8) {
    auto Value = Memory->readInteger(Address, Size);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  void ascii(uint64_t Address, std::string_view Text) {
    std::vector<uint8_t> Bytes(Text.begin(), Text.end());
    Bytes.push_back(0);
    check(Memory->write(Address, Bytes));
  }

  void unicode(uint64_t Record, std::u16string_view Text) {
    std::vector<uint8_t> Bytes(Text.size() * 2);
    for (size_t I = 0; I < Text.size(); ++I) {
      Bytes[I * 2] = static_cast<uint8_t>(Text[I]);
      Bytes[I * 2 + 1] = static_cast<uint8_t>(Text[I] >> 8);
    }
    check(Memory->write(Record + 32, Bytes));
    check(Memory->writeInteger(Record, Bytes.size(), 2));
    check(Memory->writeInteger(Record + 2, Bytes.size(), 2));
    check(Memory->writeInteger(Record + 8, Record + 32, 8));
  }

  llvm::Expected<uint64_t> debug(std::string_view Format,
                                 llvm::ArrayRef<uint64_t> Values,
                                 bool Extended = false,
                                 std::vector<unsigned> *ReadIndices = nullptr) {
    ascii(Scratch, Format);
    const unsigned First = Extended ? 3 : 1;
    auto Reader = [&](unsigned Index) -> llvm::Expected<uint64_t> {
      if (ReadIndices)
        ReadIndices->push_back(Index);
      if (Index < First || Index - First >= Values.size())
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "test guest argument unavailable");
      return Values[Index - First];
    };
    if (Extended)
      return Model->call("DbgPrintEx", {4, 3, Scratch}, Reader);
    return Model->call("DbgPrint", {Scratch}, Reader);
  }

  std::string formatted(std::string_view Format,
                        std::initializer_list<uint64_t> Values,
                        bool Extended = false) {
    auto Value = debug(Format, Values, Extended);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    EXPECT_EQ(*Value, 0u);
    return Result.Messages.back();
  }
};

TEST_F(KernelRuntime, ActiveProcessorMaskMatchesTheSingleProcessorSchedule) {
  EXPECT_EQ(invoke("KeQueryActiveProcessors", {}), 1u);
  Model->enterExecution(0x100);
  const auto Previous = invoke("KfRaiseIrql", {15});
  EXPECT_EQ(invoke("KeQueryActiveProcessors", {}), 1u);
  invoke("KeLowerIrql", {Previous});
  EXPECT_FALSE(failure("KeQueryActiveProcessors", {0}).empty());
}

TEST_F(KernelRuntime, SystemAffinityFollowsLogicalThreadsAndNestedCalls) {
  EXPECT_NE(failure("KeSetSystemAffinityThread", {1}).find("thread"),
            std::string::npos);
  Model->enterExecution(0x100, 0x42);
  invoke("KeSetSystemAffinityThread", {1});
  auto Retained = Model->validateExecutionReturn(0x100, 0);
  ASSERT_TRUE(bool(Retained));
  EXPECT_NE(llvm::toString(std::move(Retained)).find("affinity"),
            std::string::npos);
  const auto Parent = Model->captureExecutionContext();
  Model->enterExecution(0x300, 0x99);
  check(Model->validateExecutionReturn(0x300, 0));
  EXPECT_NE(failure("KeRevertToUserAffinityThread", {}).find("affinity"),
            std::string::npos);
  check(Model->restoreExecutionContext(Parent));
  check(Model->inheritExecutionContext(0x200, 0x100));
  Model->enterExecution(0x200, 0x42);
  check(Model->validateExecutionReturn(0x200, 0, true));
  invoke("KeRevertToUserAffinityThread", {});
  check(Model->restoreExecutionContext(Parent));
  check(Model->validateExecutionReturn(0x100, 0));
  // Legacy setters replace system affinity rather than nesting restorations.
  invoke("KeSetSystemAffinityThread", {1});
  invoke("KeSetSystemAffinityThread", {1});
  invoke("KeRevertToUserAffinityThread", {});
  check(Model->validateExecutionReturn(0x100, 0));
}

TEST_F(KernelRuntime, InvalidAffinityAndIRQLPreserveThreadState) {
  Model->enterExecution(0x100);
  invoke("KeSetSystemAffinityThread", {1});
  for (uint64_t Mask : {0ull, 2ull, 3ull, 0x100000001ull}) {
    SCOPED_TRACE(Mask);
    EXPECT_NE(failure("KeSetSystemAffinityThread", {Mask}).find("affinity"),
              std::string::npos);
  }
  const auto Previous = invoke("KfRaiseIrql", {15});
  EXPECT_NE(failure("KeSetSystemAffinityThread", {1}).find("IRQL"),
            std::string::npos);
  EXPECT_NE(failure("KeRevertToUserAffinityThread", {}).find("IRQL"),
            std::string::npos);
  invoke("KeLowerIrql", {Previous});
  auto Retained = Model->validateExecutionReturn(0x100, 0);
  ASSERT_TRUE(bool(Retained));
  llvm::consumeError(std::move(Retained));
  invoke("KeRevertToUserAffinityThread", {});
  check(Model->validateExecutionReturn(0x100, 0));
  EXPECT_FALSE(failure("KeRevertToUserAffinityThread", {}).empty());
  const auto DispatchPrevious = invoke("KfRaiseIrql", {2});
  invoke("KeSetSystemAffinityThread", {1});
  invoke("KeRevertToUserAffinityThread", {});
  invoke("KeLowerIrql", {DispatchPrevious});
  check(Model->validateExecutionReturn(0x100, 0));
}

TEST_F(KernelRuntime, ModuleQueryWithoutAnInventoryStopsBeforeWriting) {
  Model->enterExecution(profile::StackBase);
  check(Memory->writeInteger(Scratch, UINT64_MAX, 8));
  for (const char *Name :
       {"NtQuerySystemInformation", "ZwQuerySystemInformation"})
    EXPECT_NE(failure(Name, {11, 0, 0, Scratch}).find("unavailable"),
              std::string::npos);
  EXPECT_EQ(integer(Scratch), UINT64_MAX);
}

TEST_F(KernelRuntime, Pool2ZeroesAndPreservesTaggedLifetime) {
  const uint64_t Pointer = invoke("ExAllocatePool2", {0x40, 47, Tag});
  ASSERT_NE(Pointer, 0u);
  EXPECT_EQ(Pointer % 16, 0u);
  for (unsigned I = 0; I < 47; ++I)
    EXPECT_EQ(integer(Pointer + I, 1), 0u);
  check(Model->validateGuestAccess(Pointer, 47, true));
  invoke("ExFreePoolWithTag", {Pointer, Tag});
  auto Error = Model->validateGuestAccess(Pointer, 1, false);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("freed"), std::string::npos);
  const uint64_t Uninitialized = invoke("ExAllocatePool2", {0x102, 16, Tag});
  ASSERT_NE(Uninitialized, 0u);
  EXPECT_EQ(integer(Uninitialized, 8), 0xcdcdcdcdcdcdcdcdULL);
  invoke("ExFreePool", {Uninitialized});
}

TEST_F(KernelRuntime, LegacyUntaggedPoolSharesCheckedAllocationLifetime) {
  for (uint32_t Type : {0u, 1u, 512u})
    for (uint64_t Size : {1u, 16u, 4095u, 4096u, 4097u}) {
      SCOPED_TRACE(Type);
      SCOPED_TRACE(Size);
      const auto Pointer = invoke("ExAllocatePool", {Type, Size});
      ASSERT_NE(Pointer, 0u);
      EXPECT_EQ(Pointer % (Size >= 4096 ? 4096 : 16), 0u);
      if (Size < 4096)
        EXPECT_LE((Pointer % 4096) + Size, 4096u);
      EXPECT_EQ(integer(Pointer, 1), 0xcdu);
      EXPECT_EQ(integer(Pointer + Size - 1, 1), 0xcdu);
      check(Model->validateGuestAccess(Pointer, Size, true));
      EXPECT_NE(failure("ExFreePoolWithTag", {Pointer, Tag}).find("tag"),
                std::string::npos);
      check(Model->validateGuestAccess(Pointer, Size, true));
      if (Size & 1)
        invoke("ExFreePool", {Pointer});
      else
        invoke("ExFreePoolWithTag", {Pointer, 0});
      auto Error = Model->validateGuestAccess(Pointer, 1, false);
      ASSERT_TRUE(bool(Error));
      EXPECT_NE(llvm::toString(std::move(Error)).find("freed"),
                std::string::npos);
      EXPECT_NE(failure("ExFreePool", {Pointer}).find("already freed"),
                std::string::npos);
    }
}

TEST_F(KernelRuntime, LegacyUntaggedPoolRetainsArityIRQLAndFailureChecks) {
  EXPECT_NE(failure("ExAllocatePool", {0, 1, Tag}).find("argument count"),
            std::string::npos);
  EXPECT_NE(failure("ExAllocatePool", {0}).find("argument count"),
            std::string::npos);
  EXPECT_NE(failure("ExAllocatePool", {0, 0}).find("non-zero"),
            std::string::npos);
  EXPECT_NE(failure("ExAllocatePool", {2, 16}).find("pool model"),
            std::string::npos);
  EXPECT_EQ(invoke("ExAllocatePool", {0, UINT64_MAX}), 0u);
  Model->enterExecution(1);
  invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("ExAllocatePool", {1, 16}).find("APC_LEVEL"),
            std::string::npos);
  const auto Pointer = invoke("ExAllocatePool", {512, 16});
  ASSERT_NE(Pointer, 0u);
  invoke("ExFreePool", {Pointer});
  invoke("KeLowerIrql", {0});
  const auto Paged = invoke("ExAllocatePool", {1, 16});
  ASSERT_NE(Paged, 0u);
  invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("ExFreePool", {Paged}).find("APC_LEVEL"),
            std::string::npos);
  invoke("KeLowerIrql", {0});
  invoke("ExFreePool", {Paged});
  invoke("KfRaiseIrql", {3});
  EXPECT_NE(failure("ExAllocatePool", {512, 16}).find("IRQL"),
            std::string::npos);
  invoke("KeLowerIrql", {0});
  EXPECT_NE(failure("ExAllocatePoolWithTag", {512, 16, 0}).find("tag"),
            std::string::npos);
}

TEST_F(KernelRuntime, Pool2AlignmentOptionalFlagsAndFailureContracts) {
  const uint64_t Cached = invoke("ExAllocatePool2", {0x48, 31, Tag});
  ASSERT_NE(Cached, 0u);
  EXPECT_EQ(Cached % 64, 0u);
  const uint64_t Large = invoke("ExAllocatePool2", {0x40, 4097, Tag});
  ASSERT_NE(Large, 0u);
  EXPECT_EQ(Large % 4096, 0u);
  EXPECT_NE(invoke("ExAllocatePool2", {0x8000000000000040ULL, 16, Tag}), 0u);
  EXPECT_EQ(invoke("ExAllocatePool2", {0x40, 16, 0}), 0u);
  EXPECT_EQ(invoke("ExAllocatePool2", {0, 16, Tag}), 0u);
  EXPECT_EQ(invoke("ExAllocatePool2", {0x140, 16, Tag}), 0u);
  EXPECT_EQ(invoke("ExAllocatePool2", {0x80000040, 16, Tag}), 0u);
  EXPECT_EQ(invoke("ExAllocatePool2", {0x40, UINT64_MAX, Tag}), 0u);
  EXPECT_NE(
      failure("ExAllocatePool2", {0x60, UINT64_MAX, Tag}).find("exception"),
      std::string::npos);
  EXPECT_NE(invoke("ExAllocatePool2", {0x60, 16, Tag}), 0u);
  EXPECT_NE(failure("ExAllocatePool2", {0x41, 16, Tag}).find("quota"),
            std::string::npos);
  EXPECT_NE(failure("ExAllocatePool2", {0x80, 16, Tag}).find("executable"),
            std::string::npos);
}

TEST_F(KernelRuntime,
       CountedCopyTruncatesInBytesWithoutTouchingMetadataOrTail) {
  const uint64_t Destination = Scratch;
  const uint64_t Source = Scratch + 0x100;
  unicode(Destination, u"xxxx");
  unicode(Source, u"A\U0001f600Z");
  check(Memory->writeInteger(Destination, 0, 2));
  check(Memory->writeInteger(Destination + 2, 6, 2));
  check(Memory->writeInteger(Destination + 4, 0x12345678, 4));
  invoke("RtlCopyUnicodeString", {Destination, Source});
  EXPECT_EQ(integer(Destination, 2), 6u);
  EXPECT_EQ(integer(Destination + 2, 2), 6u);
  EXPECT_EQ(integer(Destination + 4, 4), 0x12345678u);
  EXPECT_EQ(integer(Destination + 8), Destination + 32);
  EXPECT_EQ(integer(Destination + 32, 2), 'A');
  EXPECT_EQ(integer(Destination + 34, 2), 0xd83du);
  EXPECT_EQ(integer(Destination + 36, 2), 0xde00u);
  EXPECT_EQ(integer(Destination + 38, 2), 'x');
  invoke("RtlCopyUnicodeString", {Destination, 0});
  EXPECT_EQ(integer(Destination, 2), 0u);
  EXPECT_EQ(integer(Destination + 32, 2), 'A');
}

TEST_F(KernelRuntime, CountedCopyTerminatesOnlyWhenSourceAndCapacityPermit) {
  // Independently checked against the Microsoft Windows 10.0.19041.6456
  // kernel export at RVA 0x2d3c70: non-null sources append a UTF-16 NUL when
  // Length + 2 <= MaximumLength; null sources only reset Length.
  const uint64_t Destination = Scratch;
  const uint64_t Source = Scratch + 0x100;
  unicode(Destination, u"xxxx");
  unicode(Source, u"AB");
  invoke("RtlCopyUnicodeString", {Destination, Source});
  EXPECT_EQ(integer(Destination, 2), 4u);
  EXPECT_EQ(integer(Destination + 2, 2), 8u);
  EXPECT_EQ(integer(Destination + 8), Destination + 32);
  EXPECT_EQ(integer(Destination + 32, 2), 'A');
  EXPECT_EQ(integer(Destination + 34, 2), 'B');
  EXPECT_EQ(integer(Destination + 36, 2), 0u);
  EXPECT_EQ(integer(Destination + 38, 2), 'x');

  unicode(Destination, u"xxxx");
  unicode(Source, u"");
  invoke("RtlCopyUnicodeString", {Destination, Source});
  EXPECT_EQ(integer(Destination, 2), 0u);
  EXPECT_EQ(integer(Destination + 32, 2), 0u);
  EXPECT_EQ(integer(Destination + 34, 2), 'x');

  unicode(Destination, u"xxxx");
  invoke("RtlCopyUnicodeString", {Destination, 0});
  EXPECT_EQ(integer(Destination, 2), 0u);
  EXPECT_EQ(integer(Destination + 32, 2), 'x');

  unicode(Destination, u"xxxx");
  unicode(Source, u"ABCD");
  check(Memory->writeInteger(Destination + 40, '!', 2));
  invoke("RtlCopyUnicodeString", {Destination, Source});
  EXPECT_EQ(integer(Destination, 2), 8u);
  EXPECT_EQ(integer(Destination + 38, 2), 'D');
  EXPECT_EQ(integer(Destination + 40, 2), '!');
}

TEST_F(KernelRuntime, CountedCopyValidatesTerminatorAllocationAndPermissions) {
  const uint64_t Destination = Scratch;
  const uint64_t Source = Scratch + 0x100;
  unicode(Destination, u"");
  unicode(Source, u"A");
  const uint64_t Pool = invoke("ExAllocatePool2", {0x40, 2, Tag});
  ASSERT_NE(Pool, 0u);
  check(Memory->writeInteger(Destination + 2, 4, 2));
  check(Memory->writeInteger(Destination + 8, Pool, 8));
  EXPECT_NE(failure("RtlCopyUnicodeString", {Destination, Source})
                .find("unallocated"),
            std::string::npos);

  check(Memory->writeInteger(Destination + 8, Scratch + 0xffe, 8));
  check(Memory->protect(Scratch + 0x1000, 0x1000, Read));
  EXPECT_NE(failure("RtlCopyUnicodeString", {Destination, Source})
                .find("write fault"),
            std::string::npos);
}

TEST_F(KernelRuntime, UnicodeComparisonUsesAllCountedUTF16Units) {
  constexpr char16_t Left[] = {u'A', 0, 0xd83d, 0xde00};
  constexpr char16_t Right[] = {u'A', 0, 0xd83d, 0xde01};
  unicode(Scratch, std::u16string_view(Left, 4));
  unicode(Scratch + 0x100, std::u16string_view(Right, 4));
  EXPECT_EQ(invoke("RtlEqualUnicodeString", {Scratch, Scratch, 0}), 1u);
  EXPECT_EQ(invoke("RtlEqualUnicodeString", {Scratch, Scratch + 0x100, 0}), 0u);
  EXPECT_LT(static_cast<int32_t>(invoke("RtlCompareUnicodeString",
                                        {Scratch, Scratch + 0x100, 0})),
            0);
  EXPECT_GT(static_cast<int32_t>(invoke("RtlCompareUnicodeString",
                                        {Scratch + 0x100, Scratch, 0})),
            0);
  EXPECT_NE(failure("RtlCompareUnicodeString", {Scratch, Scratch, 1})
                .find("case table"),
            std::string::npos);
  check(Memory->writeInteger(Scratch, 9, 2));
  EXPECT_NE(failure("RtlEqualUnicodeString", {Scratch, Scratch + 0x100, 0})
                .find("length"),
            std::string::npos);
}

TEST_F(KernelRuntime, CountedStringOperationsHonorFreedAndReadOnlyMemory) {
  unicode(Scratch, u"AB");
  const uint64_t Pointer = invoke("ExAllocatePool2", {0x40, 8, Tag});
  check(Memory->writeInteger(Scratch + 8, Pointer, 8));
  invoke("ExFreePool", {Pointer});
  EXPECT_NE(
      failure("RtlEqualUnicodeString", {Scratch, Scratch, 0}).find("freed"),
      std::string::npos);
  unicode(Scratch, u"AB");
  const uint64_t Destination = Scratch + 0x1000;
  unicode(Destination, u"CD");
  check(Memory->protect(Destination, 0x1000, Read));
  EXPECT_NE(failure("RtlCopyUnicodeString", {Destination, Scratch})
                .find("write fault"),
            std::string::npos);
}

TEST_F(KernelRuntime, DebugIntegersUseWindowsWidthsAndFormattingRules) {
  EXPECT_EQ(
      formatted(
          "%+06d %#08x %I64u %ld %hhd %hu %p %%",
          {uint64_t(-42), 0xab, UINT64_MAX, 0x100000001, 0xff, 0xffff, 0x1234}),
      "-00042 0x0000ab 18446744073709551615 1 -1 65535 0000000000001234 %");
  EXPECT_EQ(formatted("%#.0o|%.0u|%08.4X|%lld|%zu",
                      {0, 0, 0xab, 0x8000000000000000ULL, UINT64_MAX}),
            "0||    00AB|-9223372036854775808|18446744073709551615");
}

TEST_F(KernelRuntime, DebugStarsAndCountedStringsUseAbsoluteArgumentIndices) {
  ascii(Scratch + 0x100, "abcdef");
  unicode(Scratch + 0x200, u"XY");
  const uint64_t Values[] = {uint64_t(-8), 3, Scratch + 0x100, 'Q',
                             Scratch + 0x200};
  std::vector<unsigned> Reads;
  auto Value = debug("%*.*s|%-4c|%wZ", Values, true, &Reads);
  ASSERT_TRUE(static_cast<bool>(Value)) << llvm::toString(Value.takeError());
  EXPECT_EQ(Result.Messages.back(), "abc     |Q   |XY");
  EXPECT_EQ(Reads, (std::vector<unsigned>{3, 4, 5, 6, 7}));
  EXPECT_EQ(formatted("%.3s", {0}), "(nu");
}

TEST_F(KernelRuntime, StringPrecisionDoesNotReadBeyondRequestedGuestBytes) {
  const uint64_t Address = Scratch + 0x10000 - 3;
  check(Memory->write(Address, std::vector<uint8_t>{'A', 'B', 'C'}));
  EXPECT_EQ(formatted("%.*s", {3, Address}), "ABC");
  auto Value = debug("%s", {Address});
  ASSERT_FALSE(static_cast<bool>(Value));
  llvm::consumeError(Value.takeError());
}

TEST_F(KernelRuntime,
       DebugRejectsUnsupportedFormatsBudgetsAndUnknownCodePages) {
  for (const char *Format :
       {"%n", "%f", "%99999999999d", "%I64s", "%w", "%+p"}) {
    auto Value = debug(Format, {0});
    ASSERT_FALSE(static_cast<bool>(Value)) << Format;
    llvm::consumeError(Value.takeError());
  }
  std::string Many;
  std::vector<uint64_t> Arguments(33, 1);
  for (unsigned I = 0; I < 33; ++I)
    Many += "%d";
  auto Value = debug(Many, Arguments);
  ASSERT_FALSE(static_cast<bool>(Value));
  EXPECT_NE(llvm::toString(Value.takeError()).find("argument limit"),
            std::string::npos);
  unicode(Scratch + 0x100, u"\u4e2d");
  auto Wide = debug("%wZ", {Scratch + 0x100});
  ASSERT_FALSE(static_cast<bool>(Wide));
  EXPECT_NE(llvm::toString(Wide.takeError()).find("code page"),
            std::string::npos);
  EXPECT_TRUE(Result.Messages.empty());
}

TEST_F(KernelRuntime, DebugChecksFormatAndStringLifetimesBeforePublication) {
  const uint64_t Pointer = invoke("ExAllocatePool2", {0x40, 16, Tag});
  ascii(Pointer, "%u");
  invoke("ExFreePool", {Pointer});
  EXPECT_NE(failure("DbgPrint", {Pointer}).find("freed"), std::string::npos);
  auto Value = debug("prefix %s", {Pointer});
  ASSERT_FALSE(static_cast<bool>(Value));
  EXPECT_NE(llvm::toString(Value.takeError()).find("freed"), std::string::npos);
  EXPECT_TRUE(Result.Messages.empty());
}
TEST(KernelRuntimeExecution, CompiledDriverUsesDynamicPoolAndWin64Varargs) {
  auto Image = loadDriverImage(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   "driver_runtime.sys",
                               4 * 1024 * 1024);
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  bool HasHALImport = false;
  for (const auto &Import : Image->Imports)
    if (Import.Name == "KeQueryPerformanceCounter") {
      EXPECT_TRUE(llvm::StringRef(Import.Module).equals_insensitive("hal.dll"));
      HasHALImport = true;
    } else
      EXPECT_NE(Import.Name, "ExAllocatePool2");
  EXPECT_TRUE(HasHALImport);
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_runtime.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 2u);
  EXPECT_EQ(Result->Messages[0],
            "runtime -7 42 ab CD 0000000000001234 text Q %");
  EXPECT_EQ(Result->Messages[1],
            "-00042|0x0000ab|18446744073709551615|abc|Copied");
  unsigned DebugCalls = 0;
  unsigned AllocateCalls = 0;
  unsigned LegacyCalls = 0;
  unsigned ModuleQueries = 0;
  unsigned CounterCalls = 0;
  for (const auto &Call : Result->Calls) {
    if (Call.Name == "DbgPrint") {
      ++DebugCalls;
      ASSERT_EQ(Call.Arguments.size(), 8u);
      EXPECT_EQ(static_cast<int32_t>(Call.Arguments[1]), -7);
      EXPECT_EQ(Call.Arguments[5], 0x1234u);
      EXPECT_EQ(Call.Arguments[7], 'Q');
    } else if (Call.Name == "DbgPrintEx") {
      ++DebugCalls;
      ASSERT_EQ(Call.Arguments.size(), 9u);
      EXPECT_EQ(static_cast<int32_t>(Call.Arguments[3]), -42);
      EXPECT_EQ(Call.Arguments[5], UINT64_MAX);
      // Win64 passes an int in the low 32 bits; the raw slot's high bits
      // are unspecified and are intentionally retained by the trace.
      EXPECT_EQ(static_cast<uint32_t>(Call.Arguments[6]), 3u);
    } else if (Call.Name == "KeQueryPerformanceCounter") {
      ++CounterCalls;
      EXPECT_EQ(Call.Arguments.size(), 1u);
      EXPECT_EQ(Call.Result, 0u);
    } else if (Call.Name == "ExAllocatePool") {
      ++LegacyCalls;
      EXPECT_EQ(Call.Arguments.size(), 2u);
      ASSERT_TRUE(Call.Result);
      EXPECT_NE(*Call.Result, 0u);
    } else if (Call.Name == "NtQuerySystemInformation" ||
               Call.Name == "ZwQuerySystemInformation") {
      ++ModuleQueries;
      ASSERT_EQ(Call.Arguments.size(), 4u);
      EXPECT_EQ(Call.Arguments[0], 11u);
      ASSERT_TRUE(Call.Result);
      EXPECT_EQ(uint32_t(*Call.Result), Call.Arguments[2] ? 0u : 0xc0000004u);
    } else if (Call.Name == "ExAllocatePool2")
      ++AllocateCalls;
  }
  EXPECT_EQ(DebugCalls, 2u);
  EXPECT_EQ(AllocateCalls, 1u);
  EXPECT_EQ(LegacyCalls, 3u);
  EXPECT_EQ(ModuleQueries, 3u);
  EXPECT_EQ(CounterCalls, 3u);
}
} // namespace
} // namespace neverd::emulation
