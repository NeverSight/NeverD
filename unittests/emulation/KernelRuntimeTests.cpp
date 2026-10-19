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

#include "HvfTestPolicy.h"
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/FormatVariadic.h"

#include <initializer_list>
#include <map>
#include <string_view>
#include <tuple>

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

  void resetModel() {
    // Terminal CPU faults allow observation but cannot be resumed. Independent
    // fault probes need their own backend and model, including RAM ownership.
    Model.reset();
    Memory.reset();
    Result = {};
    SetUp();
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

  // Fills spare capacity with '!' so tests observe every written unit.
  void counted(uint64_t Record, uint64_t Buffer, std::u16string_view Text,
               uint16_t Maximum) {
    std::vector<uint8_t> Bytes(Maximum);
    for (size_t I = 0; I < Maximum / 2; ++I) {
      const char16_t Unit = I < Text.size() ? Text[I] : u'!';
      Bytes[I * 2] = static_cast<uint8_t>(Unit);
      Bytes[I * 2 + 1] = static_cast<uint8_t>(Unit >> 8);
    }
    check(Memory->write(Buffer, Bytes));
    check(Memory->writeInteger(Record, Text.size() * 2, 2));
    check(Memory->writeInteger(Record + 2, Maximum, 2));
    check(Memory->writeInteger(Record + 8, Buffer, 8));
  }

  void wide(uint64_t Address, std::u16string_view Text) {
    std::vector<uint8_t> Bytes((Text.size() + 1) * 2);
    for (size_t I = 0; I < Text.size(); ++I) {
      Bytes[I * 2] = static_cast<uint8_t>(Text[I]);
      Bytes[I * 2 + 1] = static_cast<uint8_t>(Text[I] >> 8);
    }
    check(Memory->write(Address, Bytes));
  }

  // A counted 8-bit record whose spare capacity holds '!'.
  void ansi(uint64_t Record, uint64_t Buffer, std::string_view Text,
            uint16_t Maximum) {
    std::vector<uint8_t> Bytes(Maximum, '!');
    std::copy(Text.begin(), Text.end(), Bytes.begin());
    if (!Bytes.empty())
      check(Memory->write(Buffer, Bytes));
    check(Memory->writeInteger(Record, Text.size(), 2));
    check(Memory->writeInteger(Record + 2, Maximum, 2));
    check(Memory->writeInteger(Record + 8, Buffer, 8));
  }

  std::string bytesText(uint64_t Address, size_t Count) {
    std::string Text;
    for (size_t I = 0; I < Count; ++I)
      Text.push_back(static_cast<char>(integer(Address + I, 1)));
    return Text;
  }

  std::u16string units(uint64_t Address, size_t Count) {
    std::u16string Text;
    for (size_t I = 0; I < Count; ++I)
      Text.push_back(static_cast<char16_t>(integer(Address + I * 2, 2)));
    return Text;
  }

  // RtlUnicodeStringToInteger over Text; the output starts non-zero so every
  // path must store it.
  std::pair<uint64_t, uint32_t> parse(std::u16string_view Text, uint64_t Base) {
    unicode(Scratch + 0x400, Text);
    check(Memory->writeInteger(Scratch + 0x800, 0xa5a5a5a5, 4));
    const uint64_t Status = invoke("RtlUnicodeStringToInteger",
                                   {Scratch + 0x400, Base, Scratch + 0x800});
    return {Status, static_cast<uint32_t>(integer(Scratch + 0x800, 4))};
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

TEST_F(KernelRuntime, PcLookupRequiresAnOwnedModuleInventory) {
  check(Memory->writeInteger(Scratch, UINT64_MAX, 8));
  EXPECT_NE(
      failure("RtlPcToFileHeader", {0x180001000, Scratch}).find("inventory"),
      std::string::npos);
  EXPECT_EQ(integer(Scratch), UINT64_MAX);
}

TEST_F(KernelRuntime, ProcessorNumberWritesFourBytesAtEverySupportedIRQL) {
  Model->enterExecution(0x100);
  for (uint8_t IRQL : {0, 1, 2, 15}) {
    const auto Previous = invoke("KfRaiseIrql", {IRQL});
    for (uint64_t Offset : {0u, 1u, 4u}) {
      check(Memory->writeInteger(Scratch, UINT64_MAX, 8));
      check(Memory->writeInteger(Scratch + 8, UINT64_MAX, 8));
      EXPECT_EQ(invoke("KeGetCurrentProcessorNumberEx", {Scratch + Offset}),
                0u);
      for (unsigned I = 0; I < 16; ++I)
        EXPECT_EQ(integer(Scratch + I, 1),
                  I >= Offset && I < Offset + 4 ? 0u : 0xffu);
    }
    EXPECT_EQ(invoke("KeGetCurrentProcessorNumberEx", {0}), 0u);
    EXPECT_EQ(invoke("KeGetCurrentIrql", {}), IRQL);
    invoke("KeLowerIrql", {Previous});
  }
  EXPECT_FALSE(failure("KeGetCurrentProcessorNumberEx", {}).empty());
}

TEST_F(KernelRuntime, ProcessorNumberRejectsInvalidOutputBeforeAnyWrite) {
  check(Memory->writeInteger(Scratch + 0xfffc, 0xaabbccdd, 4));
  EXPECT_FALSE(
      failure("KeGetCurrentProcessorNumberEx", {Scratch + 0xfffe}).empty());
  EXPECT_EQ(integer(Scratch + 0xfffc, 4), 0xaabbccddu);
  check(Memory->protect(Scratch, 0x1000, Read));
  EXPECT_FALSE(failure("KeGetCurrentProcessorNumberEx", {Scratch}).empty());
  EXPECT_FALSE(
      failure("KeGetCurrentProcessorNumberEx", {UINT64_MAX - 1}).empty());
  EXPECT_FALSE(failure("KeGetCurrentProcessorNumberEx", {Model->driverObject()})
                   .empty());
  const auto Pool = invoke("ExAllocatePoolWithTag", {0, 16, Tag});
  invoke("ExFreePoolWithTag", {Pool, Tag});
  EXPECT_NE(failure("KeGetCurrentProcessorNumberEx", {Pool}).find("freed"),
            std::string::npos);
}

TEST_F(KernelRuntime, DpcLevelRaiseUsesTheSharedExecutionRestoreStack) {
  EXPECT_NE(failure("KeRaiseIrqlToDpcLevel", {}).find("execution"),
            std::string::npos);
  Model->enterExecution(0x100);
  for (uint8_t Starting : {0, 1, 2}) {
    const auto Original = invoke("KfRaiseIrql", {Starting});
    EXPECT_EQ(invoke("KeRaiseIrqlToDpcLevel", {}), Starting);
    EXPECT_EQ(invoke("KeGetCurrentIrql", {}), 2u);
    EXPECT_EQ(invoke("KeRaiseIrqlToDpcLevel", {}), 2u);
    invoke("KeLowerIrql", {2});
    invoke("KeLowerIrql", {Starting});
    invoke("KeLowerIrql", {Original});
    EXPECT_EQ(invoke("KeGetCurrentIrql", {}), 0u);
  }
  const auto Previous = invoke("KfRaiseIrql", {3});
  EXPECT_FALSE(failure("KeRaiseIrqlToDpcLevel", {}).empty());
  EXPECT_EQ(invoke("KeGetCurrentIrql", {}), 3u);
  invoke("KeLowerIrql", {Previous});
  EXPECT_FALSE(failure("KeRaiseIrqlToDpcLevel", {2}).empty());
  invoke("KeRaiseIrqlToDpcLevel", {});
  Model->enterExecution(0x200);
  EXPECT_FALSE(failure("KeLowerIrql", {0}).empty());
  Model->enterExecution(0x100);
  invoke("KeLowerIrql", {0});
  check(Model->validateExecutionReturn(0x100, 0));
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

TEST_F(KernelRuntime, AnsiInitializationCountsTerminatorAndClearsPadding) {
  // Independently checked against the Windows 10.0.19041.6456 kernel export
  // at RVA 0x24fb00, which both names share.
  ascii(Scratch + 0x100, "driver");
  for (const char *Name : {"RtlInitAnsiString", "RtlInitString"}) {
    SCOPED_TRACE(Name);
    check(Memory->write(Scratch, std::vector<uint8_t>(16, 0xff)));
    EXPECT_EQ(invoke(Name, {Scratch, Scratch + 0x100}), 0u);
    EXPECT_EQ(integer(Scratch, 2), 6u);
    EXPECT_EQ(integer(Scratch + 2, 2), 7u);
    EXPECT_EQ(integer(Scratch + 4, 4), 0u);
    EXPECT_EQ(integer(Scratch + 8), Scratch + 0x100);
  }
  ascii(Scratch + 0x200, "");
  invoke("RtlInitAnsiString", {Scratch, Scratch + 0x200});
  EXPECT_EQ(integer(Scratch, 2), 0u);
  EXPECT_EQ(integer(Scratch + 2, 2), 1u);
  EXPECT_EQ(integer(Scratch + 8), Scratch + 0x200);
  check(Memory->write(Scratch, std::vector<uint8_t>(16, 0xff)));
  invoke("RtlInitAnsiString", {Scratch, 0});
  EXPECT_EQ(integer(Scratch), 0u);
  EXPECT_EQ(integer(Scratch + 8), 0u);
}

TEST_F(KernelRuntime, AnsiInitializationTruncatesLongSourcesLikeTheKernel) {
  check(Memory->map(Scratch + 0x10000, 0x10000, Read | Write));
  // 0xFFFD and 0xFFFE bytes are exact; longer sources report 0xFFFE.
  for (uint64_t Size : {0xfffdull, 0xfffeull, 0xffffull, 0x10010ull}) {
    SCOPED_TRACE(Size);
    std::vector<uint8_t> Text(Size, 'a');
    Text.push_back(0);
    check(Memory->write(Scratch + 0x100, Text));
    invoke("RtlInitAnsiString", {Scratch, Scratch + 0x100});
    const uint64_t Expected = std::min<uint64_t>(Size, 0xfffe);
    EXPECT_EQ(integer(Scratch, 2), Expected);
    EXPECT_EQ(integer(Scratch + 2, 2), Expected + 1);
  }
}

TEST_F(KernelRuntime, CountedAppendHonorsCapacityAndTerminatorRules) {
  // RtlAppendUnicodeStringToString at RVA 0x3480c0: the terminator needs two
  // spare bytes, and failure or an empty source leaves the destination as is.
  const uint64_t Destination = Scratch;
  const uint64_t Buffer = Scratch + 0x100;
  const uint64_t Source = Scratch + 0x200;
  unicode(Source, u"cd");
  counted(Destination, Buffer, u"ab", 12);
  EXPECT_EQ(invoke("RtlAppendUnicodeStringToString", {Destination, Source}),
            0u);
  EXPECT_EQ(integer(Destination, 2), 8u);
  EXPECT_EQ(units(Buffer, 6), std::u16string(u"abcd\0!", 6));

  counted(Destination, Buffer, u"ab", 8);
  check(Memory->writeInteger(Buffer + 8, u'?', 2));
  EXPECT_EQ(invoke("RtlAppendUnicodeStringToString", {Destination, Source}),
            0u);
  EXPECT_EQ(integer(Destination, 2), 8u);
  EXPECT_EQ(units(Buffer, 5), u"abcd?");

  counted(Destination, Buffer, u"ab", 10);
  invoke("RtlAppendUnicodeStringToString", {Destination, Source});
  EXPECT_EQ(units(Buffer, 5), std::u16string(u"abcd\0", 5));

  counted(Destination, Buffer, u"ab", 6);
  EXPECT_EQ(invoke("RtlAppendUnicodeStringToString", {Destination, Source}),
            0xc0000023u);
  EXPECT_EQ(integer(Destination, 2), 4u);
  EXPECT_EQ(units(Buffer, 3), u"ab!");

  unicode(Source, u"");
  counted(Destination, Buffer, u"ab", 12);
  EXPECT_EQ(invoke("RtlAppendUnicodeStringToString", {Destination, Source}),
            0u);
  EXPECT_EQ(integer(Destination, 2), 4u);
  EXPECT_EQ(units(Buffer, 3), u"ab!");
}

TEST_F(KernelRuntime, NullTerminatedAppendTreatsNullAsEmptyAndStillTerminates) {
  // RtlAppendUnicodeToString at RVA 0x32ead0 returns early only for a null
  // source; an empty string still terminates when two spare bytes remain.
  const uint64_t Destination = Scratch;
  const uint64_t Buffer = Scratch + 0x100;
  const uint64_t Source = Scratch + 0x200;
  wide(Source, u"cd");
  counted(Destination, Buffer, u"ab", 12);
  EXPECT_EQ(invoke("RtlAppendUnicodeToString", {Destination, Source}), 0u);
  EXPECT_EQ(integer(Destination, 2), 8u);
  EXPECT_EQ(units(Buffer, 6), std::u16string(u"abcd\0!", 6));

  counted(Destination, Buffer, u"ab", 12);
  EXPECT_EQ(invoke("RtlAppendUnicodeToString", {Destination, 0}), 0u);
  EXPECT_EQ(integer(Destination, 2), 4u);
  EXPECT_EQ(units(Buffer, 3), u"ab!");

  wide(Source, u"");
  EXPECT_EQ(invoke("RtlAppendUnicodeToString", {Destination, Source}), 0u);
  EXPECT_EQ(integer(Destination, 2), 4u);
  EXPECT_EQ(units(Buffer, 3), std::u16string(u"ab\0", 3));

  wide(Source, u"cdef");
  counted(Destination, Buffer, u"ab", 10);
  EXPECT_EQ(invoke("RtlAppendUnicodeToString", {Destination, Source}),
            0xc0000023u);
  EXPECT_EQ(integer(Destination, 2), 4u);
  EXPECT_EQ(units(Buffer, 3), u"ab!");
}

TEST_F(KernelRuntime, NullTerminatedAppendRejectsSourcesLongerThanInitEx) {
  // RtlInitUnicodeStringEx at RVA 0x32eb60 accepts 0x7FFE units. One more
  // fails the append even though the destination capacity would hold it.
  check(Memory->map(Scratch + 0x10000, 0x30000, Read | Write));
  const uint64_t Destination = Scratch;
  const uint64_t Buffer = Scratch + 0x10000;
  const uint64_t Source = Scratch + 0x20000;
  for (size_t Count : {size_t(0x7ffe), size_t(0x7fff)}) {
    SCOPED_TRACE(Count);
    wide(Source, std::u16string(Count, u'x'));
    counted(Destination, Buffer, u"", 0xfffe);
    const uint64_t Status =
        invoke("RtlAppendUnicodeToString", {Destination, Source});
    if (Count == 0x7ffe) {
      EXPECT_EQ(Status, 0u);
      EXPECT_EQ(integer(Destination, 2), 0xfffcu);
      EXPECT_EQ(integer(Buffer + 0xfffc, 2), 0u);
    } else {
      EXPECT_EQ(Status, 0xc0000023u);
      EXPECT_EQ(integer(Destination, 2), 0u);
    }
  }
}

TEST_F(KernelRuntime, PrefixFoldsCaseOnlyWhenCountedUnitsDiffer) {
  // RVA 0x5edbe0 consults the NLS upcase table only for unequal units.
  unicode(Scratch, u"Dev");
  unicode(Scratch + 0x100, u"Device");
  unicode(Scratch + 0x200, u"dev");
  unicode(Scratch + 0x300, u"");
  EXPECT_EQ(invoke("RtlPrefixUnicodeString", {Scratch, Scratch + 0x100, 0}),
            1u);
  EXPECT_EQ(invoke("RtlPrefixUnicodeString", {Scratch + 0x100, Scratch, 0}),
            0u);
  EXPECT_EQ(
      invoke("RtlPrefixUnicodeString", {Scratch + 0x200, Scratch + 0x100, 0}),
      0u);
  EXPECT_EQ(invoke("RtlPrefixUnicodeString", {Scratch + 0x300, Scratch, 0}),
            1u);
  // BOOLEAN arguments use only their low byte.
  EXPECT_EQ(invoke("RtlPrefixUnicodeString",
                   {Scratch + 0x200, Scratch + 0x100, 0x100}),
            0u);
  EXPECT_EQ(invoke("RtlPrefixUnicodeString", {Scratch, Scratch + 0x100, 1}),
            1u);
  EXPECT_EQ(invoke("RtlPrefixUnicodeString", {Scratch + 0x100, Scratch, 1}),
            0u);
  EXPECT_EQ(
      invoke("RtlPrefixUnicodeString", {Scratch + 0x200, Scratch + 0x100, 1}),
      1u);
}

TEST_F(KernelRuntime, IntegerFormattingUsesUnsignedRadicesAndTerminator) {
  // RtlIntegerToUnicodeString at RVA 0x6f6280 formats through
  // RtlIntegerToChar (0x6f7000) and RtlAnsiStringToUnicodeString (0x6f6840).
  const uint64_t Record = Scratch;
  const uint64_t Buffer = Scratch + 0x100;
  auto Format = [&](uint64_t Value, uint64_t Base) {
    counted(Record, Buffer, u"", 66);
    EXPECT_EQ(invoke("RtlIntegerToUnicodeString", {Value, Base, Record}), 0u);
    const uint64_t Length = integer(Record, 2);
    EXPECT_EQ(integer(Buffer + Length, 2), 0u);
    return units(Buffer, Length / 2);
  };
  EXPECT_EQ(Format(1234, 10), u"1234");
  EXPECT_EQ(Format(1234, 0), u"1234");
  EXPECT_EQ(Format(0xbeef, 16), u"BEEF");
  EXPECT_EQ(Format(8, 8), u"10");
  EXPECT_EQ(Format(5, 2), u"101");
  EXPECT_EQ(Format(0, 10), u"0");
  EXPECT_EQ(Format(0xffffffff, 10), u"4294967295");
  EXPECT_EQ(Format(0xffffffff, 2), std::u16string(32, u'1'));
  // ULONG arguments occupy the low 32 bits of their Win64 slots.
  EXPECT_EQ(Format(0xdeadbeef00000007ULL, 0x100000010ULL), u"7");
}

TEST_F(KernelRuntime,
       IntegerFormattingRejectsRadicesAndStoresLengthOnOverflow) {
  const uint64_t Record = Scratch;
  const uint64_t Buffer = Scratch + 0x100;
  counted(Record, Buffer, u"xy", 66);
  EXPECT_EQ(invoke("RtlIntegerToUnicodeString", {1, 3, Record}), 0xc000000du);
  EXPECT_EQ(integer(Record, 2), 4u);
  // The terminator must fit too. Length is already stored on overflow.
  counted(Record, Buffer, u"xy", 8);
  EXPECT_EQ(invoke("RtlIntegerToUnicodeString", {1234, 10, Record}),
            0x80000005u);
  EXPECT_EQ(integer(Record, 2), 8u);
  EXPECT_EQ(units(Buffer, 4), u"xy!!");
  counted(Record, Buffer, u"xy", 10);
  EXPECT_EQ(invoke("RtlIntegerToUnicodeString", {1234, 10, Record}), 0u);
  EXPECT_EQ(units(Buffer, 5), std::u16string(u"1234\0", 5));
}

TEST_F(KernelRuntime, Int64FormattingPreservesTheFullUnsignedValue) {
  for (uint64_t Offset : {0u, 1u}) {
    const uint64_t Record = Scratch + Offset;
    const uint64_t Buffer = Scratch + 0x200 + Offset;
    auto Format = [&](uint64_t Value, uint64_t Base) {
      counted(Record, Buffer, u"", 134);
      EXPECT_EQ(invoke("RtlInt64ToUnicodeString", {Value, Base, Record}), 0u);
      const uint64_t Length = integer(Record, 2);
      EXPECT_EQ(integer(Record + 2, 2), 134u);
      EXPECT_EQ(integer(Record + 8), Buffer);
      EXPECT_EQ(integer(Buffer + Length, 2), 0u);
      EXPECT_EQ(integer(Buffer + Length + 2, 2), uint64_t(u'!'));
      return units(Buffer, Length / 2);
    };
    EXPECT_EQ(Format(0, 0), u"0");
    EXPECT_EQ(Format(0x100000000ULL, 10), u"4294967296");
    EXPECT_EQ(Format(UINT64_MAX, 0), u"18446744073709551615");
    EXPECT_EQ(Format(UINT64_MAX, 10), u"18446744073709551615");
    EXPECT_EQ(Format(UINT64_MAX, 2), std::u16string(64, u'1'));
    EXPECT_EQ(Format(UINT64_MAX, 8), u"1777777777777777777777");
    EXPECT_EQ(Format(UINT64_MAX, 16), u"FFFFFFFFFFFFFFFF");
    EXPECT_EQ(Format(0x123456789abcdef0ULL, 0x100000010ULL),
              u"123456789ABCDEF0");
    EXPECT_EQ(Format(0x8000000000000000ULL, 10), u"9223372036854775808");
  }
}

TEST_F(KernelRuntime, Int64FormattingPublishesLengthBeforeOverflowOnly) {
  for (uint16_t Capacity : {0, 1, 127, 128, 129, 130, 131}) {
    counted(Scratch, Scratch + 0x200, u"xy", 134);
    check(Memory->writeInteger(Scratch + 2, Capacity, 2));
    EXPECT_EQ(invoke("RtlInt64ToUnicodeString", {UINT64_MAX, 2, Scratch}),
              Capacity < 130 ? 0x80000005u : 0u);
    EXPECT_EQ(integer(Scratch, 2), 128u);
    EXPECT_EQ(integer(Scratch + 2, 2), Capacity);
    EXPECT_EQ(integer(Scratch + 8), Scratch + 0x200);
    if (Capacity < 130)
      EXPECT_EQ(units(Scratch + 0x200, 67), u"xy" + std::u16string(65, u'!'));
    else {
      EXPECT_EQ(units(Scratch + 0x200, 64), std::u16string(64, u'1'));
      EXPECT_EQ(integer(Scratch + 0x280, 2), 0u);
      EXPECT_EQ(integer(Scratch + 0x282, 2), uint64_t(u'!'));
    }
  }
  counted(Scratch, Scratch + 0x200, u"xy", 134);
  EXPECT_EQ(invoke("RtlInt64ToUnicodeString", {UINT64_MAX, 3, Scratch}),
            0xc000000du);
  EXPECT_EQ(integer(Scratch, 2), 4u);
  EXPECT_EQ(units(Scratch + 0x200, 2), u"xy");
  EXPECT_EQ(invoke("RtlInt64ToUnicodeString", {UINT64_MAX, 3, 0}), 0xc000000du);
}

TEST_F(KernelRuntime, IntegerFormattersReloadAliasedTerminatorPointers) {
  // Original target exports reload Buffer after writing the digits. Formatting
  // zero over the pointer changes its low word to 0x0030 before termination.
  for (const char *Name :
       {"RtlIntegerToUnicodeString", "RtlInt64ToUnicodeString"}) {
    const uint64_t Record = Scratch + 0x1000;
    check(Memory->writeInteger(Scratch + 0x32, 0xffff, 2));
    check(Memory->writeInteger(Record, 0x7777, 2));
    check(Memory->writeInteger(Record + 2, 32, 2));
    check(Memory->writeInteger(Record + 8, Record + 8, 8));
    EXPECT_EQ(invoke(Name, {0, 10, Record}), 0u);
    EXPECT_EQ(integer(Record, 2), 2u);
    EXPECT_EQ(integer(Record + 8), Scratch + 0x30);
    EXPECT_EQ(integer(Scratch + 0x32, 2), 0u);
    EXPECT_EQ(integer(Record + 10, 2), 0x6000u);
  }
}

TEST_F(KernelRuntime, Int64FormattingReportsInvalidStorageAndIRQL) {
  EXPECT_FALSE(failure("RtlInt64ToUnicodeString", {1, 10, 0}).empty());
  EXPECT_FALSE(failure("RtlInt64ToUnicodeString", {1, 10}).empty());
  counted(Scratch, Scratch + 0x200, u"xy", 134);
  Model->enterExecution(0x100);
  const auto Previous = invoke("KfRaiseIrql", {1});
  EXPECT_NE(failure("RtlInt64ToUnicodeString", {1, 10, Scratch})
                .find("PASSIVE_LEVEL"),
            std::string::npos);
  EXPECT_EQ(integer(Scratch, 2), 4u);
  invoke("KeLowerIrql", {Previous});
  check(Memory->protect(Scratch, 0x1000, Read));
  EXPECT_FALSE(failure("RtlInt64ToUnicodeString", {1, 10, Scratch}).empty());
  EXPECT_EQ(integer(Scratch, 2), 4u);
}

TEST_F(KernelRuntime, Int64FormattingKeepsDigitsWhenTheTerminatorFaults) {
  counted(Scratch, Scratch + 0xfffc, u"", 4);
  check(Memory->writeInteger(Scratch + 2, 6, 2));
  EXPECT_FALSE(failure("RtlInt64ToUnicodeString", {12, 10, Scratch}).empty());
  EXPECT_EQ(integer(Scratch, 2), 4u);
  EXPECT_EQ(units(Scratch + 0xfffc, 2), u"12");
}

TEST_F(KernelRuntime, IntegerParsingFollowsTheDocumentedExamples) {
  // Microsoft Learn's RtlUnicodeStringToInteger table, independently checked
  // against the Windows 10.0.19041.6456 export at RVA 0x6638e0.
  using Result = std::pair<uint64_t, uint32_t>;
  EXPECT_EQ(parse(u"123", 10), Result(0, 123));
  EXPECT_EQ(parse(u"-345", 10), Result(0, uint32_t(-345)));
  EXPECT_EQ(parse(u"xyz", 10), Result(0, 0));
  EXPECT_EQ(parse(u"+678abc", 10), Result(0, 678));
  EXPECT_EQ(parse(u"+678abc", 16), Result(0, 6785724));
  EXPECT_EQ(parse(u"007", 10), Result(0, 7));
  EXPECT_EQ(parse(u"789", 8), Result(0, 7));
  EXPECT_EQ(parse(u"FGH", 16), Result(0, 15));
  EXPECT_EQ(parse(u" ", 10), Result(0, 0));
}

TEST_F(KernelRuntime, IntegerParsingMatchesKernelPrefixWhitespaceAndWrapRules) {
  using Result = std::pair<uint64_t, uint32_t>;
  // Only a zero radix recognizes the lowercase 0x, 0o and 0b prefixes.
  EXPECT_EQ(parse(u"0x1F", 0), Result(0, 31));
  EXPECT_EQ(parse(u"0o17", 0), Result(0, 15));
  EXPECT_EQ(parse(u"0b101", 0), Result(0, 5));
  EXPECT_EQ(parse(u"0X1F", 0), Result(0, 0));
  EXPECT_EQ(parse(u"012", 0), Result(0, 12));
  EXPECT_EQ(parse(u"0", 0), Result(0, 0));
  EXPECT_EQ(parse(u"0x1F", 16), Result(0, 0));
  // Every unit up to U+0020 is whitespace; the sign precedes the prefix.
  EXPECT_EQ(parse(u"\t\r\n -0x10", 0), Result(0, uint32_t(-16)));
  EXPECT_EQ(parse(u"- 5", 10), Result(0, 0));
  // Accumulation wraps in 32 bits, and a zero unit ends the digits.
  EXPECT_EQ(parse(u"4294967297", 10), Result(0, 1));
  EXPECT_EQ(parse(std::u16string_view(u"12\0"
                                      u"3",
                                      4),
                  10),
            Result(0, 12));
  // ULONG radices occupy the low 32 bits of their Win64 slots.
  EXPECT_EQ(parse(u"ff", 0x100000010ULL), Result(0, 255));
}

TEST_F(KernelRuntime, IntegerParsingStoresZeroForInvalidLengthsAndRadices) {
  using Result = std::pair<uint64_t, uint32_t>;
  EXPECT_EQ(parse(u"", 10), Result(0xc000000d, 0));
  EXPECT_EQ(parse(u"12", 7), Result(0xc000000d, 0));
  unicode(Scratch + 0x400, u"12");
  check(Memory->writeInteger(Scratch + 0x400, 3, 2));
  check(Memory->writeInteger(Scratch + 0x800, 0xa5a5a5a5, 4));
  EXPECT_EQ(invoke("RtlUnicodeStringToInteger",
                   {Scratch + 0x400, 10, Scratch + 0x800}),
            0xc000000du);
  EXPECT_EQ(integer(Scratch + 0x800, 4), 0u);
}

TEST_F(KernelRuntime, CountedStringHelpersEnforceArityAndIRQLContracts) {
  unicode(Scratch, u"12");
  unicode(Scratch + 0x300, u"");
  ascii(Scratch + 0x100, "a");
  EXPECT_NE(failure("RtlPrefixUnicodeString", {Scratch, Scratch})
                .find("argument count"),
            std::string::npos);
  Model->enterExecution(0x100);
  const auto Previous = invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("RtlUnicodeStringToInteger", {Scratch, 10, Scratch + 0x800})
                .find("PASSIVE_LEVEL"),
            std::string::npos);
  EXPECT_NE(failure("RtlIntegerToUnicodeString", {1, 10, Scratch})
                .find("PASSIVE_LEVEL"),
            std::string::npos);
  EXPECT_NE(failure("RtlPrefixUnicodeString", {Scratch, Scratch, 0})
                .find("PASSIVE_LEVEL"),
            std::string::npos);
  // DISPATCH_LEVEL still permits initialization and resident appends.
  EXPECT_EQ(invoke("RtlInitAnsiString", {Scratch + 0x200, Scratch + 0x100}),
            0u);
  EXPECT_EQ(
      invoke("RtlAppendUnicodeStringToString", {Scratch, Scratch + 0x300}), 0u);
  invoke("KeLowerIrql", {Previous});
  const auto Lowered = invoke("KfRaiseIrql", {3});
  EXPECT_NE(failure("RtlInitAnsiString", {Scratch + 0x200, Scratch + 0x100})
                .find("IRQL"),
            std::string::npos);
  EXPECT_EQ(
      invoke("RtlAppendUnicodeStringToString", {Scratch, Scratch + 0x300}), 0u);
  invoke("KeLowerIrql", {Lowered});
}

TEST_F(KernelRuntime, CurrentThreadAndProcessQueriesMatchTheirKernelAliases) {
  // The 19041.6456 kernel exports PsGetCurrentThread and KeGetCurrentThread as
  // one routine (RVA 0x2c9210), and PsGetCurrentProcess as IoGetCurrentProcess
  // (0x2bbc90).
  EXPECT_NE(failure("PsGetCurrentThread", {}).find("active execution"),
            std::string::npos);
  EXPECT_NE(failure("PsGetCurrentProcess", {}).find("thread"),
            std::string::npos);
  Model->enterExecution(profile::StackBase, 0x42);
  const uint64_t Thread = invoke("PsGetCurrentThread", {});
  ASSERT_NE(Thread, 0u);
  EXPECT_EQ(invoke("KeGetCurrentThread", {}), Thread);
  auto Inline = Model->currentThreadObject();
  ASSERT_TRUE(bool(Inline)) << llvm::toString(Inline.takeError());
  EXPECT_EQ(*Inline, Thread);
  const uint64_t Process = invoke("IoGetCurrentProcess", {});
  ASSERT_NE(Process, 0u);
  EXPECT_EQ(invoke("PsGetCurrentProcess", {}), Process);
  EXPECT_EQ(invoke("PsGetProcessId", {Process}), 4u);
  // Both identities are available at any IRQL.
  const auto Previous = invoke("KfRaiseIrql", {15});
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), Thread);
  EXPECT_EQ(invoke("PsGetCurrentProcess", {}), Process);
  invoke("KeLowerIrql", {Previous});
  // Each logical thread keeps its own identity.
  Model->enterExecution(profile::StackBase, 0x43);
  const uint64_t Other = invoke("PsGetCurrentThread", {});
  EXPECT_NE(Other, Thread);
  EXPECT_EQ(invoke("PsGetCurrentProcess", {}), Process);
  Model->enterExecution(profile::StackBase, 0x42);
  EXPECT_EQ(invoke("PsGetCurrentThread", {}), Thread);
}

