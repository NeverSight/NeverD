//===- AArch64CrossPageTests.cpp - Checked ARM64 RAM fragments ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <climits>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "../UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_ARM_CROSS_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_ARM_CROSS_TEXT(Name, Text) constexpr char Name[] = Text;
#include "AArch64CrossPageCases.def"
#undef NEVERD_ARM_CROSS_TEXT
#undef NEVERD_ARM_CROSS_VALUE

struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
};
const Profile Profiles[] = {
#define NEVERD_ARM_CROSS_PROFILE(Name, Backend, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, ExecutionContract::Contract},
#include "AArch64CrossPageCases.def"
#undef NEVERD_ARM_CROSS_PROFILE
};
struct InstructionCase {
  const char *Name;
  bool Load;
  unsigned Width, Count;
  int64_t AddressOffset, Writeback;
  uint64_t First, Second;
  uint32_t Word;
  unsigned bytes() const { return Width * Count; }
};
const InstructionCase Cases[] = {
#define NEVERD_ARM_CROSS_INSTRUCTION(Name, Load, Width, Count, Offset,         \
                                     Writeback, First, Second, Word)           \
  {#Name, Load, Width, Count, Offset, Writeback, First, Second, Word},
#include "AArch64CrossPageCases.def"
#undef NEVERD_ARM_CROSS_INSTRUCTION
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
void PrintTo(const InstructionCase &C, std::ostream *OS) { *OS << C.Name; }
using Parameter = std::tuple<Profile, InstructionCase>;

class AArch64CrossPage : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  uint64_t Address = 0;
  const InstructionCase &testCase() const { return std::get<1>(GetParam()); }
  bool userMode() const {
    return std::get<0>(GetParam()).Contract ==
           ExecutionContract::CheckedUserAArch64;
  }
  uint64_t base() const { return Address - uint64_t(testCase().AddressOffset); }
  void SetUp() override {
    const auto &P = std::get<0>(GetParam());
    auto B = createExecutionBackend(P.Backend, P.Contract, Limit,
                                    GuestArchitecture::AArch64);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(P.Backend, GuestArchitecture::AArch64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    // Interpose an allocation between adjacent virtual data pages.
    llvm::cantFail(CPU->map(Alias, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->map(Data + PageSize, PageSize, Read | Write | UserAccessible));
    std::array<uint8_t, CodeWords * sizeof(uint32_t)> Bytes{};
    llvm::support::endian::write32le(Bytes.data(), testCase().Word);
    llvm::support::endian::write32le(Bytes.data() + sizeof(uint32_t), Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    Address = Data + PageSize - testCase().bytes() / RegisterCount;
    seed();
  }
  std::array<uint8_t, MaxBytes> original() const {
    std::array<uint8_t, MaxBytes> Bytes{};
    llvm::support::endian::write64le(Bytes.data(), MemoryFirst);
    llvm::support::endian::write64le(Bytes.data() + WordBytes, MemorySecond);
    return Bytes;
  }
  void seed() {
    llvm::cantFail(CPU->write(Address, original()));
    llvm::cantFail(CPU->setReg(AArch64Register::X0, InitialFirst));
    llvm::cantFail(CPU->setReg(AArch64Register::X2, InitialSecond));
    llvm::cantFail(CPU->setReg(AArch64Register::X1, base()));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, InitialNZCV));
  }
  std::array<uint8_t, MaxBytes> memory(bool InObserver = false) const {
    std::array<uint8_t, MaxBytes> Bytes{};
    if (InObserver)
      llvm::cantFail(CPU->read(Address, Bytes));
    else
      llvm::cantFail(CPU->snapshotBacking(Address, Bytes));
    return Bytes;
  }
  ExecutionExit run(BackendHooks Hooks = {}) {
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectCPUUnchanged() {
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), InitialFirst);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X2)), InitialSecond);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X1)), base());
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)), Code);
  }
  void expectUnchanged() {
    EXPECT_EQ(memory(), original());
    expectCPUUnchanged();
  }
  void expectResult() {
    auto Expected = original();
    if (!testCase().Load) {
      const uint64_t Values[] = {InitialFirst, InitialSecond};
      for (unsigned N = 0; N < testCase().Count; ++N)
        for (unsigned I = 0; I < testCase().Width; ++I)
          Expected[N * testCase().Width + I] =
              uint8_t(Values[N] >> (I * CHAR_BIT));
    }
    EXPECT_EQ(memory(), Expected);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X0)), testCase().First);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X2)), testCase().Second);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::X1)),
              base() + uint64_t(testCase().Writeback));
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::NZCV)), InitialNZCV);
    EXPECT_EQ(llvm::cantFail(CPU->reg(AArch64Register::PC)),
              Code + sizeof(uint32_t));
  }
  void expectFault(const ExecutionExit &Exit, BackendFaultKind Kind) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, Kind);
    EXPECT_EQ(Exit.Fault->PC, Code);
    EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
    const unsigned Prefix = testCase().bytes() / RegisterCount;
    EXPECT_EQ(Exit.Fault->Size, testCase().Width - Prefix % testCase().Width);
    EXPECT_EQ(Exit.Fault->Access, testCase().Load ? BackendAccessKind::Read
                                                  : BackendAccessKind::Write);
  }
};

