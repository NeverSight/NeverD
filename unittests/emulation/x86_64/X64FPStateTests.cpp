//===- X64FPStateTests.cpp - Native x87 transfer and physical stack state
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../HvfTestPolicy.h"
#include "arch/x86_64/X64Machine.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Memory.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace neverd::emulation {
namespace {
#define NEVERD_FP_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_FP_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_FP_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FPCases.def"
#undef NEVERD_FP_BYTES
#undef NEVERD_FP_TEXT
#undef NEVERD_FP_VALUE
const RegisterValue Payloads[] = {
#define NEVERD_FP_PAYLOAD(Low, High) {Low, High},
#include "X64FPCases.def"
#undef NEVERD_FP_PAYLOAD
};
X64MachineState seed(unsigned Top) {
  X64MachineState S;
  S.FP.Control = SeedControl;
  S.FP.Status = SeedStatus | (Top << x64::fp::TopShift);
  S.FP.Opcode = SeedOpcode;
  S.FP.Instruction = SeedInstruction;
  S.FP.Data = SeedData;
  S.FP.Tag = 0;
  for (unsigned I = 0; I < S.FP.Registers.size(); ++I) {
    S.FP.Registers[I] = Payloads[I];
    if (I % (EmptySlot + 1) != EmptySlot)
      S.FP.Tag |= 1u << I;
  }
  S.MXCSR = SeedMXCSR;
  for (unsigned I = 0; I < S.Xmm.size(); ++I)
    S.Xmm[I] = {SeedXmm + I, ~SeedXmm - I};
  S.reg(X64Register::FLAGS) = x64::InitialFlags;
  S.reg(X64Register::PC) = Code;
  S.reg(X64Register::CX) = Data;
  S.reg(X64Register::SP) = Data + memory::PageSize;
  return S;
}
void expectFP(const X64MachineState &Actual, const X64MachineState &Expected,
              bool Metadata = true) {
  EXPECT_EQ(Actual.FP.Control, Expected.FP.Control);
  EXPECT_EQ(Actual.FP.Status, Expected.FP.Status);
  EXPECT_EQ(Actual.FP.Tag, Expected.FP.Tag);
  EXPECT_EQ(Actual.FP.Registers, Expected.FP.Registers);
  if (Metadata) {
    EXPECT_EQ(Actual.FP.Opcode, Expected.FP.Opcode);
    EXPECT_EQ(Actual.FP.Instruction, Expected.FP.Instruction);
    EXPECT_EQ(Actual.FP.Data, Expected.FP.Data);
  }
  EXPECT_EQ(Actual.MXCSR, Expected.MXCSR);
  EXPECT_EQ(Actual.Xmm, Expected.Xmm);
}
TEST(X64FPState, PhysicalTagClassificationAndLogicalFXSaveSlots) {
  for (unsigned Top = 0; Top < x64::fp::RegisterCount; ++Top) {
    auto S = seed(Top);
    EXPECT_EQ(S.FP.fullTag(), ExpectedTag);
    X64FPState F;
    F.setFullTag(S.FP.fullTag());
    EXPECT_EQ(F.Tag, S.FP.Tag);
    std::array<uint8_t, x64::fp::LegacyBytes> Bytes{};
    llvm::cantFail(encodeX64FXState(S, Bytes));
    X64MachineState Result;
    llvm::cantFail(decodeX64FXState(Result, Bytes));
    expectFP(Result, S);
    auto Bad = decodeX64FXState(Result, llvm::ArrayRef(Bytes).drop_back());
    EXPECT_TRUE(bool(Bad));
    llvm::consumeError(std::move(Bad));
    expectFP(Result, S);
  }
}

TEST(X64FPState, InvalidStateCannotTruncateLanesOrPublishAStateBuffer) {
  auto S = seed(EmptySlot);
  std::array<uint8_t, x64::fp::LegacyBytes> Bytes{};
  llvm::cantFail(encodeX64FXState(S, Bytes));
  const auto Before = Bytes;
  S.FP.Registers.back()[1] |= InvalidPadding;
  auto E = encodeX64FXState(S, Bytes);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(Bytes, Before);
  S.FP.Registers.back() = Payloads[EmptySlot];
  S.FP.Control = InvalidControl;
  E = validateX64FPState(S.FP);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(X64FPState, StandardAndCompactedXsavePreserveEveryPhysicalTOP) {
  for (const bool Compact : {false, true})
    for (unsigned Top = 0; Top < x64::fp::RegisterCount; ++Top) {
      const auto Before = seed(Top);
      std::array<uint8_t, x64::fp::XsaveBytes> Bytes{};
      llvm::cantFail(encodeX64XsaveState(Before, Bytes, Compact));
      X64MachineState Next;
      llvm::cantFail(decodeX64XsaveState(Next, Bytes));
      expectFP(Next, Before);
    }
}
TEST(X64FPState, AbsentXsaveComponentsIgnoreStalePayloadAndUseInitState) {
  for (const auto Present :
       {uint64_t(0), x64::fp::X87Present, x64::fp::SSEPresent}) {
    const auto Before = seed(EmptySlot);
    std::array<uint8_t, x64::fp::XsaveBytes> Bytes{};
    llvm::cantFail(encodeX64XsaveState(Before, Bytes, true));
    llvm::support::endian::write64le(Bytes.data() + x64::fp::XStateOffset,
                                     Present);
    if (!(Present & x64::fp::X87Present))
      llvm::support::endian::write16le(Bytes.data() + x64::fp::ControlOffset,
                                       InvalidControl);
    if (!(Present & x64::fp::SSEPresent))
      llvm::support::endian::write32le(Bytes.data() + x64::fp::MXCSROffset,
                                       InvalidPadding);
    auto Expected = Before;
    if (!(Present & x64::fp::X87Present))
      Expected.FP = {};
    if (!(Present & x64::fp::SSEPresent)) {
      Expected.MXCSR = x64::InitialMXCSR;
      Expected.Xmm = {};
    }
    auto Next = Before;
    llvm::cantFail(decodeX64XsaveState(Next, Bytes));
    expectFP(Next, Expected);
  }
}
TEST(X64FPState, MXCSRMaskComesFromTheCPUAndCannotBeChangedByAPacket) {
  for (const bool Compact : {false, true}) {
    auto Before = seed(EmptySlot);
    Before.MXCSR |= x64::fp::DenormalsAreZero;
    std::array<uint8_t, x64::fp::XsaveBytes> Bytes{};
    llvm::cantFail(encodeX64XsaveState(Before, Bytes, Compact,
                                       x64::fp::ArchitecturalMXCSRMask));
    EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() +
                                              x64::fp::MXCSRMaskOffset),
              x64::fp::ArchitecturalMXCSRMask);
    const auto Packet = Bytes;
    auto E = encodeX64XsaveState(Before, Bytes, Compact);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(Bytes, Packet);
    X64MachineState Result;
    const auto Unchanged = Result;
    E = decodeX64XsaveState(Result, Bytes);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(Result, Unchanged);
    llvm::cantFail(
        decodeX64XsaveState(Result, Bytes, x64::fp::ArchitecturalMXCSRMask));
    expectFP(Result, Before);
  }
}

