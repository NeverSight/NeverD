//===- KernelIntegerUnicodeTests.cpp - Native unsigned RTL formatting -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native unsigned RTL formatting.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
class KernelIntegerUnicode : public testing::Test {
protected:
  static constexpr uint64_t Base = 0x60000000, Header = Base + 0x100,
                            Buffer = Base + 0x4000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void SetUp() override {
    Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
    ok(Memory->map(Base, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, DriverOptions{}));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void put(uint64_t Address, uint64_t Value, unsigned Size = 8) {
    ok(Memory->writeInteger(Address, Value, Size));
  }
  uint64_t get(uint64_t Address, unsigned Size = 8) {
    return llvm::cantFail(Memory->readInteger(Address, Size));
  }
  void record(uint64_t At, uint64_t Target = Buffer, uint16_t Capacity = 130) {
    put(At, 0x1234, 2);
    put(At + 2, Capacity, 2);
    put(At + 4, 0x5a5a5a5a, 4);
    put(At + 8, Target);
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    auto R = Model->call(Name, A);
    if (!R) {
      ADD_FAILURE() << Name << ": " << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void reject(const char *Name, std::initializer_list<uint64_t> A) {
    auto R = Model->call(Name, A);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
};

TEST_F(KernelIntegerUnicode, NativeMatrixMatchesUnsignedDigitsAndEveryByte) {
  struct Case {
    const char *Name;
    uint64_t Value;
    uint32_t Base;
    uint16_t Capacity;
    uint32_t Status;
    uint16_t Length;
    const char *Hex;
  };
  static constexpr Case Cases[] = {
#define NEVERD_INTEGER_UNICODE_CASE(API, Value, Radix, Capacity, Status,       \
                                    Length, Hex)                               \
  {#API, Value, Radix, Capacity, Status, Length, Hex},
#include "IntegerUnicodeCases.def"
#undef NEVERD_INTEGER_UNICODE_CASE
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(std::string(C.Name) + ":" + std::to_string(C.Value) + ":" +
                 std::to_string(C.Base) + ":" + std::to_string(C.Capacity));
    ok(Memory->write(Buffer, std::vector<uint8_t>(160, 0xa5)));
    record(Header, Buffer, C.Capacity);
    EXPECT_EQ(call(C.Name, {C.Value, C.Base, Header}), C.Status);
    EXPECT_EQ(get(Header, 2), C.Length);
    EXPECT_EQ(get(Header + 2, 2), C.Capacity);
    EXPECT_EQ(get(Header + 4, 4), 0x5a5a5a5au);
    EXPECT_EQ(get(Header + 8), Buffer);
    std::vector<uint8_t> Actual(160);
    ok(Memory->read(Buffer, Actual));
    EXPECT_EQ(llvm::toHex(Actual, true), C.Hex);
  }
}

TEST_F(KernelIntegerUnicode, InvalidRadixPrecedesAllDestinationAccess) {
  for (auto Name : {"RtlInt64ToUnicodeString", "RtlIntegerToUnicodeString"}) {
    EXPECT_EQ(call(Name, {UINT64_MAX, 3, 0}), 0xc000000du);
    EXPECT_EQ(call(Name, {UINT64_MAX, UINT64_MAX, UINT64_MAX}), 0xc000000du);
    record(Header);
    EXPECT_EQ(call(Name, {0x100000000, 0x12340000000aULL, Header}), 0u);
    EXPECT_EQ(get(Header, 2),
              std::string(Name) == "RtlInt64ToUnicodeString" ? 20u : 2u);
  }
}

TEST_F(KernelIntegerUnicode, ShortBufferConsumesOnlyLengthAndCapacity) {
  constexpr uint64_t Tail = Base + 4092;
  put(Tail, 0x1234, 2);
  put(Tail + 2, 128, 2);
  ok(Memory->protect(Base + 4096, 4096, 0));
  EXPECT_EQ(call("RtlInt64ToUnicodeString", {UINT64_MAX, 2, Tail}),
            0x80000005u);
  EXPECT_EQ(get(Tail, 2), 128u);
  EXPECT_EQ(get(Tail + 2, 2), 128u);
}

TEST_F(KernelIntegerUnicode, OnlyLengthRequiresWritableHeaderStorage) {
  constexpr uint64_t Tail = Base + 4094;
  record(Tail);
  ok(Memory->protect(Base + 4096, 4096, Read));
  EXPECT_EQ(call("RtlInt64ToUnicodeString", {UINT64_MAX, 16, Tail}), 0u);
  EXPECT_EQ(get(Tail, 2), 32u);
  EXPECT_EQ(get(Tail + 2, 2), 130u);
  EXPECT_EQ(get(Buffer, 2), 'F');
  EXPECT_EQ(get(Buffer + 32, 2), 0u);
}

TEST_F(KernelIntegerUnicode, HeaderAliasesFollowNativeLengthAndPointerReloads) {
  record(Header, Header);
  EXPECT_EQ(call("RtlInt64ToUnicodeString", {123, 10, Header}), 0u);
  EXPECT_EQ(get(Header, 2), 49u);
  EXPECT_EQ(get(Header + 2, 2), 50u);
  EXPECT_EQ(get(Header + 4, 4), 51u);
  EXPECT_EQ(get(Header + 8), Header);
  record(Header, Header + 2);
  EXPECT_EQ(call("RtlIntegerToUnicodeString", {123, 10, Header}), 0u);
  EXPECT_EQ(get(Header, 2), 6u);
  EXPECT_EQ(get(Header + 2, 2), 49u);
  EXPECT_EQ(get(Header + 4, 4), 0x00330032u);
  EXPECT_EQ(get(Header + 8), Base);
  for (bool Alias : {false, true}) {
    constexpr uint64_t Other = 0x61000000;
    if (Alias)
      ok(Memory->mapAlias(Other, Base, 4096, Read | Write));
    put(Base + 0x30, UINT64_MAX);
    record(Header, (Alias ? Other : Base) + 0x108);
    EXPECT_EQ(call("RtlInt64ToUnicodeString", {0, 10, Header}), 0u);
    EXPECT_EQ(get(Header + 8), (Alias ? Other : Base) + 0x30);
    EXPECT_EQ(get(Base + 0x30, 4), 0xffffu);
    EXPECT_EQ(get(Header, 2), 2u);
  }
}

TEST_F(KernelIntegerUnicode, ExistingAnsiConversionUsesTheSameUnicodeWriter) {
  constexpr uint64_t Source = Base + 0x300;
  record(Header, Header + 8);
  put(Source, 1, 2);
  put(Source + 2, 1, 2);
  put(Source + 8, Buffer);
  put(Buffer, '0', 1);
  put(Base + 0x30, UINT64_MAX);
  EXPECT_EQ(call("RtlAnsiStringToUnicodeString", {Header, Source, 0}), 0u);
  EXPECT_EQ(get(Header, 2), 2u);
  EXPECT_EQ(get(Header + 8), Base + 0x30);
  EXPECT_EQ(get(Base + 0x30, 4), 0xffffu);
}

TEST_F(KernelIntegerUnicode,
       BadOutputIsRejectedAndCopiedPointerFaultsRemainVisible) {
  record(Header);
  ok(Memory->protect(Buffer, 4096, Read));
  reject("RtlInt64ToUnicodeString", {UINT64_MAX, 10, Header});
  EXPECT_EQ(get(Header, 2), 0x1234u);
  ok(Memory->protect(Buffer, 4096, Read | Write));
  record(Header, UINT64_MAX - 1);
  reject("RtlInt64ToUnicodeString", {UINT64_MAX, 10, Header});
  EXPECT_EQ(get(Header, 2), 0x1234u);
  record(Header, Header + 8);
  reject("RtlInt64ToUnicodeString", {UINT64_MAX, 10, Header});
  EXPECT_EQ(get(Header, 2), 40u);
  EXPECT_EQ(get(Header + 8), 0x0034003400380031ULL);
}

TEST_F(KernelIntegerUnicode, PassiveIRQLAndArityStayExplicit) {
  record(Header);
  call("KfRaiseIrql", {1});
  reject("RtlInt64ToUnicodeString", {1, 10, Header});
  reject("RtlIntegerToUnicodeString", {1, 10, Header});
  call("KeLowerIrql", {0});
  reject("RtlInt64ToUnicodeString", {1, 10});
  reject("RtlIntegerToUnicodeString", {1, 10, Header, 0});
}

TEST(DriverIntegerUnicode, StaticAndDynamicClientsCompleteAndUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{31, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_integer_unicode.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}
} // namespace
} // namespace neverd::emulation