TEST_P(AArch64CrossPage, ExecutesEveryCrossingWithoutContiguousBacking) {
  for (unsigned Prefix = 1; Prefix < testCase().bytes(); ++Prefix) {
    SCOPED_TRACE(Prefix);
    Address = Data + PageSize - Prefix;
    seed();
    unsigned Reads = 0, Writes = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t A, uint32_t Size) {
      EXPECT_EQ(A, Address + Reads * testCase().Width);
      EXPECT_EQ(Size, testCase().Width);
      EXPECT_EQ(memory(true), original());
      ++Reads;
    };
    Hooks.Write = [&](uint64_t A, uint32_t Size, uint64_t Value) {
      EXPECT_EQ(A, Address + Writes * testCase().Width);
      EXPECT_EQ(Size, testCase().Width);
      const uint64_t Mask = UINT64_MAX >> ((WordBytes - Size) * CHAR_BIT);
      EXPECT_EQ(Value, (Writes ? InitialSecond : InitialFirst) & Mask);
      EXPECT_EQ(memory(true), original());
      ++Writes;
    };
    auto Exit = run(std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, testCase().Load ? testCase().Count : 0u);
    EXPECT_EQ(Writes, testCase().Load ? 0u : testCase().Count);
    expectResult();
  }
}

TEST_P(AArch64CrossPage, ObserverStopsBeforeMemoryRegistersFlagsOrWriteback) {
  unsigned Seen = 0;
  BackendHooks Hooks;
  if (testCase().Load)
    Hooks.Read = [&](uint64_t, uint32_t) {
      if (++Seen == testCase().Count)
        CPU->stop();
    };
  else
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
      if (++Seen == testCase().Count)
        CPU->stop();
    };
  auto Exit = run(std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Seen, testCase().Count);
  expectUnchanged();
}

TEST_P(AArch64CrossPage, SecondPageFaultPrecedesPairPrefixAndWriteback) {
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, UserAccessible));
  expectFault(run(), BackendFaultKind::Protection);
  expectUnchanged();
}

TEST_P(AArch64CrossPage, SecondPageReadPermissionCannotAuthorizeStores) {
  llvm::cantFail(
      CPU->protect(Data + PageSize, PageSize, Read | UserAccessible));
  auto Exit = run();
  if (testCase().Load) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectResult();
  } else {
    expectFault(Exit, BackendFaultKind::Protection);
    expectUnchanged();
  }
}

TEST_P(AArch64CrossPage, SecondPageSupervisorRightsDoNotAuthorizeUserAccess) {
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, Read | Write));
  auto Exit = run();
  if (userMode()) {
    expectFault(Exit, BackendFaultKind::Protection);
    expectUnchanged();
  } else {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectResult();
  }
}

TEST_P(AArch64CrossPage, UnmappedSecondPagePreservesBothAllocationsAndCPU) {
  auto View = llvm::cantFail(CPU->pinBacking(Address, MaxBytes));
  llvm::cantFail(CPU->addressSpace()->unmap(Data + PageSize, PageSize));
  expectFault(run(), BackendFaultKind::UnmappedMemory);
  std::array<uint8_t, MaxBytes> Bytes{};
  llvm::cantFail(View.read(0, Bytes));
  EXPECT_EQ(Bytes, original());
  expectCPUUnchanged();
}