TEST_F(KernelRuntime, ThreadClientIDsBelongToTheirThreadObjects) {
  EXPECT_NE(failure("PsGetCurrentThreadId", {}).find("active execution"),
            std::string::npos);
  Model->enterExecution(profile::StackBase, 0x42);
  const uint64_t Thread = invoke("PsGetCurrentThread", {});
  // IDs are assigned on first observation from the profile base in steps of 4.
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), 0x2000u);
  EXPECT_EQ(invoke("PsGetThreadId", {Thread}), 0x2000u);
  Model->enterExecution(profile::StackBase, 0x43);
  const uint64_t Other = invoke("PsGetCurrentThread", {});
  EXPECT_EQ(invoke("PsGetThreadId", {Other}), 0x2004u);
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), 0x2004u);
  EXPECT_EQ(invoke("PsGetThreadId", {Thread}), 0x2000u);
  EXPECT_NE(failure("PsGetThreadId", {Scratch}).find("known thread object"),
            std::string::npos);
  // The current ID is available at any IRQL, a lookup only to DISPATCH_LEVEL.
  const auto Previous = invoke("KfRaiseIrql", {3});
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), 0x2004u);
  EXPECT_NE(failure("PsGetThreadId", {Thread}).find("IRQL"), std::string::npos);
  invoke("KeLowerIrql", {Previous});
}

