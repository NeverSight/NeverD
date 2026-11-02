//===- UnicornStateTransferTests.cpp - Atomic ARM64 engine capture --------===//
//
// NeverD Decompiler
//
/// \file
/// Checks complete Unicorn register transfers under injected failures.
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64Machine.h"
#include "backends/MachineFactories.h"
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <atomic>
#include <thread>
#include <unicorn/unicorn.h>
#include <vector>

namespace {
#define NEVERD_UNICORN_STATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "UnicornStateTransferCases.def"
#undef NEVERD_UNICORN_STATE_VALUE

std::atomic<int> FailureRegister{UC_ARM64_REG_INVALID};
std::atomic<int> WriteFailureRegister{UC_ARM64_REG_INVALID};
std::atomic<int> CancellationRegister{UC_ARM64_REG_INVALID};
std::atomic<bool> CancellationStop;
std::atomic<bool> ExpireCapture;
unsigned PacketWrites = 0;
} // namespace
extern "C" uc_err __real_uc_reg_read(uc_engine *, int, void *);
// Only this executable injects a single failed transport read, after the
// original guest ADD has already changed the real engine's register state.
extern "C" uc_err __wrap_uc_reg_read(uc_engine *Engine, int Register,
                                     void *Value) {
  auto Failure = FailureRegister.load();
  if (Register == Failure &&
      FailureRegister.compare_exchange_strong(Failure, UC_ARM64_REG_INVALID))
    return UC_ERR_RESOURCE;
  const auto Result = __real_uc_reg_read(Engine, Register, Value);
  auto Cancel = CancellationRegister.load();
  if (Register == Cancel && CancellationRegister.compare_exchange_strong(
                                Cancel, UC_ARM64_REG_INVALID)) {
    if (ExpireCapture)
      std::this_thread::sleep_for(std::chrono::microseconds(
          2 *
          neverd::emulation::execution_limits::NativeStepGraceMicroseconds));
    else
      CancellationStop = true;
  }
  return Result;
}

extern "C" uc_err __real_uc_reg_read_batch(uc_engine *, const int *, void **,
                                           int);
extern "C" uc_err __wrap_uc_reg_read_batch(uc_engine *Engine, const int *IDs,
                                           void **Values, int Count) {
  if (FailureRegister == UC_ARM64_REG_INVALID &&
      CancellationRegister == UC_ARM64_REG_INVALID)
    return __real_uc_reg_read_batch(Engine, IDs, Values, Count);
  // Split only injected packets so each existing register failure/stop can
  // occur after a populated prefix. Ordinary captures use the real full batch.
  for (int I = 0; I < Count; ++I)
    if (auto Status = __wrap_uc_reg_read(Engine, IDs[I], Values[I]))
      return Status;
  return UC_ERR_OK;
}
extern "C" uc_err __real_uc_reg_write_batch(uc_engine *, const int *,
                                            void *const *, int);
extern "C" uc_err __wrap_uc_reg_write_batch(uc_engine *Engine, const int *IDs,
                                            void *const *Values, int Count) {
  ++PacketWrites;
  for (int I = 0; I < Count; ++I) {
    auto Failure = WriteFailureRegister.load();
    if (IDs[I] == Failure && WriteFailureRegister.compare_exchange_strong(
                                 Failure, UC_ARM64_REG_INVALID)) {
      if (I)
        if (auto Status = __real_uc_reg_write_batch(Engine, IDs, Values, I))
          return Status;
      return UC_ERR_RESOURCE;
    }
  }
  return __real_uc_reg_write_batch(Engine, IDs, Values, Count);
}

