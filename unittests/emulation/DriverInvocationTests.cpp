//===- DriverInvocationTests.cpp - Calls in the retained kernel model -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Calls in the retained kernel model.
///

#include "gtest/gtest.h"
#include "os/windows/driver/DriverExecutionRun.h"

using namespace neverd::emulation;
namespace {
std::filesystem::path fixture() {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
         "driver_invocation.sys";
}
llvm::Expected<ExecutionRunEvent> advance(ExecutionRun &Run) {
  for (unsigned N = 0; N < 100000; ++N) {
    auto E = Run.run();
    if (!E)
      return E.takeError();
    if (E->State == ExecutionRunState::Paused &&
        E->Reason == ExecutionRunReason::Quantum) {
      if (auto Error = Run.resume())
        return std::move(Error);
      continue;
    }
    return E;
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "call test exceeded action bound");
}
class DriverCall : public testing::TestWithParam<ExecutionContract> {
protected:
  std::optional<PreparedDriverExecution> Driver;
  std::unique_ptr<neverd::modules::ModuleSet> Symbols;
  void SetUp() override {
    auto Available = queryExecutionBackendBuild(ExecutionBackendKind::Unicorn,
                                                GuestArchitecture::X64);
    ASSERT_TRUE(bool(Available));
    if (Available->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Available->Reason;
    neverd::modules::ModuleOptions Inputs;
    Inputs.Modules.push_back({"driver", fixture()});
    Inputs.Modules.back().Debug = neverd::modules::DebugInput{
        neverd::modules::DebugKind::PDB,
        fixture().parent_path() / "driver_invocation.pdb"};
    auto M = neverd::modules::ModuleSet::create(std::move(Inputs));
    ASSERT_TRUE(bool(M)) << llvm::toString(M.takeError());
    Symbols = std::move(*M);
    start();
  }
  void start(bool Scheduling = false) {
    DriverOptions O;
    O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
    O.Backend = ExecutionBackendKind::Unicorn;
    O.Contract = GetParam();
    O.LoadAddress = 0x190000000;
    if (Scheduling)
      O.Scheduling = DriverScheduling{3, 1};
    ExecutionRunOptions C;
    C.Limits = {O.InstructionLimit, O.EventLimit, O.TimeoutMilliseconds * 1000};
    C.PauseStages = {"after-entry"};
    C.InstructionQuantum = 2;
    C.ActionsPerRun = 1;
    C.EnableResourcePauses = false;
    auto P = createDriverExecutionRun(fixture(), O, C);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    Driver = std::move(*P);
    auto E = advance(*Driver->Run);
    ASSERT_TRUE(bool(E));
    ASSERT_EQ(E->Reason, ExecutionRunReason::Stage) << E->Diagnostic;
  }
  uint64_t address(const char *Name) {
    auto Symbol = Symbols->lookupSymbol("driver", Name);
    if (!Symbol) {
      ADD_FAILURE() << llvm::toString(Symbol.takeError());
      return 0;
    }
    if (!*Symbol) {
      ADD_FAILURE() << "missing PDB test function " << Name;
      return 0;
    }
    return (**Symbol).Address - Driver->State->Image->PreferredBase +
           Driver->State->Image->Base;
  }
  GuestInvocationRequest request(const char *Name) {
    GuestInvocationRequest R;
    R.Entry = address(Name);
    R.ABI = IntegerCallingConvention::Win64;
    return R;
  }
  llvm::Expected<ExecutionRunEvent> call(GuestInvocationRequest R) {
    if (auto E = Driver->Run->invoke(std::move(R)))
      return std::move(E);
    return advance(*Driver->Run);
  }
};
TEST_P(DriverCall, StackArgumentsUseThePreparedFrameAndRestoreCallerControl) {
  auto R = request("ProbeSum");
  for (uint64_t I = 1; I <= 7; ++I)
    R.Arguments.push_back({I, {}});
  const auto SP = *Driver->State->CPU->reg(X64Register::SP);
  const auto AX = *Driver->State->CPU->reg(X64Register::AX);
  const auto Context = Driver->State->Kernel->captureExecutionContext();
  const auto Before = Driver->Run->inspect()->Instructions;
  auto E = call(std::move(R));
  ASSERT_TRUE(bool(E)) << llvm::toString(E.takeError());
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  ASSERT_TRUE(E->Invocation);
  EXPECT_EQ(E->Invocation->Value, 69u);
  EXPECT_EQ(E->Stage, "after-entry");
  EXPECT_EQ(*Driver->State->CPU->reg(X64Register::SP), SP);
  EXPECT_EQ(*Driver->State->CPU->reg(X64Register::AX), AX);
  EXPECT_EQ(Driver->State->Kernel->captureExecutionContext().Thread,
            Context.Thread);
  EXPECT_EQ(Driver->State->Kernel->captureExecutionContext().Execution,
            Context.Execution);
  EXPECT_GT(Driver->Run->inspect()->Instructions, Before);
  ASSERT_EQ(llvm::toString(Driver->Run->resume()), "");
  EXPECT_EQ(advance(*Driver->Run)->State, ExecutionRunState::Completed);
}
TEST_P(DriverCall, BuffersAndGlobalEffectsSurviveRepeatedCalls) {
  for (uint64_t Call = 0; Call < 2; ++Call) {
    auto R = request("ProbeBuffers");
    R.Buffers.push_back({4, {0, 0x80, 0xff, 1}, true});
    R.Arguments = {{0, 0}, {4, {}}};
    auto E = call(std::move(R));
    ASSERT_TRUE(bool(E)) << llvm::toString(E.takeError());
    ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete)
        << E->Diagnostic;
    ASSERT_TRUE(E->Invocation);
    EXPECT_EQ(E->Invocation->Value, 42 + Call);
    const uint8_t Key = 41 + Call;
    EXPECT_EQ(E->Invocation->Buffers[0].Bytes,
              (std::vector<uint8_t>{Key, uint8_t(0x80 ^ Key),
                                    uint8_t(0xff ^ Key), uint8_t(1 ^ Key)}));
  }
}
TEST_P(DriverCall, ModeledTailCallReturnsAtItsExactInstructionLimit) {
  auto R = request("ProbeModelTail");
  R.Limits.Instructions = 1;
  auto E = call(std::move(R));
  ASSERT_TRUE(bool(E)) << llvm::toString(E.takeError());
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 0u);
  EXPECT_EQ(E->Invocation->Instructions, 1u);
  EXPECT_EQ(Driver->State->Result.Calls.back().Name,
            "PsGetCurrentThreadWin32Thread");
}
TEST_P(DriverCall, ApiAndNestedCallbackUseTheSameKernelPump) {
  auto R = request("ProbeNotify");
  R.Arguments = {{5, {}}};
  auto E = call(std::move(R));
  ASSERT_TRUE(bool(E)) << llvm::toString(E.takeError());
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 18u);
  EXPECT_EQ(Driver->State->Result.NTStatus, 0u);
  EXPECT_EQ(Driver->State->Kernel->currentIRQL(), 0u);
}
TEST_P(DriverCall, ContextApiWritesTheCallersOwnedCaptureBuffer) {
  auto R = request("ProbeCapture");
  R.Buffers.push_back({1232, {}, true});
  R.Arguments = {{0, 0}};
  auto E = call(std::move(R));
  ASSERT_TRUE(bool(E)) << llvm::toString(E.takeError());
  if (GetParam() == ExecutionContract::Legacy) {
    EXPECT_EQ(E->State, ExecutionRunState::Failed);
    EXPECT_NE(E->Diagnostic.find("checked-x64"), std::string::npos);
    return;
  }
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 17u);
  EXPECT_EQ(E->Invocation->Buffers[0].Bytes.size(), 1232u);
  EXPECT_NE(E->Invocation->Buffers[0].Bytes, std::vector<uint8_t>(1232));
}
TEST_P(DriverCall, InvalidAbiAndTargetsLeaveTheStageUntouched) {
  const auto Count = Driver->Run->inspect()->Instructions;
  auto R = request("ProbeSum");
  R.ABI = IntegerCallingConvention::SysVAMD64;
  EXPECT_NE(llvm::toString(Driver->Run->invoke(R)), "");
  R.ABI = IntegerCallingConvention::Win64;
  R.Entry = Driver->State->Image->Base;
  EXPECT_NE(llvm::toString(Driver->Run->invoke(R)), "");
  EXPECT_EQ(Driver->Run->state(), ExecutionRunState::Paused);
  EXPECT_EQ(Driver->Run->inspect()->Instructions, Count);
}
TEST_P(DriverCall, UnbalancedIrqlIsTerminalRatherThanRestored) {
  // Each contract gets its own run; the first invalid return terminates it.
  auto E = call(request("ProbeIRQLLeak"));
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_EQ(Driver->State->Kernel->currentIRQL(), 2u);
  EXPECT_NE(E->Diagnostic.find("IRQL"), std::string::npos);
}
TEST_P(DriverCall, BadReturnStackIsTerminalAndCannotBeReused) {
  auto E = call(request("ProbeBadStack"));
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_NE(E->Diagnostic.find("stack"), std::string::npos);
  EXPECT_NE(llvm::toString(Driver->Run->resume()), "");
}
TEST_P(DriverCall, UnmappedCallMemoryIsATerminalFault) {
  auto E = call(request("ProbeFault"));
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  ASSERT_TRUE(Driver->State->CPU->fault());
  EXPECT_EQ(Driver->State->CPU->fault()->Address, 0x12345000u);
}
TEST_P(DriverCall, PreemptiveCallsRetainTheSameThreadAndKernelPump) {
  start(true);
  ASSERT_EQ(Driver->Run->state(), ExecutionRunState::Paused);
  const auto Before = Driver->State->Kernel->captureExecutionContext();
  auto R = request("ProbeNotify");
  R.Arguments = {{5, {}}};
  auto E = call(std::move(R));
  ASSERT_TRUE(bool(E));
  ASSERT_EQ(E->Reason, ExecutionRunReason::InvocationComplete) << E->Diagnostic;
  EXPECT_EQ(E->Invocation->Value, 18u);
  EXPECT_EQ(Driver->State->Kernel->captureExecutionContext().Thread,
            Before.Thread);
  EXPECT_EQ(Driver->State->Kernel->captureExecutionContext().Execution,
            Before.Execution);
  EXPECT_EQ(Driver->State->Kernel->currentIRQL(), 0u);
}
TEST_P(DriverCall, LocalInstructionExhaustionKeepsItsTerminalReason) {
  auto R = request("ProbeLoop");
  R.Limits.Instructions = 17;
  auto E = call(std::move(R));
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(E->State, ExecutionRunState::Failed);
  EXPECT_EQ(E->Reason, ExecutionRunReason::InstructionLimit);
}
INSTANTIATE_TEST_SUITE_P(
    Contracts, DriverCall,
    testing::Values(ExecutionContract::Legacy, ExecutionContract::CheckedX64),
    [](const testing::TestParamInfo<ExecutionContract> &P) {
      return P.param == ExecutionContract::Legacy ? "Legacy" : "Checked";
    });
} // namespace
