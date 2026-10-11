//===- UnicornObservationTests.cpp - Flat x64 stopped observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x100000, Data = 0x200000, Alias = 0x300000;
constexpr uint64_t Page = 4096, Timeout = 1000000;
constexpr uint8_t Stores[] = {0x89, 0x07, 0x83, 0xc0, 0x01,
                              0x89, 0x07, 0xeb, 0xfe};
class UnicornObservation : public testing::Test {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto Backend = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                          ExecutionContract::Legacy, 16 * Page);
    if (!Backend) {
      auto E = Backend.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(Backend->CPU);
    llvm::cantFail(CPU->map(Code, Page, Read | Write | Execute));
    llvm::cantFail(CPU->map(Data, Page, Read | Write));
    llvm::cantFail(CPU->mapAlias(Alias, Data, Page, Read | Execute));
    llvm::cantFail(CPU->write(Code, Stores));
    llvm::cantFail(CPU->setReg(X64Register::DI, Data));
    llvm::cantFail(CPU->setReg(X64Register::AX, 1));
  }
};

TEST_F(UnicornObservation, CommittedAliasWritesStopBeforeTheNextAdmission) {
  ASSERT_EQ(llvm::toString(CPU->setMemoryWriteWatches({{Alias + 1, 2}})), "");
  for (const auto W :
       {MemoryWriteWatch{Data, 0}, MemoryWriteWatch{UINT64_MAX, 2}}) {
    auto E = CPU->setMemoryWriteWatches({W});
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  unsigned Instructions = 0, Writes = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) {
    if (++Instructions > 10)
      CPU->stop();
  };
  Hooks.MemoryWritten = [&] {
    ++Writes;
    CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  llvm::cantFail(CPU->run(Code, Timeout));
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(Instructions, 1u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code + 2);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 4)), 1u);
  llvm::cantFail(CPU->run(Code + 2, Timeout));
  EXPECT_EQ(Writes, 2u);
  EXPECT_EQ(Instructions, 3u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code + 7);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 4)), 2u);
  // A watch follows its alias's new physical backing without being replaced.
  llvm::cantFail(CPU->map(Data + Page, Page, Read | Write));
  llvm::cantFail(CPU->replaceAliases(
      {{Alias, Page}}, {{Alias, Data + Page, Page, Read | Execute}}));
  llvm::cantFail(CPU->run(Code, Timeout));
  EXPECT_EQ(Writes, 2u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + Page, 4)), 0u);
}

TEST_F(UnicornObservation, CancelledInstructionDoesNotPublishAWriteWatch) {
  ASSERT_EQ(llvm::toString(CPU->setMemoryWriteWatches({{Data, 4}})), "");
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) { CPU->stop(); };
  Hooks.MemoryWritten = [&] { ++Writes; };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  llvm::cantFail(CPU->run(Code, Timeout));
  EXPECT_EQ(Writes, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 4)), 0u);
  Hooks = {};
  Hooks.MemoryWritten = [&] {
    ++Writes;
    CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  llvm::cantFail(CPU->run(Code, Timeout));
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 4)), 1u);
}

TEST_F(UnicornObservation, CodeWritesNotifyOnlyAfterTheStoreRetires) {
  ASSERT_EQ(llvm::toString(CPU->setMemoryWriteWatches({{Code + 5, 4}})), "");
  llvm::cantFail(CPU->setReg(X64Register::DI, Code + 5));
  llvm::cantFail(CPU->setReg(X64Register::AX, 0x90909090));
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.MemoryWritten = [&] {
    ++Writes;
    CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  llvm::cantFail(CPU->run(Code, Timeout));
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code + 2);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Code + 5, 4)), 0x90909090u);
}

TEST_F(UnicornObservation, RepeatedStoresNotifyWithTheSamePCAndUpdatedState) {
  constexpr uint8_t Rep[] = {0xf3, 0xaa, 0xeb, 0xfe}; // REP STOSB
  llvm::cantFail(CPU->write(Code, Rep));
  llvm::cantFail(CPU->setReg(X64Register::CX, 2));
  ASSERT_EQ(llvm::toString(CPU->setMemoryWriteWatches({{Data, 2}})), "");
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.MemoryWritten = [&] {
    ++Writes;
    CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  llvm::cantFail(CPU->run(Code, Timeout));
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), 1u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DI)), Data + 1);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 2)), 1u);
  llvm::cantFail(CPU->run(Code, Timeout));
  EXPECT_EQ(Writes, 2u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DI)), Data + 2);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 2)), 0x101u);
}

TEST_F(UnicornObservation, SelfCallNotifiesEvenWhenTheStoredValueIsUnchanged) {
  constexpr uint8_t Call[] = {0xe8, 0xfb, 0xff, 0xff, 0xff};
  llvm::cantFail(CPU->write(Code, Call));
  llvm::cantFail(CPU->setReg(X64Register::SP, Data + 16));
  llvm::cantFail(CPU->writeInteger(Data + 8, Code + 5, 8));
  ASSERT_EQ(llvm::toString(CPU->setMemoryWriteWatches({{Data, 16}})), "");
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.MemoryWritten = [&] {
    ++Writes;
    CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  llvm::cantFail(CPU->run(Code, Timeout));
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SP)), Data + 8);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + 8, 8)), Code + 5);
}

TEST_F(UnicornObservation, FaultingStoreKeepsPriorityOverAWriteWatch) {
  ASSERT_EQ(llvm::toString(CPU->setMemoryWriteWatches({{Data, Page}})), "");
  llvm::cantFail(CPU->setReg(X64Register::DI, Data + Page - 2));
  unsigned Writes = 0;
  BackendHooks Hooks;
  Hooks.MemoryWritten = [&] { ++Writes; };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  auto Exit = CPU->runUntilExit(Code, Timeout);
  ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
  EXPECT_EQ(Exit->Kind, ExecutionExitKind::GuestFault);
  EXPECT_EQ(Writes, 0u);
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->PC, Code);
}

TEST_F(UnicornObservation, StoppedInspectionKeepsStateAndGuestFaultsUntouched) {
  const auto Before = llvm::cantFail(CPU->reg(X64Register::PC));
  constexpr uint8_t Nop[] = {0x90}, Prefix[] = {0x0f};
  llvm::cantFail(CPU->write(Code + Page - 1, Nop));
  auto Size = CPU->instructionSize(Code + Page - 1);
  ASSERT_TRUE(bool(Size)) << llvm::toString(Size.takeError());
  EXPECT_EQ(*Size, 1u);
  llvm::cantFail(CPU->write(Code + Page - 1, Prefix));
  for (const auto Address : {Code + Page - 1, Data, Code + 2 * Page}) {
    auto Invalid = CPU->instructionSize(Address);
    ASSERT_FALSE(bool(Invalid));
    llvm::consumeError(Invalid.takeError());
    EXPECT_FALSE(CPU->fault());
  }
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Before);
}
} // namespace
} // namespace neverd::emulation