TEST(X64FPState, MalformedXsaveHeadersCannotPublishPartialState) {
  const auto Before = seed(EmptySlot);
  for (const auto Offset : {x64::fp::XStateOffset, x64::fp::XCompOffset,
                            x64::fp::XCompOffset + sizeof(uint64_t)}) {
    std::array<uint8_t, x64::fp::XsaveBytes> Bytes{};
    llvm::cantFail(encodeX64XsaveState(Before, Bytes));
    llvm::support::endian::write64le(Bytes.data() + Offset, InvalidPadding);
    auto Next = Before;
    auto E = decodeX64XsaveState(Next, Bytes);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(Next, Before);
  }
  std::array<uint8_t, x64::fp::XsaveBytes> Short{};
  auto Next = Before;
  auto E = decodeX64XsaveState(Next, llvm::ArrayRef(Short).drop_back());
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(Next, Before);
}

TEST(X64FPState, SoftwareResetAndContextsRetainExtendedRegisters) {
  auto B = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                  ExecutionContract::Legacy, Limit);
  if (!B) {
    auto E = B.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    auto Text = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
  auto &CPU = *B->CPU;
  EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FPCW)),
            x64::fp::InitialControl);
  EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FPSW)), 0u);
  EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FPTag)), 0u);
  EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::MXCSR)), x64::InitialMXCSR);
  const auto S = seed(EmptySlot);
  for (unsigned I = 0; I < S.FP.Registers.size(); ++I) {
    const auto R = static_cast<CPURegister>(unsigned(CPURegister::X64FP0) + I);
    llvm::cantFail(CPU.writeRegister(R, S.FP.Registers[I]));
  }
  llvm::cantFail(CPU.setReg(X64Register::FPSW, S.FP.Status));
  llvm::cantFail(CPU.setReg(X64Register::FPTag, S.FP.Tag));
  auto Context = llvm::cantFail(CPU.saveContext());
  llvm::cantFail(CPU.setReg(X64Register::FPSW, 0));
  llvm::cantFail(CPU.setReg(X64Register::FPTag, 0));
  llvm::cantFail(CPU.writeRegister(CPURegister::X64FP7, {}));
  llvm::cantFail(CPU.restoreContext(*Context));
  EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FPSW)), S.FP.Status);
  EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FPTag)), S.FP.Tag);
  EXPECT_EQ(llvm::cantFail(CPU.readRegister(CPURegister::X64FP7)),
            S.FP.Registers.back());
}

