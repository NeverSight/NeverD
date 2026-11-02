//===- X64StateTransitionTests.cpp - Native state across entry boundaries -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "arch/x86_64/X64ExceptionMonitor.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
#define NEVERD_X64_TRANSITION_VALUE(Name, Value)                               \
  constexpr uint64_t Name = Value;
#define NEVERD_X64_TRANSITION_CODE(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64StateTransitionCases.def"
#undef NEVERD_X64_TRANSITION_CODE
#undef NEVERD_X64_TRANSITION_VALUE
constexpr uint64_t Levels[] = {
#define NEVERD_X64_TRANSITION_LEVEL(Value) Value,
#include "X64StateTransitionCases.def"
#undef NEVERD_X64_TRANSITION_LEVEL
};

class X64StateTransition : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  X64MachineState State;
  uint64_t Root = 0;

  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto Created =
        GetParam() == ExecutionBackendKind::KVM   ? createKvmMachine(*Memory)
        : GetParam() == ExecutionBackendKind::HVF ? createHvfX64Machine(*Memory)
                                                  : createWhpMachine(*Memory);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(GetParam(), GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Machine = std::move(*Created);
    llvm::cantFail(
        Memory->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(Memory->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(Memory->write(Code, LoadTLS));
    llvm::cantFail(Memory->write(Code + FaultOffset, Divide));
    uint8_t Bytes[2 * WordBytes];
    llvm::support::endian::write64le(Bytes, First);
    llvm::support::endian::write64le(Bytes + WordBytes, Second);
    llvm::cantFail(Memory->write(Data, Bytes));
    State.reg(X64Register::PC) = Code;
    State.reg(X64Register::SP) = Data + PageSize - WordBytes;
    State.reg(X64Register::FLAGS) = InitialFlags;
  }

  llvm::Error step(const std::atomic<bool> *Stop = nullptr) {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    Root = llvm::cantFail(buildX64PageTables(
        *Memory, State.UserMode, Machine->requiresExceptionMonitor()));
    return Machine->step(
        State, Root,
        {std::chrono::steady_clock::now() + std::chrono::microseconds(Timeout),
         Stop});
  }

  void expectUnchanged(const X64MachineState &Before) {
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.FSBase, Before.FSBase);
    EXPECT_EQ(State.GSBase, Before.GSBase);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Xmm, Before.Xmm);
    EXPECT_EQ(State.MXCSR, Before.MXCSR);
    EXPECT_EQ(State.FP, Before.FP);
  }

  void expectDivideFault() {
    State.reg(X64Register::PC) = Code + FaultOffset;
    State.reg(X64Register::BX) = State.reg(X64Register::DX) = 0;
    const auto Before = State;
    bool Caught = false;
    auto Remaining =
        llvm::handleErrors(step(), [&](const X64ExceptionError &E) {
          Caught = true;
          EXPECT_EQ(E.exception().Vector,
                    unsigned(x64::ExceptionVector::Divide));
          EXPECT_EQ(E.exception().ErrorCode, std::nullopt);
        });
    EXPECT_EQ(llvm::toString(std::move(Remaining)), "");
    ASSERT_TRUE(Caught);
    expectUnchanged(Before);
  }
};

TEST_P(X64StateTransition, RefreshesTLSAndPrivilegeAcrossSuccessAndExceptions) {
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    SCOPED_TRACE(Round);
    for (bool UserMode : {false, true}) {
      SCOPED_TRACE(UserMode);
      State.UserMode = UserMode;
      const bool Swap = Round % 2;
      State.FSBase = Data + (Swap ? WordBytes : 0);
      State.GSBase = Data + (Swap ? 0 : WordBytes);
      State.reg(X64Register::PC) = Code;
      State.reg(X64Register::CX) = 0;
      ASSERT_EQ(llvm::toString(step()), "");
      EXPECT_EQ(State.reg(X64Register::AX), Swap ? Second : First);
      EXPECT_EQ(State.reg(X64Register::PC), Code + LoadGSOffset);
      ASSERT_EQ(llvm::toString(step()), "");
      EXPECT_EQ(State.reg(X64Register::DX), Swap ? First : Second);
      expectDivideFault();
    }
  }
}

