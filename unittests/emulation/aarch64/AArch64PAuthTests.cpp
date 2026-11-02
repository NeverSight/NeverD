//===- AArch64PAuthTests.cpp - Disabled-key compatibility at EL0/EL1
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <iterator>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint64_t Code = 0x400000, Stack = 0x800000, PageSize = 4096;
// Independently assembled HINT selectors for PAC/AUT IA/IB, 1716/Z/SP.
constexpr unsigned Compatible[] = {8,  10, 12, 14, 24, 25,
                                   26, 27, 28, 29, 30, 31};
class AArch64PAuth
    : public testing::TestWithParam<std::tuple<ExecutionBackendKind, bool>> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override { initialize(); }
  void initialize() {
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
    llvm::cantFail(CPU->map(Stack, PageSize, Read | Write | UserAccessible));
    std::vector<uint8_t> Canary(64, 0xa5);
    llvm::cantFail(CPU->write(Stack, Canary));
    for (unsigned I = 0; I < 31; ++I)
      llvm::cantFail(CPU->setReg(static_cast<AArch64Register>(I),
                                 0xdabc012345678900ULL + I * 0x101));
    llvm::cantFail(CPU->setReg(AArch64Register::SP, Stack + PageSize - 16));
    llvm::cantFail(CPU->setReg(AArch64Register::FPCR, 0x03400000));
    llvm::cantFail(CPU->setReg(AArch64Register::FPSR, 0x08000002));
    llvm::cantFail(CPU->setReg(AArch64Register::NZCV, 0xa0000000));
    for (unsigned I = 0; I < 32; ++I)
      llvm::cantFail(CPU->writeRegister(
          vectorRegister(GuestArchitecture::AArch64, I),
          {0x123456789abcdef0ULL + I, 0xfedcba9876543210ULL - I}));
  }
  void check(uint32_t Word, bool Admitted) {
    if (!CPU) {
      ASSERT_NO_FATAL_FAILURE(initialize());
      ASSERT_TRUE(CPU);
    }
    uint8_t Bytes[8];
    llvm::support::endian::write32le(Bytes, Word);
    llvm::support::endian::write32le(Bytes + 4, 0xd503201f);
    llvm::cantFail(CPU->write(Code, Bytes));
    llvm::cantFail(CPU->setReg(AArch64Register::PC, Code));
    std::vector<uint64_t> Scalars;
    std::vector<RegisterValue> Vectors;
    for (unsigned I = 0; I <= unsigned(AArch64Register::FPSR); ++I)
      Scalars.push_back(
          llvm::cantFail(CPU->reg(static_cast<AArch64Register>(I))));
    for (unsigned I = 0; I < 32; ++I)
      Vectors.push_back(llvm::cantFail(
          CPU->readRegister(vectorRegister(GuestArchitecture::AArch64, I))));
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    auto Exit = CPU->runUntilExit(Code, 1000000);
    ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
    ASSERT_EQ(Exit->Kind, Admitted ? ExecutionExitKind::Stopped
                                   : ExecutionExitKind::UnsupportedOperation)
        << Exit->Diagnostic;
    Scalars[unsigned(AArch64Register::PC)] += Admitted ? 4 : 0;
    for (unsigned I = 0; I < Scalars.size(); ++I)
      EXPECT_EQ(llvm::cantFail(CPU->reg(static_cast<AArch64Register>(I))),
                Scalars[I])
          << I;
    for (unsigned I = 0; I < Vectors.size(); ++I)
      EXPECT_EQ(llvm::cantFail(CPU->readRegister(
                    vectorRegister(GuestArchitecture::AArch64, I))),
                Vectors[I])
          << I;
    std::vector<uint8_t> Canary(64), CodeBytes(8);
    llvm::cantFail(CPU->snapshotBacking(Stack, Canary));
    llvm::cantFail(CPU->snapshotBacking(Code, CodeBytes));
    EXPECT_EQ(Canary, std::vector<uint8_t>(64, 0xa5));
    EXPECT_EQ(CodeBytes, std::vector<uint8_t>(Bytes, Bytes + 8));
    // Instruction rejection is terminal. Each negative case needs a fresh
    // backend; successful instructions may continue in the same machine.
    if (!Admitted)
      CPU.reset();
  }
};
TEST_P(AArch64PAuth, OnlyDeclaredHintWordsPreserveCompleteState) {
  for (unsigned Selector = 0; Selector < 128; ++Selector) {
    SCOPED_TRACE(Selector);
    const bool Admitted =
        Selector == 0 || Selector == 32 || Selector == 34 || Selector == 36 ||
        Selector == 38 ||
        std::find(std::begin(Compatible), std::end(Compatible), Selector) !=
            std::end(Compatible);
    ASSERT_NO_FATAL_FAILURE(check(0xd503201f | (Selector << 5), Admitted));
  }
}
TEST_P(AArch64PAuth, CompatibilityPreservesZeroAndHighReturnAddressBits) {
  for (uint64_t Value : {uint64_t(0), uint64_t(0x1000), UINT64_MAX,
                         uint64_t(0x81800000abcdef00)}) {
    SCOPED_TRACE(Value);
    llvm::cantFail(CPU->setReg(AArch64Register::X17, Value));
    llvm::cantFail(CPU->setReg(AArch64Register::X30, Value));
    for (unsigned Selector : Compatible) {
      SCOPED_TRACE(Selector);
      ASSERT_NO_FATAL_FAILURE(check(0xd503201f | (Selector << 5), true));
    }
  }
}
TEST_P(AArch64PAuth, XPACLRIPreservesUnsignedLow48BitReturnAddresses) {
  // Independently assembled XPACLRI (HINT #7). Stripping a PAC has no effect
  // on these addresses under the checked machine's 48-bit translation regime.
  for (uint64_t Value : {uint64_t(0), uint64_t(1), uint64_t(0x401234),
                         uint64_t(0x800000000000), uint64_t(0xffffffffffff)}) {
    SCOPED_TRACE(Value);
    llvm::cantFail(CPU->setReg(AArch64Register::X30, Value));
    ASSERT_NO_FATAL_FAILURE(check(0xd50320ff, true));
  }
}
TEST_P(AArch64PAuth,
       XPACLRIRejectsReturnAddressesOutsideTheFixedPointContract) {
  // SCTLR authentication enables do not disable XPACLRI. A transport with
  // PAuth can change these bits while a baseline CPU treats HINT #7 as a NOP.
  for (uint64_t Value :
       {uint64_t(1) << 48, uint64_t(1) << 55, uint64_t(1) << 63,
        uint64_t(0x81800000abcdef00), UINT64_MAX}) {
    SCOPED_TRACE(Value);
    if (!CPU) {
      ASSERT_NO_FATAL_FAILURE(initialize());
      ASSERT_TRUE(CPU);
    }
    llvm::cantFail(CPU->setReg(AArch64Register::X30, Value));
    ASSERT_NO_FATAL_FAILURE(check(0xd50320ff, false));
  }
}
TEST_P(AArch64PAuth, NonHintAuthenticationAndControlAccessRemainUnsupported) {
  // pacia x0,x1; autia x0,x1; xpaci x0; retaa; braa x0,x1;
  // msr sctlr_el1,x0; mrs x0,apiakeylo_el1.
  for (uint32_t Word : {0xdac10020u, 0xdac11020u, 0xdac143e0u, 0xd65f0bffu,
                        0xd71f0801u, 0xd5181000u, 0xd5382100u}) {
    SCOPED_TRACE(Word);
    ASSERT_NO_FATAL_FAILURE(check(Word, false));
  }
}
INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64PAuth,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool()));
} // namespace
} // namespace neverd::emulation