TEST_F(KernelRuntime, ThreadClientIDsSkipModeledProcessIDs) {
  // Thread and process IDs share one namespace, so the requesting process's
  // ID is never issued to a thread.
  Model->enterExecution(profile::StackBase);
  check(Model->setUserRequestContext(true, 0x2000));
  EXPECT_EQ(invoke("PsGetCurrentProcessId", {}), 0x2000u);
  EXPECT_EQ(invoke("PsGetCurrentThreadId", {}), 0x2004u);
  check(Model->setUserRequestContext(false));
}

TEST_F(KernelRuntime, AnsiToUnicodeConversionAllocatesTaggedPagedPool) {
  // RtlAnsiStringToUnicodeString (RVA 0x6f6840) allocates through
  // ExpAllocateStringRoutine; RtlFreeUnicodeString and RtlFreeAnsiString share
  // RVA 0x602cb0.
  const uint64_t Source = Scratch;
  const uint64_t Destination = Scratch + 0x40;
  ansi(Source, Scratch + 0x100, "Ab\\9", 8);
  check(Memory->write(Destination, std::vector<uint8_t>(16, 0xff)));
  EXPECT_EQ(invoke("RtlAnsiStringToUnicodeString", {Destination, Source, 1}),
            0u);
  EXPECT_EQ(integer(Destination, 2), 8u);
  EXPECT_EQ(integer(Destination + 2, 2), 10u);
  const uint64_t Buffer = integer(Destination + 8);
  ASSERT_NE(Buffer, 0u);
  EXPECT_EQ(units(Buffer, 5), std::u16string(u"Ab\\9\0", 5));
  // The buffer is paged pool tagged 'Strg'.
  EXPECT_NE(failure("ExFreePoolWithTag", {Buffer, Tag}).find("tag"),
            std::string::npos);
  Model->enterExecution(0x100);
  const auto Previous = invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("ExFreePool", {Buffer}).find("APC_LEVEL"),
            std::string::npos);
  invoke("KeLowerIrql", {Previous});
  invoke("RtlFreeUnicodeString", {Destination});
  EXPECT_EQ(integer(Destination), 0u);
  EXPECT_EQ(integer(Destination + 8), 0u);
  auto Freed = Model->validateGuestAccess(Buffer, 1, false);
  ASSERT_TRUE(bool(Freed));
  EXPECT_NE(llvm::toString(std::move(Freed)).find("freed"), std::string::npos);
  // The exact tag frees a second conversion.
  invoke("RtlAnsiStringToUnicodeString", {Destination, Source, 1});
  invoke("ExFreePoolWithTag", {integer(Destination + 8), 0x67727453});
  // A null buffer leaves the record untouched under both names.
  check(Memory->writeInteger(Destination, 0x12345678, 4));
  check(Memory->writeInteger(Destination + 8, 0, 8));
  invoke("RtlFreeAnsiString", {Destination});
  invoke("RtlFreeUnicodeString", {Destination});
  EXPECT_EQ(integer(Destination, 4), 0x12345678u);
}

TEST_F(KernelRuntime, AnsiToUnicodeConversionNeedsRoomForTheTerminator) {
  const uint64_t Source = Scratch;
  const uint64_t Destination = Scratch + 0x40;
  const uint64_t Buffer = Scratch + 0x200;
  ansi(Source, Scratch + 0x100, "abc", 4);
  counted(Destination, Buffer, u"", 8);
  // BOOLEAN arguments use only their low byte.
  EXPECT_EQ(
      invoke("RtlAnsiStringToUnicodeString", {Destination, Source, 0x100}), 0u);
  EXPECT_EQ(integer(Destination, 2), 6u);
  EXPECT_EQ(integer(Destination + 8), Buffer);
  EXPECT_EQ(units(Buffer, 4), std::u16string(u"abc\0", 4));
  // Length is stored before the capacity check fails.
  counted(Destination, Buffer, u"", 6);
  EXPECT_EQ(invoke("RtlAnsiStringToUnicodeString", {Destination, Source, 0}),
            0x80000005u);
  EXPECT_EQ(integer(Destination, 2), 6u);
  EXPECT_EQ(units(Buffer, 3), u"!!!");
  // A source longer than a counted UTF-16 string fails before any effect.
  check(Memory->map(Scratch + 0x10000, 0x10000, Read | Write));
  ansi(Source, Scratch + 0x10000, std::string(0x7fff, 'x'), 0x7fff);
  counted(Destination, Buffer, u"", 8);
  EXPECT_EQ(invoke("RtlAnsiStringToUnicodeString", {Destination, Source, 1}),
            0xc00000f0u);
  EXPECT_EQ(integer(Destination, 2), 0u);
  EXPECT_EQ(integer(Destination + 8), Buffer);
  ansi(Source, Scratch + 0x100, "\xe9", 1);
  EXPECT_NE(failure("RtlAnsiStringToUnicodeString", {Destination, Source, 0})
                .find("code page"),
            std::string::npos);
  EXPECT_EQ(integer(Destination, 2), 0u);
}

TEST_F(KernelRuntime, UnicodeToAnsiConversionTruncatesIntoShortBuffers) {
  // RtlUnicodeStringToAnsiString (RVA 0x5edb00) converts what fits.
  const uint64_t Source = Scratch;
  const uint64_t Destination = Scratch + 0x100;
  const uint64_t Buffer = Scratch + 0x200;
  unicode(Source, u"Hello");
  EXPECT_EQ(invoke("RtlUnicodeStringToAnsiString", {Destination, Source, 1}),
            0u);
  EXPECT_EQ(integer(Destination, 2), 5u);
  EXPECT_EQ(integer(Destination + 2, 2), 6u);
  const uint64_t Allocated = integer(Destination + 8);
  ASSERT_NE(Allocated, 0u);
  EXPECT_EQ(bytesText(Allocated, 6), std::string("Hello\0", 6));
  invoke("RtlFreeAnsiString", {Destination});
  EXPECT_EQ(integer(Destination), 0u);
  EXPECT_EQ(integer(Destination + 8), 0u);

  ansi(Destination, Buffer, "", 6);
  EXPECT_EQ(invoke("RtlUnicodeStringToAnsiString", {Destination, Source, 0}),
            0u);
  EXPECT_EQ(integer(Destination, 2), 5u);
  EXPECT_EQ(bytesText(Buffer, 6), std::string("Hello\0", 6));
  ansi(Destination, Buffer, "", 8);
  check(Memory->writeInteger(Destination + 2, 4, 2));
  EXPECT_EQ(invoke("RtlUnicodeStringToAnsiString", {Destination, Source, 0}),
            0x80000005u);
  EXPECT_EQ(integer(Destination, 2), 3u);
  EXPECT_EQ(bytesText(Buffer, 5), std::string("Hel\0!", 5));
  // A zero-capacity record stores the full length and writes nothing.
  ansi(Destination, Buffer, "", 0);
  check(Memory->write(Buffer, std::vector<uint8_t>{'?'}));
  EXPECT_EQ(invoke("RtlUnicodeStringToAnsiString", {Destination, Source, 0}),
            0x80000005u);
  EXPECT_EQ(integer(Destination, 2), 5u);
  EXPECT_EQ(integer(Buffer, 1), '?');
  // A trailing odd byte is not a UTF-16 unit.
  check(Memory->writeInteger(Source, 11, 2));
  check(Memory->writeInteger(Source + 2, 12, 2));
  ansi(Destination, Buffer, "", 8);
  EXPECT_EQ(invoke("RtlUnicodeStringToAnsiString", {Destination, Source, 0}),
            0u);
  EXPECT_EQ(integer(Destination, 2), 5u);
  unicode(Source, u"é");
  EXPECT_NE(failure("RtlUnicodeStringToAnsiString", {Destination, Source, 0})
                .find("code page"),
            std::string::npos);
}

TEST_F(KernelRuntime, StringConversionAndReleaseFollowTheirIRQLContracts) {
  unicode(Scratch, u"a");
  ansi(Scratch + 0x100, Scratch + 0x200, "", 4);
  check(Memory->write(Scratch + 0x300, std::vector<uint8_t>(16, 0)));
  Model->enterExecution(0x100);
  const auto Previous = invoke("KfRaiseIrql", {1});
  EXPECT_NE(
      failure("RtlUnicodeStringToAnsiString", {Scratch + 0x100, Scratch, 0})
          .find("PASSIVE_LEVEL"),
      std::string::npos);
  EXPECT_NE(failure("RtlAnsiStringToUnicodeString",
                    {Scratch + 0x300, Scratch + 0x100, 0})
                .find("PASSIVE_LEVEL"),
            std::string::npos);
  // Releasing a string is permitted at APC_LEVEL.
  invoke("RtlFreeUnicodeString", {Scratch + 0x300});
  invoke("KeLowerIrql", {Previous});
  const auto Dispatch = invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("RtlFreeAnsiString", {Scratch + 0x300}).find("IRQL"),
            std::string::npos);
  invoke("KeLowerIrql", {Dispatch});
}

TEST_F(KernelRuntime, TimeFieldsMatchRecordedKernelConversions) {
  // Recorded from the 19041.6456 kernel's RtlTimeToTimeFields and
  // RtlTimeFieldsToTime executed in isolation, without leap seconds.
  struct Forward {
    uint64_t Time;
    int16_t Fields[8];
  };
  static constexpr Forward Times[] = {
      {0x0ULL, {1601, 1, 1, 0, 0, 0, 0, 1}},
      {0x270fULL, {1601, 1, 1, 0, 0, 0, 0, 1}},
      {0x2710ULL, {1601, 1, 1, 0, 0, 0, 1, 1}},
      {0xc92a69bfffULL, {1601, 1, 1, 23, 59, 59, 999, 1}},
      {0xc92a69c000ULL, {1601, 1, 2, 0, 0, 0, 0, 2}},
      {0x743551217a5eef5eULL, {28136, 2, 29, 20, 23, 59, 411, 3}},
      {0x7fffffffffffffffULL, {30828, 9, 14, 2, 48, 5, 477, 4}},
  };
  for (const auto &Case : Times) {
    SCOPED_TRACE(Case.Time);
    check(Memory->writeInteger(Scratch, Case.Time, 8));
    check(Memory->write(Scratch + 0x20, std::vector<uint8_t>(16, 0xee)));
    invoke("RtlTimeToTimeFields", {Scratch, Scratch + 0x20});
    for (unsigned I = 0; I < 8; ++I)
      EXPECT_EQ(static_cast<int16_t>(integer(Scratch + 0x20 + I * 2, 2)),
                Case.Fields[I])
          << I;
  }
  struct Backward {
    int16_t Fields[8];
    bool Valid;
    uint64_t Time;
  };
  static constexpr Backward Inputs[] = {
      {{1601, 1, 1, 0, 0, 0, 0, 5}, true, 0x0ULL},
      {{30827, 12, 31, 23, 59, 59, 999, 0}, true, 0x7fff35f4f06c58f0ULL},
      {{2024, 2, 29, 0, 0, 0, 0, 0}, true, 0x1da6aa23ce90000ULL},
      {{2000, 2, 29, 0, 0, 0, 0, 0}, true, 0x1bf8247ebcc8000ULL},
      // Weekday is ignored on input.
      {{2026, 10, 15, 12, 34, 56, 789, 9}, true, 0x1dd5ca1960cbc50ULL},
      {{2023, 2, 29, 0, 0, 0, 0, 0}, false, 0},
      {{1900, 2, 29, 0, 0, 0, 0, 0}, false, 0},
      {{2026, 4, 31, 0, 0, 0, 0, 0}, false, 0},
      {{1600, 12, 31, 0, 0, 0, 0, 0}, false, 0},
      {{30828, 1, 1, 0, 0, 0, 0, 0}, false, 0},
      {{-1, 1, 1, 0, 0, 0, 0, 0}, false, 0},
      {{2026, 0, 1, 0, 0, 0, 0, 0}, false, 0},
      {{2026, 13, 1, 0, 0, 0, 0, 0}, false, 0},
      {{2026, 1, 0, 0, 0, 0, 0, 0}, false, 0},
      {{2026, 1, 1, 24, 0, 0, 0, 0}, false, 0},
      {{2026, 1, 1, -1, 0, 0, 0, 0}, false, 0},
      {{2026, 1, 1, 0, 60, 0, 0, 0}, false, 0},
      {{2026, 1, 1, 0, 0, 60, 0, 0}, false, 0},
      {{2026, 1, 1, 0, 0, 0, 1000, 0}, false, 0},
      {{2026, 1, 1, 0, 0, 0, -1, 0}, false, 0},
  };
  for (const auto &Input : Inputs) {
    SCOPED_TRACE(Input.Fields[0]);
    for (unsigned I = 0; I < 8; ++I)
      check(
          Memory->writeInteger(Scratch + I * 2, uint16_t(Input.Fields[I]), 2));
    check(Memory->writeInteger(Scratch + 0x40, 0xeeeeeeeeeeeeeeeeULL, 8));
    EXPECT_EQ(invoke("RtlTimeFieldsToTime", {Scratch, Scratch + 0x40}),
              Input.Valid ? 1u : 0u);
    // A rejected conversion leaves the output untouched.
    EXPECT_EQ(integer(Scratch + 0x40),
              Input.Valid ? Input.Time : 0xeeeeeeeeeeeeeeeeULL);
  }
}

