//===- X64MemoryUpdateTests.cpp - Precise unprefixed memory updates
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include <climits>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "../UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_MEMORY_UPDATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MEMORY_UPDATE(Name, Size, Increment, ...)                       \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64MemoryUpdateCases.def"
#undef NEVERD_MEMORY_UPDATE
#undef NEVERD_MEMORY_UPDATE_VALUE
struct UpdateCase {
  const char *Name;
  unsigned Size;
  bool Increment;
  llvm::ArrayRef<uint8_t> Bytes;
};
const UpdateCase Cases[] = {
#define NEVERD_MEMORY_UPDATE(Name, Size, Increment, ...)                       \
  {#Name, Size, Increment, Name},
#include "X64MemoryUpdateCases.def"
#undef NEVERD_MEMORY_UPDATE
};
void PrintTo(const UpdateCase &C, std::ostream *OS) { *OS << C.Name; }
using Parameter = std::tuple<ExecutionBackendKind, UpdateCase>;
class X64MemoryUpdate : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  const UpdateCase &testCase() const { return std::get<1>(GetParam()); }
  uint64_t mask() const {
    return UINT64_MAX >> ((sizeof(uint64_t) - testCase().Size) * CHAR_BIT);
  }
  uint64_t before() const { return testCase().Increment ? mask() : 0; }
  uint64_t after() const { return testCase().Increment ? 0 : mask(); }
  void SetUp() override {
    auto B = createExecutionBackend(std::get<0>(GetParam()),
                                    ExecutionContract::CheckedUserX64, Limit,
                                    GuestArchitecture::X64);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(std::get<0>(GetParam()), GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->write(Code, testCase().Bytes));
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags | Carry));
    llvm::cantFail(CPU->writeInteger(Data, before(), testCase().Size));
    llvm::cantFail(
        CPU->writeInteger(Data + testCase().Size, Canary, sizeof(uint64_t)));
  }
  ExecutionExit run(BackendHooks Hooks = {}) {
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};

TEST_P(X64MemoryUpdate, WrapsOnlyTheOperandWidthAndPreservesCarry) {
  for (uint64_t CF : {uint64_t(0), Carry}) {
    llvm::cantFail(CPU->writeInteger(Data, before(), testCase().Size));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags | CF));
    unsigned Reads = 0, Writes = 0;
    BackendHooks H;
    H.Read = [&](uint64_t Address, uint32_t Size) {
      EXPECT_EQ(Address, Data);
      EXPECT_EQ(Size, testCase().Size);
      ++Reads;
    };
    H.Write = [&](uint64_t Address, uint32_t Size, uint64_t Value) {
      EXPECT_EQ(Address, Data);
      EXPECT_EQ(Size, testCase().Size);
      EXPECT_EQ(Value, after());
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Size)), before());
      ++Writes;
    };
    auto Exit = run(std::move(H));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(Writes, 1u);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, testCase().Size)), after());
    EXPECT_EQ(llvm::cantFail(
                  CPU->readInteger(Data + testCase().Size, sizeof(uint64_t))),
              Canary);
    const uint64_t Flags = llvm::cantFail(CPU->reg(X64Register::FLAGS));
    EXPECT_EQ(Flags & Carry, CF);
    EXPECT_EQ(bool(Flags & Zero), testCase().Increment);
    EXPECT_EQ(bool(Flags & Sign), !testCase().Increment);
  }
}
TEST_P(X64MemoryUpdate, WriteObserverStopsBeforeBytesFlagsOrPCChange) {
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t Value) {
    EXPECT_EQ(Value, after());
    ++Writes;
    CPU->stop();
  };
  auto Exit = run(std::move(H));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, testCase().Size)), before());
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags | Carry);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
}
TEST_P(X64MemoryUpdate, RequiresBothReadAndWritePermissions) {
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Write);
  EXPECT_EQ(Exit.Fault->PC, Code);
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  llvm::cantFail(CPU->snapshotBacking(
      Data, llvm::MutableArrayRef<uint8_t>(Bytes).take_front(testCase().Size)));
  for (unsigned I = 0; I < testCase().Size; ++I)
    EXPECT_EQ(Bytes[I], uint8_t(before() >> (I * CHAR_BIT)));
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags | Carry);
}
TEST_P(X64MemoryUpdate, ReadFaultPrecedesEveryWriteObservation) {
  llvm::cantFail(CPU->protect(Data, PageSize, Write | UserAccessible));
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
  auto Exit = run(std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
  EXPECT_EQ(Writes, 0u);
}
TEST_P(X64MemoryUpdate, AlignedLockedUpdatesUseTheSameMemoryContract) {
  std::vector<uint8_t> Bytes{LockPrefix};
  Bytes.insert(Bytes.end(), testCase().Bytes.begin(), testCase().Bytes.end());
  llvm::cantFail(CPU->write(Code, Bytes));
  auto Exit = run();
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, testCase().Size)), after());
}
INSTANTIATE_TEST_SUITE_P(
    Backends, X64MemoryUpdate,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::ValuesIn(Cases)),
    [](const testing::TestParamInfo<Parameter> &P) {
      return std::string(executionBackendName(std::get<0>(P.param))) + "_" +
             std::get<1>(P.param).Name;
    });
} // namespace
} // namespace neverd::emulation
