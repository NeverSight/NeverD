//===- ExecutionExitTests.cpp - Real CPU exit precedence -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <chrono>
#include <stdexcept>

namespace neverd::emulation {
namespace {
#define NEVERD_CONFIGURATION_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_CONFIGURATION_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_CONFIGURATION_X64(Name, ...)                                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_CONFIGURATION_ARM(Name, ...)                                    \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_ARM
#undef NEVERD_CONFIGURATION_X64
#undef NEVERD_CONFIGURATION_TEXT
#undef NEVERD_CONFIGURATION_VALUE

struct Profile {
  GuestArchitecture ISA;
  ExecutionContract Contract;
};
constexpr Profile Profiles[] = {
    {GuestArchitecture::X64, ExecutionContract::Software},
    {GuestArchitecture::AArch64, ExecutionContract::Software},
    {GuestArchitecture::X64, ExecutionContract::CheckedX64},
    {GuestArchitecture::AArch64, ExecutionContract::CheckedAArch64}};

constexpr uint64_t InvalidTimeouts[] = {
#define NEVERD_CONFIGURATION_INVALID_TIMEOUT(Name, Value) Value,
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_INVALID_TIMEOUT
};

std::unique_ptr<ExecutionBackend> cpu(Profile Profile) {
  auto CPU = llvm::cantFail(createExecutionBackend(
                                ExecutionBackendKind::Unicorn, Profile.Contract,
                                MemoryLimit, Profile.ISA))
                 .CPU;
  llvm::cantFail(CPU->map(Code, PageSize, Read | Write | Execute));
  if (Profile.ISA == GuestArchitecture::X64)
    llvm::cantFail(CPU->setReg(X64Register::CX, Device));
  else
    llvm::cantFail(CPU->setReg(AArch64Register::X1, Device));
  return CPU;
}
void code(ExecutionBackend &CPU, llvm::ArrayRef<uint8_t> X64,
          llvm::ArrayRef<uint32_t> ARM) {
  if (CPU.architecture() == GuestArchitecture::X64) {
    llvm::cantFail(CPU.write(Code, X64));
    return;
  }
  std::vector<uint8_t> Bytes(ARM.size() * sizeof(uint32_t));
  for (size_t I = 0; I < ARM.size(); ++I)
    llvm::support::endian::write32le(Bytes.data() + I * sizeof(uint32_t),
                                     ARM[I]);
  llvm::cantFail(CPU.write(Code, Bytes));
}

TEST(ExecutionExit, InstructionObserverStopsBeforeGuestEffects) {
  for (const auto Profile : Profiles) {
    SCOPED_TRACE(executionContractName(Profile.Contract));
    auto CPU = cpu(Profile);
    code(*CPU, LoadDeviceX64, LoadDeviceARM);
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t, uint32_t) { CPU->stop(); };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
    EXPECT_TRUE(Exit.StopRequested);
    EXPECT_FALSE(Exit.Fault);
    EXPECT_FALSE(Exit.DeadlineReached);
    EXPECT_TRUE(Exit.Diagnostic.empty());
  }
}

TEST(ExecutionExit, InvalidBudgetsFailBeforeEffectsAndLeaveCPUUsable) {
  for (const auto Profile : Profiles) {
    SCOPED_TRACE(executionContractName(Profile.Contract));
    for (const uint64_t Budget : InvalidTimeouts) {
      SCOPED_TRACE(Budget);
      auto CPU = cpu(Profile);
      code(*CPU, LoadDeviceX64, LoadDeviceARM);
      const auto PC = Profile.ISA == GuestArchitecture::X64
                          ? CPURegister::X64PC
                          : CPURegister::AArch64PC;
      const auto Before = llvm::cantFail(CPU->readRegister(PC));
      unsigned Observations = 0;
      BackendHooks Hooks;
      Hooks.Instruction = [&](uint64_t, uint32_t) {
        ++Observations;
        CPU->stop();
      };
      llvm::cantFail(CPU->installHooks(std::move(Hooks)));
      auto Exit = CPU->runUntilExit(Code, Budget);
      ASSERT_FALSE(bool(Exit));
      EXPECT_EQ(llvm::toString(Exit.takeError()), diagnostic::ExecutionTimeout);
      EXPECT_EQ(llvm::toString(CPU->run(Code, Budget)),
                diagnostic::ExecutionTimeout);
      EXPECT_EQ(llvm::cantFail(CPU->readRegister(PC)), Before);
      EXPECT_EQ(Observations, 0u);
      EXPECT_FALSE(CPU->fault());
      EXPECT_FALSE(CPU->timedOut());
      const auto Next = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
      EXPECT_EQ(Next.Kind, ExecutionExitKind::Stopped);
      EXPECT_EQ(Observations, 1u);
    }
  }
}

TEST(ExecutionExit, DeadlineIsIndependentOfTheNextRun) {
  for (const auto Profile : Profiles) {
    auto CPU = cpu(Profile);
    code(*CPU, LoopX64, LoopARM);
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, ShortTimeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::Deadline);
    EXPECT_TRUE(Exit.DeadlineReached);
    EXPECT_TRUE(CPU->timedOut());
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t, uint32_t) { CPU->stop(); };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
    EXPECT_FALSE(Exit.DeadlineReached);
  }
}

