//===- AArch64AcquireReleaseTests.cpp - Checked ordered scalar RAM -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x400000, Data = 0x800000, PageSize = 4096;
struct OrderedCase {
  const char *Name;
  uint32_t Word;
  unsigned Width;
  bool Load;
};
// Independently assembled baseline instructions with Rt=0 and Rn=1.
constexpr OrderedCase Cases[] = {
    {"ldarb", 0x08dffc20, 1, true},   {"ldarh", 0x48dffc20, 2, true},
    {"ldar_w", 0x88dffc20, 4, true},  {"ldar_x", 0xc8dffc20, 8, true},
    {"stlrb", 0x089ffc20, 1, false},  {"stlrh", 0x489ffc20, 2, false},
    {"stlr_w", 0x889ffc20, 4, false}, {"stlr_x", 0xc89ffc20, 8, false}};
void PrintTo(const OrderedCase &C, std::ostream *OS) { *OS << C.Name; }
using Parameter = std::tuple<ExecutionBackendKind, bool, OrderedCase>;
struct State {
  std::array<uint64_t, unsigned(AArch64Register::FPSR) + 1> Scalars;
  std::array<RegisterValue, 32> Vectors;
};
class AArch64AcquireRelease : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  std::vector<uint8_t> Original;
  unsigned Rt = 0, Rn = 1;
  uint64_t Address = Data + 128;
  State Before;
  const OrderedCase &testCase() const { return std::get<2>(GetParam()); }
  bool userMode() const { return std::get<1>(GetParam()); }
  void SetUp() override { initialize(); }
  void initialize() {
    auto Backend = std::get<0>(GetParam());
    auto Created = createExecutionBackend(
        Backend,
        userMode() ? ExecutionContract::CheckedUserAArch64
                   : ExecutionContract::CheckedAArch64,
        4 * 1024 * 1024, GuestArchitecture::AArch64);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(Backend, GuestArchitecture::AArch64))
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
    seed();
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
  uint32_t word() const { return (testCase().Word & ~0x3ffu) | (Rn << 5) | Rt; }
  void seed() {
    uint8_t Bytes[8];
    llvm::support::endian::write32le(Bytes, word());
    llvm::support::endian::write32le(Bytes + 4, 0xd503201f);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->write(Data, Original));
    for (unsigned I = 0; I < 31; ++I)
      llvm::cantFail(
          CPU->setReg(AArch64Register(I), 0xdef0123456789abcULL + 0x101 * I));
    llvm::cantFail(CPU->setReg(AArch64Register::SP, Data + PageSize - 16));
    llvm::cantFail(CPU->setReg(
        Rn == 31 ? AArch64Register::SP : AArch64Register(Rn), Address));
    llvm::cantFail(CPU->setReg(AArch64Register::PC, Code));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, 0xb0000000));
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 0x03400000));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 0x08000002));
    for (unsigned I = 0; I < 32; ++I)
      llvm::cantFail(CPU->writeRegister(
          vectorRegister(GuestArchitecture::AArch64, I),
          {0x123456789abcdef0ULL + I, 0xfedcba9876543210ULL - I}));
    Before = state();
  }
  uint64_t storeValue() const {
    const uint64_t Mask = UINT64_MAX >> ((8 - testCase().Width) * 8);
    return Rt == 31 ? 0 : Before.Scalars[Rt] & Mask;
  }
  void expectStateAndMemory(bool Executed, bool InObserver = false) {
    auto Expected = Before;
    auto Bytes = Original;
    if (Executed) {
      Expected.Scalars[unsigned(AArch64Register::PC)] += 4;
      if (testCase().Load && Rt != 31) {
        uint64_t Value = 0;
        for (unsigned I = 0; I < testCase().Width; ++I)
          Value |= uint64_t(Bytes[Address - Data + I]) << (I * 8);
        Expected.Scalars[Rt] = Value;
      } else if (!testCase().Load) {
        for (unsigned I = 0; I < testCase().Width; ++I)
          Bytes[Address - Data + I] = uint8_t(storeValue() >> (I * 8));
      }
    }
    const auto Actual = state();
    EXPECT_EQ(Actual.Scalars, Expected.Scalars);
    EXPECT_EQ(Actual.Vectors, Expected.Vectors);
    std::vector<uint8_t> ActualBytes(PageSize);
    if (InObserver)
      llvm::cantFail(CPU->read(Data, ActualBytes));
    else
      llvm::cantFail(CPU->snapshotBacking(Data, ActualBytes));
    EXPECT_EQ(ActualBytes, Bytes);
    uint8_t CodeBytes[8];
    if (InObserver)
      llvm::cantFail(CPU->read(Code, CodeBytes));
    else
      llvm::cantFail(CPU->snapshotBacking(Code, CodeBytes));
    EXPECT_EQ(llvm::support::endian::read32le(CodeBytes), word());
    EXPECT_EQ(llvm::support::endian::read32le(CodeBytes + 4), 0xd503201fu);
  }
  ExecutionExit run(BackendHooks Hooks = {}) {
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, 1000000));
  }
  void expectFault(const ExecutionExit &Exit, BackendFaultKind Kind) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, Kind);
    EXPECT_EQ(Exit.Fault->PC, Code);
    EXPECT_EQ(Exit.Fault->Address, Address);
    EXPECT_EQ(Exit.Fault->Size, testCase().Width);
    EXPECT_EQ(Exit.Fault->Access, testCase().Load ? BackendAccessKind::Read
                                                  : BackendAccessKind::Write);
    expectStateAndMemory(false);
  }
};