TEST_F(KernelRuntime, GUIDFormattingAllocatesLowercaseRegistryText) {
  // RtlStringFromGUID (RVA 0x6f34d0) formats into a 78-byte paged 'Strg'
  // allocation that RtlFreeUnicodeString releases.
  const std::vector<uint8_t> Guid = {0x67, 0xe9, 0x36, 0x4d, 0x25, 0xe3,
                                     0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00,
                                     0x2b, 0xe1, 0x03, 0x18};
  check(Memory->write(Scratch, Guid));
  check(Memory->write(Scratch + 0x20, std::vector<uint8_t>(16, 0xee)));
  EXPECT_EQ(invoke("RtlStringFromGUID", {Scratch, Scratch + 0x20}), 0u);
  EXPECT_EQ(integer(Scratch + 0x20, 2), 76u);
  EXPECT_EQ(integer(Scratch + 0x22, 2), 78u);
  const uint64_t Buffer = integer(Scratch + 0x28);
  EXPECT_EQ(units(Buffer, 39),
            std::u16string(u"{4d36e967-e325-11ce-bfc1-08002be10318}\0", 39));
  EXPECT_NE(failure("ExFreePoolWithTag", {Buffer, Tag}).find("tag"),
            std::string::npos);
  invoke("RtlFreeUnicodeString", {Scratch + 0x20});
  EXPECT_EQ(integer(Scratch + 0x28), 0u);
  check(Memory->write(Scratch, std::vector<uint8_t>(16, 0xff)));
  invoke("RtlStringFromGUID", {Scratch, Scratch + 0x20});
  EXPECT_EQ(units(integer(Scratch + 0x28), 38),
            u"{ffffffff-ffff-ffff-ffff-ffffffffffff}");
  invoke("RtlFreeUnicodeString", {Scratch + 0x20});
}

TEST_F(KernelRuntime, GUIDParsingMatchesTheKernelScanner) {
  // RtlGUIDFromString (RVA 0x6bd650): exact widths in either case. Data1
  // through Data3 are stored while scanning, Data4 only after a full match,
  // and text may follow only after a zero unit.
  struct Case {
    std::u16string_view Text;
    uint64_t Status;
    const char *Guid;
  };
  const Case Cases[] = {
      {u"{4d36e967-e325-11ce-bfc1-08002be10318}", 0,
       "67e9364d25e3ce11bfc108002be10318"},
      {u"{4D36E967-E325-11CE-BFC1-08002BE10318}", 0,
       "67e9364d25e3ce11bfc108002be10318"},
      {std::u16string_view(u"{4d36e967-e325-11ce-bfc1-08002be10318}\0x", 40), 0,
       "67e9364d25e3ce11bfc108002be10318"},
      {u"{4d36e967-e325-11ce-bfc1-08002be10318}x", 0xc000000d,
       "67e9364d25e3ce11eeeeeeeeeeeeeeee"},
      {u"{4d36e967-e325-11ce-bfc108002be10318}", 0xc000000d,
       "67e9364d25e3ce11eeeeeeeeeeeeeeee"},
      {u"{4d36e967-e3z5-11ce-bfc1-08002be10318}", 0xc000000d,
       "67e9364deeeeeeeeeeeeeeeeeeeeeeee"},
      {u"{4d36e96g-e325-11ce-bfc1-08002be10318}", 0xc000000d,
       "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"},
      {u"4d36e967-e325-11ce-bfc1-08002be10318}", 0xc000000d,
       "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"},
      {u"", 0xc000000d, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(std::string(Case.Text.begin(), Case.Text.end()));
    unicode(Scratch, Case.Text);
    check(Memory->write(Scratch + 0x100, std::vector<uint8_t>(16, 0xee)));
    EXPECT_EQ(invoke("RtlGUIDFromString", {Scratch, Scratch + 0x100}),
              Case.Status);
    std::string Hex;
    for (unsigned I = 0; I < 16; ++I)
      Hex += llvm::formatv("{0:x-2}", integer(Scratch + 0x100 + I, 1)).str();
    EXPECT_EQ(Hex, Case.Guid);
  }
  Model->enterExecution(0x100);
  const auto Previous = invoke("KfRaiseIrql", {1});
  EXPECT_NE(failure("RtlGUIDFromString", {Scratch, Scratch + 0x100})
                .find("PASSIVE_LEVEL"),
            std::string::npos);
  EXPECT_NE(failure("RtlStringFromGUID", {Scratch + 0x100, Scratch + 0x20})
                .find("PASSIVE_LEVEL"),
            std::string::npos);
  invoke("KeLowerIrql", {Previous});
}

TEST_F(KernelRuntime, TimeFieldsRefuseNegativeTimesAndAllowAnyIRQL) {
  check(Memory->writeInteger(Scratch, UINT64_MAX, 8));
  EXPECT_NE(failure("RtlTimeToTimeFields", {Scratch, Scratch + 0x20})
                .find("non-negative"),
            std::string::npos);
  check(Memory->writeInteger(Scratch, 0, 8));
  Model->enterExecution(0x100);
  const auto Previous = invoke("KfRaiseIrql", {15});
  invoke("RtlTimeToTimeFields", {Scratch, Scratch + 0x20});
  EXPECT_EQ(invoke("RtlTimeFieldsToTime", {Scratch + 0x20, Scratch + 0x40}),
            1u);
  EXPECT_EQ(integer(Scratch + 0x40), 0u);
  invoke("KeLowerIrql", {Previous});
}

TEST_F(KernelRuntime, BoundedUnicodeInitializationRejectsLongSources) {
  // RtlInitUnicodeStringEx (RVA 0x32eb60) clears the record before measuring.
  wide(Scratch + 0x100, u"abc");
  check(Memory->write(Scratch, std::vector<uint8_t>(16, 0xff)));
  EXPECT_EQ(invoke("RtlInitUnicodeStringEx", {Scratch, Scratch + 0x100}), 0u);
  EXPECT_EQ(integer(Scratch, 2), 6u);
  EXPECT_EQ(integer(Scratch + 2, 2), 8u);
  EXPECT_EQ(integer(Scratch + 4, 4), 0u);
  EXPECT_EQ(integer(Scratch + 8), Scratch + 0x100);
  check(Memory->write(Scratch, std::vector<uint8_t>(16, 0xff)));
  EXPECT_EQ(invoke("RtlInitUnicodeStringEx", {Scratch, 0}), 0u);
  EXPECT_EQ(integer(Scratch), 0u);
  EXPECT_EQ(integer(Scratch + 8), 0u);
  check(Memory->map(Scratch + 0x10000, 0x20000, Read | Write));
  for (size_t Count : {size_t(0x7ffe), size_t(0x7fff)}) {
    SCOPED_TRACE(Count);
    wide(Scratch + 0x10000, std::u16string(Count, u'x'));
    const uint64_t Status =
        invoke("RtlInitUnicodeStringEx", {Scratch, Scratch + 0x10000});
    EXPECT_EQ(integer(Scratch + 8), Scratch + 0x10000);
    if (Count == 0x7ffe) {
      EXPECT_EQ(Status, 0u);
      EXPECT_EQ(integer(Scratch, 2), 0xfffcu);
      EXPECT_EQ(integer(Scratch + 2, 2), 0xfffeu);
    } else {
      EXPECT_EQ(Status, 0xc0000106u);
      EXPECT_EQ(integer(Scratch, 4), 0u);
    }
  }
}

TEST_F(KernelRuntime, CountedByteCopyNeverWritesATerminator) {
  const uint64_t Destination = Scratch;
  const uint64_t Buffer = Scratch + 0x100;
  const uint64_t Source = Scratch + 0x200;
  ansi(Source, Scratch + 0x300, "abcdef", 6);
  ansi(Destination, Buffer, "", 8);
  invoke("RtlCopyString", {Destination, Source});
  EXPECT_EQ(integer(Destination, 2), 6u);
  EXPECT_EQ(bytesText(Buffer, 8), "abcdef!!");
  ansi(Destination, Buffer, "", 8);
  check(Memory->writeInteger(Destination + 2, 4, 2));
  invoke("RtlCopyString", {Destination, Source});
  EXPECT_EQ(integer(Destination, 2), 4u);
  EXPECT_EQ(bytesText(Buffer, 6), "abcd!!");
  // A null source only clears the length.
  invoke("RtlCopyString", {Destination, 0});
  EXPECT_EQ(integer(Destination, 2), 0u);
  EXPECT_EQ(bytesText(Buffer, 1), "a");
}

TEST_F(KernelRuntime, CountedByteComparisonUsesUnsignedBytesAndLengths) {
  ansi(Scratch, Scratch + 0x100, "abc", 3);
  ansi(Scratch + 0x20, Scratch + 0x120, "abc", 3);
  ansi(Scratch + 0x40, Scratch + 0x140, "abd", 3);
  ansi(Scratch + 0x60, Scratch + 0x160, "ab", 2);
  ansi(Scratch + 0x80, Scratch + 0x180, "a\x80", 2);
  ansi(Scratch + 0xa0, Scratch + 0x1a0, "a\x01", 2);
  ansi(Scratch + 0xc0, Scratch + 0x1c0, "", 0);
  EXPECT_EQ(invoke("RtlEqualString", {Scratch, Scratch + 0x20, 0}), 1u);
  EXPECT_EQ(invoke("RtlEqualString", {Scratch, Scratch + 0x40, 0}), 0u);
  EXPECT_EQ(invoke("RtlEqualString", {Scratch, Scratch + 0x60, 0}), 0u);
  EXPECT_EQ(invoke("RtlEqualString", {Scratch + 0xc0, Scratch + 0xc0, 0}), 1u);
  auto Compare = [&](uint64_t Left, uint64_t Right) {
    return static_cast<int32_t>(invoke("RtlCompareString", {Left, Right, 0}));
  };
  EXPECT_EQ(Compare(Scratch, Scratch + 0x20), 0);
  EXPECT_EQ(Compare(Scratch, Scratch + 0x40), -1);
  EXPECT_EQ(Compare(Scratch + 0x60, Scratch), -1);
  EXPECT_EQ(Compare(Scratch, Scratch + 0x60), 1);
  EXPECT_EQ(Compare(Scratch + 0x80, Scratch + 0xa0), 0x7f);
  EXPECT_EQ(Compare(Scratch + 0xc0, Scratch), -3);
  // Case-insensitive requests that never meet unequal bytes are exact.
  EXPECT_EQ(invoke("RtlEqualString", {Scratch, Scratch + 0x20, 1}), 1u);
  EXPECT_EQ(invoke("RtlEqualString", {Scratch, Scratch + 0x60, 1}), 0u);
  EXPECT_EQ(static_cast<int32_t>(
                invoke("RtlCompareString", {Scratch + 0x60, Scratch, 1})),
            -1);
  EXPECT_EQ(invoke("RtlEqualString", {Scratch, Scratch + 0x40, 1}), 0u);
  EXPECT_EQ(int32_t(invoke("RtlCompareString", {Scratch, Scratch + 0x40, 1})),
            -1);
  // BOOLEAN arguments use only their low byte.
  EXPECT_EQ(invoke("RtlEqualString", {Scratch, Scratch + 0x40, 0x100}), 0u);
  // Comparison ignores capacity and rejects unequal lengths before reading
  // bytes.
  check(Memory->writeInteger(Scratch, 4, 2));
  EXPECT_EQ(invoke("RtlEqualString", {Scratch, Scratch + 0x20, 0}), 0u);
}

TEST_F(KernelRuntime, CountedByteHelpersFollowTheirIRQLContracts) {
  ansi(Scratch, Scratch + 0x100, "abc", 3);
  ansi(Scratch + 0x20, Scratch + 0x120, "", 4);
  wide(Scratch + 0x200, u"a");
  Model->enterExecution(0x100);
  const auto APC = invoke("KfRaiseIrql", {1});
  EXPECT_NE(
      failure("RtlEqualString", {Scratch, Scratch, 0}).find("PASSIVE_LEVEL"),
      std::string::npos);
  EXPECT_NE(
      failure("RtlCompareString", {Scratch, Scratch, 0}).find("PASSIVE_LEVEL"),
      std::string::npos);
  invoke("KeLowerIrql", {APC});
  const auto Dispatch = invoke("KfRaiseIrql", {2});
  invoke("RtlCopyString", {Scratch + 0x20, Scratch});
  EXPECT_EQ(invoke("RtlInitUnicodeStringEx", {Scratch + 0x40, Scratch + 0x200}),
            0u);
  invoke("KeLowerIrql", {Dispatch});
  const auto Above = invoke("KfRaiseIrql", {3});
  EXPECT_NE(failure("RtlInitUnicodeStringEx", {Scratch + 0x40, Scratch + 0x200})
                .find("IRQL"),
            std::string::npos);
  invoke("RtlCopyString", {Scratch + 0x20, Scratch});
  invoke("KeLowerIrql", {Above});
}

TEST_F(KernelRuntime, DebugIntegersUseWindowsWidthsAndFormattingRules) {
  EXPECT_EQ(
      formatted(
          "%+06d %#08x %I64u %ld %hhd %hu %p %%",
          {uint64_t(-42), 0xab, UINT64_MAX, 0x100000001, 0xff, 0xffff, 0x1234}),
      "-00042 0x0000ab 18446744073709551615 1 255 65535 0000000000001234 %");
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

TEST_F(KernelRuntime, DebugWrappersShareRepeatedShortAndBufferRules) {
  for (bool Extended : {false, true}) {
    EXPECT_EQ(formatted("%hhd|%hhu|%hhx", {384, 0xffff, 0x12347788}, Extended),
              "384|65535|7788");
    EXPECT_EQ(formatted("A%cz", {0}, Extended), "A");
    EXPECT_EQ(formatted(std::string(511, 'x'), {}, Extended),
              std::string(511, 'x'));
    EXPECT_EQ(formatted(std::string(512, 'x'), {}, Extended),
              std::string(510, 'x') + "\n");
  }
}

TEST_F(KernelRuntime, PrefixedDebugUsesGrowingAttemptsAndOverflowNewline) {
  struct Case {
    unsigned PrefixLength, BodyLength, KeptPrefix, KeptBody;
    bool Newline;
  };
  // Independent observations of the pinned kernel's four buffer attempts.
  const Case Cases[] = {{700, 0, 127, 0, false}, {127, 1, 127, 1, false},
                        {254, 1, 254, 1, false}, {255, 1, 255, 1, false},
                        {383, 1, 383, 1, false}, {510, 1, 510, 1, false},
                        {511, 1, 510, 0, true},  {700, 2, 510, 0, true},
                        {1, 512, 1, 509, true},  {0, 512, 0, 510, true}};
  for (const auto &C : Cases) {
    ascii(Scratch, std::string(C.PrefixLength, 'P'));
    ascii(Scratch + 0x1000, std::string(C.BodyLength, 'b'));
    EXPECT_EQ(invoke("vDbgPrintExWithPrefix",
                     {Scratch, UINT64_MAX, UINT64_MAX, Scratch + 0x1000, 0}),
              0u);
    EXPECT_EQ(Result.Messages.back(), std::string(C.KeptPrefix, 'P') +
                                          std::string(C.KeptBody, 'b') +
                                          (C.Newline ? "\n" : ""));
  }
}

TEST_F(KernelRuntime, PrefixedDebugReadsExactScalarWidthsAtGuestBoundary) {
  struct Case {
    const char *Format;
    unsigned Size;
    uint64_t Value;
    const char *Text;
  };
  const Case Cases[] = {{"%c", 1, 'A', "A"},
                        {"%lc", 2, 'B', "B"},
                        {"%hhd", 2, 384, "384"},
                        {"%hd", 2, 0xffff, "-1"},
                        {"%hhx", 2, 0x7788, "7788"},
                        {"%ld", 4, 0xfffffffe, "-2"},
                        {"%I32u", 4, 0xffffffff, "4294967295"},
                        {"%I64x", 8, 0x1122334455667788, "1122334455667788"},
                        {"%p", 8, 0x1234, "0000000000001234"}};
  ascii(Scratch, "pre:");
  for (const auto &C : Cases) {
    const uint64_t Address = Scratch + 0x10000 - C.Size;
    ascii(Scratch + 0x1000, C.Format);
    check(Memory->writeInteger(Address, C.Value, C.Size));
    EXPECT_EQ(invoke("vDbgPrintExWithPrefix",
                     {Scratch, 7, 2, Scratch + 0x1000, Address}),
              0u);
    EXPECT_EQ(Result.Messages.back(), std::string("pre:") + C.Text);
  }
  // Width reads four bytes, but still consumes an entire x64 argument slot.
  ascii(Scratch + 0x1000, "%*d");
  const uint64_t Arguments = Scratch + 0x10000 - 12;
  check(Memory->writeInteger(Arguments, 3, 4));
  check(Memory->writeInteger(Arguments + 8, 1, 4));
  EXPECT_EQ(invoke("vDbgPrintExWithPrefix",
                   {Scratch, 7, 2, Scratch + 0x1000, Arguments}),
            0u);
  EXPECT_EQ(Result.Messages.back(), "pre:  1");
}

TEST_F(KernelRuntime, PrefixedDebugPreservesNulsOnlyOnOverflow) {
  ascii(Scratch, "pre:");
  ascii(Scratch + 0x100, "A%cz");
  check(Memory->writeInteger(Scratch + 0x800, 0, 8));
  EXPECT_EQ(invoke("vDbgPrintExWithPrefix",
                   {Scratch, 0, 0, Scratch + 0x100, Scratch + 0x800}),
            0u);
  EXPECT_EQ(Result.Messages.back(), "pre:A");
  ascii(Scratch + 0x100, std::string("%c") + std::string(511, 'b'));
  EXPECT_EQ(invoke("vDbgPrintExWithPrefix",
                   {Scratch, 0, 0, Scratch + 0x100, Scratch + 0x800}),
            0u);
  EXPECT_EQ(Result.Messages.back(),
            std::string("pre:\0", 5) + std::string(505, 'b') + "\n");
}

TEST_F(KernelRuntime, PrefixedDebugChecksAllGuestInputsBeforePublication) {
  for (unsigned BadInput : {0u, 3u, 4u}) {
    resetModel();
    ascii(Scratch, "pre:");
    ascii(Scratch + 0x100, "%u");
    check(Memory->writeInteger(Scratch + 0x800, 1, 8));
    const uint64_t Freed = invoke("ExAllocatePool2", {0x40, 32, Tag});
    invoke("ExFreePool", {Freed});
    std::vector<uint64_t> A{Scratch, 0, 0, Scratch + 0x100, Scratch + 0x800};
    A[BadInput] = Freed;
    auto Value = Model->call("vDbgPrintExWithPrefix", A);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find("freed"),
              std::string::npos);
    EXPECT_TRUE(Result.Messages.empty());
  }
  resetModel();
  ascii(Scratch, std::string(700, 'P'));
  ascii(Scratch + 0x800, "%f");
  EXPECT_FALSE(
      failure("vDbgPrintExWithPrefix", {Scratch, 0, 0, Scratch + 0x800, 0})
          .empty());
  EXPECT_FALSE(failure("vDbgPrintExWithPrefix", {0, 0, 0, 0}).empty());
  EXPECT_TRUE(Result.Messages.empty());
}

TEST_F(KernelRuntime, PrefixedDebugEnforcesIrqlAndPagedArgumentLifetime) {
  Model->enterExecution(1);
  ascii(Scratch, "pre:");
  ascii(Scratch + 0x100, "constant");
  const uint64_t Paged = invoke("ExAllocatePool2", {0x100, 8, Tag});
  check(Memory->writeInteger(Paged, 'A', 8));
  invoke("KfRaiseIrql", {12});
  EXPECT_EQ(
      invoke("vDbgPrintExWithPrefix", {Scratch, 0, 0, Scratch + 0x100, Paged}),
      0u);
  EXPECT_EQ(Result.Messages.back(), "pre:constant");
  ascii(Scratch + 0x100, "%c");
  EXPECT_NE(
      failure("vDbgPrintExWithPrefix", {Scratch, 0, 0, Scratch + 0x100, Paged})
          .find("paged"),
      std::string::npos);
  resetModel();
  Model->enterExecution(1);
  ascii(Scratch, "");
  ascii(Scratch + 0x100, "%lc");
  check(Memory->writeInteger(Scratch + 0x800, 'A', 2));
  invoke("KfRaiseIrql", {1});
  EXPECT_NE(failure("vDbgPrintExWithPrefix",
                    {Scratch, 0, 0, Scratch + 0x100, Scratch + 0x800})
                .find("PASSIVE_LEVEL"),
            std::string::npos);
  invoke("KfRaiseIrql", {13});
  EXPECT_NE(failure("vDbgPrintExWithPrefix", {0, 0, 0, 0, 0}).find("IRQL"),
            std::string::npos);
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

TEST(KernelRuntimeExecution, CompiledDriverBuildsAndParsesCountedStrings) {
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_strings.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 5u);
  EXPECT_EQ(Result->Messages[0], "strings \\Device\\Sample42 42 -42 driver 32");
  // The overflow still stores the formatted length in the short record.
  EXPECT_EQ(Result->Messages[1], "status c0000023 80000005 10");
  // A short ANSI conversion keeps the terminated prefix that fits.
  EXPECT_EQ(Result->Messages[2], "convert 80000005 3 \\De");
  // A byte copy without a terminator compares by unsigned bytes and length.
  EXPECT_EQ(Result->Messages[3], "bytes 6 -6");
  // Registry-form GUID text is lowercase and parses back to the same GUID.
  EXPECT_EQ(Result->Messages[4],
            "guid {4d36e967-e325-11ce-bfc1-08002be10318} 76 1");
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  EXPECT_EQ(Calls["RtlInitAnsiString"], 1u);
  EXPECT_EQ(Calls["RtlInitString"], 1u);
  EXPECT_EQ(Calls["RtlAppendUnicodeToString"], 2u);
  EXPECT_EQ(Calls["RtlAppendUnicodeStringToString"], 2u);
  EXPECT_EQ(Calls["RtlPrefixUnicodeString"], 2u);
  EXPECT_EQ(Calls["RtlIntegerToUnicodeString"], 2u);
  EXPECT_EQ(Calls["RtlUnicodeStringToInteger"], 2u);
  EXPECT_EQ(Calls["RtlUnicodeStringToAnsiString"], 2u);
  EXPECT_EQ(Calls["RtlAnsiStringToUnicodeString"], 1u);
  EXPECT_EQ(Calls["RtlFreeAnsiString"], 1u);
  // The converted copy and the GUID text are both released.
  EXPECT_EQ(Calls["RtlFreeUnicodeString"], 2u);
  EXPECT_EQ(Calls["RtlInitUnicodeStringEx"], 1u);
  EXPECT_EQ(Calls["RtlCopyString"], 1u);
  EXPECT_EQ(Calls["RtlEqualString"], 1u);
  EXPECT_EQ(Calls["RtlCompareString"], 2u);
  EXPECT_EQ(Calls["RtlStringFromGUID"], 1u);
  EXPECT_EQ(Calls["RtlGUIDFromString"], 1u);
}

TEST(KernelRuntimeExecution, CompiledDriverRoundTripsCalendarTimes) {
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_time_fields.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 1u);
  // 2024-02-29 was a Thursday; the impossible date is rejected untouched.
  EXPECT_EQ(Result->Messages[0], "time 2024-02-29 23:59:58.999 weekday 4 0 1");
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  EXPECT_EQ(Calls["RtlTimeFieldsToTime"], 3u);
  EXPECT_EQ(Calls["RtlTimeToTimeFields"], 1u);
}

