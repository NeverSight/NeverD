//===- AArch64CacheTests.cpp - Original cache synchronization words ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <string>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x400000, Data = 0x800000, PageSize = 4096;
// Independently assembled MRS CTR_EL0, DC CVAU, IC IVAU, DSB ISH and ISB.
constexpr uint32_t ReadCTR = 0xd53b0020, Clean = 0xd50b7b20,
                   Invalidate = 0xd50b7520, DSB = 0xd5033b9f, ISB = 0xd5033fdf;
struct State {
  std::array<uint64_t, unsigned(AArch64Register::FPSR) + 1> Scalars;
  std::array<RegisterValue, 32> Vectors;
};
using Parameter = std::tuple<ExecutionBackendKind, bool>;
class AArch64Cache : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> Original;
  bool userMode() const { return std::get<1>(GetParam()); }
  void SetUp() override { initialize(); }
  void initialize() {
    CPU.reset();
    const auto Kind = std::get<0>(GetParam());
    auto Created = createExecutionBackend(
        Kind,
        userMode() ? ExecutionContract::CheckedUserAArch64
                   : ExecutionContract::CheckedAArch64,
        4 * 1024 * 1024, GuestArchitecture::AArch64);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(Kind, GuestArchitecture::AArch64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(Created->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    Original.resize(PageSize);
    for (unsigned I = 0; I < Original.size(); ++I)
      Original[I] = uint8_t(0xf1 + I * 37);
    llvm::cantFail(CPU->write(Data, Original));
    seed();
  }
  void seed() {
    for (unsigned I = 0; I < 31; ++I)
      llvm::cantFail(
          CPU->setReg(AArch64Register(I), 0xdef0123456789abcULL + 0x101 * I));
    set(AArch64Register::SP, Data + PageSize - 16);
    set(AArch64Register::PC, Code);
    set(AArch64Register::NZCV, 0xb0000000);
    set(AArch64Register::FPCR, 0x03400000);
    set(AArch64Register::FPSR, 0x08000002);
    for (unsigned I = 0; I < 32; ++I)
      llvm::cantFail(CPU->writeRegister(
          vectorRegister(GuestArchitecture::AArch64, I),
          {0x123456789abcdef0ULL + I, 0xfedcba9876543210ULL - I}));
  }
  void set(AArch64Register R, uint64_t Value) {
    llvm::cantFail(CPU->setReg(R, Value));
  }
  State state() const {
    State S;
    for (unsigned I = 0; I < S.Scalars.size(); ++I)
      S.Scalars[I] = llvm::cantFail(CPU->reg(AArch64Register(I)));
    for (unsigned I = 0; I < S.Vectors.size(); ++I)
      S.Vectors[I] = llvm::cantFail(
          CPU->readRegister(vectorRegister(GuestArchitecture::AArch64, I)));
    return S;
  }
  void expectState(const State &Expected) {
    const auto Actual = state();
    EXPECT_EQ(Actual.Scalars, Expected.Scalars);
    EXPECT_EQ(Actual.Vectors, Expected.Vectors);
  }
  std::vector<uint8_t> snapshot(uint64_t Address, size_t Size) {
    std::vector<uint8_t> Bytes(Size);
    llvm::cantFail(CPU->snapshotBacking(Address, Bytes));
    return Bytes;
  }
  std::vector<uint8_t> code(llvm::ArrayRef<uint32_t> Words) {
    std::vector<uint8_t> Bytes(Words.size() * 4);
    for (unsigned I = 0; I < Words.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + 4 * I, Words[I]);
    llvm::cantFail(CPU->write(Code, Bytes));
    return Bytes;
  }
  ExecutionExit run(uint64_t End, BackendHooks Hooks = {}) {
    Hooks.Instruction = [&, End](uint64_t PC, uint32_t) {
      if (PC == End)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, 1000000));
  }
  void expectStopped(const ExecutionExit &Exit) {
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  }
};