namespace neverd::emulation {
namespace {

constexpr int RegisterIDs[] = {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Backend)
#define NEVERD_REGISTER_X64(Backend)
#define NEVERD_REGISTER_AArch64(Backend) Backend,
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#undef NEVERD_REGISTER_X64
#undef NEVERD_SCALAR_REGISTER
};
constexpr int VectorIDs[] = {
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend)                           \
  NEVERD_VECTOR_##Arch(Backend)
#define NEVERD_VECTOR_X64(Backend)
#define NEVERD_VECTOR_AArch64(Backend) Backend,
#include "neverd/emulation/Registers.def"
#undef NEVERD_VECTOR_AArch64
#undef NEVERD_VECTOR_X64
#undef NEVERD_VECTOR_REGISTER
};
class UnicornStateTransfer : public testing::TestWithParam<bool> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<AArch64Machine> Machine;
  AArch64MachineState State;
  void SetUp() override {
    FailureRegister = UC_ARM64_REG_INVALID;
    WriteFailureRegister = UC_ARM64_REG_INVALID;
    CancellationRegister = UC_ARM64_REG_INVALID;
    CancellationStop = false;
    ExpireCapture = false;
    PacketWrites = 0;
    Memory = llvm::cantFail(MemoryProjection::create(MemoryLimit));
    const unsigned User = GetParam() ? UserAccessible : SupervisorPermission;
    llvm::cantFail(
        Memory->map(Code, memory::PageSize, Read | Write | Execute | User));
    llvm::cantFail(Memory->map(Stack, memory::PageSize, Read | Write | User));
    uint8_t Bytes[aarch64::InstructionBytes];
    llvm::support::endian::write32le(Bytes, IncrementInteger);
    llvm::cantFail(Memory->write(Code, Bytes));
    auto Created = createUnicornAArch64Machine(*Memory, GetParam());
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Machine = std::move(*Created);
    State.UserMode = GetParam();
    for (unsigned I = 0; I < State.Registers.size(); ++I)
      State.Registers[I] = InitialScalar + I;
    State.reg(AArch64Register::X0) = InitialInteger;
    State.reg(AArch64Register::PC) = Code;
    State.reg(AArch64Register::SP) = Stack + memory::PageSize - StackAlignment;
    State.reg(AArch64Register::NZCV) = PoisonedNZCV;
    State.reg(AArch64Register::FPCR) = PoisonedFPControl;
    State.reg(AArch64Register::FPSR) = PoisonedFPControl;
    for (unsigned I = 0; I < State.Vectors.size(); ++I)
      State.Vectors[I] = {InitialVectorLow + I, InitialVectorHigh - I};
  }
  void TearDown() override {
    FailureRegister = UC_ARM64_REG_INVALID;
    WriteFailureRegister = UC_ARM64_REG_INVALID;
    CancellationRegister = UC_ARM64_REG_INVALID;
    CancellationStop = false;
    ExpireCapture = false;
  }
  llvm::Error step(MachineRunControl Control = {}) {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    if (auto E = buildAArch64PageTables(*Memory, GetParam()))
      return E;
    return Machine->step(State, Control);
  }
  void expectCaptured(const AArch64MachineState &Before) {
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    for (unsigned I = 0; I < State.Registers.size(); ++I) {
      const auto Register = AArch64Register(I);
      const auto Expected =
          Register == AArch64Register::X0   ? InitialInteger + IntegerIncrement
          : Register == AArch64Register::PC ? Code + aarch64::InstructionBytes
          : Register == AArch64Register::NZCV ? ExpectedNZCV
          : Register == AArch64Register::FPCR ||
                  Register == AArch64Register::FPSR
              ? ExpectedFPControl
              : Before.reg(Register);
      EXPECT_EQ(State.reg(Register), Expected) << I;
    }
  }
};
TEST_P(UnicornStateTransfer, FailedScalarReadRetainsAllInputAndAllowsRetry) {
  const auto Before = State;
  ASSERT_EQ(std::size(RegisterIDs), unsigned(AArch64Register::FPSR) + 1);
  for (int Register : RegisterIDs) {
    SCOPED_TRACE(Register);
    State = Before;
    FailureRegister = Register;
    EXPECT_EQ(llvm::toString(step()), uc_strerror(UC_ERR_RESOURCE));
    EXPECT_EQ(FailureRegister.load(), UC_ARM64_REG_INVALID);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    // The engine already advanced. Retrying must install the untouched input
    // and execute the original instruction exactly once.
    ASSERT_EQ(llvm::toString(step()), "");
    expectCaptured(Before);
  }
}
TEST_P(UnicornStateTransfer, CapturesDeclaredWidthsWithoutStaleUpperBits) {
  const auto Before = State;
  ASSERT_EQ(llvm::toString(step()), "");
  expectCaptured(Before);
}
TEST_P(UnicornStateTransfer, ContinuedStepsReuseOnlyCapturedUserInput) {
  ASSERT_EQ(llvm::toString(step()), "");
  uint8_t Bytes[aarch64::InstructionBytes];
  llvm::support::endian::write32le(Bytes, IncrementInteger);
  for (unsigned I = 0; I < SequentialSteps; ++I) {
    ASSERT_EQ(
        llvm::toString(Memory->write(State.reg(AArch64Register::PC), Bytes)),
        "");
    const auto Before = PacketWrites;
    ASSERT_EQ(llvm::toString(step()), "");
    if (GetParam())
      EXPECT_EQ(PacketWrites, Before);
    else
      EXPECT_GT(PacketWrites, Before);
  }
  State.reg(AArch64Register::PC) = Code;
  const auto Before = PacketWrites;
  ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_GT(PacketWrites, Before);
}
TEST_P(UnicornStateTransfer, FailedWarmCaptureRestoresInputBeforeRetry) {
  ASSERT_EQ(llvm::toString(step()), "");
  std::vector<int> Registers(std::begin(RegisterIDs), std::end(RegisterIDs));
  Registers.insert(Registers.end(), std::begin(VectorIDs), std::end(VectorIDs));
  uint8_t Bytes[aarch64::InstructionBytes];
  llvm::support::endian::write32le(Bytes, IncrementInteger);
  for (int Register : Registers) {
    SCOPED_TRACE(Register);
    ASSERT_EQ(
        llvm::toString(Memory->write(State.reg(AArch64Register::PC), Bytes)),
        "");
    const auto Before = State;
    FailureRegister = Register;
    EXPECT_EQ(llvm::toString(step()), uc_strerror(UC_ERR_RESOURCE));
    EXPECT_EQ(FailureRegister.load(), UC_ARM64_REG_INVALID);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    const auto WritesBeforeRetry = PacketWrites;
    ASSERT_EQ(llvm::toString(step()), "");
    EXPECT_GT(PacketWrites, WritesBeforeRetry);
    auto Expected = Before;
    Expected.reg(AArch64Register::X0) += IntegerIncrement;
    Expected.reg(AArch64Register::PC) += aarch64::InstructionBytes;
    EXPECT_EQ(State.Registers, Expected.Registers);
    EXPECT_EQ(State.Vectors, Expected.Vectors);
  }
}
TEST_P(UnicornStateTransfer, PartialInstallRetainsInputAndRetriesAllRegisters) {
  const auto Before = State;
  std::vector<int> Registers(std::begin(RegisterIDs), std::end(RegisterIDs));
  Registers.insert(Registers.end(), std::begin(VectorIDs), std::end(VectorIDs));
  for (int Register : Registers) {
    SCOPED_TRACE(Register);
    State = Before;
    WriteFailureRegister = Register;
    EXPECT_EQ(llvm::toString(step()), uc_strerror(UC_ERR_RESOURCE));
    EXPECT_EQ(WriteFailureRegister.load(), UC_ARM64_REG_INVALID);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    ASSERT_EQ(llvm::toString(step()), "");
    expectCaptured(Before);
  }
}
TEST_P(UnicornStateTransfer, FailedVectorReadCannotPublishScalarOrVectorState) {
  const auto Before = State;
  for (int Register : VectorIDs) {
    SCOPED_TRACE(Register);
    State = Before;
    FailureRegister = Register;
    EXPECT_EQ(llvm::toString(step()), uc_strerror(UC_ERR_RESOURCE));
    EXPECT_EQ(FailureRegister.load(), UC_ARM64_REG_INVALID);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    ASSERT_EQ(llvm::toString(step()), "");
    expectCaptured(Before);
  }
}
TEST_P(UnicornStateTransfer, StopDuringCaptureCannotPublishAndAllowsRetry) {
  const auto Before = State;
  std::vector<int> Registers(std::begin(RegisterIDs), std::end(RegisterIDs));
  Registers.insert(Registers.end(), std::begin(VectorIDs), std::end(VectorIDs));
  for (int Register : Registers) {
    SCOPED_TRACE(Register);
    State = Before;
    CancellationRegister = Register;
    CancellationStop = false;
    EXPECT_EQ(llvm::toString(step({{}, &CancellationStop})),
              diagnostic::UnicornRun);
    ASSERT_EQ(CancellationRegister.load(), UC_ARM64_REG_INVALID);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    ASSERT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    CancellationStop = false;
    ASSERT_EQ(llvm::toString(step()), "");
    expectCaptured(Before);
  }
}
TEST_P(UnicornStateTransfer, ExpiredCaptureCannotPublishAndAllowsRetry) {
  const auto Before = State;
  CancellationRegister = std::end(VectorIDs)[-1];
  ExpireCapture = true;
  EXPECT_EQ(llvm::toString(step()), diagnostic::UnicornRun);
  ASSERT_EQ(CancellationRegister.load(), UC_ARM64_REG_INVALID);
  EXPECT_FALSE(CancellationStop);
  ASSERT_EQ(State.Registers, Before.Registers);
  EXPECT_EQ(State.Vectors, Before.Vectors);
  ExpireCapture = false;
  ASSERT_EQ(llvm::toString(step()), "");
  expectCaptured(Before);
}
INSTANTIATE_TEST_SUITE_P(Privileges, UnicornStateTransfer, testing::Bool());
} // namespace
} // namespace neverd::emulation