TEST(KernelRuntimeExecution, CompiledDriverSharesQueuesUnderOneSpinLock) {
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_interlocked.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 1u);
  // FIFO drain after one head insertion, LIFO reuse from the free list.
  EXPECT_EQ(Result->Messages[0],
            "queue 1 1 1 order 30 10 20 free 20 10 30 pending 0 bytes 60");
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  EXPECT_EQ(Calls["ExInterlockedInsertTailList"], 2u);
  EXPECT_EQ(Calls["ExInterlockedInsertHeadList"], 1u);
  EXPECT_EQ(Calls["ExInterlockedRemoveHeadList"], 4u);
  EXPECT_EQ(Calls["ExInterlockedPushEntryList"], 3u);
  EXPECT_EQ(Calls["ExInterlockedPopEntryList"], 4u);
  EXPECT_EQ(Calls["ExInterlockedAddUlong"], 6u);
  EXPECT_EQ(Calls["ExInterlockedAddLargeInteger"], 3u);
}

TEST(KernelRuntimeExecution, CompiledDriverCachesEntriesOnASequencedList) {
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_slist.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 1u);
  // LIFO order; the flushed chain keeps its links.
  EXPECT_EQ(Result->Messages[0],
            "slist depth 4 top 4 popped 4 chain 321 count 3 empty 1");
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  EXPECT_EQ(Calls["InitializeSListHead"], 1u);
  EXPECT_EQ(Calls["ExpInterlockedPushEntrySList"], 4u);
  EXPECT_EQ(Calls["ExpInterlockedPopEntrySList"], 2u);
  EXPECT_EQ(Calls["ExpInterlockedFlushSList"], 1u);
  EXPECT_EQ(Calls["ExQueryDepthSList"], 2u);
  EXPECT_EQ(Calls["FirstEntrySList"], 1u);
}

TEST(KernelRuntimeExecution, CompiledDriverUsesCRuntimeStringHelpers) {
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_cstrings.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 1u);
  // Lengths | ordered/raw/folded compares | char search | substrings | spans.
  EXPECT_EQ(Result->Messages[0],
            "cstrings 11 5 3 5 | -1 0 0 0 0 0 0 0 | 2 2 9 3 -1 | 6 0 2 | "
            "4 2 2");
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  EXPECT_EQ(Calls["strlen"], 1u);
  EXPECT_EQ(Calls["strchr"], 2u); // first hit and a miss
  EXPECT_EQ(Calls["wcsstr"], 1u);
  EXPECT_EQ(Calls["strspn"], 1u);
  EXPECT_EQ(Calls["wcscspn"], 1u);
}

class KernelCStringWriteExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

TEST_P(KernelCStringWriteExecution, OriginalDriverCopiesPadsAndAppends) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = Backend;
  Options.Contract = Contract;
  if (Rebase)
    Options.LoadAddress = 0x1c0000000;
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                  "driver_cstring_writes.sys",
                              Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  for (const char *Name : {"strcpy", "strncpy", "strcat", "strncat", "wcscpy",
                           "wcsncpy", "wcscat", "wcsncat"})
    EXPECT_GT(Calls[Name], 0u) << Name;
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelCStringWriteExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()));

class KernelLookasideExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

TEST_P(KernelLookasideExecution, OriginalDriverUsesInlineCacheAcrossUnload) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = Backend;
  Options.Contract = Contract;
  Options.Unload = true;
  if (Rebase)
    Options.LoadAddress = 0x1c0000000;
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                  "driver_lookaside.sys",
                              Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  EXPECT_TRUE(Result->UnloadCompleted);
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  for (const char *Name :
       {"ExInitializeNPagedLookasideList", "ExInitializePagedLookasideList",
        "ExDeleteNPagedLookasideList", "ExDeletePagedLookasideList",
        "ExpInterlockedPushEntrySList", "ExpInterlockedPopEntrySList"})
    EXPECT_GT(Calls[Name], 0u) << Name;
  EXPECT_EQ(Calls["ExAllocatePoolWithTag"], 21u);
  EXPECT_EQ(Calls["ExFreePool"], 9u);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelLookasideExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()));

class KernelFastMutexExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

TEST_P(KernelFastMutexExecution,
       OriginalDriverPreservesOwnershipIrqlAndUnsafeRegions) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = Backend;
  Options.Contract = Contract;
  Options.Unload = true;
  if (Rebase)
    Options.LoadAddress = 0x1c0000000;
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                  "driver_fast_mutex.sys",
                              Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  EXPECT_TRUE(Result->UnloadCompleted);
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  for (const char *Name :
       {"KeInitializeGuardedMutex", "ExAcquireFastMutex",
        "ExTryToAcquireFastMutex", "ExReleaseFastMutex",
        "ExAcquireFastMutexUnsafe", "ExReleaseFastMutexUnsafe",
        "KeAcquireGuardedMutex", "KeTryToAcquireGuardedMutex",
        "KeReleaseGuardedMutex", "KeAcquireGuardedMutexUnsafe",
        "KeReleaseGuardedMutexUnsafe", "KeInitializeEvent"})
    EXPECT_GT(Calls[Name], 0u) << Name;
  EXPECT_EQ(Calls["ExAllocatePoolWithTag"], 2u);
  EXPECT_EQ(Calls["ExFreePool"], 2u);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelFastMutexExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()));

class KernelExecutiveResourceExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