TEST_P(AArch64Cache, CacheTypeComesFromTheMachineAndOnlyChangesItsDestination) {
  const auto Bytes = code({ReadCTR, ReadCTR | 30, ReadCTR | 31, 0xd503201f});
  auto Expected = state();
  expectStopped(run(Code + 4));
  const auto CTR = llvm::cantFail(CPU->reg(AArch64Register::X0));
  EXPECT_NE(CTR, Expected.Scalars[0]); // A substituted NOP cannot pass.
  Expected.Scalars[0] = CTR;
  Expected.Scalars[unsigned(AArch64Register::PC)] = Code + 4;
  expectState(Expected);
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC == Code + 12)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  expectStopped(llvm::cantFail(CPU->runUntilExit(Code + 4, 1000000)));
  Expected.Scalars[30] = CTR;
  Expected.Scalars[unsigned(AArch64Register::PC)] = Code + 12;
  expectState(Expected); // XZR discards the read without changing SP.
  EXPECT_EQ(snapshot(Code, Bytes.size()), Bytes);
  EXPECT_EQ(snapshot(Data, PageSize), Original);
}

TEST_P(AArch64Cache, ReadOnlyUnalignedCacheTargetsDoNotCreateDataObservations) {
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  for (unsigned R : {0u, 17u, 30u}) {
    for (uint32_t Word : {Clean | R, Invalidate | R}) {
      SCOPED_TRACE(Word);
      seed();
      set(AArch64Register(R), Data + PageSize - 1);
      const auto Bytes = code({Word, 0xd503201f});
      auto Expected = state();
      unsigned Reads = 0, Writes = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, uint32_t) { ++Reads; };
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
      expectStopped(run(Code + 4, std::move(Hooks)));
      Expected.Scalars[unsigned(AArch64Register::PC)] += 4;
      expectState(Expected);
      EXPECT_EQ(Reads, 0u);
      EXPECT_EQ(Writes, 0u);
      EXPECT_EQ(snapshot(Data, PageSize), Original);
      EXPECT_EQ(snapshot(Code, Bytes.size()), Bytes);
    }
  }
}

TEST_P(AArch64Cache, NamedBaselineBarriersPreserveCompleteStateAndMemory) {
  // Independently assembled OSHLD/OSHST/OSH, NSHLD/NSHST/NSH,
  // ISHLD/ISHST/ISH, LD/ST/SY; ISB SY is the only admitted ISB option.
  constexpr uint32_t Words[] = {0xd503319f, 0xd503329f, 0xd503339f, 0xd503359f,
                                0xd503369f, 0xd503379f, 0xd503399f, 0xd5033a9f,
                                0xd5033b9f, 0xd5033d9f, 0xd5033e9f, 0xd5033f9f,
                                0xd5033fdf};
  for (uint32_t Word : Words) {
    SCOPED_TRACE(Word);
    seed();
    const auto Bytes = code({Word, 0xd503201f});
    auto Expected = state();
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, uint32_t) { ++Reads; };
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
    expectStopped(run(Code + 4, std::move(Hooks)));
    Expected.Scalars[unsigned(AArch64Register::PC)] += 4;
    expectState(Expected);
    EXPECT_EQ(Reads, 0u);
    EXPECT_EQ(Writes, 0u);
    EXPECT_EQ(snapshot(Code, Bytes.size()), Bytes);
    EXPECT_EQ(snapshot(Data, PageSize), Original);
  }
}