TEST_P(X64StateTransition, RefreshesCR8AcrossSuccessAndExceptions) {
  llvm::cantFail(Memory->write(Code, ReadCR8));
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    SCOPED_TRACE(Round);
    for (auto Level : Levels) {
      SCOPED_TRACE(Level);
      State.reg(X64Register::CR8) = Level;
      State.reg(X64Register::PC) = Code;
      ASSERT_EQ(llvm::toString(step()), "");
      EXPECT_EQ(State.reg(X64Register::AX), Level);
      expectDivideFault();
    }
  }
}

TEST_P(X64StateTransition, CR8ReadsEveryGPRAndPreservesUserPrivilegeFaults) {
  const X64Register Destinations[] = {
      X64Register::AX,  X64Register::CX,  X64Register::DX,  X64Register::BX,
      X64Register::SP,  X64Register::BP,  X64Register::SI,  X64Register::DI,
      X64Register::R8,  X64Register::R9,  X64Register::R10, X64Register::R11,
      X64Register::R12, X64Register::R13, X64Register::R14, X64Register::R15};
  for (unsigned GPR = 0; GPR < std::size(Destinations); ++GPR) {
    SCOPED_TRACE(GPR);
    // Independent MOV r64,CR8 encodings, including REX.B and RSP.
    const uint8_t Program[] = {uint8_t(GPR < 8 ? 0x44 : 0x45), 0x0f, 0x20,
                               uint8_t(0xc0 | (GPR & 7))};
    llvm::cantFail(Memory->write(Code, Program));
    for (bool User : {false, true}) {
      State.UserMode = User;
      State.reg(X64Register::CR8) = GPR;
      State.reg(X64Register::PC) = Code;
      State.reg(Destinations[GPR]) = First;
      State.reg(X64Register::FLAGS) = InitialFlags | x64::ResumeFlag;
      const auto Before = State;
      auto E = step();
      if (User) {
        bool Caught = false;
        auto Remaining = llvm::handleErrors(
            std::move(E), [&](const X64ExceptionError &Fault) {
              Caught = true;
              EXPECT_EQ(Fault.exception().Vector, 13u);
              EXPECT_EQ(Fault.exception().ErrorCode, 0u);
            });
        ASSERT_EQ(llvm::toString(std::move(Remaining)), "");
        ASSERT_TRUE(Caught);
        expectUnchanged(Before);
      } else {
        ASSERT_EQ(llvm::toString(std::move(E)), "");
        auto Expected = Before;
        Expected.reg(Destinations[GPR]) = GPR;
        Expected.reg(X64Register::PC) += sizeof(Program);
        Expected.reg(X64Register::FLAGS) = InitialFlags;
        expectUnchanged(Expected);
      }
    }
  }
}

TEST_P(X64StateTransition,
       CancelledEntryPreservesStateAndAllowsNewTLSProjection) {
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    SCOPED_TRACE(Round);
    State.UserMode = Round % 2;
    State.FSBase = Data;
    State.GSBase = Data + WordBytes;
    State.reg(X64Register::PC) = Code;
    State.reg(X64Register::CX) = 0;
    ASSERT_EQ(llvm::toString(step()), "");
    const auto Before = State;
    const std::atomic<bool> Stop{true};
    auto E = step(&Stop);
    ASSERT_TRUE(bool(E));
    EXPECT_FALSE(E.isA<X64ExceptionError>());
    llvm::consumeError(std::move(E));
    expectUnchanged(Before);
    State.GSBase = Data;
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State.reg(X64Register::DX), First);
  }
}

