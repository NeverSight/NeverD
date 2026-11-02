//===- X64SIMDExceptionTests.cpp - Native SSE faults and continuations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64Exception.h"
#include "arch/x86_64/X64Machine.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <map>

namespace neverd::emulation {
namespace {
#define NEVERD_SIMD_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_SIMD_TEXT(Name, Value) constexpr char Name[] = Value;
#include "X64SIMDExceptionCases.def"
#undef NEVERD_SIMD_TEXT
#undef NEVERD_SIMD_VALUE
struct Operation {
  const char *Name;
  uint32_t Status, Left, Right, FaultStatus, MaskedStatus, MaskedResult,
      RepairedResult;
  std::array<uint8_t, FourBytes> Bytes;
};
constexpr Operation Operations[] = {
#define NEVERD_SIMD_CASE(Name, Status, Left, Right, FaultStatus, MaskedStatus, \
                         MaskedResult, RepairedResult, ...)                    \
  {#Name,        Status,       Left,           Right,        FaultStatus,      \
   MaskedStatus, MaskedResult, RepairedResult, {__VA_ARGS__}},
#include "X64SIMDExceptionCases.def"
#undef NEVERD_SIMD_CASE
};
struct Backend {
  const char *Name;
  ExecutionBackendKind Kind;
  bool User;
};
constexpr Backend Backends[] = {
#define NEVERD_SIMD_BACKEND(Name, Kind, User)                                  \
  {#Name, ExecutionBackendKind::Kind, User},
#include "X64SIMDExceptionCases.def"
#undef NEVERD_SIMD_BACKEND
};
struct Parameter {
  Backend Transport;
  Operation Instruction;
  std::string name() const {
    return std::string(Transport.Name) + Separator + Instruction.Name;
  }
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.name(); }
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (const auto &B : Backends)
    for (const auto &O : Operations)
      Result.push_back({B, O});
  return Result;
}

TEST(X64SIMDCapability, RequirementsAreNativeBackendAndContractQualified) {
  for (auto Kind : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP, ExecutionBackendKind::HVF})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64,
          ExecutionContract::CheckedUserX64}) {
      const bool Expected = Kind == ExecutionBackendKind::KVM ||
                            Kind == ExecutionBackendKind::WHP;
      auto Caps = llvm::cantFail(
          executionCapabilities(Contract, GuestArchitecture::X64, Kind));
      EXPECT_EQ(Caps.supports(ExecutionFeature::SIMDExceptions), Expected);
      EXPECT_FALSE(llvm::cantFail(
                       executionCapabilities(Contract, GuestArchitecture::X64))
                       .supports(ExecutionFeature::SIMDExceptions));
      ExecutionConfiguration Config;
      Config.Backend = Kind;
      Config.Contract = Contract;
      Config.RequiredFeatures = ExecutionFeature::SIMDExceptions;
      auto Resolved = resolveExecutionConfiguration(Config);
      EXPECT_EQ(bool(Resolved), Expected);
      if (!Resolved)
        llvm::consumeError(Resolved.takeError());
    }
  for (auto Kind : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP})
    for (auto Contract : {ExecutionContract::CheckedAArch64,
                          ExecutionContract::CheckedUserAArch64})
      EXPECT_FALSE(
          llvm::cantFail(
              executionCapabilities(Contract, GuestArchitecture::AArch64, Kind))
              .supports(ExecutionFeature::SIMDExceptions));
}

class X64CheckedSIMDException : public testing::TestWithParam<Parameter> {};

