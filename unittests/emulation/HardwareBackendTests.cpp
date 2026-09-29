//===- HardwareBackendTests.cpp - Hardware execution tests---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64Machine.h"
#include "core/BackendRegistry.h"
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
#define NEVERD_HARDWARE_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_HARDWARE_TEST_CODE(Name, ...)                                   \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_HARDWARE_DRIVER_CASE(Name, Text) constexpr char Name[] = Text;
#include "HardwareBackendCases.def"
#undef NEVERD_HARDWARE_TEST_VALUE
#undef NEVERD_HARDWARE_TEST_CODE
#undef NEVERD_HARDWARE_DRIVER_CASE
class HardwareBackend : public ::testing::Test {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto B = createExecutionBackend(ExecutionBackendKind::Auto,
                                    ExecutionContract::CheckedX64,
                                    MemoryPages * x64::PageSize);
    if (!B) {
      auto Error = B.takeError();
      const bool Unavailable = Error.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(Error));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    ASSERT_EQ(
        llvm::toString(CPU->map(Code, x64::PageSize, Read | Write | Execute)),
        "");
    ASSERT_EQ(llvm::toString(CPU->map(Data, x64::PageSize, Read | Write)), "");
    ASSERT_EQ(llvm::toString(CPU->map(Stack, x64::PageSize, Read | Write)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(
                  X64Register::SP, Stack + x64::PageSize - x64::WordBytes)),
              "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CX, Data)), "");
  }
  llvm::Error steps(unsigned Count) {
    unsigned Seen = 0;
    BackendHooks H;
    H.Instruction = [&](uint64_t, uint32_t) {
      if (Seen++ == Count)
        CPU->stop();
    };
    if (auto E = CPU->installHooks(std::move(H)))
      return E;
    auto E = CPU->run(Code, Timeout);
    llvm::consumeError(CPU->installHooks({}));
    return E;
  }
};
TEST_F(HardwareBackend, ExecutesHighVirtualCodeAndSharedBacking) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, StoreLoad)), "");
  unsigned Writes = 0, Instructions = 0;
  BackendHooks H;
  H.Instruction = [&](uint64_t, uint32_t) {
    if (Instructions++ == 3)
      CPU->stop();
  };
  H.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
    EXPECT_EQ(A, Data);
    EXPECT_EQ(N, x64::WordBytes);
    EXPECT_EQ(V, Value);
    ++Writes;
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(H))), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, Timeout)), "");
  EXPECT_EQ(*CPU->reg(X64Register::DX), Value);
  EXPECT_EQ(*CPU->readInteger(Data, x64::WordBytes), Value);
  EXPECT_EQ(Writes, 1u);
}

TEST_F(HardwareBackend, ObserverRejectsWriteBeforeEffect) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Store)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, Value)), "");
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { CPU->stop(); };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(H))), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, Timeout)), "");
  EXPECT_EQ(*CPU->readInteger(Data, x64::WordBytes), 0u);
  EXPECT_EQ(*CPU->reg(X64Register::PC), Code);
}

TEST_F(HardwareBackend, ObserverRejectsReadBeforeEffect) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Load)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, Value, x64::WordBytes)), "");
  BackendHooks H;
  H.Read = [&](uint64_t, uint32_t) { CPU->stop(); };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(H))), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, Timeout)), "");
  EXPECT_EQ(*CPU->reg(X64Register::AX), 0u);
  EXPECT_EQ(*CPU->reg(X64Register::PC), Code);
}

TEST_F(HardwareBackend, NativeFlagsExcludeSingleStepMachinery) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Arithmetic)), "");
  ASSERT_EQ(llvm::toString(steps(2)), "");
  EXPECT_EQ(*CPU->reg(X64Register::AX), 0u);
  EXPECT_EQ(*CPU->reg(X64Register::FLAGS), ArithmeticFlags);
  EXPECT_NE(llvm::toString(CPU->setReg(X64Register::FLAGS,
                                       x64::InitialFlags | x64::TrapFlag)),
            "");
}

TEST_F(HardwareBackend, CrossPageStoreIsRejectedBeforeAnyEffect) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Store)), "");
  ASSERT_EQ(llvm::toString(
                CPU->map(Data + x64::PageSize, x64::PageSize, Read | Write)),
            "");
  const uint64_t Address = Data + x64::PageSize - 1;
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CX, Address)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, Value)), "");
  EXPECT_NE(llvm::toString(CPU->run(Code, Timeout)), "");
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::InvalidInstruction);
  std::array<uint8_t, x64::WordBytes> Bytes{};
  ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Address, Bytes)), "");
  EXPECT_EQ(Bytes, (std::array<uint8_t, x64::WordBytes>{}));
}

TEST_F(HardwareBackend, FailedAliasReplacementPreservesOriginalMapping) {
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, x64::PageSize, Read)),
            "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, Value, x64::WordBytes)), "");
  EXPECT_NE(llvm::toString(CPU->replaceAliases(
                {GuestAliasRange{Alias, x64::PageSize}},
                {GuestAliasMapping{Alias, Stack + x64::PageSize, x64::PageSize,
                                   Read}})),
            "");
  EXPECT_EQ(*CPU->readInteger(Alias, x64::WordBytes), Value);
}

