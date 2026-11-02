//===- CPUArchitectureTests.cpp - Multi-ISA execution contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <cstdlib>

namespace neverd::emulation {
namespace {
#define NEVERD_CPU_TEST_CODE(Name, ...)                                        \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#define NEVERD_CPU_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_CPU_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "CPUArchitectureCases.def"
#undef NEVERD_CPU_TEST_CODE
#undef NEVERD_CPU_TEST_VALUE
#undef NEVERD_CPU_TEST_TEXT

template <typename T, typename U>
void expectValue(llvm::Expected<T> Actual, const U &Expected) {
  ASSERT_TRUE(bool(Actual)) << llvm::toString(Actual.takeError());
  EXPECT_EQ(*Actual, Expected);
}

llvm::Error code(ExecutionBackend &CPU, llvm::ArrayRef<uint32_t> Words,
                 uint64_t Address = Code) {
  std::vector<uint8_t> Bytes(Words.size() * sizeof(uint32_t));
  for (size_t N = 0; N < Words.size(); ++N)
    llvm::support::endian::write32le(Bytes.data() + N * sizeof(uint32_t),
                                     Words[N]);
  return CPU.write(Address, Bytes);
}
llvm::Error steps(ExecutionBackend &CPU, unsigned Count,
                  uint64_t Address = Code) {
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) {
    if (Seen++ == Count)
      CPU.stop();
  };
  if (auto E = CPU.installHooks(std::move(Hooks)))
    return E;
  auto Result = CPU.run(Address, Timeout);
  llvm::consumeError(CPU.installHooks({}));
  return Result;
}
#if defined(_WIN32)
constexpr auto Native = ExecutionBackendKind::WHP;
#elif defined(__APPLE__)
constexpr auto Native = ExecutionBackendKind::HVF;
#else
constexpr auto Native = ExecutionBackendKind::KVM;
#endif
class AArch64CPU : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto B =
        createExecutionBackend(GetParam(), ExecutionContract::CheckedAArch64,
                               Limit, GuestArchitecture::AArch64);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !std::getenv(RequireNative) &&
          !requireHvf(GetParam(), GuestArchitecture::AArch64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    ASSERT_EQ(llvm::toString(CPU->map(Code, PageSize, Read | Write | Execute)),
              "");
    ASSERT_EQ(llvm::toString(CPU->map(Data, PageSize, Read | Write)), "");
    ASSERT_EQ(llvm::toString(CPU->map(Stack, PageSize, Read | Write)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X1, Data)), "");
    ASSERT_EQ(
        llvm::toString(CPU->setReg(AArch64Register::SP, Stack + PageSize)), "");
  }
};
TEST_P(AArch64CPU, ExecutesIntegerBranchAndMemoryAtHighAddresses) {
  ASSERT_EQ(llvm::toString(code(*CPU, Arithmetic)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 7)), "");
  expectValue(CPU->reg(AArch64Register::X0), ArithmeticValue);
  expectValue(CPU->reg(AArch64Register::X2), ArithmeticValue);
  expectValue(CPU->reg(AArch64Register::X3), 0u);
  expectValue(CPU->reg(AArch64Register::X4), 7u);
  expectValue(CPU->reg(AArch64Register::NZCV), ArithmeticFlags);
  expectValue(CPU->readInteger(Data, sizeof(uint64_t)), ArithmeticValue);
}
TEST_P(AArch64CPU, PreservesLinkRegisterAndReturnControlFlow) {
  ASSERT_EQ(llvm::toString(code(*CPU, Call)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 5)), "");
  expectValue(CPU->reg(AArch64Register::X0), 12u);
  expectValue(CPU->reg(AArch64Register::X30), Code + 2 * sizeof(uint32_t));
  expectValue(CPU->reg(AArch64Register::PC), Code + 5 * sizeof(uint32_t));
}
TEST_P(AArch64CPU, PairWritebackAndZeroExtendedRegisters) {
  ASSERT_EQ(llvm::toString(code(*CPU, PairStack)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X2, Updated)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 2)), "");
  expectValue(CPU->reg(AArch64Register::SP), Stack + PageSize);
  expectValue(CPU->reg(AArch64Register::X3), Value);
  expectValue(CPU->reg(AArch64Register::X4), Updated);
}
TEST_P(AArch64CPU, IndexedMemoryUsesTheArchitecturalAddress) {
  ASSERT_EQ(llvm::toString(code(*CPU, Indexed)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X2, 3)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 2)), "");
  expectValue(CPU->reg(AArch64Register::X3), uint32_t(Value));
  expectValue(CPU->readInteger(Data + 3 * sizeof(uint32_t), sizeof(uint32_t)),
              uint32_t(Value));
}
TEST_P(AArch64CPU, SignedLoadsPreserveRegisterWidthSemantics) {
  ASSERT_EQ(llvm::toString(code(*CPU, SignedLoads)), "");
  ASSERT_EQ(
      llvm::toString(CPU->writeInteger(Data, UINT64_MAX, sizeof(uint64_t))),
      "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 3)), "");
  expectValue(CPU->reg(AArch64Register::X0), UINT64_MAX);
  expectValue(CPU->reg(AArch64Register::X2), UINT32_MAX);
  expectValue(CPU->reg(AArch64Register::X3), UINT64_MAX);
}
TEST_P(AArch64CPU, ObserverStopsStoreBeforeEffects) {
  ASSERT_EQ(llvm::toString(code(*CPU, Store)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  BackendHooks H;
  H.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
    EXPECT_EQ(A, Data);
    EXPECT_EQ(N, sizeof(uint64_t));
    EXPECT_EQ(V, Value);
    CPU->stop();
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(H))), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, Timeout)), "");
  expectValue(CPU->reg(AArch64Register::PC), Code);
  expectValue(CPU->readInteger(Data, sizeof(uint64_t)), 0u);
}
TEST_P(AArch64CPU, PairFaultPrecedesBothWritesAndCannotBeRestoredAway) {
  ASSERT_EQ(llvm::toString(code(*CPU, PairStore)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X1,
                                       Data + PageSize - sizeof(uint64_t))),
            "");
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(bool(Saved));
  EXPECT_NE(llvm::toString(CPU->run(Code, Timeout)), "");
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::UnmappedMemory);
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  ASSERT_EQ(llvm::toString(
                CPU->snapshotBacking(Data + PageSize - Bytes.size(), Bytes)),
            "");
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), 0u);
  EXPECT_NE(llvm::toString(CPU->restoreContext(**Saved)), "");
}
TEST_P(AArch64CPU, ContextRestoresRegistersAgainstCurrentAliasMappings) {
  ASSERT_EQ(llvm::toString(code(*CPU, Load)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, PageSize, Read)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, Value, sizeof(uint64_t))),
            "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Stack, Updated, sizeof(uint64_t))),
            "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X1, Alias)), "");
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(bool(Saved));
  for (unsigned N = 0; N < Remaps; ++N) {
    const uint64_t Source = N % 2 ? Stack : Data;
    ASSERT_EQ(llvm::toString(CPU->replaceAliases(
                  {{Alias, PageSize}}, {{Alias, Source, PageSize, Read}})),
              "");
    ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
    ASSERT_EQ(llvm::toString(steps(*CPU, 1)), "");
    expectValue(CPU->reg(AArch64Register::X0), N % 2 ? Updated : Value);
  }
}
TEST_P(AArch64CPU, ChangedCodeInvalidatesCachedInstructions) {
  ASSERT_EQ(llvm::toString(code(*CPU, Load)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 1)), "");
  ASSERT_EQ(llvm::toString(code(*CPU, Modified)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 1)), "");
  expectValue(CPU->reg(AArch64Register::X0), 123u);
}
TEST_P(AArch64CPU, UnsupportedSystemInstructionStopsBeforeEffect) {
  ASSERT_EQ(llvm::toString(code(*CPU, System)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  EXPECT_NE(llvm::toString(CPU->run(Code, Timeout)), "");
  expectValue(CPU->reg(AArch64Register::X0), Value);
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::InvalidInstruction);
}
TEST_P(AArch64CPU, LoopHonorsDeadline) {
  ASSERT_EQ(llvm::toString(code(*CPU, Loop)), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, ShortTimeout)), "");
  EXPECT_TRUE(CPU->timedOut());
}
INSTANTIATE_TEST_SUITE_P(Checked, AArch64CPU,
                         testing::Values(ExecutionBackendKind::Unicorn,
                                         Native));