TEST_P(X64CheckedSIMDException,
       PublicFaultStateAndBothRetriesAreArchitectural) {
  const auto &P = GetParam();
  const auto &O = P.Instruction;
  for (bool Legacy : {false, true}) {
    if (Legacy && P.Transport.User)
      continue;
    auto Created = createExecutionBackend(
        P.Transport.Kind,
        Legacy             ? ExecutionContract::Legacy
        : P.Transport.User ? ExecutionContract::CheckedUserX64
                           : ExecutionContract::CheckedX64,
        Limit);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    auto &CPU = *Created->CPU;
    ASSERT_TRUE(CPU.supportsSIMDExceptions());
    llvm::cantFail(
        CPU.map(Code, Page, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU.map(Data, Page, Read | Write | UserAccessible));
    auto Snapshot = [&] {
      std::map<CPURegister, RegisterValue> State;
      for (unsigned R = unsigned(CPURegister::X64AX);
           R <= unsigned(CPURegister::X64FP7); ++R)
        if (registerMatches(CPURegister(R), GuestArchitecture::X64))
          State[CPURegister(R)] =
              llvm::cantFail(CPU.readRegister(CPURegister(R)));
      return State;
    };
    for (bool FromMemory : {false, true})
      for (auto Previous : {uint64_t(0), Sticky}) {
        SCOPED_TRACE(Legacy);
        SCOPED_TRACE(FromMemory);
        SCOPED_TRACE(Previous);
        auto Bytes = O.Bytes;
        if (FromMemory)
          Bytes.back() = MemoryOperand;
        llvm::cantFail(CPU.write(Code, Bytes));
        const uint8_t Stop = Nop;
        llvm::cantFail(CPU.write(Code + Bytes.size(), {&Stop, 1}));
        const uint64_t Control =
            (DefaultMXCSR & ~(O.Status << MaskShift)) | Previous;
        llvm::cantFail(CPU.setReg(X64Register::MXCSR, Control));
        llvm::cantFail(CPU.setReg(X64Register::PC, Code));
        llvm::cantFail(CPU.setReg(X64Register::FLAGS, Flags));
        llvm::cantFail(CPU.setReg(X64Register::CX, Data));
        llvm::cantFail(CPU.setReg(X64Register::SP, Data + HalfPage));
        for (unsigned N = 0; N < x64::XmmCount; ++N)
          llvm::cantFail(CPU.setXmm(N, {Sentinel + N, Sentinel - N}));
        llvm::cantFail(CPU.setXmm(
            0, {(Sentinel & ~uint64_t(UINT32_MAX)) | O.Left, Sentinel}));
        llvm::cantFail(CPU.setXmm(
            1, {(Sentinel & ~uint64_t(UINT32_MAX)) | O.Right, Sentinel}));
        std::vector<uint8_t> RAM(Page, Fill), After(Page);
        llvm::support::endian::write32le(RAM.data(), O.Right);
        llvm::cantFail(CPU.write(Data, RAM));
        unsigned Faults = 0;
        BackendHooks H;
        H.Instruction = [&](uint64_t PC, uint32_t) {
          if (PC == Code + Bytes.size())
            CPU.stop();
        };
        H.RecoverableFault = [&](const BackendFault &F) {
          ++Faults;
          EXPECT_EQ(F.PC, Code);
          EXPECT_EQ(F.Interrupt, Vector);
          EXPECT_FALSE(F.ErrorCode);
          EXPECT_FALSE(F.Address);
          return true;
        };
        llvm::cantFail(CPU.installHooks(H));
        auto Expected = Snapshot();
        auto Exit = llvm::cantFail(CPU.runUntilExit(Code, Timeout));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
            << Exit.Diagnostic;
        ASSERT_TRUE(CPU.takeRecoverableFault());
        EXPECT_EQ(Faults, 1u);
        Expected[CPURegister::X64MXCSR][0] |= O.FaultStatus;
        EXPECT_EQ(Snapshot(), Expected);
        llvm::cantFail(CPU.read(Data, After));
        EXPECT_EQ(After, RAM);
        auto FaultState = llvm::cantFail(CPU.saveContext());
        for (bool Repair : {false, true}) {
          llvm::cantFail(CPU.restoreContext(*FaultState));
          auto Result = Expected;
          if (Repair) {
            Result[CPURegister::X64V0][0] =
                (Sentinel & ~uint64_t(UINT32_MAX)) | One;
            Result[CPURegister::X64V1][0] = Result[CPURegister::X64V0][0];
            llvm::cantFail(CPU.setXmm(0, Result[CPURegister::X64V0]));
            llvm::cantFail(CPU.setXmm(1, Result[CPURegister::X64V1]));
            llvm::support::endian::write32le(RAM.data(), One);
            llvm::cantFail(CPU.write(Data, RAM));
          } else {
            Result[CPURegister::X64MXCSR][0] |= DefaultMXCSR;
            llvm::cantFail(CPU.setReg(X64Register::MXCSR,
                                      Result[CPURegister::X64MXCSR][0]));
            Result[CPURegister::X64MXCSR][0] |= O.MaskedStatus;
          }
          Result[CPURegister::X64V0][0] =
              (Sentinel & ~uint64_t(UINT32_MAX)) |
              (Repair ? O.RepairedResult : O.MaskedResult);
          Result[CPURegister::X64PC][0] += Bytes.size();
          Exit = llvm::cantFail(CPU.runUntilExit(Code, Timeout));
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
          EXPECT_EQ(Faults, 1u);
          EXPECT_EQ(Snapshot(), Result);
          llvm::cantFail(CPU.read(Data, After));
          EXPECT_EQ(After, RAM);
        }
      }
  }
}

INSTANTIATE_TEST_SUITE_P(Native, X64CheckedSIMDException,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });

