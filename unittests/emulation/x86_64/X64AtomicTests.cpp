//===- X64AtomicTests.cpp - Native results and observer rollback
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "arch/x86_64/X64Machine.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Memory.h"

#include <cstring>
#include <stdexcept>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_ATOMIC_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_ATOMIC_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_ATOMIC_ORACLE_BYTES(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_ATOMIC_CASE(Name, Size, Compare, ...)                           \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64AtomicCases.def"
#undef NEVERD_ATOMIC_CASE
#undef NEVERD_ATOMIC_ORACLE_BYTES
#undef NEVERD_ATOMIC_TEXT
#undef NEVERD_ATOMIC_VALUE
struct AtomicCase {
  const char *Name;
  unsigned Size;
  bool Compare;
  llvm::ArrayRef<uint8_t> Bytes;
};
const AtomicCase Cases[] = {
#define NEVERD_ATOMIC_CASE(Name, Size, Compare, ...)                           \
  {#Name, Size, Compare, Name},
#include "X64AtomicCases.def"
#undef NEVERD_ATOMIC_CASE
};
void PrintTo(const AtomicCase &C, std::ostream *OS) { *OS << C.Name; }
using Parameter = std::tuple<ExecutionBackendKind, bool, bool, AtomicCase>;

class X64Atomic : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> Bytes;
  const AtomicCase &testCase() const { return std::get<3>(GetParam()); }
  uint64_t mask() const {
    return UINT64_MAX >> ((WordBytes - testCase().Size) * CHAR_BIT);
  }
  void SetUp() override {
    const auto Contract = std::get<1>(GetParam())
                              ? ExecutionContract::CheckedUserX64
                              : ExecutionContract::CheckedX64;
    auto B = createExecutionBackend(std::get<0>(GetParam()), Contract, Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(std::get<0>(GetParam()), GuestArchitecture::X64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(CPU->map(Code, memory::PageSize,
                            Read | Write | Execute | UserAccessible));
    llvm::cantFail(
        CPU->map(Data, memory::PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->mapAlias(Alias, Data, memory::PageSize,
                                 Read | Write | UserAccessible));
    if (std::get<2>(GetParam()))
      Bytes.push_back(Lock);
    Bytes.insert(Bytes.end(), testCase().Bytes.begin(), testCase().Bytes.end());
    const uint8_t Stop = Nop;
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->write(Code + Bytes.size(), {&Stop, 1}));
  }
  void seed(uint64_t Accumulator) {
    llvm::cantFail(CPU->setReg(X64Register::AX, Accumulator));
    llvm::cantFail(CPU->setReg(X64Register::DX, Source));
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
    llvm::cantFail(CPU->writeInteger(Data, Original, WordBytes));
    llvm::cantFail(CPU->writeInteger(Data + WordBytes, Source, WordBytes));
  }
  ExecutionExit run(BackendHooks Hooks = {}) {
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    auto Exit = CPU->runUntilExit(Code, Timeout);
    if (!Exit) {
      ADD_FAILURE() << llvm::toString(Exit.takeError());
      return {};
    }
    return std::move(*Exit);
  }
  void expectOriginal(uint64_t Accumulator) {
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Accumulator);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), Source);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)) & x64::AllowedFlags,
              InitialFlags);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), Original);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Alias, WordBytes)), Original);
  }
  uint64_t matchingAccumulator(bool Match) const {
    const uint64_t Low = Original & mask();
    return (AccumulatorHigh & ~mask()) | (Match ? Low : Low ^ 1);
  }
};

