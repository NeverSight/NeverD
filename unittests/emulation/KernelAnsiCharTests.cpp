//===- KernelAnsiCharTests.cpp - Single-character RTL conversion ---------===//
///
/// \file
/// Single-character RTL conversion.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
class KernelAnsiChar : public testing::Test {
protected:
  static constexpr uint64_t B = 0x60000000, Cell = B + 0x100,
                            Source = B + 0x2000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void SetUp() override {
    Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
    ok(Memory->map(B, 0x6000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, {}));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  void put(uint64_t At, uint64_t Value, unsigned Size = 8) {
    ok(Memory->writeInteger(At, Value, Size));
  }
  uint64_t get(uint64_t At, unsigned Size = 8) {
    return llvm::cantFail(Memory->readInteger(At, Size));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    auto R = Model->call(Name, A);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  std::string error(std::initializer_list<uint64_t> A) {
    auto R = Model->call("RtlAnsiCharToUnicodeChar", A);
    if (R) {
      ADD_FAILURE() << "unexpected conversion success";
      return {};
    }
    return llvm::toString(R.takeError());
  }
  void counted(uint64_t At, uint64_t Buffer, uint16_t Length, uint16_t Max) {
    put(At, Length, 2);
    put(At + 2, Max, 2);
    put(At + 8, Buffer);
  }
};
TEST_F(KernelAnsiChar, EveryASCIIByteMatchesCountedConversionsAndAdvancesOnce) {
  std::vector<uint8_t> Bytes;
  for (unsigned V = 0; V < 128; ++V)
    Bytes.push_back(uint8_t(V));
  ok(Memory->write(Source, Bytes));
  constexpr auto A = B + 0x200, W = B + 0x300, Back = B + 0x400,
                 Wide = B + 0x3000, Narrow = B + 0x4000;
  counted(A, Source, 128, 128);
  counted(W, Wide, 7, 512);
  counted(Back, Narrow, 9, 256);
  EXPECT_EQ(call("RtlAnsiStringToUnicodeString", {W, A, 0}), 0u);
  EXPECT_EQ(get(W, 2), 256u);
  put(Cell, Source);
  for (unsigned V = 0; V < 128; ++V) {
    SCOPED_TRACE(V);
    EXPECT_EQ(call("RtlAnsiCharToUnicodeChar", {Cell}), V);
    EXPECT_EQ(get(Cell), Source + V + 1);
    EXPECT_EQ(get(Wide + V * 2, 2), V);
    EXPECT_EQ(get(Source + V, 1), V);
  }
  EXPECT_EQ(call("RtlUnicodeStringToAnsiString", {Back, W, 0}), 0u);
  EXPECT_EQ(get(Back, 2), 128u);
  std::vector<uint8_t> Actual(128);
  ok(Memory->read(Narrow, Actual));
  EXPECT_EQ(Actual, Bytes);
}
TEST_F(KernelAnsiChar, ReadOnlyLastSourceByteNeedsNoFollowingByteOrTerminator) {
  constexpr auto Last = B + 0x2fff;
  ok(Memory->protect(B + 0x3000, 4096, 0));
  for (uint8_t V : {uint8_t(0), uint8_t(0x5c), uint8_t(0x7f)}) {
    ok(Memory->protect(Source, 4096, Read | Write));
    put(Last, V, 1);
    put(Cell, Last);
    ok(Memory->protect(Source, 4096, Read));
    EXPECT_EQ(call("RtlAnsiCharToUnicodeChar", {Cell}), V);
    EXPECT_EQ(get(Cell), Last + 1);
    EXPECT_EQ(get(Last, 1), V);
  }
}
TEST_F(KernelAnsiChar, SelfAndBackingAliasesCaptureBeforePointerWrite) {
  constexpr auto Alias = 0x61000000;
  ok(Memory->mapAlias(Alias, B, 4096, Read | Write));
  for (bool Aliased : {false, true})
    for (unsigned Offset : {0, 0x40, 0x41, 0x7f}) {
      const auto At = B + Offset, Target = (Aliased ? Alias : B) + Offset;
      put(At, Target);
      EXPECT_EQ(call("RtlAnsiCharToUnicodeChar", {At}), Offset);
      EXPECT_EQ(get(At), Target + 1);
      EXPECT_EQ(get(Alias + Offset), Target + 1);
    }
}
TEST_F(KernelAnsiChar, ReadOnlyAndSplitPointerCellsPreserveAllBytes) {
  put(Source, 'A', 1);
  put(Cell, Source);
  ok(Memory->protect(B, 4096, Read));
  EXPECT_FALSE(error({Cell}).empty());
  EXPECT_EQ(get(Cell), Source);
  EXPECT_EQ(get(Source, 1), 'A');
  ok(Memory->protect(B, 4096, Read | Write));
  constexpr auto Split = B + 4092;
  put(Split, Source);
  ok(Memory->protect(B + 4096, 4096, Read));
  EXPECT_FALSE(error({Split}).empty());
  EXPECT_EQ(get(Split), Source);
}
TEST_F(KernelAnsiChar, UnsupportedBytesAndBadSourceNeverConsumeThePointer) {
  for (unsigned V : {0x80, 0x81, 0xa0, 0xff}) {
    put(Source, V, 1);
    put(Cell, Source);
    EXPECT_NE(error({Cell}).find("code page"), std::string::npos);
    EXPECT_EQ(get(Cell), Source);
    EXPECT_EQ(get(Source, 1), V);
  }
  constexpr uint64_t BadAddresses[] = {0, 0x800000000000ULL, UINT64_MAX};
  for (uint64_t Address : BadAddresses) {
    put(Cell, Address);
    EXPECT_FALSE(error({Cell}).empty());
    EXPECT_EQ(get(Cell), Address);
  }
  EXPECT_FALSE(error({0}).empty());
}
TEST_F(KernelAnsiChar,
       CountedAndCharacterConversionsShareTheMissingPageBoundary) {
  constexpr auto A = B + 0x200, W = B + 0x300, Wide = B + 0x3000;
  counted(A, Source, 1, 128);
  counted(W, Wide, 6, 128);
  put(Source, 0x80, 1);
  put(Cell, Source);
  auto R = Model->call("RtlAnsiStringToUnicodeString", {W, A, 0});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("code page"), std::string::npos);
  EXPECT_EQ(get(W, 2), 6u);
  put(Wide, 0x100, 2);
  counted(W, Wide, 2, 128);
  counted(A, Source, 9, 128);
  R = Model->call("RtlUnicodeStringToAnsiString", {A, W, 0});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("code page"), std::string::npos);
  EXPECT_EQ(get(A, 2), 9u);
}
TEST_F(KernelAnsiChar, APCIsSupportedAndDispatchAndWrongArityAreRejected) {
  EXPECT_EQ(Model->argumentCount("RtlAnsiCharToUnicodeChar"), 1u);
  put(Source, 'Z', 1);
  put(Cell, Source);
  call("KfRaiseIrql", {1});
  EXPECT_EQ(call("RtlAnsiCharToUnicodeChar", {Cell}), 'Z');
  call("KeLowerIrql", {0});
  put(Cell, Source);
  call("KfRaiseIrql", {2});
  EXPECT_NE(error({Cell}).find("IRQL"), std::string::npos);
  EXPECT_EQ(get(Cell), Source);
  call("KeLowerIrql", {0});
  EXPECT_FALSE(error({}).empty());
  EXPECT_FALSE(error({Cell, Cell}).empty());
}
TEST(DriverAnsiChar, StaticAndDynamicClientsCompleteAndUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{31, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_ansi_char.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted);
    }
}
} // namespace
} // namespace neverd::emulation