struct Parameter {
  ExecutionBackendKind Backend;
  bool UserMode;
  std::string name() const {
    return std::string(executionBackendName(Backend)) +
           (UserMode ? UserSuffix : SupervisorSuffix);
  }
};
void PrintTo(const Parameter &Value, std::ostream *OS) { *OS << Value.name(); }
std::string parameterName(const testing::TestParamInfo<Parameter> &Info) {
  return Info.param.name();
}
const Parameter Parameters[] = {
#define NEVERD_FP_TRANSPORT(Backend, User)                                     \
  {ExecutionBackendKind::Backend, User},
#include "X64FPCases.def"
#undef NEVERD_FP_TRANSPORT
};
class X64FPTransport : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  llvm::Expected<std::unique_ptr<X64Machine>>
  createMachine(MemoryProjection &Storage) {
    return GetParam().Backend == ExecutionBackendKind::KVM
               ? createKvmMachine(Storage)
           : GetParam().Backend == ExecutionBackendKind::WHP
               ? createWhpMachine(Storage)
           : GetParam().Backend == ExecutionBackendKind::HVF
               ? createHvfX64Machine(Storage)
               : createUnicornX64Machine(Storage, GetParam().UserMode);
  }
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto M = createMachine(*Memory);
    if (!M) {
      auto E = M.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(GetParam().Backend, GuestArchitecture::X64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    Machine = std::move(*M);
    llvm::cantFail(Memory->map(Code, memory::PageSize,
                               Read | Write | Execute | UserAccessible));
    llvm::cantFail(
        Memory->map(Data, memory::PageSize, Read | Write | UserAccessible));
  }
  void step(X64MachineState &State, llvm::ArrayRef<uint8_t> Bytes) {
    stepOn(*Machine, *Memory, State, Bytes);
  }
  void stepOn(X64Machine &CPU, MemoryProjection &Storage,
              X64MachineState &State, llvm::ArrayRef<uint8_t> Bytes) {
    State.UserMode = GetParam().UserMode;
    State.reg(X64Register::PC) = Code;
    llvm::cantFail(Storage.write(Code, Bytes));
    llvm::cantFail(Storage.beginRun());
    auto Release = llvm::scope_exit([&] { Storage.endRun(); });
    const auto Root = llvm::cantFail(buildX64PageTables(
        Storage, State.UserMode, CPU.requiresExceptionMonitor()));
    auto E = CPU.step(State, Root,
                      {std::chrono::steady_clock::now() +
                       std::chrono::microseconds(Timeout)});
    EXPECT_EQ(llvm::toString(std::move(E)), "");
  }
};
TEST_P(X64FPTransport, PreservesAllPhysicalLanesAndControlAcrossEveryTOP) {
  for (unsigned Top = 0; Top < x64::fp::RegisterCount; ++Top) {
    SCOPED_TRACE(Top);
    auto S = seed(Top);
    auto Expected = S;
    step(S, Nop);
    expectFP(S, Expected);
    EXPECT_EQ(S.reg(X64Register::PC), Code + sizeof(Nop));
  }
}
TEST_P(X64FPTransport, LogicalCPUSwitchingRestoresPhysicalFPAndTLS) {
  auto PeerMemory = llvm::cantFail(MemoryProjection::create(Limit));
  auto Created = createMachine(*PeerMemory);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto Peer = std::move(*Created);
  llvm::cantFail(PeerMemory->map(Code, memory::PageSize,
                                 Read | Write | Execute | UserAccessible));
  llvm::cantFail(
      PeerMemory->map(Data, memory::PageSize, Read | Write | UserAccessible));
  auto First = seed(EmptySlot), Second = seed(0);
  First.FSBase = SeedInstruction;
  First.GSBase = SeedData;
  Second.FSBase = SeedData;
  Second.GSBase = SeedInstruction;
  for (auto &Lane : Second.Xmm)
    std::swap(Lane[0], Lane[1]);
  std::reverse(Second.FP.Registers.begin(), Second.FP.Registers.end());
  auto Check = [&](X64Machine &CPU, MemoryProjection &Storage,
                   X64MachineState &State) {
    const auto Before = State;
    stepOn(CPU, Storage, State, Nop);
    expectFP(State, Before);
    EXPECT_EQ(State.FSBase, Before.FSBase);
    EXPECT_EQ(State.GSBase, Before.GSBase);
    EXPECT_EQ(State.reg(X64Register::PC), Code + sizeof(Nop));
  };
  Check(*Machine, *Memory, First);
  Check(*Peer, *PeerMemory, Second);
  Check(*Machine, *Memory, First);
  Check(*Peer, *PeerMemory, Second);
  // Destroying an inactive logical CPU must not retire its peer's native state.
  Machine.reset();
  Check(*Peer, *PeerMemory, Second);
}
TEST_P(X64FPTransport, StackChangesMatchIndependentHostFXSaveAndRestore) {
#if defined(__x86_64__) || defined(_M_X64)
  for (auto Instruction :
       {llvm::ArrayRef<uint8_t>(Load80), llvm::ArrayRef<uint8_t>(AddPop),
        llvm::ArrayRef<uint8_t>(StorePop80)}) {
    // Use finite nonempty operands. The full precision payload is deliberately
    // not representable as binary64, and the physical TOP changes each time.
    for (unsigned Top = 0; Top < x64::fp::RegisterCount; ++Top) {
      SCOPED_TRACE(Top);
      auto S = seed(Top);
      S.FP.Control = x64::fp::InitialControl;
      S.FP.Status = Top << x64::fp::TopShift;
      S.FP.Tag = UINT8_MAX;
      for (unsigned I = 0; I < S.FP.Registers.size(); ++I)
        S.FP.Registers[I] = {Payloads[0][0] + I * PayloadStride,
                             Payloads[0][1]};
      if (Instruction.data() == Load80)
        S.FP.Tag &= ~(1u << ((Top + x64::fp::RegisterCount - 1) %
                             x64::fp::RegisterCount));
      alignas(x64::fp::RegisterSlotBytes)
          std::array<uint8_t, x64::fp::LegacyBytes>
              Input{}, Output{}, Host{};
      llvm::cantFail(encodeX64FXState(S, Input));
      std::array<uint8_t, x64::fp::RegisterSlotBytes> RAM{};
      std::memcpy(RAM.data(), Payloads[0].data(), x64::fp::RegisterBytes);
      llvm::cantFail(Memory->write(Data, RAM));
#ifdef _WIN32
      std::vector<uint8_t> Oracle(std::begin(Win64Before),
                                  std::end(Win64Before));
      const auto After = llvm::ArrayRef<uint8_t>(Win64After);
#else
      std::vector<uint8_t> Oracle(std::begin(SysVBefore), std::end(SysVBefore));
      const auto After = llvm::ArrayRef<uint8_t>(SysVAfter);
#endif
      Oracle.insert(Oracle.end(), Instruction.begin(), Instruction.end());
      Oracle.insert(Oracle.end(), After.begin(), After.end());
      std::error_code EC;
      auto Block = llvm::sys::Memory::allocateMappedMemory(
          memory::PageSize, nullptr,
          llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
      ASSERT_FALSE(bool(EC)) << EC.message();
      auto Release = llvm::scope_exit(
          [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
      std::memcpy(Block.base(), Oracle.data(), Oracle.size());
      EC = llvm::sys::Memory::protectMappedMemory(
          Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
      ASSERT_FALSE(bool(EC)) << EC.message();
      llvm::sys::Memory::InvalidateInstructionCache(Block.base(),
                                                    Oracle.size());
      auto Execute = reinterpret_cast<void (*)(void *, void *, void *, void *)>(
          Block.base());
      Execute(Input.data(), Output.data(), Host.data(), RAM.data());
      X64MachineState Expected;
      llvm::cantFail(decodeX64FXState(Expected, Output));
      step(S, Instruction);
      expectFP(S, Expected, false);
      std::array<uint8_t, x64::fp::RegisterSlotBytes> ActualRAM{};
      llvm::cantFail(Memory->read(Data, ActualRAM));
      EXPECT_EQ(ActualRAM, RAM);
    }
  }
#else
  GTEST_SKIP() << OracleUnavailable;
#endif
}
class X64FPContext : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto B = createExecutionBackend(GetParam().Backend,
                                    GetParam().UserMode
                                        ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64,
                                    Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(GetParam().Backend, GuestArchitecture::X64))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(CPU->map(Code, memory::PageSize,
                            Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->write(Code, Nop));
  }
};
TEST_P(X64FPContext, CapturesFullPhysicalStateAndRestoresAfterExecution) {
  const auto S = seed(EmptySlot);
  for (unsigned Top = 0; Top < x64::fp::RegisterCount; ++Top) {
    const auto R =
        static_cast<CPURegister>(unsigned(CPURegister::X64FP0) + Top);
    llvm::cantFail(CPU->writeRegister(R, S.FP.Registers[Top]));
  }
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  llvm::cantFail(CPU->setReg(X64Register::Name, S.FP.Member));
#include "arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
  llvm::cantFail(CPU->setReg(X64Register::FPTag, S.FP.Tag));
  llvm::cantFail(CPU->setReg(X64Register::MXCSR, S.MXCSR));
  auto Saved = llvm::cantFail(CPU->saveContext());
  BackendHooks H;
  H.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC != Code)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::Name)), S.FP.Member);
