//===- AArch64BTITests.cpp - Original landing pads in unguarded pages -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x400000, Data = 0x800000, PageSize = 4096;
// Independent encodings: bti; bti c; bti j; bti jc.
constexpr uint32_t Targets[] = {0xd503241f, 0xd503245f, 0xd503249f, 0xd50324df};
class AArch64BTI
    : public testing::TestWithParam<std::tuple<ExecutionBackendKind, bool>> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    const auto [Kind, User] = GetParam();
    const auto Contract = User ? ExecutionContract::CheckedUserAArch64
                               : ExecutionContract::CheckedAArch64;
    const auto Capabilities = llvm::cantFail(
        executionCapabilities(Contract, GuestArchitecture::AArch64, Kind));
    EXPECT_EQ(std::count_if(Capabilities.InstructionFamilies.begin(),
                            Capabilities.InstructionFamilies.end(),
                            [](const char *Name) {
                              return llvm::StringRef(Name) == "BTI";
                            }),
              1);
    auto Created = createExecutionBackend(Kind, Contract, 4 * 1024 * 1024,
                                          GuestArchitecture::AArch64);
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
    llvm::cantFail(CPU->setReg(AArch64Register::PC, Code));
    llvm::cantFail(CPU->setReg(AArch64Register::SP, Data + PageSize - 16));
    llvm::cantFail(CPU->setReg(AArch64Register::X2, 41));
    llvm::cantFail(CPU->setReg(AArch64Register::X3, Data));
  }
  std::vector<uint8_t> code(uint64_t Address, llvm::ArrayRef<uint32_t> Words) {
    std::vector<uint8_t> Bytes(Words.size() * 4);
    for (unsigned I = 0; I < Words.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + 4 * I, Words[I]);
    llvm::cantFail(CPU->write(Address, Bytes));
    return Bytes;
  }
  uint64_t reg(AArch64Register R) { return llvm::cantFail(CPU->reg(R)); }
  void stopped(uint64_t PC) {
    auto Exit = CPU->runUntilExit(PC, 1000000);
    ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
    ASSERT_EQ(Exit->Kind, ExecutionExitKind::Stopped) << Exit->Diagnostic;
  }
};

TEST_P(AArch64BTI, DirectIndirectAndReturnTargetsExecuteOriginalWords) {
  // b; bl; br x0; br x16; br x17; blr x0; ret. Each goes to Code + 64.
  constexpr uint32_t Branches[] = {0x14000010, 0x94000010, 0xd61f0000,
                                   0xd61f0200, 0xd61f0220, 0xd63f0000,
                                   0xd65f03c0};
  constexpr uint64_t Target = Code + 64, End = Target + 12;
  for (uint32_t Landing : Targets)
    for (uint32_t Branch : Branches) {
      SCOPED_TRACE(Landing);
      SCOPED_TRACE(Branch);
      const auto Original =
          code(Target, {Landing, 0x91000442, 0xf9000062, 0xd503201f});
      code(Code, {Branch, 0xd503201f});
      for (auto R : {AArch64Register::X0, AArch64Register::X16,
                     AArch64Register::X17, AArch64Register::X30})
        llvm::cantFail(CPU->setReg(R, Target));
      llvm::cantFail(CPU->setReg(AArch64Register::X2, 41));
      llvm::cantFail(CPU->writeInteger(Data, 0, 8));
      std::vector<uint64_t> Trace;
      BackendHooks Hooks;
      Hooks.Instruction = [&](uint64_t PC, uint32_t) {
        Trace.push_back(PC);
        if (PC == End || PC == Code + 4)
          CPU->stop();
      };
      llvm::cantFail(CPU->installHooks(std::move(Hooks)));
      ASSERT_NO_FATAL_FAILURE(stopped(Code));
      EXPECT_EQ(Trace, (std::vector<uint64_t>{Code, Target, Target + 4,
                                              Target + 8, End}));
      EXPECT_EQ(reg(AArch64Register::PC), End);
      EXPECT_EQ(reg(AArch64Register::X2), 42u);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 8)), 42u);
      const bool Call = Branch == 0x94000010 || Branch == 0xd63f0000;
      EXPECT_EQ(reg(AArch64Register::X30), Call ? Code + 4 : Target);
      std::vector<uint8_t> After(Original.size());
      llvm::cantFail(CPU->snapshotBacking(Target, After));
      EXPECT_EQ(After, Original);
    }
}

TEST_P(AArch64BTI, ObserverStopAndSavedContextPreserveTheInstructionBoundary) {
  code(Code, {0xd503245f, 0x91000442, 0xf9000062, 0xd50324df, 0xd503201f});
  auto Before = llvm::cantFail(CPU->saveContext());
  bool Cancel = true;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if ((Cancel && PC == Code) || PC == Code + 12)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  ASSERT_NO_FATAL_FAILURE(stopped(Code));
  EXPECT_EQ(reg(AArch64Register::PC), Code);
  EXPECT_EQ(reg(AArch64Register::X2), 41u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 8)), 0u);
  Cancel = false;
  ASSERT_NO_FATAL_FAILURE(stopped(Code));
  EXPECT_EQ(reg(AArch64Register::PC), Code + 12);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 8)), 42u);
  auto After = llvm::cantFail(CPU->saveContext());
  llvm::cantFail(CPU->writeInteger(Data, 99, 8));
  llvm::cantFail(CPU->restoreContext(*Before));
  EXPECT_EQ(reg(AArch64Register::PC), Code);
  EXPECT_EQ(reg(AArch64Register::X2), 41u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 8)), 99u);
  ASSERT_NO_FATAL_FAILURE(stopped(Code));
  llvm::cantFail(CPU->restoreContext(*After));
  BackendHooks Final;
  Final.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC == Code + 16)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Final)));
  ASSERT_NO_FATAL_FAILURE(stopped(Code + 12));
  EXPECT_EQ(reg(AArch64Register::PC), Code + 16);
  EXPECT_EQ(reg(AArch64Register::X2), 42u);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 8)), 42u);
}

TEST_P(AArch64BTI, EveryLandingPadConsumesTheSameSharedBudget) {
  code(Code, {Targets[0], Targets[1], Targets[2], Targets[3], 0x91000442});
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
  }
  EXPECT_EQ(Trace,
            (std::vector<uint64_t>{Code, Code + 4, Code + 8, Code + 12}));
  EXPECT_EQ(llvm::cantFail(Session->cpu().reg(AArch64Register::PC)), Code + 16);
  EXPECT_EQ(llvm::cantFail(Session->cpu().reg(AArch64Register::X2)), 41u);
}

INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64BTI,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool()));
} // namespace
} // namespace neverd::emulation