TEST_P(AArch64Cache, GuestStoresUpdateExecutableAliasesAcrossTwoPages) {
  constexpr uint64_t Source = 0xc00000, Alias = 0x1000000;
  llvm::cantFail(CPU->map(Source, 2 * PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->mapAlias(Alias, Source, 2 * PageSize,
                               Read | Execute | UserAccessible));
  set(AArch64Register::X2, Source + PageSize - 4);
  set(AArch64Register::X3, Alias + PageSize - 4);
  set(AArch64Register::X4, 0x52800520); // mov w0, #41
  set(AArch64Register::X5, 0xd65f03c0); // ret
  set(AArch64Register::X6, Source + PageSize);
  set(AArch64Register::X8, 0x52800920); // mov w0, #73
  set(AArch64Register::X10, Alias + PageSize);
  // Store both generated words; synchronize both sides of the page boundary;
  // call through RX alias, replace the first word through RW, call again.
  const auto Bytes = code({0xb9000044,
                           0xb9000445,
                           Clean | 2,
                           Clean | 6,
                           DSB,
                           Invalidate | 3,
                           Invalidate | 10,
                           DSB,
                           ISB,
                           0xd63f0060,
                           0xaa0003e7,
                           0x2a0803e4,
                           0xb9000044,
                           Clean | 2,
                           Clean | 6,
                           DSB,
                           Invalidate | 3,
                           Invalidate | 10,
                           DSB,
                           ISB,
                           0xd63f0060,
                           0xd503201f});
  auto Expected = state();
  unsigned Reads = 0;
  std::vector<std::tuple<uint64_t, uint32_t, uint64_t>> Writes;
  std::vector<uint64_t> Executed;
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, uint32_t) { ++Reads; };
  Hooks.Write = [&](uint64_t A, uint32_t Size, uint64_t V) {
    Writes.emplace_back(A, Size, V);
  };
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    Executed.push_back(PC);
    if (PC == Code + 84)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  expectStopped(llvm::cantFail(CPU->runUntilExit(Code, 1000000)));
  Expected.Scalars[0] = 73;
  Expected.Scalars[4] = 0x52800920;
  Expected.Scalars[7] = 41;
  Expected.Scalars[30] = Code + 84;
  Expected.Scalars[unsigned(AArch64Register::PC)] = Code + 84;
  expectState(Expected);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(Writes, (decltype(Writes){{Source + PageSize - 4, 4, 0x52800520},
                                      {Source + PageSize, 4, 0xd65f03c0},
                                      {Source + PageSize - 4, 4, 0x52800920}}));
  std::vector<uint64_t> ExpectedTrace;
  for (unsigned I = 0; I < 22; ++I) {
    ExpectedTrace.push_back(Code + 4 * I);
    if (I == 9 || I == 20) {
      ExpectedTrace.push_back(Alias + PageSize - 4);
      ExpectedTrace.push_back(Alias + PageSize);
    }
  }
  EXPECT_EQ(Executed, ExpectedTrace);
  auto Generated = std::vector<uint8_t>(2 * PageSize);
  llvm::support::endian::write32le(Generated.data() + PageSize - 4, 0x52800920);
  llvm::support::endian::write32le(Generated.data() + PageSize, 0xd65f03c0);
  EXPECT_EQ(snapshot(Source, 2 * PageSize), Generated);
  EXPECT_EQ(snapshot(Alias, 2 * PageSize), Generated);
  EXPECT_EQ(snapshot(Data, PageSize), Original);
  EXPECT_EQ(snapshot(Code, Bytes.size()), Bytes);
}

TEST_P(AArch64Cache, InaccessibleTargetsRejectBeforeStateAndObservers) {
  for (uint32_t Operation : {Clean, Invalidate}) {
    for (unsigned Case = 0; Case < 5; ++Case) {
      SCOPED_TRACE(Operation);
      SCOPED_TRACE(Case);
      initialize();
      uint64_t Address = Data + PageSize;
      if (Case == 1) {
        Address = Data + 1;
        llvm::cantFail(CPU->protect(Data, PageSize, 0));
      } else if (Case == 2) {
        Address = uint64_t(1) << 48;
      } else if (Case == 3) {
        Address = 0;
      } else if (Case == 4) {
        if (!userMode())
          continue;
        Address = Data;
        llvm::cantFail(CPU->protect(Data, PageSize, Read | Write));
      }
      set(AArch64Register::X0, Address);
      const auto Bytes = code({Operation | (Case == 3 ? 31 : 0), 0xd503201f});
      const auto Before = state();
      unsigned Reads = 0, Writes = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, uint32_t) { ++Reads; };
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
      const auto Exit = run(Code + 4, std::move(Hooks));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
      expectState(Before);
      EXPECT_EQ(Reads, 0u);
      EXPECT_EQ(Writes, 0u);
      EXPECT_EQ(snapshot(Data, PageSize), Original);
      EXPECT_EQ(snapshot(Code, Bytes.size()), Bytes);
    }
  }
}

