//===- AArch64NZCVTests.cpp - Original condition flag register words -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {
// SP has nonzero bits31:28, so reading SP instead of XZR cannot pass a write.
constexpr uint64_t Code = 0x400000, Data = 0xa0800000, PageSize = 4096;
// Independently assembled MRS Xt, NZCV and MSR NZCV, Xt. Rt occupies bits4:0.
constexpr uint32_t ReadFlags = 0xd53b4200, WriteFlags = 0xd51b4200;
constexpr uint64_t FlagMask = 0xf0000000;
struct State {
  std::array<uint64_t, unsigned(AArch64Register::FPSR) + 1> Scalars;
  std::array<RegisterValue, 32> Vectors;
};
State state(ExecutionBackend &CPU) {
  State S;
  for (unsigned I = 0; I < S.Scalars.size(); ++I)
    S.Scalars[I] = llvm::cantFail(CPU.reg(AArch64Register(I)));
  for (unsigned I = 0; I < S.Vectors.size(); ++I)
    S.Vectors[I] = llvm::cantFail(
        CPU.readRegister(vectorRegister(GuestArchitecture::AArch64, I)));
  return S;
}
void expectState(ExecutionBackend &CPU, const State &Expected) {
  const auto Actual = state(CPU);
  EXPECT_EQ(Actual.Scalars, Expected.Scalars);
  EXPECT_EQ(Actual.Vectors, Expected.Vectors);
}

TEST(AArch64NZCVOracle, OriginalInstructionsSelectOnlyTheFourConditionBits) {
#if defined(__aarch64__) || defined(__arm64__)
  for (unsigned F = 0; F < 16; ++F) {
    const uint64_t Flags = uint64_t(F) << 28;
    for (uint64_t Reserved :
         {0ULL, 0xffffffff0fffffffULL, 0x123456780abcdef0ULL}) {
      SCOPED_TRACE(Flags | Reserved);
      uint64_t Saved, Read, N, Z, C, V, Discard, Cleared;
      // CSET observes each condition independently of the MRS round trip.
      // Preserve the caller's flags within this single compiler boundary.
      __asm__ volatile(
          "mrs %0, nzcv\n\tmsr nzcv, %8\n\tmrs %1, nzcv\n\t"
          "cset %2, mi\n\tcset %3, eq\n\tcset %4, cs\n\tcset %5, vs\n\t"
          "mrs xzr, nzcv\n\tmrs %6, nzcv\n\t"
          "msr nzcv, xzr\n\tmrs %7, nzcv\n\tmsr nzcv, %0"
          : "=&r"(Saved), "=&r"(Read), "=&r"(N), "=&r"(Z), "=&r"(C), "=&r"(V),
            "=&r"(Discard), "=&r"(Cleared)
          : "r"(Flags | Reserved)
          : "cc");
      EXPECT_EQ(Read, Flags);
      EXPECT_EQ((N << 31) | (Z << 30) | (C << 29) | (V << 28), Flags);
      EXPECT_EQ(Discard, Flags);
      EXPECT_EQ(Cleared, 0u);
    }
  }
#else
  GTEST_SKIP() << "Original NZCV instructions require an AArch64 host";
#endif
}

using Parameter = std::tuple<ExecutionBackendKind, bool>;
class AArch64NZCV : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> Original;
  void SetUp() override { initialize(); }
  void initialize() {
    CPU.reset();
    const auto [Kind, User] = GetParam();
    auto Created =
        createExecutionBackend(Kind,
                               User ? ExecutionContract::CheckedUserAArch64
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
      Original[I] = uint8_t(0xe7 + I * 37);
    llvm::cantFail(CPU->write(Data, Original));
    seed();
  }
  void seed() {
    for (unsigned I = 0; I < 31; ++I)
      set(AArch64Register(I), 0xdef0123456789abcULL + 0x101 * I);
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
  ExecutionExit run(uint64_t End) {
    BackendHooks Hooks;
    Hooks.Instruction = [&, End](uint64_t PC, uint32_t) {
      if (PC == End)
        CPU->stop();
    };
    Hooks.Read = [](uint64_t, uint32_t) { ADD_FAILURE() << "Unexpected read"; };
    Hooks.Write = [](uint64_t, uint32_t, uint64_t) {
      ADD_FAILURE() << "Unexpected write";
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, 1000000));
  }
  void expectStopped(const ExecutionExit &Exit) {
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  }
  void expectMemory(llvm::ArrayRef<uint8_t> Bytes) {
    EXPECT_EQ(snapshot(Code, Bytes.size()), Bytes.vec());
    EXPECT_EQ(snapshot(Data, PageSize), Original);
  }
};

TEST_P(AArch64NZCV, ReadsOnlyChangeTheDestinationAndDiscardThroughXZR) {
  for (unsigned F = 0; F < 16; ++F)
    for (unsigned R : {0u, 17u, 30u, 31u}) {
      SCOPED_TRACE(F);
      SCOPED_TRACE(R);
      seed();
      const uint64_t Flags = uint64_t(F) << 28;
      set(AArch64Register::NZCV, Flags);
      const auto Bytes = code({ReadFlags | R, 0xd503201f});
      auto Expected = state(*CPU);
      if (R != 31)
        Expected.Scalars[R] = Flags;
      Expected.Scalars[unsigned(AArch64Register::PC)] += 4;
      expectStopped(run(Code + 4));
      expectState(*CPU, Expected);
      expectMemory(Bytes);
    }
}