TEST_F(HardwareBackend, ContextRestoreUsesCurrentAliasBacking) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Load)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, Value, x64::WordBytes)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, x64::PageSize, Read)),
            "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CX, Alias)), "");
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(bool(Saved));
  ASSERT_EQ(llvm::toString(steps(1)), "");
  EXPECT_EQ(*CPU->reg(X64Register::AX), Value);
  ASSERT_EQ(llvm::toString(CPU->unmapAlias(Alias, x64::PageSize)), "");
  ASSERT_EQ(llvm::toString(CPU->map(Alias, x64::PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Alias, Updated, x64::WordBytes)),
            "");
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
  ASSERT_EQ(llvm::toString(steps(1)), "");
  EXPECT_EQ(*CPU->reg(X64Register::AX), Updated);
  EXPECT_EQ(*CPU->readInteger(Data, x64::WordBytes), Value);
}

TEST_F(HardwareBackend, AliasChangesInvalidateNativeTranslations) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Load)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, Value, x64::WordBytes)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Stack, Updated, x64::WordBytes)),
            "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, x64::PageSize, Read)),
            "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CX, Alias)), "");
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(bool(Saved));
  for (unsigned Round = 0; Round < RemapIterations; ++Round) {
    for (auto Source : {Data, Stack}) {
      ASSERT_EQ(llvm::toString(CPU->replaceAliases(
                    {GuestAliasRange{Alias, x64::PageSize}},
                    {GuestAliasMapping{Alias, Source, x64::PageSize, Read}})),
                "");
      ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
      ASSERT_EQ(llvm::toString(steps(1)), "");
      EXPECT_EQ(*CPU->reg(X64Register::AX), Source == Data ? Value : Updated);
    }
  }
}

TEST_F(HardwareBackend, CatchablePermissionFaultLeavesOriginalContext) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Store)), "");
  ASSERT_EQ(llvm::toString(CPU->protect(Data, x64::PageSize, Read)), "");
  BackendHooks H;
  H.RecoverableFault = [](const BackendFault &) { return true; };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(H))), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, Timeout)), "");
  EXPECT_FALSE(CPU->fault());
  auto F = CPU->takeRecoverableFault();
  ASSERT_TRUE(F);
  EXPECT_EQ(F->Kind, BackendFaultKind::Protection);
  EXPECT_EQ(F->PC, Code);
  EXPECT_EQ(*CPU->readInteger(Data, x64::WordBytes), 0u);
}

TEST_F(HardwareBackend, TerminalFaultCannotBeRestoredAway) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Store)), "");
  auto C = CPU->saveContext();
  ASSERT_TRUE(bool(C));
  ASSERT_EQ(llvm::toString(CPU->protect(Data, x64::PageSize, Read)), "");
  EXPECT_NE(llvm::toString(CPU->run(Code, Timeout)), "");
  EXPECT_NE(llvm::toString(CPU->restoreContext(**C)), "");
  EXPECT_NE(llvm::toString(CPU->run(Code, Timeout)), "");
}

TEST_F(HardwareBackend, BoundedLoopAndUnsupportedInstruction) {
  ASSERT_EQ(llvm::toString(CPU->write(Code, Loop)), "");
  ASSERT_EQ(llvm::toString(steps(3)), "");
  ASSERT_EQ(llvm::toString(CPU->installHooks({})), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, Timeout / 100)), "");
  EXPECT_TRUE(CPU->timedOut());
  ASSERT_EQ(llvm::toString(CPU->write(Code, Unsupported)), "");
  EXPECT_NE(llvm::toString(CPU->run(Code, Timeout)), "");
  EXPECT_EQ(*CPU->reg(X64Register::PC), Code);
}

TEST_F(HardwareBackend, DriverBehaviorMatchesSoftwareOnAdmittedFixtures) {
  for (const char *Name : {Success, Failure, Fault, Unknown}) {
    DriverOptions Options;
    auto Expected = emulateDriver(
        std::filesystem::path(NEVERD_DRIVER_FIXTURES) / Name, Options);
    ASSERT_TRUE(bool(Expected)) << llvm::toString(Expected.takeError());
    Options.Backend = ExecutionBackendKind::Auto;
    Options.Contract = ExecutionContract::CheckedX64;
    auto Actual = emulateDriver(
        std::filesystem::path(NEVERD_DRIVER_FIXTURES) / Name, Options);
    ASSERT_TRUE(bool(Actual)) << llvm::toString(Actual.takeError());
    EXPECT_EQ(Actual->Stop, Expected->Stop)
        << Name << ": " << Actual->Diagnostic;
    EXPECT_EQ(Actual->NTStatus, Expected->NTStatus) << Name;
    EXPECT_EQ(Actual->MajorFunctions, Expected->MajorFunctions) << Name;
    EXPECT_EQ(Actual->Writes.size(), Expected->Writes.size()) << Name;
  }
}

TEST(BackendSelection, LegacyAutoPreservesCompleteContract) {
  auto B = createExecutionBackend(ExecutionBackendKind::Auto,
                                  ExecutionContract::Legacy,
                                  MemoryPages * x64::PageSize);
  ASSERT_TRUE(bool(B)) << llvm::toString(B.takeError());
  EXPECT_EQ(B->Kind, ExecutionBackendKind::Unicorn);
  EXPECT_FALSE(B->Reason.empty());
  auto Explicit = createExecutionBackend(ExecutionBackendKind::KVM,
                                         ExecutionContract::Legacy,
                                         MemoryPages * x64::PageSize);
  ASSERT_FALSE(bool(Explicit));
  llvm::consumeError(Explicit.takeError());
}
} // namespace
} // namespace neverd::emulation