TEST_P(AArch64AcquireRelease, RegisterViewsZeroRegisterAndAliasedBase) {
  for (auto Registers :
       {std::pair{0u, 1u}, {31u, 1u}, {1u, 1u}, {30u, 31u}, {31u, 31u}}) {
    std::tie(Rt, Rn) = Registers;
    SCOPED_TRACE(Rt);
    SCOPED_TRACE(Rn);
    // The fixed machine disables SP alignment traps. The data access itself
    // still needs natural alignment, including when its base is SP.
    Address = Data + 3 * testCase().Width;
    seed();
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, uint32_t Size) {
      ++Reads;
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, testCase().Width);
      expectStateAndMemory(false, true);
    };
    Hooks.Write = [&](uint64_t A, uint32_t Size, uint64_t Value) {
      ++Writes;
      EXPECT_EQ(A, Address);
      EXPECT_EQ(Size, testCase().Width);
      EXPECT_EQ(Value, storeValue());
      expectStateAndMemory(false, true);
    };
    auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, testCase().Load ? 1u : 0u);
    EXPECT_EQ(Writes, testCase().Load ? 0u : 1u);
    expectStateAndMemory(true);
  }
}

TEST_P(AArch64AcquireRelease, ExactPageEndNeedsOnlyItsOwnWidth) {
  Address = Data + PageSize - testCase().Width;
  seed();
  auto Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  expectStateAndMemory(true);
}

TEST_P(AArch64AcquireRelease, ObserverStopPreservesAllStateAndAllowsRetry) {
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t, uint32_t) {
    ++Seen;
    CPU->stop();
  };
  Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
    ++Seen;
    CPU->stop();
  };
  auto Exit = run(std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Seen, 1u);
  expectStateAndMemory(false);
  Exit = run();
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  expectStateAndMemory(true);
}

TEST_P(AArch64AcquireRelease, ReadOnlyPageCannotAuthorizeStore) {
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  auto Exit = run();
  if (testCase().Load) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectStateAndMemory(true);
  } else
    expectFault(Exit, BackendFaultKind::Protection);
}

TEST_P(AArch64AcquireRelease, WriteOnlyPageCannotAuthorizeLoad) {
  llvm::cantFail(CPU->protect(Data, PageSize, Write | UserAccessible));
  auto Exit = run();
  if (!testCase().Load) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectStateAndMemory(true);
  } else
    expectFault(Exit, BackendFaultKind::Protection);
}

TEST_P(AArch64AcquireRelease, SupervisorPageDoesNotAuthorizeUserAccess) {
  llvm::cantFail(CPU->protect(Data, PageSize, Read | Write));
  auto Exit = run();
  if (userMode())
    expectFault(Exit, BackendFaultKind::Protection);
  else {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectStateAndMemory(true);
  }
}

TEST_P(AArch64AcquireRelease, ZeroRegisterStillRequiresMappedMemory) {
  Rt = 31;
  Address = Data + PageSize;
  seed();
  expectFault(run(), BackendFaultKind::UnmappedMemory);
}

TEST_P(AArch64AcquireRelease, MisalignmentIsRejectedBeforeObserversOrEffects) {
  for (unsigned Offset = 1; Offset < testCase().Width; ++Offset) {
    SCOPED_TRACE(Offset);
    if (Offset != 1) {
      ASSERT_NO_FATAL_FAILURE(initialize());
      ASSERT_TRUE(CPU);
    }
    Address = Data + 128 + Offset;
    seed();
    unsigned Seen = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, uint32_t) { ++Seen; };
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Seen; };
    auto Exit = run(std::move(Hooks));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
    EXPECT_EQ(Seen, 0u);
    expectStateAndMemory(false);
  }
}

TEST_P(AArch64AcquireRelease, LimitedOrderingRemainsUnsupported) {
  // LDLAR/STLLR, LDAPR w0,[x1], and STLR w0,[x1,#-4]!.
  // The last encoding shares the STLR ID but needs FEAT_LRCPC3.
  const uint32_t Words[] = {word() ^ (1u << 15), 0xb8bfc020, 0x99800820};
  for (unsigned I = 0; I < std::size(Words); ++I) {
    if (I != 0) {
      ASSERT_NO_FATAL_FAILURE(initialize());
      ASSERT_TRUE(CPU);
    }
    uint8_t Bytes[4];
    llvm::support::endian::write32le(Bytes, Words[I]);
    llvm::cantFail(CPU->write(Code, Bytes));
    auto Exit = run();
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
    const auto Actual = state();
    EXPECT_EQ(Actual.Scalars, Before.Scalars);
    EXPECT_EQ(Actual.Vectors, Before.Vectors);
    std::vector<uint8_t> ActualBytes(PageSize);
    llvm::cantFail(CPU->snapshotBacking(Data, ActualBytes));
    EXPECT_EQ(ActualBytes, Original);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64AcquireRelease,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool(), testing::ValuesIn(Cases)));
} // namespace
} // namespace neverd::emulation
