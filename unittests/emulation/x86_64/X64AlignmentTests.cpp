//===- X64AlignmentTests.cpp - SSE faults before observable memory use ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64VectorTestSupport.h"
#include "arch/x86_64/X64Exception.h"
#include "arch/x86_64/X64Machine.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"

#include "llvm/ADT/ScopeExit.h"

#include <map>

namespace neverd::emulation {
namespace {
using namespace vector_test;
#define NEVERD_ALIGNMENT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_ALIGNMENT_INSTRUCTION(Name, Kind, ...)                          \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64AlignmentCases.def"
#undef NEVERD_ALIGNMENT_INSTRUCTION
#undef NEVERD_ALIGNMENT_VALUE
enum class ResultKind { Load, Store, Add, Shift, Xor };
struct Instruction {
  const char *Name;
  ResultKind Kind;
  llvm::ArrayRef<uint8_t> Bytes;
};
const Instruction Instructions[] = {
#define NEVERD_ALIGNMENT_INSTRUCTION(Name, Kind, ...)                          \
  {#Name, ResultKind::Kind, Name},
#include "X64AlignmentCases.def"
#undef NEVERD_ALIGNMENT_INSTRUCTION
};
constexpr Input Initial{{SentinelLow, SentinelHigh},
                        {SentinelHigh, SentinelLow}};

void expectFault(const BackendFault &Fault) {
  EXPECT_EQ(Fault.Kind, BackendFaultKind::Interrupt);
  EXPECT_EQ(Fault.PC, Code);
  EXPECT_EQ(Fault.Interrupt, GeneralProtection);
  EXPECT_EQ(Fault.ErrorCode, 0);
  EXPECT_EQ(Fault.Cause, BackendFaultCause::OperandAlignment);
  EXPECT_EQ(Fault.Address, std::nullopt);
  EXPECT_EQ(Fault.Size, std::nullopt);
  EXPECT_EQ(Fault.Access, std::nullopt);
}

class X64Alignment : public X64VectorTest {
protected:
  void initialize() {
    seed(Initial);
    llvm::cantFail(CPU->setReg(X64Register::PC, Code));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FP0,
                                      {SentinelLow, PhysicalExponent}));
    llvm::cantFail(CPU->writeRegister(CPURegister::X64FPTag, {1, 0}));
  }
  auto snapshot() {
    std::map<CPURegister, RegisterValue> Result;
#define NEVERD_SCALAR_REGISTER(ISA, Name, Bits, Backend)                       \
  if (GuestArchitecture::ISA == GuestArchitecture::X64)                        \
    Result[CPURegister::ISA##Name] =                                           \
        llvm::cantFail(CPU->readRegister(CPURegister::ISA##Name));
#define NEVERD_EXTENDED_REGISTER(ISA, Name, Bits, Backend)                     \
  NEVERD_SCALAR_REGISTER(ISA, Name, Bits, Backend)
#include "neverd/emulation/Registers.def"
#undef NEVERD_EXTENDED_REGISTER
#undef NEVERD_SCALAR_REGISTER
    for (unsigned I = 0; I < XmmCount; ++I) {
      const auto Register = vectorRegister(GuestArchitecture::X64, I);
      Result[Register] = llvm::cantFail(CPU->readRegister(Register));
    }
    return Result;
  }
  void expectRAM(RegisterValue Expected = Initial.Right) {
    std::array<uint8_t, VectorBytes> Bytes{};
    ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Data, Bytes)), "");
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), Expected[0]);
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + WordBytes),
              Expected[1]);
  }
};

TEST_P(X64Alignment, FaultPrecedesMemoryPermissionsMappingsAndObservers) {
  initialize();
  unsigned DeviceCalls = 0;
  if (!GetParam().User) {
    GuestMMIOCallbacks Device;
    Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++DeviceCalls;
      return 0;
    };
    Device.Write = [&](uint64_t, unsigned, uint64_t) {
      ++DeviceCalls;
      return llvm::Error::success();
    };
    llvm::cantFail(CPU->mapMMIO(Stack, PageSize, std::move(Device)));
  }
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (unsigned Offset = 1; Offset < VectorBytes; ++Offset) {
      for (const uint64_t Address :
           {Data + Offset, Alias + Offset, Data + PageSize - Offset,
            Stack + Offset, Noncanonical + Offset, UINT64_MAX - Offset + 1}) {
        SCOPED_TRACE(Address);
        llvm::cantFail(CPU->setReg(X64Register::CX, Address));
        // Denied pages, absent pages, aliases, page crossings and address
        // overflow cannot replace the earlier aligned-SSE #GP with #PF.
        llvm::cantFail(CPU->protect(Data, PageSize, UserAccessible));
        const auto Before = snapshot();
        unsigned Reads = 0, Writes = 0, Faults = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        Hooks.RecoverableFault = [&](const BackendFault &Fault) {
          ++Faults;
          expectFault(Fault);
          return true;
        };
        const auto Exit = run(I.Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
            << Exit.Diagnostic;
        ASSERT_TRUE(CPU->takeRecoverableFault());
        EXPECT_EQ(Faults, 1u);
        EXPECT_EQ(Reads, 0u);
        EXPECT_EQ(Writes, 0u);
        EXPECT_EQ(DeviceCalls, 0u);
        EXPECT_EQ(snapshot(), Before);
        expectRAM();
        llvm::cantFail(
            CPU->protect(Data, PageSize, Read | Write | UserAccessible));
      }
    }
  }
}