TEST_P(KernelExecutiveResourceExecution,
       OriginalDriverSharesResourcesAcrossParkedSystemThreads) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = Backend;
  Options.Contract = Contract;
  Options.Unload = true;
  if (Rebase)
    Options.LoadAddress = 0x1c0000000;
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                  "driver_eresource.sys",
                              Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  EXPECT_TRUE(Result->UnloadCompleted);
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  for (const char *Name :
       {"ExInitializeResourceLite",
        "ExReinitializeResourceLite",
        "ExDeleteResourceLite",
        "ExAcquireResourceExclusiveLite",
        "ExTryToAcquireResourceExclusiveLite",
        "ExAcquireResourceSharedLite",
        "ExAcquireSharedStarveExclusive",
        "ExAcquireSharedWaitForExclusive",
        "ExConvertExclusiveToSharedLite",
        "ExReleaseResourceLite",
        "ExReleaseResourceForThreadLite",
        "ExIsResourceAcquiredExclusiveLite",
        "ExIsResourceAcquiredSharedLite",
        "ExGetExclusiveWaiterCount",
        "ExGetSharedWaiterCount",
        "ExDisableResourceBoostLite",
        "ExEnterCriticalRegionAndAcquireResourceExclusive",
        "ExEnterCriticalRegionAndAcquireResourceShared",
        "ExEnterCriticalRegionAndAcquireSharedWaitForExclusive",
        "ExReleaseResourceAndLeaveCriticalRegion",
        "ExSetResourceOwnerPointer",
        "ExSetResourceOwnerPointerEx"})
    EXPECT_GT(Calls[Name], 0u) << Name;
  EXPECT_EQ(Calls["ExAllocatePoolWithTag"], 2u);
  EXPECT_EQ(Calls["ExFreePool"], 2u);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelExecutiveResourceExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()));

TEST(KernelRuntimeExecution, CompiledDriverGuardsWithAnInStackQueuedSpinLock) {
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_qspinlock.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 1u);
  // PASSIVE before, DISPATCH while held, PASSIVE after; the counter updates.
  EXPECT_EQ(Result->Messages[0], "qspin before 0 held 2 after 0 value 7");
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  EXPECT_EQ(Calls["KeAcquireInStackQueuedSpinLock"], 1u);
  EXPECT_EQ(Calls["KeReleaseInStackQueuedSpinLock"], 1u);
  EXPECT_EQ(Calls["KeInitializeSpinLock"], 1u);
}

TEST(KernelRuntimeExecution, CompiledDriverDrainsWithRundownProtection) {
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_rundown.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 1u);
  // Two protected accesses succeed, the acquire after the wait fails because
  // the object ran down, a reinitialized object protects again, and a counted
  // acquisition of three holds six in the word until its matching release.
  EXPECT_EQ(Result->Messages[0], "rundown 1 1 0 1 1 held 6 left 0");
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  EXPECT_EQ(Calls["ExAcquireRundownProtection"], 4u);
  EXPECT_EQ(Calls["ExReleaseRundownProtection"], 3u);
  EXPECT_EQ(Calls["ExWaitForRundownProtectionRelease"], 1u);
  EXPECT_EQ(Calls["ExReInitializeRundownProtection"], 1u);
  EXPECT_EQ(Calls["ExAcquireRundownProtectionEx"], 1u);
  EXPECT_EQ(Calls["ExReleaseRundownProtectionEx"], 1u);
}

TEST(KernelRuntimeExecution, CompiledDriverSeesOneIdentityThroughEachAlias) {
  auto Result = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                              "driver_identity.sys");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
  ASSERT_TRUE(Result->NTStatus.has_value());
  EXPECT_EQ(*Result->NTStatus, 0u);
  ASSERT_EQ(Result->Messages.size(), 1u);
  // The first thread ID comes from the profile base, 0x2000.
  EXPECT_EQ(Result->Messages[0], "identity 4 8192");
  std::map<std::string, unsigned> Calls;
  for (const auto &Call : Result->Calls)
    ++Calls[Call.Name];
  for (const char *Name :
       {"PsGetCurrentProcess", "IoGetCurrentProcess", "PsGetCurrentThread",
        "KeGetCurrentThread", "PsGetCurrentThreadId", "PsGetThreadId"})
    EXPECT_EQ(Calls[Name], 1u) << Name;
}
using namespace std::string_view_literals;

TEST_F(KernelRuntime, DigitClassificationKeepsTheKernelMaskAndCarrierWidth) {
  for (uint32_t Value = 0; Value < 512; ++Value) {
    const uint64_t Expected = Value % 256 >= '0' && Value % 256 <= '9' ? 4 : 0;
    EXPECT_EQ(invoke("isdigit", {Value}), Expected);
    EXPECT_EQ(invoke("isdigit", {0xaabbccdd00000000ull | Value}), Expected);
  }
  for (uint64_t Value : {uint64_t{0xffffffff}, uint64_t{0xfffffffe},
                         uint64_t{0xaabbccddffffffff}})
    EXPECT_EQ(invoke("isdigit", {Value}), 0u);
  EXPECT_EQ(invoke("isdigit", {0xaabbccdd00000130}), 4u);
  EXPECT_FALSE(failure("isdigit", {}).empty());
}

TEST_F(KernelRuntime, InPlaceCaseConversionPreservesEveryOtherUnit) {
  constexpr uint64_t Buffer = Scratch + 0x20000;
  check(Memory->map(Buffer, 0x20000, Read | Write));
  for (const char *Name : {"_strupr", "_wcslwr", "_wcsupr"}) {
    const unsigned Width = std::string_view(Name) == "_strupr" ? 1 : 2;
    const bool Lower = std::string_view(Name) == "_wcslwr";
    const unsigned Count = Width == 1 ? 256 : 65536;
    std::vector<uint8_t> Bytes(Count * Width);
    for (unsigned I = 1; I < Count; ++I) {
      Bytes[(I - 1) * Width] = uint8_t(I);
      if (Width == 2)
        Bytes[(I - 1) * Width + 1] = uint8_t(I >> 8);
    }
    check(Memory->write(Buffer, Bytes));
    EXPECT_EQ(invoke(Name, {Buffer}), Buffer);
    // Exhaustive original-export probes establish that only ASCII changes;
    // accented characters, surrogate code units and fullwidth letters remain.
    const unsigned First = Lower ? 'A' : 'a';
    for (unsigned I = First; I <= First + 25; ++I)
      Bytes[(I - 1) * Width] = uint8_t(Lower ? I + 32 : I - 32);
    std::vector<uint8_t> Actual(Bytes.size());
    check(Memory->read(Buffer, Actual));
    EXPECT_EQ(Actual, Bytes) << Name;
    ASSERT_FALSE(HasFailure());
  }
  EXPECT_EQ(invoke("_strupr", {0}), 0u);
  EXPECT_EQ(invoke("_wcslwr", {0}), 0u);
  EXPECT_FALSE(failure("_wcsupr", {0}).empty());
}

TEST_F(KernelRuntime,
       CaseConversionWritesOnlyChangedUnitsAndKeepsPartialStores) {
  for (const char *Name : {"_strupr", "_wcslwr", "_wcsupr"}) {
    resetModel();
    const unsigned Width = std::string_view(Name) == "_strupr" ? 1 : 2;
    const bool Lower = std::string_view(Name) == "_wcslwr";
    const uint64_t Address = Scratch + 0x2000;
    check(Memory->writeInteger(Address, 0x80, Width));
    check(Memory->writeInteger(Address + Width, 0, Width));
    check(Memory->protect(Address, 0x1000, Read));
    EXPECT_EQ(invoke(Name, {Address}), Address);
    check(Memory->protect(Address, 0x1000, Read | Write));
    check(Memory->writeInteger(Address, Lower ? 'A' : 'a', Width));
    check(Memory->protect(Address, 0x1000, Read));
    EXPECT_FALSE(failure(Name, {Address}).empty());
    EXPECT_EQ(integer(Address, Width), Lower ? 'A' : 'a');
    resetModel();
    const uint64_t Edge = Scratch + 0x10000 - 2 * Width;
    check(Memory->writeInteger(Edge, Lower ? 'A' : 'a', Width));
    check(Memory->writeInteger(Edge + Width, Lower ? 'B' : 'b', Width));
    EXPECT_FALSE(failure(Name, {Edge}).empty());
    EXPECT_EQ(integer(Edge, Width), Lower ? 'a' : 'A');
    EXPECT_EQ(integer(Edge + Width, Width), Lower ? 'b' : 'B');
    ASSERT_FALSE(HasFailure());
  }
}

TEST_F(KernelRuntime, CheckedCopiesPreserveCapacityErrorsAndTruncationBytes) {
  struct Case {
    uint64_t Capacity, Count, Status;
    std::string_view Output;
  };
  // Concrete byte/unit observations from the original exports, including
  // errors which clear the first unit after writing later units.
  const Case Cases[] = {{0, 6, 22, "!!!!!!!!"},
                        {1, 6, 34, "\0!!!!!!!"sv},
                        {2, 6, 34, "\0B!!!!!!"sv},
                        {4, 6, 34, "\0BCD!!!!"sv},
                        {5, 6, 0, "ABCD\0!!!"sv},
                        {8, 0, 0, "\0!!!!!!!"sv},
                        {8, 1, 0, "A\0!!!!!!"sv},
                        {8, 3, 0, "ABC\0!!!!"sv},
                        {3, 3, 34, "\0BC!!!!!"sv},
                        {4, 3, 0, "ABC\0!!!!"sv},
                        {1, UINT64_MAX, 80, "\0!!!!!!!"sv},
                        {3, UINT64_MAX, 80, "AB\0!!!!!"sv},
                        {4, UINT64_MAX, 80, "ABC\0!!!!"sv},
                        {5, UINT64_MAX, 0, "ABCD\0!!!"sv},
                        {UINT64_MAX, uint64_t{1} << 32, 0, "ABCD\0!!!"sv}};
  for (const char *Name : {"strncpy_s", "wcsncpy_s"}) {
    const unsigned Width = std::string_view(Name) == "strncpy_s" ? 1 : 2;
    for (const auto &C : Cases) {
      SCOPED_TRACE(::testing::Message()
                   << Name << ' ' << C.Capacity << ' ' << C.Count);
      const uint64_t Source = Scratch + 0x100;
      if (Width == 1) {
        ascii(Scratch, "!!!!!!!!");
        ascii(Source, "ABCD");
      } else {
        wide(Scratch, u"!!!!!!!!");
        wide(Source, u"ABCD");
      }
      EXPECT_EQ(invoke(Name, {Scratch, C.Capacity, Source, C.Count}), C.Status);
      for (unsigned I = 0; I < C.Output.size(); ++I)
        EXPECT_EQ(integer(Scratch + I * Width, Width), uint8_t(C.Output[I]));
      ASSERT_FALSE(HasFailure());
    }
  }
  for (uint64_t Capacity : {1u, 2u, 4u, 5u}) {
    ascii(Scratch, "!!!!!!!!");
    ascii(Scratch + 0x100, "ABCD");
    EXPECT_EQ(invoke("strcpy_s", {Scratch, Capacity, Scratch + 0x100}),
              Capacity == 5 ? 0u : 34u);
    const std::string_view Expected = Capacity == 1   ? "\0!!!!!!!"sv
                                      : Capacity == 2 ? "\0B!!!!!!"sv
                                      : Capacity == 4 ? "\0BCD!!!!"sv
                                                      : "ABCD\0!!!"sv;
    EXPECT_EQ(bytesText(Scratch, 8), Expected);
  }
}

TEST_F(KernelRuntime, CheckedAppendAndNullArgumentsKeepKernelErrorEffects) {
  const uint64_t Source = Scratch + 0x100;
  wide(Source, u"CD");
  for (uint64_t Capacity : {0u, 1u, 2u, 3u, 4u, 5u}) {
    wide(Scratch, u"AB\0!!!!!"sv);
    EXPECT_EQ(invoke("wcscat_s", {Scratch, Capacity, Source}),
              Capacity < 3   ? 22u
              : Capacity < 5 ? 34u
                             : 0u);
    const std::u16string_view Expected = Capacity == 0   ? u"AB\0!!!!!"sv
                                         : Capacity < 3  ? u"\0B\0!!!!!"sv
                                         : Capacity == 3 ? u"\0BC!!!!!"sv
                                         : Capacity == 4 ? u"\0BCD!!!!"sv
                                                         : u"ABCD\0!!!"sv;
    EXPECT_EQ(units(Scratch, 8), Expected);
  }
  for (const char *Name : {"strcpy_s", "strncpy_s", "wcscat_s", "wcsncpy_s"}) {
    const bool Counted =
        std::string_view(Name).find("ncpy") != std::string_view::npos;
    const unsigned Width = Name[0] == 'w' ? 2 : 1;
    auto Call = [&](uint64_t D, uint64_t Size, uint64_t S, uint64_t Count) {
      return Counted ? invoke(Name, {D, Size, S, Count})
                     : invoke(Name, {D, Size, S});
    };
    EXPECT_EQ(Call(0, 0, 0, 1), 22u);
    EXPECT_EQ(Call(0, 4, Source, 1), 22u);
    check(Memory->writeInteger(Scratch, 0x55555555, 4));
    EXPECT_EQ(Call(Scratch, 4, 0, 1), 22u);
    EXPECT_EQ(integer(Scratch, Width), 0u);
    EXPECT_EQ(integer(Scratch + Width, Width), Width == 1 ? 0x55u : 0x5555u);
    if (Counted) {
      EXPECT_EQ(Call(0, 0, 1, 0), 0u);
      EXPECT_EQ(Call(0, 1, 1, 0), 22u);
      EXPECT_EQ(Call(Scratch, 1, 1, 0), 0u);
      EXPECT_EQ(integer(Scratch, Width), 0u);
    }
  }
}

TEST_F(KernelRuntime,
       CheckedStringFaultsPreserveEarlierUnitsAndValidateLifetime) {
  for (const char *Name : {"strcpy_s", "strncpy_s", "wcscat_s", "wcsncpy_s"}) {
    const bool Counted =
        std::string_view(Name).find("ncpy") != std::string_view::npos;
    const unsigned Width = Name[0] == 'w' ? 2 : 1;
    const uint64_t Edge = Scratch + 0x10000 - 2 * Width;
    auto Fail = [&](uint64_t D, uint64_t Size, uint64_t S) {
      return Counted ? failure(Name, {D, Size, S, 4})
                     : failure(Name, {D, Size, S});
    };
    for (bool SourceFault : {false, true}) {
      resetModel();
      const uint64_t Destination = SourceFault ? Scratch : Edge;
      const uint64_t Source = SourceFault ? Edge : Scratch;
      check(Memory->writeInteger(Destination, 0, Width));
      for (unsigned I = 0; I < (SourceFault ? 2u : 4u); ++I)
        check(Memory->writeInteger(Source + I * Width, I == 3 ? 0 : 'A' + I,
                                   Width));
      EXPECT_FALSE(Fail(Destination, 4, Source).empty());
      EXPECT_EQ(integer(Destination, Width), 'A');
      EXPECT_EQ(integer(Destination + Width, Width), 'B');
    }
    resetModel();
    check(Memory->writeInteger(Scratch, 0, Width));
    const auto Pool = invoke("ExAllocatePoolWithTag", {0, 16, Tag});
    invoke("ExFreePoolWithTag", {Pool, Tag});
    EXPECT_NE(Fail(Pool, 1, Scratch).find("freed"), std::string::npos);
    EXPECT_NE(Fail(Scratch, 1, Pool).find("freed"), std::string::npos);
    ASSERT_FALSE(HasFailure());
  }
}

