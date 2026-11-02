//===- X64MXCSRTests.cpp - Checked floating control state ----------------===//
// NeverD Decompiler
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64Exception.h"
#include "arch/x86_64/X64Machine.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <map>
#include <stdexcept>

namespace neverd::emulation {
namespace {
#define NEVERD_MXCSR_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MXCSR_CODE(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_MXCSR_TEXT(Name, Text) constexpr char Name[] = Text;
#include "X64MXCSRCases.def"
#undef NEVERD_MXCSR_VALUE
#undef NEVERD_MXCSR_CODE
#undef NEVERD_MXCSR_TEXT
struct Parameter {
  ExecutionBackendKind Kind;
  bool User;
  std::string name() const {
    return std::string(executionBackendName(Kind)) +
           (User ? UserSuffix : SupervisorSuffix);
  }
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.name(); }
class X64MXCSR : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  uint64_t Mask = 0;
  void SetUp() override {
    auto B = createExecutionBackend(GetParam().Kind,
                                    GetParam().User
                                        ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64,
                                    Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Why = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Why;
      FAIL() << Why;
    }
    CPU = std::move(B->CPU);
    Mask = llvm::cantFail(CPU->supportedControlBits(CPURegister::X64MXCSR))[0];
    llvm::cantFail(
        CPU->map(Code, Page, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, 2 * Page, Read | Write | UserAccessible));
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
  }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Bytes, BackendHooks Hooks = {}) {
    std::vector<uint8_t> Program(Bytes.begin(), Bytes.end());
    Program.push_back(Nop);
    llvm::cantFail(CPU->write(Code, Program));
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  uint64_t backing(uint64_t Address, unsigned Size = Word) {
    std::array<uint8_t, Word> Bytes{};
    const auto Error = llvm::toString(CPU->snapshotBacking(
        Address, llvm::MutableArrayRef(Bytes).take_front(Size)));
    EXPECT_TRUE(Error.empty()) << Error;
    return llvm::support::endian::read32le(Bytes.data());
  }
  using State = std::map<CPURegister, RegisterValue>;
  State state() {
    State Result;
    for (unsigned R = unsigned(CPURegister::X64AX);
         R <= unsigned(CPURegister::X64FP7); ++R) {
      auto V = CPU->readRegister(static_cast<CPURegister>(R));
      if (V)
        Result[static_cast<CPURegister>(R)] = *V;
      else
        llvm::consumeError(V.takeError());
    }
    return Result;
  }
};
TEST_P(X64MXCSR, CapabilityAndRegisterWritesAreAtomic) {
  EXPECT_TRUE(Mask == Baseline || Mask == Complete);
  auto Other = CPU->supportedControlBits(CPURegister::AArch64FPCR);
  EXPECT_FALSE(bool(Other));
  llvm::consumeError(Other.takeError());
  const auto Value = Initial | (Mask & DAZ) | FTZ | Sticky;
  llvm::cantFail(CPU->setReg(X64Register::MXCSR, Value));
  const auto Before = state();
  for (const auto V : {RegisterValue{Initial | Reserved, 0},
                       RegisterValue{Initial, 1}, RegisterValue{0, 0}}) {
    auto E = CPU->writeRegister(CPURegister::X64MXCSR, V);
    if (!V[0] && !V[1] && CPU->supportsSIMDExceptions()) {
      ASSERT_EQ(llvm::toString(std::move(E)), "");
      auto Expected = Before;
      Expected[CPURegister::X64MXCSR] = V;
      EXPECT_EQ(state(), Expected);
      llvm::cantFail(CPU->setReg(X64Register::MXCSR, Value));
    } else {
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
      EXPECT_EQ(state(), Before);
    }
  }
  auto Saved = llvm::cantFail(CPU->saveContext());
  llvm::cantFail(CPU->setReg(X64Register::MXCSR, Initial));
  llvm::cantFail(CPU->restoreContext(*Saved));
  EXPECT_EQ(state(), Before);
  EXPECT_EQ(llvm::cantFail(CPU->supportedControlBits(CPURegister::X64MXCSR))[0],
            Mask);
}
TEST_P(X64MXCSR, LoadsAndStoresUseExactlyFourBytesAcrossPageBoundaries) {
  for (unsigned Offset = 0; Offset <= Word; ++Offset)
    for (uint64_t Rounding = 0; Rounding < RoundingCount; ++Rounding) {
      const uint64_t Address = Data + Page - Word + Offset;
      const auto Value =
          Initial | (Mask & DAZ) | FTZ | Sticky | Rounding * RoundingStep;
      std::vector<uint8_t> Before(2 * Page, Fill);
      llvm::cantFail(CPU->write(Data, Before));
      llvm::cantFail(CPU->setReg(X64Register::CX, Address));
      llvm::cantFail(CPU->setReg(X64Register::MXCSR, Value));
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t, uint32_t) { ++Reads; };
      H.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
        EXPECT_EQ(A, Address);
        EXPECT_EQ(N, Word);
        EXPECT_EQ(V, Value);
        ++Writes;
      };
      auto E = run(Store, H);
      ASSERT_EQ(E.Kind, ExecutionExitKind::Stopped) << E.Diagnostic;
      EXPECT_EQ(Reads, 0u);
      EXPECT_EQ(Writes, 1u);
      llvm::support::endian::write32le(Before.data() + Address - Data, Value);
      std::vector<uint8_t> Actual(Before.size());
      llvm::cantFail(CPU->read(Data, Actual));
      EXPECT_EQ(Actual, Before);
      llvm::cantFail(CPU->setReg(X64Register::MXCSR, Initial));
      H.Read = [&](uint64_t A, uint32_t N) {
        EXPECT_EQ(A, Address);
        EXPECT_EQ(N, Word);
        ++Reads;
      };
      E = run(Load, H);
      ASSERT_EQ(E.Kind, ExecutionExitKind::Stopped) << E.Diagnostic;
      EXPECT_EQ(Reads, 1u);
      EXPECT_EQ(Writes, 1u);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)), Value);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), Flags);
    }
}
TEST_P(X64MXCSR, ObserversStopOrThrowBeforeEffectsAndRetry) {
  for (const bool Loading : {false, true})
    for (const bool Throw : {false, true}) {
      SetUp();
      ASSERT_TRUE(CPU);
      llvm::cantFail(CPU->setReg(X64Register::PC, Code));
      llvm::cantFail(CPU->setReg(X64Register::MXCSR, Initial));
      const uint64_t Value = Initial | (Mask & DAZ) | FTZ | Sticky;
      llvm::cantFail(CPU->writeInteger(Data, Value, Word));
      const auto Before = state();
      unsigned Events = 0;
      auto Observe = [&] {
        ++Events;
        if (Throw)
          throw std::runtime_error(ObserverError);
        CPU->stop();
      };
      BackendHooks H;
      H.Read = [&](uint64_t, uint32_t) { Observe(); };
      H.Write = [&](uint64_t, uint32_t, uint64_t) { Observe(); };
      const auto Bytes = Loading ? llvm::ArrayRef(Load) : llvm::ArrayRef(Store);
      auto E = run(Bytes, H);
      EXPECT_EQ(E.Kind, Throw ? ExecutionExitKind::BackendFailure
                              : ExecutionExitKind::Stopped)
          << E.Diagnostic;
      EXPECT_EQ(Events, 1u);
      EXPECT_EQ(state(), Before);
      EXPECT_EQ(backing(Data), Value);
      if (Throw)
        continue;
      E = run(Bytes);
      ASSERT_EQ(E.Kind, ExecutionExitKind::Stopped) << E.Diagnostic;
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)),
                Loading ? Value : Initial);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Word)),
                Loading ? Value : Initial);
    }
}
TEST_P(X64MXCSR, ReservedBitsFaultAndUnmaskedValuesFollowCapability) {
  for (const auto Value : {Initial | Reserved, uint64_t(0)}) {
    SetUp();
    ASSERT_TRUE(CPU);
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->writeInteger(Data, Value, Word));
    const auto Before = state();
    auto E = run(Load);
    if (!Value && CPU->supportsSIMDExceptions()) {
      EXPECT_EQ(E.Kind, ExecutionExitKind::Stopped) << E.Diagnostic;
      auto Expected = Before;
      Expected[CPURegister::X64MXCSR] = {Value, 0};
      Expected[CPURegister::X64PC] = {Code + sizeof(Load), 0};
      EXPECT_EQ(state(), Expected);
      continue;
    }
    EXPECT_EQ(E.Kind, Value ? ExecutionExitKind::GuestTrap
                            : ExecutionExitKind::UnsupportedOperation)
        << E.Diagnostic;
    if (Value) {
      ASSERT_TRUE(E.Fault);
      EXPECT_EQ(E.Fault->Interrupt,
                unsigned(x64::ExceptionVector::GeneralProtection));
      EXPECT_EQ(E.Fault->ErrorCode, std::optional<uint64_t>(0));
    }
    EXPECT_EQ(state(), Before);
  }
}
TEST_P(X64MXCSR, MissingPageAndDeniedStoresCannotPublishPartialEffects) {
  for (const bool Missing : {false, true})
    for (const bool Loading : {false, true}) {
      SetUp();
      ASSERT_TRUE(CPU);
      const uint64_t Address = Data + (Missing ? 2 : 1) * Page - Word / 2;
      llvm::cantFail(CPU->setReg(X64Register::CX, Address));
      llvm::cantFail(CPU->setReg(X64Register::PC, Code));
      llvm::cantFail(CPU->writeInteger(Address, Initial, Word / 2));
      if (!Missing)
        llvm::cantFail(
            CPU->protect(Data + Page, Page, Execute | UserAccessible));
      const auto Before = state();
      auto E = run(Loading ? llvm::ArrayRef(Load) : llvm::ArrayRef(Store));
      EXPECT_EQ(E.Kind, ExecutionExitKind::GuestFault) << E.Diagnostic;
      ASSERT_TRUE(E.Fault);
      EXPECT_EQ(E.Fault->Access,
                Loading ? BackendAccessKind::Read : BackendAccessKind::Write);
      EXPECT_EQ(state(), Before);
      EXPECT_EQ(backing(Address, Word / 2), Initial);
    }
}
TEST_P(X64MXCSR, GuestLoadedDAZChangesArithmeticAndSurvivesContextSwitch) {
  if (!(Mask & DAZ))
    GTEST_SKIP();
  llvm::cantFail(CPU->writeInteger(Data, Initial | DAZ, Word));
  ASSERT_EQ(run(Load).Kind, ExecutionExitKind::Stopped);
  auto Saved = llvm::cantFail(CPU->saveContext());
  for (const bool Restored : {false, true}) {
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, Initial));
    if (Restored)
      llvm::cantFail(CPU->restoreContext(*Saved));
    llvm::cantFail(CPU->setXmm(0, {}));
    llvm::cantFail(CPU->setXmm(1, {FloatNegativeTiny, Sentinel}));
    ASSERT_EQ(run(Add).Kind, ExecutionExitKind::Stopped);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0))[0], Restored ? 0 : FloatNegativeTiny);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)),
              Restored ? Initial | DAZ : Initial | Denormal);
  }
}
INSTANTIATE_TEST_SUITE_P(
    ExplicitBackends, X64MXCSR,
    testing::Values(Parameter{ExecutionBackendKind::Unicorn, false},
                    Parameter{ExecutionBackendKind::Unicorn, true},
                    Parameter{ExecutionBackendKind::KVM, false},
                    Parameter{ExecutionBackendKind::KVM, true},
                    Parameter{ExecutionBackendKind::WHP, false},
                    Parameter{ExecutionBackendKind::WHP, true}),
    [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation
