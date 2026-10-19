///
/// \file
/// KernelAnsiUpperTests.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include <array>

namespace neverd::emulation {
namespace {
#define NEVERD_ANSI_UPPER_EXPECTED(...)                                        \
  constexpr uint8_t Expected[] = {__VA_ARGS__};
#include "AnsiUpperExpected.def"
#undef NEVERD_ANSI_UPPER_EXPECTED
class KernelAnsiUpper : public testing::Test {
protected:
  static constexpr uint64_t Base = 0x60000000;
  static constexpr uint64_t Dst = Base, Src = Base + 0x100;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void SetUp() override {
    auto B = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(B));
    Memory = std::move(*B);
    ok(Memory->map(Base, 0x40000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, DriverOptions{}));
    Model->enterExecution(profile::StackBase);
  }
  uint64_t get(uint64_t Address, unsigned Size = 1) {
    auto R = Memory->readInteger(Address, Size);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void record(uint64_t At, uint16_t Length, uint16_t Capacity,
              uint64_t Buffer) {
    ok(Memory->writeInteger(At, Length, 2));
    ok(Memory->writeInteger(At + 2, Capacity, 2));
    ok(Memory->writeInteger(At + 4, 0x12345678, 4));
    ok(Memory->writeInteger(At + 8, Buffer, 8));
  }
  void run(uint64_t Destination = Dst, uint64_t Source = Src) {
    auto R = Model->call("RtlUpperString", {Destination, Source});
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  }
  void reject(uint64_t Destination = Dst, uint64_t Source = Src) {
    auto R = Model->call("RtlUpperString", {Destination, Source});
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
};
TEST_F(KernelAnsiUpper,
       AllBytesMatchIndependentNativeResultsAndIgnoreUnusedFields) {
  std::array<uint8_t, 256> Input{};
  for (unsigned I = 0; I < 256; ++I) {
    Input[I] = I;
    auto R = Model->call("RtlUpperChar", {0xdeadbeef00000000ULL | I});
    ASSERT_TRUE(bool(R));
    EXPECT_EQ(*R, Expected[I]);
  }
  ok(Memory->write(Base + 0x1000, Input));
  ok(Memory->write(Base + 0x2000, std::array<uint8_t, 258>{}));
  ok(Memory->writeInteger(Base + 0x2100, 0xa5a5, 2));
  record(Src, 256, 0, Base + 0x1000);
  record(Dst, 0xffff, 256, Base + 0x2000);
  run();
  for (unsigned I = 0; I < 256; ++I)
    EXPECT_EQ(get(Base + 0x2000 + I), Expected[I]);
  EXPECT_EQ(get(Base + 0x2100, 2), 0xa5a5u);
  EXPECT_EQ(get(Dst, 2), 256u);
  EXPECT_EQ(get(Dst + 2, 2), 256u);
  EXPECT_EQ(get(Dst + 4, 4), 0x12345678u);
  EXPECT_EQ(get(Dst + 8, 8), Base + 0x2000);
  EXPECT_EQ(get(Src + 2, 2), 0u);
}
TEST_F(KernelAnsiUpper,
       TruncationAndZeroCountNeverReadUnusedBytesOrAddTerminator) {
  const uint64_t Tail = Base + 0x3ffd;
  ok(Memory->write(Tail, std::array<uint8_t, 3>{'a', 'z', 0}));
  ok(Memory->protect(Base + 0x4000, 4096, 0));
  record(Src, 7, 7, Tail);
  record(Dst, 0, 3, Base + 0x6000);
  ok(Memory->writeInteger(Base + 0x6003, 0xa5, 1));
  run();
  EXPECT_EQ(get(Base + 0x6000, 4), 0xa5005a41u);
  record(Dst, 0xeeee, 0, 0);
  record(Src, 0xffff, 0, 0);
  run();
  EXPECT_EQ(get(Dst, 2), 0u);
  record(Dst, 0xeeee, 16, 0);
  record(Src, 0, 0, 0);
  run();
  EXPECT_EQ(get(Dst, 2), 0u);
}
TEST_F(KernelAnsiUpper, NativeForwardOrderIncludesInPlaceAndBufferOverlap) {
  const uint8_t Text[] = {'a', 'B', 'c', 'D', 'e', 'z', 0xa5};
  const uint64_t Buffer = Base + 0x1000;
  ok(Memory->write(Buffer, Text));
  record(Src, 6, 6, Buffer);
  record(Dst, 0, 6, Buffer + 1);
  run();
  EXPECT_EQ(get(Buffer, 1), uint64_t('a'));
  for (unsigned I = 1; I < 7; ++I)
    EXPECT_EQ(get(Buffer + I), uint64_t('A'));
  ok(Memory->write(Buffer, Text));
  record(Src, 6, 6, Buffer);
  run(Src, Src);
  const uint8_t Upper[] = {'A', 'B', 'C', 'D', 'E', 'Z', 0xa5};
  for (unsigned I = 0; I < 7; ++I)
    EXPECT_EQ(get(Buffer + I), Upper[I]);
  ok(Memory->write(Buffer, Text));
  record(Dst, 0, 5, Buffer);
  record(Src, 5, 5, Buffer + 1);
  run();
  const uint8_t Backward[] = {'B', 'C', 'D', 'E', 'Z', 'z', 0xa5};
  for (unsigned I = 0; I < 7; ++I)
    EXPECT_EQ(get(Buffer + I), Backward[I]);
}
TEST_F(KernelAnsiUpper, ForwardOrderAlsoFollowsPhysicalAliases) {
  const uint64_t Alias = 0x61000000;
  ok(Memory->mapAlias(Alias, Base + 0x1000, 4096, Read | Write));
  ok(Memory->write(Base + 0x1100,
                   std::array<uint8_t, 7>{'a', 'B', 'c', 'D', 'e', 'z', 0xa5}));
  record(Src, 6, 6, Base + 0x1100);
  record(Dst, 0, 6, Alias + 0x101);
  run();
  for (unsigned I = 1; I < 7; ++I)
    EXPECT_EQ(get(Base + 0x1100 + I), uint64_t('A'));
}
TEST_F(KernelAnsiUpper,
       LengthIsPublishedBeforeAliasedBytesUsingCapturedPointers) {
  record(Dst, 0xa5a5, 4, Base + 0x1000);
  record(Src, 4, 4, Dst);
  run();
  EXPECT_EQ(get(Base + 0x1000, 4), 0x00040004u);
  ok(Memory->write(Base + 0x2000, std::array<uint8_t, 4>{'a', 'b', 'c', 'd'}));
  record(Src, 4, 4, Base + 0x2000);
  record(Dst, 0, 4, Dst);
  run();
  EXPECT_EQ(get(Dst, 4), 0x44434241u);
  EXPECT_EQ(get(Dst + 8, 8), Dst);
}
TEST_F(KernelAnsiUpper,
       OutputPermissionsAndMalformedPointersFailBeforePublication) {
  const uint64_t Input = Base + 0x1000, Output = Base + 0x2000;
  ok(Memory->write(Input, std::array<uint8_t, 3>{'a', 'b', 'c'}));
  record(Src, 3, 3, Input);
  record(Dst, 0xeeee, 3, Output);
  ok(Memory->writeInteger(Output, 0xa5a5a5a5, 4));
  ok(Memory->protect(Output, 4096, Read));
  reject();
  EXPECT_EQ(get(Dst, 2), 0xeeeeu);
  EXPECT_EQ(get(Output, 4), 0xa5a5a5a5u);
  ok(Memory->protect(Output, 4096, Read | Write));
  ok(Memory->protect(Base, 4096, Read));
  reject();
  EXPECT_EQ(get(Output, 4), 0xa5a5a5a5u);
  ok(Memory->protect(Base, 4096, Read | Write));
  record(Src, 3, 3, UINT64_MAX);
  reject();
  EXPECT_EQ(get(Dst, 2), 0xeeeeu);
  reject(0, Src);
  reject(Dst, 0);
  auto Pool = Model->call("ExAllocatePoolWithTag", {0, 16, 0x1234});
  ASSERT_TRUE(bool(Pool));
  auto Freed = Model->call("ExFreePool", {*Pool});
  ASSERT_TRUE(bool(Freed));
  record(Src, 3, 3, *Pool);
  reject();
  EXPECT_EQ(get(Dst, 2), 0xeeeeu);
}
TEST_F(KernelAnsiUpper, ReadOnlyMetadataAndFullUShortCapacityRemainValid) {
  const uint64_t Header = Base + 0x1ffe;
  record(Header, 0, 1, Base + 0x6000);
  record(Src, 1, 1, Base + 0x5000);
  ok(Memory->writeInteger(Base + 0x5000, 'q', 1));
  ok(Memory->protect(Base + 0x2000, 4096, Read));
  run(Header, Src);
  EXPECT_EQ(get(Header, 2), 1u);
  EXPECT_EQ(get(Base + 0x6000), uint64_t('Q'));
  std::vector<uint8_t> Bytes(65535, 'z');
  ok(Memory->write(Base + 0x10000, Bytes));
  ok(Memory->writeInteger(Base + 0x2ffff, 0xa5, 1));
  record(Src, 65535, 65535, Base + 0x10000);
  record(Dst, 0, 65535, Base + 0x20000);
  run();
  EXPECT_EQ(get(Dst, 2), 65535u);
  EXPECT_EQ(get(Base + 0x20000), uint64_t('Z'));
  EXPECT_EQ(get(Base + 0x2fffe), uint64_t('Z'));
  EXPECT_EQ(get(Base + 0x2ffff), 0xa5u);
}
TEST_F(KernelAnsiUpper, VersionedIRQLPolicyAndArgumentCountsStayExplicit) {
  record(Src, 0, 0, 0);
  record(Dst, 0xeeee, 0, 0);
  auto Raise = Model->call("KfRaiseIrql", {1});
  ASSERT_TRUE(bool(Raise));
  reject();
  auto Char = Model->call("RtlUpperChar", {'a'});
  ASSERT_FALSE(bool(Char));
  EXPECT_NE(llvm::toString(Char.takeError()).find("PASSIVE_LEVEL"),
            std::string::npos);
  EXPECT_EQ(get(Dst, 2), 0xeeeeu);
  auto Lower = Model->call("KeLowerIrql", {0});
  ASSERT_TRUE(bool(Lower));
  auto Missing = Model->call("RtlUpperString", {Dst});
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
}
TEST(DriverAnsiUpper, StaticAndDynamicCountedOperationsCompleteAndUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_ansi_upper.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
    }
}
} // namespace
} // namespace neverd::emulation