TEST_P(X64Alignment, InstructionObserverStopDoesNotInventAnException) {
  initialize();
  llvm::cantFail(CPU->setReg(X64Register::CX, Data + 1));
  const auto Before = snapshot();
  for (const auto &I : Instructions) {
    llvm::cantFail(CPU->write(Code, I.Bytes));
    unsigned Instructions = 0, Faults = 0;
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t PC, unsigned Size) {
      EXPECT_EQ(PC, Code);
      EXPECT_EQ(Size, I.Bytes.size());
      ++Instructions;
      CPU->stop();
    };
    Hooks.RecoverableFault = [&](const BackendFault &) {
      ++Faults;
      return true;
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Instructions, 1u);
    EXPECT_EQ(Faults, 0u);
    EXPECT_FALSE(CPU->fault());
    EXPECT_FALSE(CPU->takeRecoverableFault());
    EXPECT_EQ(snapshot(), Before);
    expectRAM();
  }
}

TEST_P(X64Alignment, TerminalFaultPreservesCompletePublicContext) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    reset();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(CPU);
    initialize();
    llvm::cantFail(CPU->setReg(X64Register::CX, Data + 1));
    const auto Before = snapshot();
    unsigned Interrupts = 0;
    BackendHooks Hooks;
    Hooks.Interrupt = [&](uint32_t Vector) {
      EXPECT_EQ(Vector, GeneralProtection);
      ++Interrupts;
    };
    const auto Exit = run(I.Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestTrap) << Exit.Diagnostic;
    ASSERT_TRUE(Exit.Fault);
    expectFault(*Exit.Fault);
    EXPECT_EQ(Interrupts, 1u);
    EXPECT_EQ(snapshot(), Before);
    expectRAM();
  }
}

TEST_P(X64Alignment, AddressRepairReexecutesTheFaultingInstruction) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    initialize();
    llvm::cantFail(CPU->setReg(X64Register::CX, Data + 1));
    BackendHooks Hooks;
    Hooks.RecoverableFault = [](const BackendFault &) { return true; };
    ASSERT_EQ(run(I.Bytes, std::move(Hooks)).Kind,
              ExecutionExitKind::RecoverableFault);
    ASSERT_TRUE(CPU->takeRecoverableFault());
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    auto Expected = snapshot();
    auto Value = Initial.Left;
    switch (I.Kind) {
    case ResultKind::Load:
      Value = Initial.Right;
      break;
    case ResultKind::Store:
      break;
    case ResultKind::Add:
      Value = {Initial.Left[0] + Initial.Right[0],
               Initial.Left[1] + Initial.Right[1]};
      break;
    case ResultKind::Shift:
      // The independent source's low 64-bit count exceeds the lane width.
      Value = {};
      break;
    case ResultKind::Xor:
      Value = {Initial.Left[0] ^ Initial.Right[0],
               Initial.Left[1] ^ Initial.Right[1]};
      break;
    }
    Expected[CPURegister::X64PC] = {Code + I.Bytes.size(), 0};
    Expected[vectorRegister(GuestArchitecture::X64, 0)] = Value;
    unsigned Reads = 0, Writes = 0;
    Hooks = {};
    Hooks.Read = [&](uint64_t A, unsigned Size) {
      EXPECT_EQ(A, Data);
      EXPECT_EQ(Size, VectorBytes);
      ++Reads;
    };
    Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
    const auto Exit = run(I.Bytes, std::move(Hooks));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(snapshot(), Expected);
    const bool Store = I.Kind == ResultKind::Store;
    EXPECT_EQ(Reads, Store ? 0u : 1u);
    EXPECT_EQ(Writes, Store ? 2u : 0u);
    expectRAM(Store ? Initial.Left : Initial.Right);
  }
}