TEST_P(X64Atomic, ExactRAMAndRegisterResultsMatchIndependentHostExecution) {
#if defined(__x86_64__) || defined(_M_X64)
  std::vector<uint8_t> Oracle(std::begin(Prefix), std::end(Prefix));
#ifdef _WIN32
  Oracle.insert(Oracle.end(), std::begin(Win64Argument),
                std::end(Win64Argument));
#else
  Oracle.insert(Oracle.end(), std::begin(SysVArgument), std::end(SysVArgument));
#endif
  Oracle.insert(Oracle.end(), std::begin(Load), std::end(Load));
  Oracle.insert(Oracle.end(), Bytes.begin(), Bytes.end());
  Oracle.insert(Oracle.end(), std::begin(Save), std::end(Save));
  std::error_code EC;
  auto Block = llvm::sys::Memory::allocateMappedMemory(
      memory::PageSize, nullptr,
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  auto Release = llvm::scope_exit(
      [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
  std::memcpy(Block.base(), Oracle.data(), Oracle.size());
  EC = llvm::sys::Memory::protectMappedMemory(
      Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
  ASSERT_FALSE(bool(EC)) << EC.message();
  llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Oracle.size());
  const auto Execute = reinterpret_cast<void (*)(uint64_t *)>(Block.base());
  for (bool Match : {false, true}) {
    const uint64_t Accumulator = matchingAccumulator(Match);
    seed(Accumulator);
    std::array<uint64_t, ScalarWidthCount> Expected = {Accumulator, Source,
                                                       InitialFlags, Original};
    Execute(Expected.data());
    unsigned Reads = 0, Writes = 0;
    std::unique_ptr<BackendContext> ObservedContext;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, unsigned S) {
      EXPECT_EQ(A, Data);
      EXPECT_EQ(S, testCase().Size);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t A, unsigned S, uint64_t V) {
      EXPECT_EQ(A, Data);
      EXPECT_EQ(S, testCase().Size);
      EXPECT_EQ(V, Expected.back() & mask());
      expectOriginal(Accumulator);
      auto Context = CPU->saveContext();
      ASSERT_TRUE(bool(Context)) << llvm::toString(Context.takeError());
      ObservedContext = std::move(*Context);
      ++Writes;
    };
    const auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(Writes, 1u);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Expected[0]);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), Expected[1]);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)) & x64::AllowedFlags,
              Expected[2] & x64::AllowedFlags);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), Expected[3]);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Alias, WordBytes)), Expected[3]);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + WordBytes, WordBytes)),
              Source);
    ASSERT_TRUE(ObservedContext);
    llvm::cantFail(CPU->restoreContext(*ObservedContext));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Accumulator);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), Source);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), Expected[3]);
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}

TEST_P(X64Atomic, StoppingTheResultObserverRestoresOriginalRAMAndCPU) {
  const uint64_t Accumulator = matchingAccumulator(true);
  seed(Accumulator);
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) {
    expectOriginal(Accumulator);
    ++Writes;
    CPU->stop();
  };
  EXPECT_EQ(run(std::move(Hooks)).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(Writes, 1u);
  expectOriginal(Accumulator);
  EXPECT_EQ(run().Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code + Bytes.size());
}

TEST_P(X64Atomic, ReadObserverStopsBeforeAnyNativeEntry) {
  const uint64_t Accumulator = matchingAccumulator(false);
  seed(Accumulator);
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, unsigned) { CPU->stop(); };
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
  EXPECT_EQ(run(std::move(Hooks)).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(Writes, 0u);
  expectOriginal(Accumulator);
}

TEST_P(X64Atomic, ObserverExceptionDiscardsTheCompletedNativeWrite) {
  const uint64_t Accumulator = matchingAccumulator(true);
  seed(Accumulator);
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) {
    expectOriginal(Accumulator);
    throw std::runtime_error(OracleUnavailable);
  };
  EXPECT_EQ(run(std::move(Hooks)).Kind, ExecutionExitKind::BackendFailure);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Accumulator);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), Source);
  std::array<uint8_t, WordBytes> Bytes;
  llvm::cantFail(CPU->addressSpace()->read(Data, Bytes));
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), Original);
}

TEST_P(X64Atomic, MissingWritePermissionDoesNotInventAResultObservation) {
  const uint64_t Accumulator = matchingAccumulator(true);
  seed(Accumulator);
  llvm::cantFail(CPU->protect(Data, memory::PageSize, Read | UserAccessible));
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
  const auto Exit = run(std::move(Hooks));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault);
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Protection);
  EXPECT_EQ(Exit.Fault->Address, Data);
  EXPECT_EQ(Writes, 0u);
  std::array<uint8_t, WordBytes> Bytes;
  llvm::cantFail(CPU->addressSpace()->read(Data, Bytes));
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), Original);
}

TEST_P(X64Atomic, MissingOperandReportsWriteAccessBeforeAnyResult) {
  const uint64_t Accumulator = matchingAccumulator(false);
  seed(Accumulator);
  llvm::cantFail(CPU->addressSpace()->unmap(Data, memory::PageSize));
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
  Hooks.RecoverableFault = [](const BackendFault &) { return true; };
  EXPECT_EQ(run(std::move(Hooks)).Kind, ExecutionExitKind::RecoverableFault);
  const auto Fault = CPU->takeRecoverableFault();
  ASSERT_TRUE(Fault);
  EXPECT_EQ(Fault->Kind, BackendFaultKind::UnmappedMemory);
  EXPECT_EQ(Fault->Access, BackendAccessKind::Write);
  EXPECT_EQ(Fault->Address, Data);
  EXPECT_EQ(Writes, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Accumulator);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Alias, WordBytes)), Original);
}

INSTANTIATE_TEST_SUITE_P(
    Transports, X64Atomic,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool(), testing::Bool(),
                     testing::ValuesIn(Cases)));
} // namespace
} // namespace neverd::emulation