TEST_P(AArch64NZCV, WritesIgnoreOtherInputBitsAndXZRDoesNotReadSP) {
  for (unsigned F = 0; F < 16; ++F)
    for (unsigned R : {0u, 17u, 30u, 31u})
      for (uint64_t Reserved : std::array<uint64_t, 2>{0, ~FlagMask}) {
        SCOPED_TRACE(F);
        SCOPED_TRACE(R);
        SCOPED_TRACE(Reserved);
        seed();
        const uint64_t Flags = uint64_t(F) << 28;
        if (R != 31)
          set(AArch64Register(R), Flags | Reserved);
        const auto Bytes = code({WriteFlags | R, 0xd503201f});
        auto Expected = state(*CPU);
        Expected.Scalars[unsigned(AArch64Register::NZCV)] = R == 31 ? 0 : Flags;
        Expected.Scalars[unsigned(AArch64Register::PC)] += 4;
        expectStopped(run(Code + 4));
        expectState(*CPU, Expected);
        expectMemory(Bytes);
      }
}

TEST_P(AArch64NZCV, NearbySystemRegistersRemainUnsupportedWithoutEffects) {
  // DAIF, CurrentEL, SPSel and an unallocated neighbor are not NZCV.
  for (uint32_t Word : {0xd53b4220, 0xd5384240, 0xd5384200, 0xd53b4300,
                        0xd51b4220, 0xd5184240, 0xd5184200, 0xd51b4300}) {
    SCOPED_TRACE(Word);
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    const auto Bytes = code({Word, 0xd503201f});
    const auto Before = state(*CPU);
    const auto Exit = run(Code + 4);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    expectState(*CPU, Before);
    expectMemory(Bytes);
  }
}

TEST_P(AArch64NZCV, ObserverStopAndContextRestoreKeepTheInstructionBoundary) {
  for (uint32_t Word :
       {ReadFlags | 17, WriteFlags | 17, ReadFlags | 31, WriteFlags | 31}) {
    SCOPED_TRACE(Word);
    seed();
    set(AArch64Register::X17, 0xffffffff6fffffffULL);
    const auto Bytes = code({Word, 0xd503201f});
    const auto Before = state(*CPU);
    auto Saved = llvm::cantFail(CPU->saveContext());
    expectStopped(run(Code));
    expectState(*CPU, Before);
    expectMemory(Bytes);
    auto After = Before;
    After.Scalars[unsigned(AArch64Register::PC)] += 4;
    if (Word == (ReadFlags | 17))
      After.Scalars[17] = 0xb0000000;
    if (Word == (WriteFlags | 17) || Word == (WriteFlags | 31))
      After.Scalars[unsigned(AArch64Register::NZCV)] =
          Word == (WriteFlags | 31) ? 0 : 0x60000000;
    for (unsigned Attempt = 0; Attempt < 2; ++Attempt) {
      llvm::cantFail(CPU->restoreContext(*Saved));
      expectState(*CPU, Before);
      expectStopped(run(Code + 4));
      expectState(*CPU, After);
      expectMemory(Bytes);
    }
  }
}

TEST_P(AArch64NZCV, ObserverFailurePreservesCompleteState) {
  for (uint32_t Word : {ReadFlags | 17, WriteFlags | 17}) {
    initialize();
    if (HasFatalFailure() || IsSkipped())
      return;
    const auto Bytes = code({Word, 0xd503201f});
    const auto Before = state(*CPU);
    BackendHooks Hooks;
    Hooks.Instruction = [](uint64_t, uint32_t) {
      throw std::runtime_error("NZCV observer failure");
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, 1000000));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure) << Exit.Diagnostic;
    EXPECT_FALSE(Exit.Diagnostic.empty());
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::UnhandledException);
    EXPECT_EQ(Exit.Fault->PC, Code);
    expectState(*CPU, Before);
    expectMemory(Bytes);
  }
}

TEST_P(AArch64NZCV, EveryReadAndWriteConsumesOneSharedBudgetAttempt) {
  set(AArch64Register::X17, 0xffffffff6fffffffULL);
  code({WriteFlags | 17, ReadFlags | 30, WriteFlags | 31, ReadFlags,
        0xd503201f});
  auto Expected = state(*CPU);
  std::shared_ptr<ExecutionBudget> Budget =
      llvm::cantFail(ExecutionBudget::create({4, 4, 1000000}));
  std::vector<uint64_t> Trace;
  auto Session = llvm::cantFail(ExecutionSession::create(
      std::move(CPU), Budget, {},
      [&](uint64_t PC, uint32_t) { Trace.push_back(PC); }));
  for (unsigned I = 0; I < 4; ++I) {
    const auto Exit = llvm::cantFail(Session->run(Code + 4 * I, 1));
    EXPECT_EQ(Exit.Kind, I == 3 ? SessionExitKind::InstructionLimit
                                : SessionExitKind::Quantum);
    EXPECT_EQ(Budget->instructions(), I + 1);
    Expected.Scalars[unsigned(AArch64Register::PC)] = Code + 4 * (I + 1);
    if (I == 0 || I == 2)
      Expected.Scalars[unsigned(AArch64Register::NZCV)] =
          I == 0 ? 0x60000000 : 0;
    if (I == 1)
      Expected.Scalars[30] = 0x60000000;
    if (I == 3)
      Expected.Scalars[0] = 0;
    expectState(Session->cpu(), Expected);
  }
  EXPECT_EQ(Trace,
            (std::vector<uint64_t>{Code, Code + 4, Code + 8, Code + 12}));
}

INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64NZCV,
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