TEST(ExecutionExit, InstructionObserverCannotAdmitEffectsAfterDeadline) {
  for (const auto Profile : Profiles) {
    if (Profile.Contract != ExecutionContract::Software)
      continue;
    SCOPED_TRACE(guestArchitectureName(Profile.ISA));
    auto CPU = cpu(Profile);
    // The unmapped load must not execute after an observer spends the budget.
    code(*CPU, LoadDeviceX64, LoadDeviceARM);
    BackendHooks Hooks;
    Hooks.Instruction = [](uint64_t, uint32_t) {
      const auto Until = std::chrono::steady_clock::now() +
                         std::chrono::microseconds(2 * ShortTimeout);
      while (std::chrono::steady_clock::now() < Until) {
      }
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, ShortTimeout));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Deadline);
    EXPECT_TRUE(Exit.DeadlineReached);
    EXPECT_FALSE(Exit.Fault);
    EXPECT_FALSE(CPU->fault());
    Hooks.Instruction = [&](uint64_t, uint32_t) { CPU->stop(); };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    const auto Next = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Next.Kind, ExecutionExitKind::Stopped);
    EXPECT_FALSE(Next.DeadlineReached);
  }
}

TEST(ExecutionExit, GuestFaultOutranksObserverStopAndPreventsResumption) {
  for (const auto Profile : Profiles) {
    auto CPU = cpu(Profile);
    code(*CPU, LoadDeviceX64, LoadDeviceARM);
    BackendHooks Hooks;
    Hooks.Fault = [&](uint64_t, uint32_t, const char *) { CPU->stop(); };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault);
    EXPECT_TRUE(Exit.StopRequested);
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->PC, Code);
    EXPECT_EQ(Exit.Fault->Address, Device);
    EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
    auto Again = CPU->runUntilExit(Code, Timeout);
    ASSERT_FALSE(bool(Again));
    llvm::consumeError(Again.takeError());
  }
}

TEST(ExecutionExit, RecoverableFaultRemainsPendingUntilOSConsumesIt) {
  for (const auto Profile : Profiles) {
    auto CPU = cpu(Profile);
    code(*CPU, LoadDeviceX64, LoadDeviceARM);
    BackendHooks Hooks;
    Hooks.RecoverableFault = [](const BackendFault &) { return true; };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Address, Device);
    EXPECT_FALSE(CPU->fault());
    auto Pending = CPU->runUntilExit(Code, Timeout);
    ASSERT_FALSE(bool(Pending));
    llvm::consumeError(Pending.takeError());
    auto Fault = CPU->takeRecoverableFault();
    ASSERT_TRUE(Fault);
    EXPECT_EQ(Fault->Address, Exit.Fault->Address);
    EXPECT_FALSE(CPU->takeRecoverableFault());
    Hooks = {};
    Hooks.Instruction = [&](uint64_t, uint32_t) { CPU->stop(); };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
    EXPECT_FALSE(Exit.Fault);
  }
}