// Bypass checked admission intentionally: the native transport must retain
// precise hardware fault state before an architecture or OS can resume it.
class X64SIMDException : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  X64MachineState State;
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto M = GetParam().Transport.Kind == ExecutionBackendKind::KVM
                 ? createKvmMachine(*Memory)
                 : createWhpMachine(*Memory);
    if (!M) {
      auto E = M.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Machine = std::move(*M);
    llvm::cantFail(
        Memory->map(Code, Page, Read | Write | Execute | UserAccessible));
    llvm::cantFail(Memory->map(Data, Page, Read | Write | UserAccessible));
  }
  llvm::Error step() {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    auto Root = buildX64PageTables(*Memory, State.UserMode,
                                   Machine->requiresExceptionMonitor());
    if (!Root)
      return Root.takeError();
    return Machine->step(State, *Root,
                         {std::chrono::steady_clock::now() +
                          std::chrono::microseconds(Timeout)});
  }
  void fault() {
    bool Trapped = false;
    auto E = step();
    ASSERT_TRUE(bool(E));
    auto Remaining =
        llvm::handleErrors(std::move(E), [&](const X64ExceptionError &E) {
          Trapped = true;
          EXPECT_EQ(E.exception().Vector, Vector);
          EXPECT_EQ(E.exception().ErrorCode, std::nullopt);
          EXPECT_EQ(E.exception().FaultAddress, std::nullopt);
        });
    ASSERT_EQ(llvm::toString(std::move(Remaining)), "");
    ASSERT_TRUE(Trapped);
  }
};

TEST_P(X64SIMDException, FaultStateAndBothRetriesAreArchitectural) {
  const auto &O = GetParam().Instruction;
  for (bool FromMemory : {false, true})
    for (auto Previous : {uint64_t(0), Sticky}) {
      SCOPED_TRACE(FromMemory);
      SCOPED_TRACE(Previous);
      State = {};
      State.UserMode = GetParam().Transport.User;
#define NEVERD_MASK_REGISTER(Name)                                             \
  State.reg(X64Register::Name) = Sentinel + unsigned(X64Register::Name);
#include "X64VectorMaskCases.def"
#undef NEVERD_MASK_REGISTER
      State.reg(X64Register::PC) = Code;
      State.reg(X64Register::SP) = Data + HalfPage;
      State.reg(X64Register::CX) = Data;
      State.reg(X64Register::FLAGS) = Flags;
      State.FSBase = Data;
      State.GSBase = Data + HalfPage;
      State.MXCSR = (DefaultMXCSR & ~(O.Status << MaskShift)) | Previous;
      for (unsigned N = 0; N < State.Xmm.size(); ++N)
        State.Xmm[N] = {Sentinel + N, Sentinel - N};
      State.FP.Tag = FPTag;
      State.FP.Status = FPStatus;
      for (unsigned N = 0; N < State.FP.Registers.size(); ++N)
        State.FP.Registers[N] = {FPSignificand + N, FPExponent};
      State.Xmm[0][0] = (Sentinel & ~uint64_t(UINT32_MAX)) | O.Left;
      State.Xmm[1][0] = (Sentinel & ~uint64_t(UINT32_MAX)) | O.Right;
      auto Bytes = O.Bytes;
      if (FromMemory)
        Bytes.back() = MemoryOperand;
      llvm::cantFail(Memory->write(Code, Bytes));
      std::vector<uint8_t> RAM(Page, Fill);
      llvm::support::endian::write32le(RAM.data(), O.Right);
      llvm::cantFail(Memory->write(Data, RAM));
      auto Expected = State;
      fault();
      ASSERT_FALSE(HasFatalFailure());
      Expected.MXCSR |= O.FaultStatus;
      ASSERT_EQ(State, Expected);
      std::vector<uint8_t> After(Page);
      llvm::cantFail(Memory->read(Data, After));
      ASSERT_EQ(After, RAM);

      // Retry the original instruction after masking exceptions. It commits
      // one result and the masked status, preserving the original source.
      State.MXCSR |= DefaultMXCSR;
      Expected = State;
      Expected.reg(X64Register::PC) += Bytes.size();
      Expected.Xmm[0][0] = (Sentinel & ~uint64_t(UINT32_MAX)) | O.MaskedResult;
      Expected.MXCSR |= O.MaskedStatus;
      ASSERT_EQ(llvm::toString(step()), "");
      ASSERT_EQ(State, Expected);
      llvm::cantFail(Memory->read(Data, After));
      ASSERT_EQ(After, RAM);

      // Retain every sticky flag, unmask the original exception and repair
      // its operands. Old status must not be mistaken for a new exception.
      State.reg(X64Register::PC) = Code;
      State.MXCSR &= ~(O.Status << MaskShift);
      State.Xmm[0][0] = (Sentinel & ~uint64_t(UINT32_MAX)) | One;
      State.Xmm[1][0] = (Sentinel & ~uint64_t(UINT32_MAX)) | One;
      llvm::support::endian::write32le(RAM.data(), One);
      llvm::cantFail(Memory->write(Data, RAM));
      Expected = State;
      Expected.reg(X64Register::PC) += Bytes.size();
      Expected.Xmm[0][0] =
          (Sentinel & ~uint64_t(UINT32_MAX)) | O.RepairedResult;
      ASSERT_EQ(llvm::toString(step()), "");
      ASSERT_EQ(State, Expected);
      llvm::cantFail(Memory->read(Data, After));
      ASSERT_EQ(After, RAM);
    }
}

INSTANTIATE_TEST_SUITE_P(Native, X64SIMDException,
                         testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.name(); });
} // namespace
} // namespace neverd::emulation
