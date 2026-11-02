//===- X64ExceptionTests.cpp - Native faults without decoder preflight ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "arch/x86_64/CheckedX64Backend.h"
#include "arch/x86_64/X64ExceptionMonitor.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"

#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_X64_TRAP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_X64_TRAP_CASE(Name, Cause, Vector, Error, ...)                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64ExceptionCases.def"
#undef NEVERD_X64_TRAP_CASE
#undef NEVERD_X64_TRAP_VALUE
enum class Cause {
#define NEVERD_X64_TRAP_CASE(Name, Cause, Vector, Error, ...) Cause,
#include "X64ExceptionCases.def"
#undef NEVERD_X64_TRAP_CASE
};
struct TrapCase {
  const char *Name;
  Cause Kind;
  unsigned Vector;
  uint64_t Error;
  llvm::ArrayRef<uint8_t> Bytes;
};
const TrapCase Cases[] = {
#define NEVERD_X64_TRAP_CASE(Name, Kind, Vector, Error, ...)                   \
  {#Name, Cause::Kind, Vector, Error, Name},
#include "X64ExceptionCases.def"
#undef NEVERD_X64_TRAP_CASE
};
void PrintTo(const TrapCase &C, std::ostream *OS) { *OS << C.Name; }
using Parameter = std::tuple<ExecutionBackendKind, bool, TrapCase>;

/// This boundary bypasses CPU admission and OS policy intentionally. The
/// processor must produce the actual exception, with no software access check.
class X64ExceptionTransport : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  X64MachineState State, Before;
  uint64_t Root = 0;
  const TrapCase &testCase() const { return std::get<2>(GetParam()); }
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto M = std::get<0>(GetParam()) == ExecutionBackendKind::KVM
                 ? createKvmMachine(*Memory)
             : std::get<0>(GetParam()) == ExecutionBackendKind::HVF
                 ? createHvfX64Machine(*Memory)
                 : createWhpMachine(*Memory);
    if (!M) {
      auto E = M.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(std::get<0>(GetParam()), GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Machine = std::move(*M);
    llvm::cantFail(
        Memory->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(Memory->map(Data, PageSize, Read | Write | UserAccessible));
    std::vector<uint8_t> FillBytes(PageSize, Fill);
    llvm::cantFail(Memory->write(Data, FillBytes));
    llvm::cantFail(Memory->write(Code, testCase().Bytes));
    const uint8_t Resume = Nop;
    llvm::cantFail(Memory->write(Code + ResumeOffset, {&Resume, 1}));
    State.UserMode = std::get<1>(GetParam());
    State.reg(X64Register::PC) = Code;
    State.reg(X64Register::SP) = Stack;
    State.reg(X64Register::FLAGS) = InitialFlags;
    State.reg(X64Register::AX) = Low;
    State.reg(X64Register::DX) = 0;
    State.reg(X64Register::CX) = Data;
    State.Xmm[0] = {Low, High};
    switch (testCase().Kind) {
    case Cause::DivideZero:
      State.reg(X64Register::CX) = 0;
      break;
    case Cause::DivideOverflow:
      State.reg(X64Register::CX) = State.reg(X64Register::DX) = 1;
      break;
    case Cause::SignedOverflow:
      State.reg(X64Register::CX) = State.reg(X64Register::DX) = UINT64_MAX;
      State.reg(X64Register::AX) = SignedMinimum;
      break;
    case Cause::AlignedVector:
      State.reg(X64Register::CX) = Data + 1;
      break;
    case Cause::NoncanonicalData:
      State.reg(X64Register::CX) = Noncanonical;
      break;
    case Cause::NoncanonicalStack:
      State.reg(X64Register::SP) = Noncanonical;
      break;
    case Cause::UnmappedData:
      State.reg(X64Register::CX) = Unmapped;
      break;
    case Cause::ReadOnlyData:
      llvm::cantFail(Memory->protect(Data, PageSize, Read | UserAccessible));
      break;
    case Cause::ExecuteDenied:
      llvm::cantFail(Memory->protect(Code, PageSize, Read | UserAccessible));
      break;
    case Cause::InvalidOpcode:
      break;
    }
    Before = State;
  }
  llvm::Error step() {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    auto Projection = buildX64PageTables(*Memory, State.UserMode,
                                         Machine->requiresExceptionMonitor());
    if (!Projection)
      return Projection.takeError();
    Root = *Projection;
    return Machine->step(State, Root,
                         {std::chrono::steady_clock::now() +
                          std::chrono::microseconds(Timeout)});
  }
  void expectException() {
    auto E = step();
    ASSERT_TRUE(bool(E));
    bool Caught = false;
    auto Remaining =
        llvm::handleErrors(std::move(E), [&](const X64ExceptionError &E) {
          Caught = true;
          const auto &Exception = E.exception();
          EXPECT_EQ(Exception.Vector, testCase().Vector);
          const uint64_t Error =
              testCase().Error |
              (State.UserMode && testCase().Vector == 14 ? UserError : 0);
          EXPECT_EQ(Exception.ErrorCode, testCase().Error == NoError
                                             ? std::nullopt
                                             : std::optional<uint64_t>(Error));
          const auto Address = testCase().Kind == Cause::ExecuteDenied
                                   ? Code
                                   : Before.reg(X64Register::CX);
          EXPECT_EQ(Exception.FaultAddress,
                    testCase().Vector == 14 ? std::optional<uint64_t>(Address)
                                            : std::nullopt);
        });
    EXPECT_EQ(llvm::toString(std::move(Remaining)), "");
    EXPECT_TRUE(Caught);
  }
  void expectOriginal() {
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Xmm, Before.Xmm);
    EXPECT_EQ(State.MXCSR, Before.MXCSR);
    EXPECT_EQ(State.GSBase, Before.GSBase);
    EXPECT_EQ(State.FSBase, Before.FSBase);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    std::vector<uint8_t> Actual(PageSize);
    llvm::cantFail(Memory->read(Data, Actual, 0));
    EXPECT_EQ(Actual, std::vector<uint8_t>(PageSize, Fill));
  }
};

TEST_P(X64ExceptionTransport, ReportsExactNativeFaultAndOriginalContext) {
  expectException();
  expectOriginal();
}

TEST_P(X64ExceptionTransport,
       ResumesAfterRepeatedExceptionsWithoutGuestStackEffects) {
  for (unsigned I = 0; I < ResumeCount; ++I) {
    SCOPED_TRACE(I);
    State = Before;
    expectException();
    expectOriginal();
    // A raw machine owns no terminal-fault policy. Its caller installs the
    // explicit continuation; the architecture/OS owns that decision.
    State.reg(X64Register::PC) = Code + ResumeOffset;
    if (testCase().Kind == Cause::ExecuteDenied)
      llvm::cantFail(
          Memory->protect(Code, PageSize, Read | Execute | UserAccessible));
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_EQ(State.reg(X64Register::PC), Code + ResumeOffset + 1);
    EXPECT_EQ(State.reg(X64Register::SP), Before.reg(X64Register::SP));
    if (testCase().Kind == Cause::ExecuteDenied)
      llvm::cantFail(Memory->protect(Code, PageSize, Read | UserAccessible));
  }
}

TEST_P(X64ExceptionTransport,
       PrivateGatewayCannotShadowAnExistingGuestMapping) {
  const uint64_t Address = x64::KernelMin;
  llvm::cantFail(Memory->map(Address, x64::gateway::Bytes, Read | Write));
  std::vector<uint8_t> Bytes(x64::gateway::Bytes, Fill);
  llvm::cantFail(Memory->write(Address, Bytes));
  expectException();
  expectOriginal();
  std::vector<uint8_t> Actual(Bytes.size());
  llvm::cantFail(Memory->read(Address, Actual));
  EXPECT_EQ(Actual, Bytes);
  if (Machine->requiresExceptionMonitor())
    EXPECT_GE(x64ExceptionMonitorBase(*Memory), Address + x64::gateway::Bytes);
}

TEST_P(X64ExceptionTransport,
       MappingGenerationCanMoveThePrivateExceptionGateway) {
  expectException();
  expectOriginal();
  if (!Machine->requiresExceptionMonitor())
    return;
  const uint64_t Address = x64ExceptionMonitorBase(*Memory);
  llvm::cantFail(Memory->map(Address, x64::gateway::Bytes, Read | Write));
  State = Before;
  expectException();
  expectOriginal();
  EXPECT_NE(x64ExceptionMonitorBase(*Memory), Address);
}

TEST_P(X64ExceptionTransport, ProjectionCacheCannotReuseUnmonitoredTables) {
  Root = llvm::cantFail(buildX64PageTables(*Memory, State.UserMode, false));
  expectException();
  expectOriginal();
}

TEST_P(X64ExceptionTransport, GuestHaltCannotForgeAPrivateExceptionFrame) {
  llvm::cantFail(
      Memory->protect(Code, PageSize, Read | Write | Execute | UserAccessible));
  const uint8_t Byte = Halt;
  llvm::cantFail(Memory->write(Code, {&Byte, 1}));
  auto E = step();
  ASSERT_TRUE(bool(E));
  if (State.UserMode) {
    bool Caught = false;
    auto Remaining =
        llvm::handleErrors(std::move(E), [&](const X64ExceptionError &E) {
          Caught = true;
          EXPECT_EQ(E.exception().Vector, GeneralProtectionVector);
          EXPECT_EQ(E.exception().ErrorCode, 0);
        });
    EXPECT_EQ(llvm::toString(std::move(Remaining)), "");
    EXPECT_TRUE(Caught);
  } else {
    EXPECT_FALSE(E.isA<X64ExceptionError>());
    llvm::consumeError(std::move(E));
  }
  expectOriginal();
}

INSTANTIATE_TEST_SUITE_P(
    NativeTransports, X64ExceptionTransport,
    testing::Combine(testing::Values(ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Bool(), testing::ValuesIn(Cases)));

class InjectedProcessorFault final : public X64Machine {
public:
  llvm::Error step(X64MachineState &, uint64_t, MachineRunControl) override {
    return llvm::make_error<X64ExceptionError>(X64Exception{
        unsigned(x64::ExceptionVector::PageFault),
        x64::gateway::PageFaultPresent | x64::gateway::PageFaultUser, Data});
  }
};

TEST(CheckedX64Exception,
     RetainsProcessorErrorCodeInRecoverableAndTerminalFaults) {
  for (bool Recoverable : {false, true}) {
    auto CPU = llvm::cantFail(CheckedX64Backend::create(
        llvm::cantFail(MemoryProjection::create(Limit)),
        std::make_unique<InjectedProcessorFault>()));
    llvm::cantFail(CPU->map(Code, PageSize, Read | Write | Execute));
    const uint8_t Byte = Nop;
    llvm::cantFail(CPU->write(Code, {&Byte, 1}));
    BackendHooks H;
    H.RecoverableFault = [&](const BackendFault &F) {
      EXPECT_EQ(F.PC, Code);
      EXPECT_EQ(F.Address, Data);
      EXPECT_EQ(F.Interrupt, unsigned(x64::ExceptionVector::PageFault));
      EXPECT_EQ(F.ErrorCode,
                x64::gateway::PageFaultPresent | x64::gateway::PageFaultUser);
      return Recoverable;
    };
    llvm::cantFail(CPU->installHooks(std::move(H)));
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, Recoverable ? ExecutionExitKind::RecoverableFault
                                     : ExecutionExitKind::GuestTrap);
    const auto Fault = Recoverable ? CPU->takeRecoverableFault() : CPU->fault();
    ASSERT_TRUE(Fault);
    EXPECT_EQ(Fault->ErrorCode,
              x64::gateway::PageFaultPresent | x64::gateway::PageFaultUser);
    EXPECT_EQ(Fault->Address, Data);
    EXPECT_EQ(Fault->PC, Code);
  }
}
} // namespace
} // namespace neverd::emulation