TEST_F(KernelRuntime, CheckedCopiesFollowThePinnedSequentialAliasBehavior) {
  // These are observed kernel instruction effects, not portable C overlap
  // guarantees. Earlier stores can replace a later source terminator.
  ascii(Scratch, "ABCD\0!!!"sv);
  EXPECT_EQ(invoke("strcpy_s", {Scratch + 2, 6, Scratch}), 34u);
  EXPECT_EQ(bytesText(Scratch, 8), "AB\0BABAB"sv);
  ascii(Scratch, "ABCD\0!!!"sv);
  EXPECT_EQ(invoke("strncpy_s", {Scratch + 2, 6, Scratch, 4}), 0u);
  EXPECT_EQ(bytesText(Scratch, 8), "ABABAB\0!"sv);
  wide(Scratch, u"ABCD\0!!!"sv);
  EXPECT_EQ(invoke("wcsncpy_s", {Scratch + 4, 6, Scratch, 4}), 0u);
  EXPECT_EQ(units(Scratch, 8), u"ABABAB\0!"sv);
  wide(Scratch, u"ABCD\0!!!"sv);
  EXPECT_EQ(invoke("wcscat_s", {Scratch + 4, 6, Scratch}), 34u);
  EXPECT_EQ(units(Scratch, 8), u"AB\0DABCD"sv);
  ascii(Scratch, "ABCD");
  EXPECT_EQ(invoke("strcpy_s", {Scratch, 5, Scratch}), 0u);
  EXPECT_EQ(bytesText(Scratch, 5), "ABCD\0"sv);
}

TEST_F(KernelRuntime, CheckedStringsBoundScansAndRespectPagedStorage) {
  Model->enterExecution(0x100);
  const uint64_t Pool = invoke("ExAllocatePoolWithTag", {1, 16, Tag});
  ascii(Pool, "a");
  const uint64_t Old = invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("_strupr", {Pool}).find("paged pool"), std::string::npos);
  EXPECT_NE(failure("strcpy_s", {Scratch, 8, Pool}).find("paged pool"),
            std::string::npos);
  EXPECT_EQ(invoke("isdigit", {'7'}), 4u);
  invoke("KeLowerIrql", {Old});
  invoke("ExFreePoolWithTag", {Pool, Tag});
  constexpr uint64_t Buffer = Scratch + 0x400000, Limit = 1024 * 1024;
  check(Memory->map(Buffer, Limit + 0x1000, Read | Write));
  check(Memory->write(Buffer, std::vector<uint8_t>(Limit, 'a')));
  EXPECT_NE(failure("_strupr", {Buffer}).find("1 MiB"), std::string::npos);
  EXPECT_EQ(integer(Buffer, 1), 'A');
  EXPECT_EQ(integer(Buffer + Limit - 1, 1), 'A');
  EXPECT_EQ(integer(Buffer + Limit, 1), 0u);
  // Capacity is a full size_t; a huge value does not force access to unused
  // capacity. An actual scan is still bounded, including alias expansion.
  ascii(Scratch, "ABCD");
  EXPECT_EQ(invoke("strcpy_s", {Scratch + 0x100, UINT64_MAX, Scratch}), 0u);
  EXPECT_EQ(bytesText(Scratch + 0x100, 5), "ABCD\0"sv);
  EXPECT_FALSE(failure("strcpy_s", {UINT64_MAX, 2, Scratch}).empty());
}

TEST_F(KernelRuntime, CheckedMemoryCopiesPreserveDistinctErrorEffects) {
  struct Case {
    uint64_t Capacity, Count, Status;
    bool NullDestination, NullSource;
    std::string_view CopyBytes, MoveBytes;
  };
  const Case Cases[] = {
      {0, 0, 0, true, true, "!!!!!!!!", "!!!!!!!!"},
      {8, 3, 22, true, false, "!!!!!!!!", "!!!!!!!!"},
      {8, 3, 22, false, true, "\0\0\0\0\0\0\0\0"sv, "!!!!!!!!"},
      {0, 3, 22, false, true, "!!!!!!!!", "!!!!!!!!"},
      {0, 3, 34, false, false, "!!!!!!!!", "!!!!!!!!"},
      {2, 3, 34, false, false, "\0\0!!!!!!"sv, "!!!!!!!!"},
      {8, uint64_t{1} << 32, 34, false, false, "\0\0\0\0\0\0\0\0"sv,
       "!!!!!!!!"},
      {3, 3, 0, false, false, "ABC!!!!!", "ABC!!!!!"},
      {UINT64_MAX, 3, 0, false, false, "ABC!!!!!", "ABC!!!!!"}};
  ascii(Scratch + 0x100, "ABCDE");
  for (const char *Name : {"memcpy_s", "memmove_s"})
    for (const auto &C : Cases) {
      SCOPED_TRACE(::testing::Message()
                   << Name << ' ' << C.Capacity << ' ' << C.Count);
      ascii(Scratch, "!!!!!!!!");
      EXPECT_EQ(invoke(Name, {C.NullDestination ? 0 : Scratch, C.Capacity,
                              C.NullSource ? 0 : Scratch + 0x100, C.Count}),
                C.Status);
      EXPECT_EQ(bytesText(Scratch, 8), std::string_view(Name) == "memcpy_s"
                                           ? C.CopyBytes
                                           : C.MoveBytes);
      ASSERT_FALSE(HasFailure());
    }
}

TEST_F(KernelRuntime, CheckedMemoryCopiesUseMoveSemanticsAndIgnoreZeroCount) {
  for (const char *Name : {"memcpy_s", "memmove_s"}) {
    ascii(Scratch, "ABCDEFGH");
    EXPECT_EQ(invoke(Name, {Scratch + 2, 4, Scratch, 4}), 0u);
    EXPECT_EQ(bytesText(Scratch, 8), "ABABCDGH");
    ascii(Scratch, "ABCDEFGH");
    EXPECT_EQ(invoke(Name, {Scratch, 4, Scratch + 2, 4}), 0u);
    EXPECT_EQ(bytesText(Scratch, 8), "CDEFEFGH");
    EXPECT_EQ(invoke(Name, {Scratch, 8, Scratch, 8}), 0u);
    EXPECT_EQ(bytesText(Scratch, 8), "CDEFEFGH");
    EXPECT_EQ(invoke(Name, {UINT64_MAX, UINT64_MAX, UINT64_MAX, 0}), 0u);
    // Capacity failure must not read a non-null source.
    ascii(Scratch, "!!!!!!!!");
    EXPECT_EQ(invoke(Name, {Scratch, 2, UINT64_MAX, 3}), 34u);
    EXPECT_EQ(bytesText(Scratch, 8), std::string_view(Name) == "memcpy_s"
                                         ? "\0\0!!!!!!"sv
                                         : "!!!!!!!!"sv);
  }
}

TEST_F(KernelRuntime, CheckedMemoryCopiesRetainStorageAndOperationLimits) {
  const uint64_t Source = Scratch + 0x100;
  ascii(Source, "ABCD");
  ascii(Scratch, "!!!!!!!!");
  check(Memory->protect(Scratch, 0x1000, Read));
  EXPECT_EQ(invoke("memmove_s", {Scratch, 2, 0, 3}), 22u);
  EXPECT_FALSE(failure("memcpy_s", {Scratch, 2, 0, 3}).empty());
  EXPECT_EQ(bytesText(Scratch, 8), "!!!!!!!!");
  for (const char *Name : {"memcpy_s", "memmove_s"}) {
    resetModel();
    ascii(Source, "ABCD");
    check(Memory->protect(Scratch, 0x1000, Read));
    EXPECT_FALSE(failure(Name, {Scratch, 4, Source, 4}).empty());
  }
  resetModel();
  Model->enterExecution(0x100);
  ascii(Source, "ABCD");
  const auto Pool = invoke("ExAllocatePoolWithTag", {0, 16, Tag});
  invoke("ExFreePoolWithTag", {Pool, Tag});
  EXPECT_NE(failure("memcpy_s", {Pool, 4, 0, 1}).find("freed"),
            std::string::npos);
  EXPECT_EQ(invoke("memmove_s", {Pool, 4, 0, 1}), 22u);
  for (const char *Name : {"memcpy_s", "memmove_s"}) {
    EXPECT_NE(
        failure(Name, {Scratch, 0x100001, Source, 0x100001}).find("1 MiB"),
        std::string::npos);
    const auto Paged = invoke("ExAllocatePoolWithTag", {1, 16, Tag});
    ascii(Paged, "A");
    const auto Old = invoke("KfRaiseIrql", {2});
    EXPECT_NE(failure(Name, {Scratch, 4, Paged, 2}).find("paged pool"),
              std::string::npos);
    invoke("KeLowerIrql", {Old});
    invoke("ExFreePoolWithTag", {Paged, Tag});
  }
  EXPECT_NE(failure("memcpy_s", {Scratch, 0x100001, 0, 1}).find("1 MiB"),
            std::string::npos);
  EXPECT_EQ(invoke("memmove_s", {Scratch, UINT64_MAX, 0, 1}), 22u);
}

TEST_F(KernelRuntime, RawImageHeadersUseSignaturesAndUnsignedDosOffsets) {
  for (uint64_t Base : {Scratch, uint64_t{0x160000000}}) {
    if (Base != Scratch)
      check(Memory->map(Base, 0x1000, Read | Write));
    check(Memory->writeInteger(Base, 0x5a4d, 2));
    check(Memory->writeInteger(Base + 60, 0x80, 4));
    check(Memory->writeInteger(Base + 0x80, 0x4550, 4));
    check(Memory->writeInteger(Base + 0x84, UINT64_MAX, 8));
    EXPECT_EQ(invoke("RtlImageNtHeader", {Base}), Base + 0x80);
    check(Memory->writeInteger(Base + 0x80, 0x12345678, 4));
    EXPECT_EQ(invoke("RtlImageNtHeader", {Base}), 0u);
    check(Memory->writeInteger(Base, 0, 2));
    check(Memory->writeInteger(Base + 60, 0xffffffff, 4));
    EXPECT_EQ(invoke("RtlImageNtHeader", {Base}), 0u);
  }
  constexpr uint64_t Far = Scratch + 0x80000000;
  check(Memory->map(Far, 0x1000, Read | Write));
  check(Memory->writeInteger(Scratch, 0x5a4d, 2));
  check(Memory->writeInteger(Scratch + 60, 0x80000100, 4));
  check(Memory->writeInteger(Far + 0x100, 0x4550, 4));
  EXPECT_EQ(invoke("RtlImageNtHeader", {Scratch}), Far + 0x100);
}

TEST_F(KernelRuntime, RawImageHeaderFencePrecedesSignatureAccess) {
  constexpr uint64_t Fence = 0x7ffffffeffff, Base = Fence - 0x1fff;
  check(Memory->map(Base, 0x3000, Read | Write));
  for (uint32_t Offset :
       {0x80u, 0x1ef6u, 0x1ef7u, 0x1ef8u, 0x1fffu, 0x2000u, 0xffffffffu}) {
    check(Memory->writeInteger(Base, 0x5a4d, 2));
    check(Memory->writeInteger(Base + 60, Offset, 4));
    if (Offset < 0x3000)
      check(Memory->writeInteger(Base + Offset, 0x4550, 4));
    EXPECT_EQ(invoke("RtlImageNtHeader", {Base}),
              Base + Offset + 264 < Fence ? Base + Offset : 0u);
  }
  EXPECT_EQ(invoke("RtlImageNtHeader", {0}), 0u);
  EXPECT_EQ(invoke("RtlImageNtHeader", {UINT64_MAX}), 0u);
  EXPECT_FALSE(failure("RtlImageNtHeader", {Scratch + 0x10000}).empty());
  resetModel();
  const uint64_t Edge = Scratch + 0x10000 - 2;
  check(Memory->writeInteger(Edge, 0, 2));
  EXPECT_EQ(invoke("RtlImageNtHeader", {Edge}), 0u);
  check(Memory->writeInteger(Edge, 0x5a4d, 2));
  EXPECT_FALSE(failure("RtlImageNtHeader", {Edge}).empty());
  resetModel();
  Model->enterExecution(0x100);
  const auto Pool = invoke("ExAllocatePoolWithTag", {1, 256, Tag});
  check(Memory->writeInteger(Pool, 0x5a4d, 2));
  check(Memory->writeInteger(Pool + 60, 0x80, 4));
  check(Memory->writeInteger(Pool + 0x80, 0x4550, 4));
  const auto Old = invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("RtlImageNtHeader", {Pool}).find("paged pool"),
            std::string::npos);
  invoke("KeLowerIrql", {Old});
  invoke("ExFreePoolWithTag", {Pool, Tag});
  EXPECT_NE(failure("RtlImageNtHeader", {Pool}).find("freed"),
            std::string::npos);
}

TEST_F(KernelRuntime, UnicodeHashPreservesNarrowCarriersAndRawUnits) {
  const auto Output = Scratch + 0x100;
  for (uint64_t Fold : {0u, 1u, 0x100u, 0x101u})
    for (uint64_t Algorithm : {0ULL, 1ULL, 0x100000001ULL}) {
      unicode(Scratch, u"ab");
      // MaximumLength is not consulted; an odd tail is not a code unit.
      check(Memory->writeInteger(Scratch, 5, 2));
      check(Memory->writeInteger(Scratch + 2, 0, 2));
      check(Memory->writeInteger(Output, UINT64_MAX, 8));
      EXPECT_EQ(
          invoke("RtlHashUnicodeString", {Scratch, Fold, Algorithm, Output}),
          0u);
      EXPECT_EQ(integer(Output, 4), Fold & 255 ? 4264001u : 6363201u);
      EXPECT_EQ(integer(Output + 4, 4), UINT32_MAX);
    }
  for (uint16_t Unit : {0u, 0xbfu, 0xc0u, 0xe9u, 0xd800u, 0xffffu}) {
    unicode(Scratch, std::u16string(1, Unit));
    EXPECT_EQ(invoke("RtlHashUnicodeString", {Scratch, 0, 0, Output}), 0u);
    EXPECT_EQ(integer(Output, 4), Unit);
  }
  unicode(Scratch, u"\u00bf");
  EXPECT_EQ(invoke("RtlHashUnicodeString", {Scratch, 1, 1, Output}), 0u);
  EXPECT_EQ(integer(Output, 4), 0xbfu);
  unicode(Scratch, u"a\u00c0");
  EXPECT_NE(
      failure("RtlHashUnicodeString", {Scratch, 1, 1, Output}).find("NLS"),
      std::string::npos);
  EXPECT_EQ(integer(Output, 4), 0u);
}

TEST_F(KernelRuntime, UnicodeHashCapturesBufferBeforeClearingAliasedOutput) {
  unicode(Scratch, u"ab");
  EXPECT_EQ(invoke("RtlHashUnicodeString", {Scratch, 1, 1, Scratch}), 0u);
  EXPECT_EQ(integer(Scratch, 4), 0u);
  unicode(Scratch, u"abc");
  EXPECT_EQ(invoke("RtlHashUnicodeString", {Scratch, 0, 1, Scratch + 32}), 0u);
  EXPECT_EQ(integer(Scratch + 32, 4), 'c');
  unicode(Scratch, u"ab");
  EXPECT_EQ(invoke("RtlHashUnicodeString", {Scratch, 0, 1, Scratch + 8}), 0u);
  EXPECT_EQ(integer(Scratch + 8, 4), 6363201u);
  EXPECT_EQ(invoke("RtlHashUnicodeString", {0, 1, 1, 0}), 0xc000000du);
  EXPECT_EQ(invoke("RtlHashUnicodeString", {UINT64_MAX, 1, 1, 0}), 0xc000000du);
  unicode(Scratch, u"ab");
  check(Memory->writeInteger(Scratch + 8, 0, 8));
  check(Memory->writeInteger(Scratch + 0x100, UINT32_MAX, 4));
  EXPECT_EQ(invoke("RtlHashUnicodeString", {Scratch, 1, 2, Scratch + 0x100}),
            0xc000000du);
  EXPECT_EQ(integer(Scratch + 0x100, 4), 0u);
}