TEST(CPUSelection, MatchingGuestUsesTheNativeHostBackend) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) ||          \
    defined(_M_ARM64)
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto ISA = GuestArchitecture::X64;
  constexpr auto Contract = ExecutionContract::CheckedX64;
#else
  constexpr auto ISA = GuestArchitecture::AArch64;
  constexpr auto Contract = ExecutionContract::CheckedAArch64;
#endif
  auto B =
      createExecutionBackend(ExecutionBackendKind::Auto, Contract, Limit, ISA);
  if (!B) {
    auto E = B.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !std::getenv(RequireNative) && !requireHvf(Native, ISA))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
  EXPECT_EQ(B->Kind, Native);
#else
  EXPECT_EQ(B->Kind, ExecutionBackendKind::Unicorn);
#endif
#else
  GTEST_SKIP();
#endif
}

TEST(UnicornAArch64, CheckedRegisterPacketsPreserveEveryLaneAndRestoration) {
  constexpr uint64_t PacketCode = 0x100000, PacketStack = 0x200000;
  constexpr uint64_t PacketData = 0x300000;
  for (bool User : {false, true}) {
    SCOPED_TRACE(User);
    auto Backend =
        createExecutionBackend(ExecutionBackendKind::Unicorn,
                               User ? ExecutionContract::CheckedUserAArch64
                                    : ExecutionContract::CheckedAArch64,
                               Limit, GuestArchitecture::AArch64);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    auto &CPU = *Backend->CPU;
    const unsigned Rights =
        Read | Write | Execute | (User ? unsigned(UserAccessible) : 0u);
    llvm::cantFail(CPU.map(PacketCode, PageSize, Rights));
    llvm::cantFail(CPU.map(PacketStack, PageSize, Rights & ~Execute));
    // ADD X0, X0, #1; the NOP is the pre-effect stop marker.
    llvm::cantFail(code(CPU, {0x91000400, 0xd503201f}, PacketCode));
    for (unsigned Round = 0; Round < 3; ++Round) {
      SCOPED_TRACE(Round);
      std::array<uint64_t, unsigned(AArch64Register::FPSR) + 1> Scalars{};
      std::array<RegisterValue, 32> Vectors{};
      for (unsigned I = 0; I <= unsigned(AArch64Register::X30); ++I)
        Scalars[I] = 0x1122334455660000ULL + 0x1000 * Round + I;
      Scalars[unsigned(AArch64Register::SP)] = PacketStack + PageSize - 16;
      Scalars[unsigned(AArch64Register::PC)] = PacketCode;
      Scalars[unsigned(AArch64Register::NZCV)] = 0xb0000000;
      Scalars[unsigned(AArch64Register::TPIDR_EL0)] = PacketData + Round;
      Scalars[unsigned(AArch64Register::TPIDRRO_EL0)] = PacketData + 16 + Round;
      Scalars[unsigned(AArch64Register::TPIDR_EL1)] = PacketData + 32 + Round;
      Scalars[unsigned(AArch64Register::FPCR)] = 0x01c00000;
      Scalars[unsigned(AArch64Register::FPSR)] = 0x08000001;
      for (unsigned I = 0; I < Scalars.size(); ++I)
        llvm::cantFail(CPU.setReg(AArch64Register(I), Scalars[I]));
      for (unsigned I = 0; I < Vectors.size(); ++I) {
        Vectors[I] = {0x0123456789abcdefULL + 0x100 * Round + I,
                      0xfedcba9876543210ULL - 0x100 * Round - I};
        llvm::cantFail(CPU.setVector(I, Vectors[I]));
      }
      auto Saved = llvm::cantFail(CPU.saveContext());
      ++Scalars[unsigned(AArch64Register::X0)];
      Scalars[unsigned(AArch64Register::PC)] += 4;
      for (unsigned Retry = 0; Retry < 2; ++Retry) {
        llvm::cantFail(CPU.restoreContext(*Saved));
        ASSERT_EQ(llvm::toString(steps(CPU, 1, PacketCode)), "");
        for (unsigned I = 0; I < Scalars.size(); ++I)
          EXPECT_EQ(llvm::cantFail(CPU.reg(AArch64Register(I))), Scalars[I])
              << I;
        for (unsigned I = 0; I < Vectors.size(); ++I)
          EXPECT_EQ(llvm::cantFail(CPU.vector(I)), Vectors[I]) << I;
      }
    }
  }
}