TEST_P(X64StateTransition,
       ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops) {
  llvm::cantFail(Memory->write(Code, StoreState));
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    SCOPED_TRACE(Round);
    State.reg(X64Register::PC) = Code;
    State.reg(X64Register::AX) = First ^ Round;
    State.reg(X64Register::BX) = WordBytes;
    State.reg(X64Register::CX) = Data + ResultOffset;
    const auto Sum = State.reg(X64Register::AX) + WordBytes;
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State.reg(X64Register::AX), Sum);
    // Continue with the complete captured state, then inspect the CPU's store.
    ASSERT_EQ(llvm::toString(step()), "");
    uint8_t Scalar[WordBytes];
    llvm::cantFail(Memory->read(Data + ResultOffset, Scalar));
    EXPECT_EQ(llvm::support::endian::read64le(Scalar), Sum);

    const auto Destination = Data + OtherResultOffset;
    const auto General = First ^ Second ^ Round;
    State.reg(X64Register::AX) = General;
    State.reg(X64Register::CX) = Destination;
    State.Xmm.front() = {Second ^ Round, First ^ Round};
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State.reg(X64Register::AX), General);
    uint8_t Vector[x64::VectorBytes];
    llvm::cantFail(Memory->read(Destination + VectorOffset, Vector));
    EXPECT_EQ(llvm::support::endian::read64le(Vector), Second ^ Round);
    EXPECT_EQ(llvm::support::endian::read64le(Vector + WordBytes),
              First ^ Round);
    // Change only the highest XMM lane, then only MXCSR and x87 control.
    State.Xmm.back() = {First + Round, Second - Round};
    ASSERT_EQ(llvm::toString(step()), "");
    llvm::cantFail(Memory->read(Destination + HighVectorOffset, Vector));
    EXPECT_EQ(llvm::support::endian::read64le(Vector), First + Round);
    EXPECT_EQ(llvm::support::endian::read64le(Vector + WordBytes),
              Second - Round);
    const auto RequestedMXCSR =
        x64::InitialMXCSR + (Round % RoundingModes) * MXCSRRoundUnit;
    State.MXCSR = RequestedMXCSR;
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State.MXCSR, RequestedMXCSR);
    llvm::cantFail(Memory->read(Destination + MXCSROffset, Scalar));
    EXPECT_EQ(llvm::support::endian::read32le(Scalar), RequestedMXCSR);
    const auto RequestedControl =
        x64::fp::InitialControl + (Round % RoundingModes) * ControlRoundUnit;
    State.FP.Control = RequestedControl;
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State.FP.Control, RequestedControl);
    llvm::cantFail(Memory->read(Destination + ControlOffset, Scalar));
    EXPECT_EQ(llvm::support::endian::read16le(Scalar), RequestedControl);

    const unsigned Top = Round % x64::fp::RegisterCount;
    State.FP.Control &= ~FPInvalidException;
    State.FP.Status = (Top << x64::fp::TopShift) | FPPendingStatus;
    State.FP.Tag = UINT8_MAX;
    State.FP.Opcode = Round;
    State.FP.Instruction = Code + Round;
    State.FP.Data = Data + Round;
    for (unsigned I = 0; I < State.FP.Registers.size(); ++I)
      State.FP.Registers[I] = {FP80Base + Round + I, FP80High};
    const auto Before = State;
    const std::atomic<bool> Stop{true};
    auto E = step(&Stop);
    ASSERT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    expectUnchanged(Before);
    ASSERT_EQ(llvm::toString(step()), "");
    // FXSAVE64 writes hardware state; no host-side state encoder is the oracle.
    std::array<uint8_t, x64::fp::LegacyBytes> FX;
    llvm::cantFail(Memory->read(Destination + FXOffset, FX));
    EXPECT_EQ(
        llvm::support::endian::read16le(FX.data() + x64::fp::ControlOffset),
        Before.FP.Control);
    EXPECT_EQ(
        llvm::support::endian::read16le(FX.data() + x64::fp::StatusOffset),
        Before.FP.Status);
    EXPECT_EQ(FX[x64::fp::TagOffset], UINT8_MAX);
    EXPECT_EQ(
        llvm::support::endian::read16le(FX.data() + x64::fp::OpcodeOffset),
        Round);
    EXPECT_EQ(
        llvm::support::endian::read64le(FX.data() + x64::fp::InstructionOffset),
        Code + Round);
    EXPECT_EQ(llvm::support::endian::read64le(FX.data() + x64::fp::DataOffset),
              Data + Round);
    for (unsigned I = 0; I < x64::fp::RegisterCount; ++I) {
      const auto *Slot =
          FX.data() + x64::fp::RegistersOffset + I * x64::fp::RegisterSlotBytes;
      EXPECT_EQ(llvm::support::endian::read64le(Slot),
                FP80Base + Round + ((Top + I) % x64::fp::RegisterCount));
      EXPECT_EQ(llvm::support::endian::read16le(Slot + WordBytes), FP80High);
    }
    expectDivideFault();
  }
}

INSTANTIATE_TEST_SUITE_P(NativeTransports, X64StateTransition,
                         testing::Values(ExecutionBackendKind::KVM,
                                         ExecutionBackendKind::WHP,
                                         ExecutionBackendKind::HVF),
                         [](const auto &Info) {
                           return executionBackendName(Info.param);
                         });
} // namespace
} // namespace neverd::emulation