TEST_P(AArch64Cache, UnadmittedSystemWordsRemainUnsupported) {
  // MSR CTR_EL0; DC ZVA; DC CVAC; IC IALLU; reserved ISB and DSB options.
  constexpr uint32_t Words[] = {0xd51b0020, 0xd50b7420, 0xd50b7a20,
                                0xd508751f, 0xd50330df, 0xd503309f,
                                0xd503349f, 0xd503389f, 0xd5033c9f};
  for (uint32_t Word : Words) {
    SCOPED_TRACE(Word);
    initialize();
    set(AArch64Register::X0, Data);
    const auto Bytes = code({Word, 0xd503201f});
    const auto Before = state();
    const auto Exit = run(Code + 4);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    expectState(Before);
    EXPECT_EQ(snapshot(Code, Bytes.size()), Bytes);
    EXPECT_EQ(snapshot(Data, PageSize), Original);
  }
}

TEST_P(AArch64Cache,
       ObserverStopsAndContextRestorePreserveInstructionBoundary) {
  for (uint32_t Word : {ReadCTR, Clean, DSB, Invalidate, ISB}) {
    seed();
    set(AArch64Register::X0, Data);
    code({Word, 0xd503201f});
    const auto Before = state();
    auto Saved = llvm::cantFail(CPU->saveContext());
    expectStopped(run(Code));
    expectState(Before);
    EXPECT_EQ(snapshot(Data, PageSize), Original);
    expectStopped(run(Code + 4));
    llvm::cantFail(CPU->restoreContext(*Saved));
    expectState(Before);
    expectStopped(run(Code + 4));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code + 4);
    EXPECT_EQ(snapshot(Data, PageSize), Original);
  }
}

TEST_P(AArch64Cache, EveryOperationConsumesTheSharedInstructionBudget) {
  set(AArch64Register::X1, Data);
  code({ReadCTR, Clean | 1, DSB, Invalidate | 1, ISB, 0xd503201f});
  std::shared_ptr<ExecutionBudget> Budget =
      llvm::cantFail(ExecutionBudget::create({5, 5, 1000000}));
  std::vector<uint64_t> Trace;
  auto Session = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {},
      [&](uint64_t PC, uint32_t) { Trace.push_back(PC); }));
  for (unsigned I = 0; I < 5; ++I) {
    const auto Exit = llvm::cantFail(Session->run(Code + 4 * I, 1));
    EXPECT_EQ(Exit.Kind, I == 4 ? SessionExitKind::InstructionLimit
                                : SessionExitKind::Quantum);
    EXPECT_EQ(Budget->instructions(), I + 1);
  }
  EXPECT_EQ(Trace, (std::vector<uint64_t>{Code, Code + 4, Code + 8, Code + 12,
                                          Code + 16}));
  EXPECT_EQ(llvm::cantFail(Session->cpu().reg(AArch64Register::PC)), Code + 20);
}

INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64Cache,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool()),
    [](const testing::TestParamInfo<Parameter> &P) {
      return std::string(executionBackendName(std::get<0>(P.param))) +
             (std::get<1>(P.param) ? "User" : "Supervisor");
    });
} // namespace
} // namespace neverd::emulation