TEST_P(AArch64CrossPage, RecoverableSecondPageFaultCanBeConsumedAndRetried) {
  llvm::cantFail(CPU->protect(Data + PageSize, PageSize, UserAccessible));
  BackendHooks Hooks;
  Hooks.RecoverableFault = [&](const BackendFault &F) {
    EXPECT_EQ(F.Address, Data + PageSize);
    return true;
  };
  auto Exit = run(std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault) << Exit.Diagnostic;
  EXPECT_FALSE(CPU->fault());
  EXPECT_NE(llvm::toString(CPU->setReg(AArch64Register::X0, InitialFirst)), "");
  ASSERT_TRUE(CPU->takeRecoverableFault());
  EXPECT_FALSE(CPU->takeRecoverableFault());
  expectUnchanged();
  llvm::cantFail(
      CPU->protect(Data + PageSize, PageSize, Read | Write | UserAccessible));
  auto Retry = run();
  ASSERT_EQ(Retry.Kind, ExecutionExitKind::Stopped) << Retry.Diagnostic;
  expectResult();
}

TEST_P(AArch64CrossPage, AdjacentAliasesCanRepeatOnePhysicalAllocation) {
  llvm::cantFail(
      CPU->mapAlias(Stack, Data, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->mapAlias(Stack + PageSize, Data, PageSize,
                               Read | Write | UserAccessible));
  for (unsigned Prefix = 1; Prefix < testCase().bytes(); ++Prefix) {
    SCOPED_TRACE(Prefix);
    Address = Stack + PageSize - Prefix;
    seed();
    auto Exit = run();
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    expectResult();
    auto Bytes = memory();
    std::array<uint8_t, MaxBytes> First{}, Second{};
    llvm::cantFail(CPU->snapshotBacking(
        Data + PageSize - Prefix,
        llvm::MutableArrayRef<uint8_t>(First).take_front(Prefix)));
    llvm::cantFail(CPU->snapshotBacking(
        Data,
        llvm::MutableArrayRef<uint8_t>(Second).take_front(MaxBytes - Prefix)));
    EXPECT_TRUE(
        std::equal(First.begin(), First.begin() + Prefix, Bytes.begin()));
    EXPECT_TRUE(std::equal(Second.begin(), Second.begin() + MaxBytes - Prefix,
                           Bytes.begin() + Prefix));
  }
}

TEST_P(AArch64CrossPage, ContextRestoreUsesCurrentAliasPagesAndRetiresOldOnes) {
  llvm::cantFail(
      CPU->mapAlias(Stack, Data, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(CPU->mapAlias(Stack + PageSize, Data + PageSize, PageSize,
                               Read | Write | UserAccessible));
  Address = Stack + PageSize - testCase().bytes() / RegisterCount;
  seed();
  auto Saved = llvm::cantFail(CPU->saveContext());
  auto First = run();
  ASSERT_EQ(First.Kind, ExecutionExitKind::Stopped) << First.Diagnostic;
  expectResult();
  llvm::cantFail(CPU->replaceAliases(
      {{Stack + PageSize, PageSize}},
      {{Stack + PageSize, Data, PageSize, Read | Write | UserAccessible}}));
  std::array<uint8_t, MaxBytes> Poison{};
  llvm::cantFail(CPU->write(Data + PageSize, Poison));
  llvm::cantFail(CPU->write(Address, original()));
  llvm::cantFail(CPU->restoreContext(*Saved));
  auto Second = run();
  ASSERT_EQ(Second.Kind, ExecutionExitKind::Stopped) << Second.Diagnostic;
  expectResult();
  std::array<uint8_t, MaxBytes> Retired{};
  llvm::cantFail(CPU->snapshotBacking(Data + PageSize, Retired));
  EXPECT_EQ(Retired, Poison);
}

INSTANTIATE_TEST_SUITE_P(Cases, AArch64CrossPage,
                         testing::Combine(testing::ValuesIn(Profiles),
                                          testing::ValuesIn(Cases)),
                         [](const testing::TestParamInfo<Parameter> &Info) {
                           return std::string(std::get<0>(Info.param).Name) +
                                  NameDelimiter + std::get<1>(Info.param).Name;
                         });
} // namespace
} // namespace neverd::emulation