TEST(UnicornAArch64, ExecutesSIMDAndTLSAndRestoresCPUOnly) {
  auto B = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                  ExecutionContract::Software, Limit,
                                  GuestArchitecture::AArch64);
  ASSERT_TRUE(bool(B)) << llvm::toString(B.takeError());
  auto &CPU = *B->CPU;
  ASSERT_EQ(llvm::toString(CPU.map(Code, PageSize, Read | Write | Execute)),
            "");
  ASSERT_EQ(llvm::toString(CPU.map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(code(CPU, CurrentEL)), "");
  ASSERT_EQ(llvm::toString(steps(CPU, 1)), "");
  expectValue(CPU.reg(AArch64Register::X0), 4u);
  ASSERT_EQ(llvm::toString(code(CPU, SIMD)), "");
  ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::X3, Value)), "");
  ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::TPIDR_EL0, Data)), "");
  ASSERT_EQ(llvm::toString(steps(CPU, 4)), "");
  ASSERT_FALSE(CPU.fault()) << std::hex << (CPU.fault() ? CPU.fault()->PC : 0);
  expectValue(CPU.vector(0), (RegisterValue{SIMDPattern, SIMDPattern}));
  expectValue(CPU.vector(1), (RegisterValue{}));
  expectValue(CPU.vector(2), (RegisterValue{Value, 0}));
  expectValue(CPU.reg(AArch64Register::X4), Data);
  auto Saved = CPU.saveContext();
  ASSERT_TRUE(bool(Saved));
  ASSERT_EQ(llvm::toString(CPU.setVector(0, {})), "");
  ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::TPIDR_EL0, Stack)), "");
  ASSERT_EQ(llvm::toString(CPU.writeInteger(Data, Updated, sizeof(uint64_t))),
            "");
  ASSERT_EQ(llvm::toString(CPU.restoreContext(**Saved)), "");
  expectValue(CPU.vector(0), (RegisterValue{SIMDPattern, SIMDPattern}));
  expectValue(CPU.reg(AArch64Register::TPIDR_EL0), Data);
  expectValue(CPU.readInteger(Data, sizeof(uint64_t)), Updated);
  auto Invalid = CPU.reg(X64Register::AX);
  EXPECT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  EXPECT_NE(llvm::toString(CPU.setXmm(0, {})), "");
}
TEST(CPUSelection, GuestAndContractAreIndependentOfTheHostOS) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    auto B = createExecutionBackend(ExecutionBackendKind::Auto,
                                    ExecutionContract::Software, Limit, ISA);
    ASSERT_TRUE(bool(B)) << llvm::toString(B.takeError());
    EXPECT_EQ(B->Kind, ExecutionBackendKind::Unicorn);
    EXPECT_EQ(B->CPU->architecture(), ISA);
  }
  auto Mismatch = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                         ExecutionContract::CheckedX64, Limit,
                                         GuestArchitecture::AArch64);
  ASSERT_FALSE(bool(Mismatch));
  llvm::consumeError(Mismatch.takeError());
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto Guest = GuestArchitecture::AArch64;
  constexpr auto Contract = ExecutionContract::CheckedAArch64;
#elif defined(__aarch64__) || defined(_M_ARM64)
  constexpr auto Guest = GuestArchitecture::X64;
  constexpr auto Contract = ExecutionContract::CheckedX64;
#else
  return;
#endif
  auto Cross = createExecutionBackend(ExecutionBackendKind::Auto, Contract,
                                      Limit, Guest);
  ASSERT_TRUE(bool(Cross)) << llvm::toString(Cross.takeError());
  EXPECT_EQ(Cross->Kind, ExecutionBackendKind::Unicorn);
  EXPECT_EQ(Cross->Reason, execution::CrossISASelection);
  auto Hardware = createExecutionBackend(Native, Contract, Limit, Guest);
  ASSERT_FALSE(bool(Hardware));
  auto E = Hardware.takeError();
  EXPECT_TRUE(E.isA<BackendUnavailableError>());
  llvm::consumeError(std::move(E));
}
} // namespace
} // namespace neverd::emulation