TEST_F(KernelRuntime, UnicodeHashFaultsRetainOnlyPrecedingStores) {
  const auto Output = Scratch + 0x100;
  check(Memory->writeInteger(Output, UINT32_MAX, 4));
  EXPECT_FALSE(failure("RtlHashUnicodeString", {Scratch + 0xfffc, 0, 1, Output})
                   .empty());
  EXPECT_EQ(integer(Output, 4), UINT32_MAX);
  resetModel();
  unicode(Scratch, u"a");
  check(Memory->writeInteger(Scratch + 8, Scratch + 0xfffe, 8));
  check(Memory->writeInteger(Scratch, 4, 2));
  check(Memory->writeInteger(Scratch + 0xfffe, 'a', 2));
  check(Memory->writeInteger(Output, UINT32_MAX, 4));
  EXPECT_FALSE(
      failure("RtlHashUnicodeString", {Scratch, 0, 1, Output}).empty());
  EXPECT_EQ(integer(Output, 4), 0u);
}

TEST_F(KernelRuntime, UnicodeUpcaseCountsBytesAndPreservesSequentialOverlap) {
  const auto Source = Scratch, Destination = Scratch + 0x100;
  const auto Buffer = Scratch + 0x200;
  unicode(Source, u"az\u00bf");
  check(Memory->writeInteger(Source, 7, 2));
  check(Memory->writeInteger(Source + 2, 0, 2));
  counted(Destination, Buffer, u"", 8);
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Source, 0x100}), 0u);
  EXPECT_EQ(integer(Destination, 2), 7u);
  EXPECT_EQ(integer(Destination + 2, 2), 8u);
  EXPECT_EQ(integer(Buffer, 8), 0x002100bf005a0041ULL);
  check(Memory->writeInteger(Destination, 0x1234, 2));
  check(Memory->writeInteger(Destination + 2, 6, 2));
  check(Memory->writeInteger(Source + 8, 0, 8));
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Source, 0}),
            0x80000005u);
  EXPECT_EQ(integer(Destination, 2), 0x1234u);
  unicode(Source, u"abcd");
  check(Memory->writeInteger(Source, 6, 2));
  check(Memory->writeInteger(Destination + 8, Source + 34, 8));
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Source, 0}), 0u);
  EXPECT_EQ(integer(Source + 32, 8), 0x0041004100410061ULL);
  // A one-byte source has no complete unit, so neither Buffer is accessed.
  check(Memory->writeInteger(Source, 1, 2));
  check(Memory->writeInteger(Source + 8, 0, 8));
  check(Memory->writeInteger(Destination + 8, 0, 8));
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Source, 0}), 0u);
  EXPECT_EQ(integer(Destination, 2), 1u);
  check(Memory->writeInteger(Scratch + 0xfffe, 1, 2));
  EXPECT_EQ(
      invoke("RtlUpcaseUnicodeString", {Destination, Scratch + 0xfffe, 0}), 0u);
}

TEST_F(KernelRuntime, UnicodeUpcaseReloadsAliasedDescriptors) {
  const auto Source = Scratch, Destination = Scratch + 0x100;
  unicode(Source, u"ab");
  check(Memory->writeInteger(Destination + 2, 4, 2));
  check(Memory->writeInteger(Destination + 8, Source, 8));
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Source, 0}), 0u);
  EXPECT_EQ(integer(Source, 4), 0x00420041u);
  EXPECT_EQ(integer(Destination, 2), 65u);
  unicode(Source, u"\u0080b");
  wide(Scratch + 0x80, u"xc");
  check(Memory->writeInteger(Destination + 8, Source + 8, 8));
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Source, 0}), 0u);
  EXPECT_EQ(integer(Source + 8, 4), 0x00430080u);
  // Destination.Buffer is reloaded too: the first output redirects it.
  unicode(Source, u"\u0080b");
  check(Memory->writeInteger(Destination + 8, Destination + 8, 8));
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Source, 0}), 0u);
  EXPECT_EQ(integer(Destination + 8), Scratch + 0x80);
  EXPECT_EQ(integer(Scratch + 0x82, 2), 'B');
}

TEST_F(KernelRuntime, UnicodeUpcaseSharesPagedStringAllocationAndFailure) {
  const auto Destination = Scratch + 0x100;
  unicode(Scratch, u"ab");
  check(Memory->writeInteger(Scratch, 5, 2));
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Scratch, 0x101}),
            0u);
  const auto Buffer = integer(Destination + 8);
  ASSERT_NE(Buffer, 0u);
  EXPECT_EQ(integer(Destination, 2), 5u);
  EXPECT_EQ(integer(Destination + 2, 2), 5u);
  EXPECT_EQ(integer(Buffer, 4), 0x00420041u);
  EXPECT_EQ(integer(Buffer + 4, 1), 0xcdu);
  EXPECT_NE(failure("ExFreePoolWithTag", {Buffer, Tag}).find("tag"),
            std::string::npos);
  invoke("RtlFreeUnicodeString", {Destination});
  EXPECT_EQ(integer(Destination), 0u);
  EXPECT_EQ(integer(Destination + 8), 0u);
  auto Dead = Model->validateGuestAccess(Buffer, 1, false);
  ASSERT_TRUE(bool(Dead));
  EXPECT_NE(llvm::toString(std::move(Dead)).find("freed"), std::string::npos);
  unicode(Scratch, u"");
  check(Memory->writeInteger(Destination, 0x1234, 2));
  EXPECT_NE(failure("RtlUpcaseUnicodeString", {Destination, Scratch, 1})
                .find("non-zero"),
            std::string::npos);
  EXPECT_EQ(integer(Destination, 2), 0x1234u);
  EXPECT_EQ(integer(Destination + 2, 2), 0u);
  // Exhaust the same physical arena used by the paged string owner.
  bool Exhausted = false;
  for (unsigned I = 0; I < 128; ++I)
    if (!invoke("ExAllocatePoolWithTag", {1, 65536, Tag})) {
      Exhausted = true;
      break;
    }
  ASSERT_TRUE(Exhausted);
  check(Memory->writeInteger(Scratch, 65535, 2));
  check(Memory->writeInteger(Destination + 8, UINT64_MAX, 8));
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Scratch, 1}),
            0xc0000017u);
  EXPECT_EQ(integer(Destination, 2), 0x1234u);
  EXPECT_EQ(integer(Destination + 2, 2), 65535u);
  EXPECT_EQ(integer(Destination + 8), 0u);
}

TEST_F(KernelRuntime, UnicodeUpcaseRetainsStoresBeforeNlsAndAccessFailures) {
  const auto Destination = Scratch + 0x100, Buffer = Scratch + 0x200;
  unicode(Scratch, u"a\u00c0");
  counted(Destination, Buffer, u"", 4);
  check(Memory->writeInteger(Destination, 0x1234, 2));
  EXPECT_NE(
      failure("RtlUpcaseUnicodeString", {Destination, Scratch, 0}).find("NLS"),
      std::string::npos);
  EXPECT_EQ(integer(Buffer, 4), 0x00210041u);
  EXPECT_EQ(integer(Destination, 2), 0x1234u);
  unicode(Scratch, u"ab");
  check(Memory->writeInteger(Destination + 8, Scratch + 0xfffe, 8));
  EXPECT_FALSE(
      failure("RtlUpcaseUnicodeString", {Destination, Scratch, 0}).empty());
  EXPECT_EQ(integer(Scratch + 0xfffe, 2), 'A');
  EXPECT_EQ(integer(Destination, 2), 0x1234u);
}

TEST_F(KernelRuntime, SecondsSince1980PreservesRangeEdgesAndOutputAliases) {
  constexpr uint64_t Epoch = 119600064000000000ULL;
  constexpr uint64_t End = Epoch + 0x100000000ULL * 10000000;
  for (auto [Time, Valid, Seconds] : {std::tuple{Epoch - 1, false, 0u},
                                      {Epoch, true, 0u},
                                      {Epoch + 9999999, true, 0u},
                                      {Epoch + 10000000, true, 1u},
                                      {End - 1, true, UINT32_MAX},
                                      {End, false, 0u},
                                      {uint64_t(INT64_MAX), false, 0u},
                                      {uint64_t(INT64_MIN), false, 0u},
                                      {UINT64_MAX, false, 0u},
                                      {uint64_t(0), false, 0u}}) {
    check(Memory->writeInteger(Scratch, Time, 8));
    check(Memory->writeInteger(Scratch + 8, 0x1234567887654321ULL, 8));
    EXPECT_EQ(invoke("RtlTimeToSecondsSince1980", {Scratch, Scratch + 8}),
              Valid);
    EXPECT_EQ(integer(Scratch + 8, 4), Valid ? Seconds : 0x87654321u);
    EXPECT_EQ(integer(Scratch + 12, 4), 0x12345678u);
    if (!Valid)
      EXPECT_EQ(invoke("RtlTimeToSecondsSince1980", {Scratch, 0}), 0u);
  }
  const auto Time = Epoch + 1239999999;
  check(Memory->writeInteger(Scratch, Time, 8));
  EXPECT_EQ(invoke("RtlTimeToSecondsSince1980", {Scratch, Scratch}), 1u);
  EXPECT_EQ(integer(Scratch, 4), 123u);
  EXPECT_EQ(integer(Scratch + 4, 4), Time >> 32);
}

TEST_F(KernelRuntime, CountedRuntimeHelpersShareIrqlAndLifetimeAdmission) {
  const auto Destination = Scratch + 0x100, Output = Scratch + 0x200;
  Model->enterExecution(0x100);
  const auto Previous = invoke("KfRaiseIrql", {1});
  unicode(Scratch, u"a");
  counted(Destination, Output, u"", 2);
  EXPECT_EQ(invoke("RtlHashUnicodeString", {Scratch, 1, 1, Output}), 0u);
  EXPECT_EQ(invoke("RtlUpcaseUnicodeString", {Destination, Scratch, 0}), 0u);
  check(Memory->writeInteger(Output, 119600064000000000ULL, 8));
  EXPECT_EQ(invoke("RtlTimeToSecondsSince1980", {Output, Output + 8}), 1u);
  invoke("KfRaiseIrql", {2});
  EXPECT_NE(failure("RtlHashUnicodeString", {0, 0, 0, 0}).find("IRQL"),
            std::string::npos);
  EXPECT_NE(failure("RtlUpcaseUnicodeString", {0, 0, 0}).find("IRQL"),
            std::string::npos);
  EXPECT_NE(failure("RtlTimeToSecondsSince1980", {0, 0}).find("IRQL"),
            std::string::npos);
  invoke("KeLowerIrql", {1});
  invoke("KeLowerIrql", {Previous});
  const auto Pool = invoke("ExAllocatePoolWithTag", {0, 16, Tag});
  invoke("ExFreePoolWithTag", {Pool, Tag});
  EXPECT_NE(failure("RtlHashUnicodeString", {Pool, 0, 0, Output}).find("freed"),
            std::string::npos);
  EXPECT_NE(
      failure("RtlUpcaseUnicodeString", {Destination, Pool, 0}).find("freed"),
      std::string::npos);
  EXPECT_NE(failure("RtlTimeToSecondsSince1980", {Pool, Output}).find("freed"),
            std::string::npos);
}

TEST_F(KernelRuntime, CountedRuntimeHelpersRejectReadOnlyOutputs) {
  for (unsigned Operation = 0; Operation < 3; ++Operation) {
    resetModel();
    const auto Destination = Scratch + 0x100, Output = Scratch + 0x1000;
    unicode(Scratch, u"a");
    counted(Destination, Output, u"", 8);
    check(Memory->writeInteger(Output, 0x1122334455667788ULL, 8));
    check(Memory->protect(Output, 0x1000, Read));
    if (Operation == 0)
      EXPECT_FALSE(
          failure("RtlHashUnicodeString", {Scratch, 0, 1, Output}).empty());
    else if (Operation == 1)
      EXPECT_FALSE(
          failure("RtlUpcaseUnicodeString", {Destination, Scratch, 0}).empty());
    else {
      check(Memory->writeInteger(Scratch, 119600064000000000ULL, 8));
      EXPECT_FALSE(
          failure("RtlTimeToSecondsSince1980", {Scratch, Output}).empty());
    }
    EXPECT_EQ(integer(Output), 0x1122334455667788ULL);
  }
}

TEST_F(KernelRuntime, ScalarUpcaseNarrowsCarriersBeforeTableAdmission) {
  for (uint64_t High : {0ULL, 0x100ULL, 0xffffff00ULL, 0x12345678abcd0000ULL})
    for (unsigned Unit = 0; Unit < 128; ++Unit)
      EXPECT_EQ(invoke("toupper", {High | Unit}),
                Unit >= 'a' && Unit <= 'z' ? Unit - 32 : Unit);
  for (const char *Name : {"towupper", "RtlUpcaseUnicodeChar"})
    for (uint64_t High :
         {0ULL, 0x10000ULL, 0xffff0000ULL, 0x12345678abcd0000ULL})
      for (unsigned Unit = 0; Unit < 192; ++Unit)
        EXPECT_EQ(invoke(Name, {High | Unit}),
                  Unit >= 'a' && Unit <= 'z' ? Unit - 32 : Unit);
  for (uint64_t Value : {0x80ULL, 0xbfULL, 0xffULL, 0xffffffffULL})
    EXPECT_NE(failure("toupper", {Value}).find("code page"), std::string::npos);
  for (const char *Name : {"towupper", "RtlUpcaseUnicodeChar"})
    for (uint64_t Value : {0xc0u, 0xe9u, 0x3b1u, 0xd800u, 0xffffu})
      EXPECT_NE(failure(Name, {Value}).find("NLS"), std::string::npos);
}

TEST_F(KernelRuntime, ScalarUpcaseSharesApcAdmissionAndExactArity) {
  Model->enterExecution(0x100);
  invoke("KfRaiseIrql", {1});
  for (const char *Name : {"toupper", "towupper", "RtlUpcaseUnicodeChar"}) {
    EXPECT_EQ(invoke(Name, {'a'}), 'A');
    EXPECT_NE(failure(Name, {}).find("argument count"), std::string::npos);
    EXPECT_NE(failure(Name, {'a', 'b'}).find("argument count"),
              std::string::npos);
  }
  invoke("KfRaiseIrql", {2});
  for (const char *Name : {"toupper", "towupper", "RtlUpcaseUnicodeChar"})
    EXPECT_NE(failure(Name, {'a'}).find("IRQL"), std::string::npos);
  invoke("KeLowerIrql", {1});
  invoke("KeLowerIrql", {0});
}

} // namespace
} // namespace neverd::emulation