TEST_P(X64Alignment, SegmentBaseFollowsAddressSizeWrappingBeforeAlignment) {
  initialize();
  for (bool GS : {false, true}) {
    for (bool Misaligned : {false, true}) {
      const auto Base = GS ? CPURegister::X64GSBase : CPURegister::X64FSBase;
      llvm::cantFail(CPU->writeRegister(Base, {Data + 1, 0}));
      const uint64_t Offset = Misaligned ? 0 : VectorBytes - 1;
      llvm::cantFail(CPU->setReg(X64Register::CX, HighAddressBits + Offset));
      llvm::cantFail(CPU->setReg(X64Register::PC, Code));
      const auto Before = snapshot();
      std::vector<uint8_t> Bytes{AddressSizePrefix,
                                 uint8_t(GS ? GSPrefix : FSPrefix)};
      Bytes.insert(Bytes.end(), std::begin(MOVDQALoad), std::end(MOVDQALoad));
      BackendHooks Hooks;
      Hooks.RecoverableFault = [&](const BackendFault &Fault) {
        expectFault(Fault);
        return true;
      };
      const auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, Misaligned ? ExecutionExitKind::RecoverableFault
                                      : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      if (Misaligned) {
        ASSERT_TRUE(CPU->takeRecoverableFault());
        EXPECT_EQ(snapshot(), Before);
      } else {
        EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{}));
        EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)),
                  HighAddressBits + Offset);
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Alignment,
                         testing::ValuesIn(Parameters),
                         [](const auto &I) { return I.param.Name; });

// Bypass checked preflight: real native machines must independently reproduce
// the alignment fault before considering missing or protected guest pages.
class NativeX64Alignment : public testing::TestWithParam<Parameter> {};
TEST_P(NativeX64Alignment, HardwarePrioritizesAlignmentAndPreservesAllState) {
  const auto &P = GetParam();
  if (P.Backend == ExecutionBackendKind::Unicorn)
    GTEST_SKIP();
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  auto Created =
      P.Backend == ExecutionBackendKind::KVM   ? createKvmMachine(*Memory)
      : P.Backend == ExecutionBackendKind::WHP ? createWhpMachine(*Memory)
                                               : createHvfX64Machine(*Memory);
  if (!Created) {
    auto E = Created.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(P.Backend, GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  auto Machine = std::move(*Created);
  llvm::cantFail(
      Memory->map(Code, PageSize, Read | Write | Execute | UserAccessible));
  llvm::cantFail(Memory->map(Data, PageSize, UserAccessible));
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    llvm::cantFail(Memory->write(Code, I.Bytes));
    for (uint64_t Address : {Data + 1, Data + PageSize - 1, Stack + 1,
                             Noncanonical + 1, UINT64_MAX}) {
      SCOPED_TRACE(Address);
      X64MachineState State;
      State.UserMode = P.User;
      State.reg(X64Register::PC) = Code;
      State.reg(X64Register::SP) = Data + PageSize;
      State.reg(X64Register::CX) = Address;
      State.reg(X64Register::FLAGS) = Flags;
      State.MXCSR = MXCSR;
      for (unsigned N = 0; N < State.Xmm.size(); ++N)
        State.Xmm[N] = {SentinelLow + N, SentinelHigh - N};
      const auto Before = State;
      llvm::cantFail(Memory->beginRun());
      auto Release = llvm::scope_exit([&] { Memory->endRun(); });
      const auto Root = llvm::cantFail(buildX64PageTables(
          *Memory, P.User, Machine->requiresExceptionMonitor()));
      bool Caught = false;
      auto E = Machine->step(State, Root,
                             {std::chrono::steady_clock::now() +
                              std::chrono::microseconds(Timeout)});
      auto Remaining =
          llvm::handleErrors(std::move(E), [&](const X64ExceptionError &Fault) {
            Caught = true;
            EXPECT_EQ(Fault.exception().Vector, GeneralProtection);
            EXPECT_EQ(Fault.exception().ErrorCode, 0);
            EXPECT_EQ(Fault.exception().FaultAddress, std::nullopt);
          });
      ASSERT_EQ(llvm::toString(std::move(Remaining)), "");
      ASSERT_TRUE(Caught);
      EXPECT_EQ(State, Before);
    }
  }
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, NativeX64Alignment,
                         testing::ValuesIn(Parameters),
                         [](const auto &I) { return I.param.Name; });
} // namespace
} // namespace neverd::emulation
