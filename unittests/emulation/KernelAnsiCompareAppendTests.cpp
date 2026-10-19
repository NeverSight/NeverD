///
/// \file
/// KernelAnsiCompareAppendTests.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include <array>
#include <string_view>

namespace neverd::emulation {
namespace {
class KernelAnsiOperations : public testing::Test {
protected:
  static constexpr uint64_t Base = 0x60000000, Left = Base,
                            Right = Base + 0x100;
  static constexpr uint64_t A = Base + 0x1000, B = Base + 0x2000;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;
  void ok(llvm::Error E) {
    ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
  }
  void SetUp() override {
    auto M = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(M));
    Memory = std::move(*M);
    ok(Memory->map(Base, 0x20000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage I;
    I.Base = 0x180000000;
    I.Entry = I.Base + 4096;
    I.Size = 0x3000;
    ok(Model->initialize(I, DriverOptions{}));
    Model->enterExecution(profile::StackBase);
  }
  void record(uint64_t P, uint16_t L, uint16_t C, uint64_t Buffer) {
    ok(Memory->writeInteger(P, L, 2));
    ok(Memory->writeInteger(P + 2, C, 2));
    ok(Memory->writeInteger(P + 4, 0x12345678, 4));
    ok(Memory->writeInteger(P + 8, Buffer, 8));
  }
  void bytes(uint64_t P, std::string_view Text) {
    ok(Memory->write(
        P, llvm::ArrayRef(reinterpret_cast<const uint8_t *>(Text.data()),
                          Text.size())));
  }
  uint64_t get(uint64_t P, unsigned N = 1) {
    auto R = Memory->readInteger(P, N);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    auto R = Model->call(Name, Args);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return UINT64_MAX;
    }
    return *R;
  }
  void reject(const char *Name, std::initializer_list<uint64_t> Args) {
    auto R = Model->call(Name, Args);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
};
TEST_F(KernelAnsiOperations,
       CompareMatchesIndependentNativeCasesAndIgnoresCapacity) {
#define NEVERD_ANSI_COMPARE_CASE(Name, L, LL, R, RL, Flag, Expected)           \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    bytes(A, std::string_view(L, sizeof(L) - 1));                              \
    bytes(B, std::string_view(R, sizeof(R) - 1));                              \
    record(Left, LL, 0, A);                                                    \
    record(Right, RL, 0, B);                                                   \
    EXPECT_EQ(call("RtlCompareString", {Left, Right, Flag}),                   \
              uint64_t(uint32_t(int32_t(Expected))));                          \
  }
#include "AnsiCompareCases.def"
#undef NEVERD_ANSI_COMPARE_CASE
  EXPECT_EQ(get(Left + 4, 4), 0x12345678u);
}
TEST_F(KernelAnsiOperations,
       CompareStopsBeforeUnreadableTailsAndUsesLowByteBoolean) {
  const auto TailA = A + 4095, TailB = B + 0x3fff;
  ok(Memory->writeInteger(TailA, 'a', 1));
  ok(Memory->writeInteger(TailB, 'A', 1));
  record(Left, 4, 0, TailA);
  record(Right, 4, 0, TailB);
  ok(Memory->protect(A + 4096, 4096, 0));
  ok(Memory->protect(B + 0x4000, 4096, 0));
  EXPECT_EQ(call("RtlCompareString", {Left, Right, 0x100}), 32u);
  reject("RtlCompareString", {Left, Right, 0x101});
}
TEST_F(KernelAnsiOperations,
       CompareAllByteCaseFoldingMatchesCapturedScalarTruth) {
#define NEVERD_ANSI_UPPER_EXPECTED(...)                                        \
  constexpr uint8_t Upper[] = {__VA_ARGS__};
#include "AnsiUpperExpected.def"
#undef NEVERD_ANSI_UPPER_EXPECTED
  record(Left, 1, 0, A);
  record(Right, 1, 0, B);
  for (unsigned I = 0; I < 256; ++I) {
    ok(Memory->writeInteger(A, I, 1));
    ok(Memory->writeInteger(B, Upper[I], 1));
    EXPECT_EQ(call("RtlCompareString", {Left, Right, 1}), 0u);
  }
}
TEST_F(KernelAnsiOperations,
       AppendCapacityFailurePreservesFieldsAndDoesNotReadBuffers) {
  for (uint16_t Capacity : {0, 1, 2, 4, 5, 6, 16}) {
    ok(Memory->write(A, std::array<uint8_t, 8>{'a', 'b', 0xa5, 0xa5, 0xa5, 0xa5,
                                               0xa5, 0xa5}));
    bytes(B, std::string_view("C\0D", 3));
    record(Left, 2, Capacity, A);
    record(Right, 3, 0, B);
    EXPECT_EQ(call("RtlAppendStringToString", {Left, Right}),
              Capacity >= 5 ? 0u : 0xc0000023u);
    EXPECT_EQ(get(Left, 2), Capacity >= 5 ? 5u : 2u);
    EXPECT_EQ(get(Left + 2, 2), Capacity);
    EXPECT_EQ(get(Left + 4, 4), 0x12345678u);
    EXPECT_EQ(get(A + 5), 0xa5u);
    if (Capacity >= 5) {
      EXPECT_EQ(get(A + 2), 'C');
      EXPECT_EQ(get(A + 3), 0u);
      EXPECT_EQ(get(A + 4), 'D');
    } else
      EXPECT_EQ(get(A + 2), 0xa5u);
  }
  record(Left, 65535, 65535, UINT64_MAX);
  record(Right, 1, 0, UINT64_MAX);
  ok(Memory->protect(Base, 4096, Read));
  EXPECT_EQ(call("RtlAppendStringToString", {Left, Right}), 0xc0000023u);
  EXPECT_EQ(get(Left, 2), 65535u);
}
TEST_F(KernelAnsiOperations, EmptyAppendReadsOnlyTheSourceLength) {
  const auto Tail = Base + 0x4ffe;
  ok(Memory->writeInteger(Tail, 0, 2));
  ok(Memory->protect(Base + 0x5000, 4096, 0));
  EXPECT_EQ(call("RtlAppendStringToString", {0, Tail}), 0u);
  EXPECT_EQ(call("RtlAppendStringToString", {UINT64_MAX, Tail}), 0u);
  reject("RtlAppendStringToString", {0, 0});
}
TEST_F(KernelAnsiOperations, AppendOverlapAndHeaderAliasUsePostCopyLength) {
  bytes(A, "abcdeX");
  record(Left, 1, 6, A);
  record(Right, 5, 0, A);
  EXPECT_EQ(call("RtlAppendStringToString", {Left, Right}), 0u);
  const char Expected[] = "aabcde";
  for (unsigned I = 0; I < 6; ++I)
    EXPECT_EQ(get(A + I), uint8_t(Expected[I]));
  bytes(A, "abc***");
  record(Left, 3, 6, A);
  EXPECT_EQ(call("RtlAppendStringToString", {Left, Left}), 0u);
  for (unsigned I = 0; I < 6; ++I)
    EXPECT_EQ(get(A + I), uint8_t("abcabc"[I]));
  bytes(B, "AB");
  record(Left, 0, 2, Left);
  record(Right, 2, 0, B);
  EXPECT_EQ(call("RtlAppendStringToString", {Left, Right}), 0u);
  EXPECT_EQ(get(Left, 2), 0x4243u);
  EXPECT_EQ(get(Left + 2, 2), 2u);
}
TEST_F(KernelAnsiOperations, AppendBadMemoryFailsBeforeCopyOrLengthChanges) {
  bytes(A, "x****");
  bytes(B, "ab");
  record(Left, 1, 8, A);
  record(Right, 2, 0, B);
  ok(Memory->protect(A, 4096, Read));
  reject("RtlAppendStringToString", {Left, Right});
  EXPECT_EQ(get(Left, 2), 1u);
  EXPECT_EQ(get(A + 1), '*');
  ok(Memory->protect(A, 4096, Read | Write));
  ok(Memory->protect(Base, 4096, Read));
  reject("RtlAppendStringToString", {Left, Right});
  EXPECT_EQ(get(A + 1), '*');
  ok(Memory->protect(Base, 4096, Read | Write));
  record(Left, 4, 8, UINT64_MAX - 1);
  reject("RtlAppendStringToString", {Left, Right});
  EXPECT_EQ(get(Left, 2), 4u);
  auto Pool = call("ExAllocatePoolWithTag", {0, 16, 0x1234});
  call("ExFreePool", {Pool});
  record(Left, 1, 8, A);
  record(Right, 2, 0, Pool);
  reject("RtlAppendStringToString", {Left, Right});
  EXPECT_EQ(get(A + 1), '*');
}
TEST_F(KernelAnsiOperations, CompareAndAppendKeepTheirDistinctIRQLContracts) {
  record(Left, 0, 0, 0);
  record(Right, 0, 0, 0);
  call("KfRaiseIrql", {1});
  EXPECT_EQ(call("RtlAppendStringToString", {Left, Right}), 0u);
  reject("RtlCompareString", {Left, Right, 0});
  call("KfRaiseIrql", {2});
  reject("RtlAppendStringToString", {Left, Right});
  call("KeLowerIrql", {1});
  call("KeLowerIrql", {0});
  reject("RtlCompareString", {Left, Right});
  reject("RtlAppendStringToString", {Left});
}
TEST(DriverAnsiOperations,
     StaticAndDynamicCompareAppendClientsCompleteAndUnload) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true}) {
      DriverOptions O;
      O.Contract = Contract;
      O.Unload = true;
      if (Preemptive)
        O.Scheduling = DriverScheduling{23, 1};
      auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                 "driver_ansi_operations.sys",
                             O);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      EXPECT_EQ(R->NTStatus, 0u) << R->Diagnostic;
      EXPECT_TRUE(R->UnloadCompleted) << R->Diagnostic;
      EXPECT_EQ(R->Stop, DriverStopReason::Returned) << R->Diagnostic;
    }
}
} // namespace
} // namespace neverd::emulation