#include "arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
  for (unsigned I = 0; I < S.FP.Registers.size(); ++I) {
    const auto R = static_cast<CPURegister>(unsigned(CPURegister::X64FP0) + I);
    EXPECT_EQ(llvm::cantFail(CPU->readRegister(R)), S.FP.Registers[I]);
    llvm::cantFail(CPU->writeRegister(R, {}));
  }
  llvm::cantFail(CPU->setReg(X64Register::FPSW, 0));
  llvm::cantFail(CPU->setReg(X64Register::FPTag, 0));
  llvm::cantFail(CPU->restoreContext(*Saved));
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FPSW)), S.FP.Status);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FPTag)), S.FP.Tag);
  for (unsigned I = 0; I < S.FP.Registers.size(); ++I) {
    const auto R = static_cast<CPURegister>(unsigned(CPURegister::X64FP0) + I);
    EXPECT_EQ(llvm::cantFail(CPU->readRegister(R)), S.FP.Registers[I]);
  }
}
TEST_P(X64FPContext, RejectsInvalidWidthsAndEncodingsWithoutTruncation) {
  for (auto R :
       {CPURegister::X64FPCW, CPURegister::X64FPSW, CPURegister::X64FPOP,
        CPURegister::X64FPIP, CPURegister::X64FPDP, CPURegister::X64FPTag,
        CPURegister::X64FP0}) {
    const auto Before = llvm::cantFail(CPU->readRegister(R));
    RegisterValue Bad = Before;
    Bad[1] = InvalidPadding;
    auto E = CPU->writeRegister(R, Bad);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(llvm::cantFail(CPU->readRegister(R)), Before);
  }
  for (auto [R, V] : {std::pair(CPURegister::X64FPCW, InvalidControl),
                      std::pair(CPURegister::X64FPCW, ReservedPrecisionControl),
                      std::pair(CPURegister::X64FPOP, InvalidOpcode),
                      std::pair(CPURegister::X64FPTag, InvalidTag)}) {
    const auto Before = llvm::cantFail(CPU->readRegister(R));
    auto E = CPU->writeRegister(R, {V, 0});
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(llvm::cantFail(CPU->readRegister(R)), Before);
  }
  auto E = CPU->reg(X64Register::FP0);
  EXPECT_FALSE(bool(E));
  llvm::consumeError(E.takeError());
  auto W = CPU->setReg(X64Register::FP0, 0);
  EXPECT_TRUE(bool(W));
  llvm::consumeError(std::move(W));
  EXPECT_FALSE(registerMatches(CPURegister::Invalid, GuestArchitecture::X64));
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64FPContext,
                         testing::ValuesIn(Parameters), parameterName);
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64FPTransport,
                         testing::ValuesIn(Parameters), parameterName);
} // namespace
} // namespace neverd::emulation