TEST(ExecutionExit, SecondaryObserverExceptionPreservesOriginalGuestFault) {
  for (const auto Profile : Profiles) {
    auto CPU = cpu(Profile);
    code(*CPU, LoadDeviceX64, LoadDeviceARM);
    BackendHooks Hooks;
    Hooks.Fault = [&](uint64_t, uint32_t, const char *) {
      CPU->stop();
      throw std::runtime_error(CallbackFailure);
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault);
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::UnmappedMemory);
    EXPECT_EQ(Exit.Fault->Address, Device);
    EXPECT_TRUE(Exit.StopRequested);
    EXPECT_FALSE(Exit.Diagnostic.empty());
  }
}

TEST(ExecutionExit, DeviceFailureOutranksTheSameCallbacksStopRequest) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    auto CPU = cpu({ISA, ExecutionContract::Software});
    code(*CPU, LoadDeviceX64, LoadDeviceARM);
    GuestMMIOCallbacks IO;
    IO.Validate = [](uint64_t, uint64_t, bool) {
      return llvm::Error::success();
    };
    IO.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      CPU->stop();
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     CallbackFailure);
    };
    IO.Write = [](uint64_t, unsigned, uint64_t) {
      return llvm::Error::success();
    };
    llvm::cantFail(CPU->mapMMIO(Device, PageSize, std::move(IO)));
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::DeviceFailure);
    EXPECT_TRUE(Exit.StopRequested);
    EXPECT_EQ(Exit.Diagnostic, CallbackFailure);
    EXPECT_TRUE(CPU->hasDeviceError());
  }
}

TEST(ExecutionExit, DistinguishesGuestTrapFromCheckedInstructionRejection) {
  for (const auto Profile : Profiles) {
    auto CPU = cpu(Profile);
    code(*CPU, TrapX64, TrapARM);
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, Profile.Contract == ExecutionContract::Software
                             ? ExecutionExitKind::GuestTrap
                             : ExecutionExitKind::UnsupportedOperation);
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->PC, Code);
  }
}

TEST(ExecutionExit, ObserverFailureIsNotReportedAsSuccessfulStop) {
  for (const auto Profile : Profiles) {
    auto CPU = cpu(Profile);
    code(*CPU, LoadDeviceX64, LoadDeviceARM);
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t, uint32_t) {
      CPU->stop();
      throw std::runtime_error(CallbackFailure);
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
    EXPECT_TRUE(Exit.StopRequested);
    EXPECT_FALSE(Exit.Diagnostic.empty());
  }
}

TEST(ExecutionExit, InstructionRejectionSurvivesASecondaryObserverFailure) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    auto CPU = cpu({ISA, ISA == GuestArchitecture::X64
                             ? ExecutionContract::CheckedX64
                             : ExecutionContract::CheckedAArch64});
    code(*CPU, TrapX64, TrapARM);
    BackendHooks Hooks;
    Hooks.InvalidInstruction = [&] {
      CPU->stop();
      throw std::runtime_error(CallbackFailure);
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::InvalidInstruction);
    EXPECT_FALSE(Exit.Diagnostic.empty());
    EXPECT_TRUE(Exit.StopRequested);
  }
}

TEST(ExecutionExit, SoftwareHaltIsNotWorkloadCompletion) {
  auto CPU = cpu({GuestArchitecture::X64, ExecutionContract::Software});
  llvm::cantFail(CPU->write(Code, HaltX64));
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::EngineStop);
  EXPECT_FALSE(Exit.StopRequested);
  EXPECT_FALSE(Exit.Fault);
}
} // namespace
} // namespace neverd::emulation
