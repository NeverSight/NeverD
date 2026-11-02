//===- WindowsSIMDExecutionTests.cpp - Real #XM through guest VEH/VCH -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64Machine.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessExceptions.h"

namespace neverd::emulation {
namespace {
namespace simd {
#define NEVERD_SIMD_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "x86_64/X64SIMDExceptionCases.def"
#undef NEVERD_SIMD_VALUE
} // namespace simd
#define NEVERD_WINDOWS_SIMD_EXECUTION_VALUE(Name, Value)                       \
  constexpr uint64_t Name = Value;
#define NEVERD_WINDOWS_SIMD_EXECUTION_CODE(Name, ...)                          \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "WindowsSIMDExecutionCases.def"
#undef NEVERD_WINDOWS_SIMD_EXECUTION_CODE
#undef NEVERD_WINDOWS_SIMD_EXECUTION_VALUE
struct Operation {
  const char *Name;
  uint32_t Status, Left, Right, FaultStatus, MaskedStatus, MaskedResult,
      RepairedResult;
  std::array<uint8_t, simd::FourBytes> Bytes;
};
constexpr Operation Operations[] = {
// These original instruction results predate the production dispatcher.
#define NEVERD_SIMD_CASE(Name, Status, Left, Right, FaultStatus, MaskedStatus, \
                         MaskedResult, RepairedResult, ...)                    \
  {#Name,        Status,       Left,         Right,                            \
   FaultStatus,  MaskedStatus, MaskedResult, simd::RepairedResult,             \
   {__VA_ARGS__}},
#include "x86_64/X64SIMDExceptionCases.def"
#undef NEVERD_SIMD_CASE
};
constexpr uint32_t StatusCodes[] = {
#define NEVERD_WINDOWS_SIMD_STATUS(Active, MXCSR, Code) Code,
#include "fixtures/WindowsSIMDStatusCases.def"
#undef NEVERD_WINDOWS_SIMD_STATUS
};
struct Parameter {
  ExecutionBackendKind Kind;
};
void PrintTo(const Parameter &P, std::ostream *OS) {
  *OS << executionBackendName(P.Kind);
}
class WindowsSIMDExecution : public testing::TestWithParam<Parameter> {};

TEST_P(WindowsSIMDExecution, ActualFaultExecutesGuestHandlersAndRetries) {
  auto Created = createExecutionBackend(
      GetParam().Kind, ExecutionContract::CheckedUserX64, simd::Limit);
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
  const auto Top = windows_process::value::StackTop;
  const auto Base = Top - StackPages * simd::Page;
  const auto Gate = windows_process::value::ExceptionReturnGate;
  llvm::cantFail(
      CPU.map(Code, simd::Page, Read | Write | Execute | UserAccessible));
  llvm::cantFail(
      CPU.map(simd::Data, simd::Page, Read | Write | UserAccessible));
  llvm::cantFail(CPU.map(Base, Top - Base, Read | Write | UserAccessible));
  llvm::cantFail(CPU.map(Gate & ~(simd::Page - 1), simd::Page,
                         Read | Write | Execute | UserAccessible));
  const uint8_t Nop = simd::Nop;
  llvm::cantFail(CPU.write(Code + simd::FourBytes, {&Nop, 1}));
  llvm::cantFail(CPU.write(Gate, {&Nop, 1}));
  llvm::cantFail(CPU.write(ContinueHandler, Return));
  const auto ABI =
      llvm::cantFail(IntegerABI::get(IntegerCallingConvention::Win64));
  using Dispatcher = windows_process::ExceptionDispatcher;
  Dispatcher Dispatch(CPU, ABI, Base);
  llvm::cantFail(
      Dispatch.add(Dispatcher::HandlerKind::Exception, true, Handler));
  llvm::cantFail(
      Dispatch.add(Dispatcher::HandlerKind::Continue, true, ContinueHandler));
  unsigned Faults = 0;
  BackendHooks Hooks;
  Hooks.RecoverableFault = [&](const BackendFault &F) {
    ++Faults;
    return Dispatch.accepts(F);
  };
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC == Gate || PC == Code + simd::FourBytes)
      CPU.stop();
  };
  llvm::cantFail(CPU.installHooks(Hooks));
  for (const auto &O : Operations)
    for (bool Memory : {false, true})
      for (auto Sticky : {uint64_t(0), simd::Sticky})
        for (unsigned Mode = 0; Mode < ModeCount; ++Mode) {
          SCOPED_TRACE(O.Name);
          SCOPED_TRACE(Memory);
          SCOPED_TRACE(Sticky);
          SCOPED_TRACE(Mode);
          Faults = 0;
          auto Instruction = O.Bytes;
          if (Memory)
            Instruction.back() = simd::MemoryOperand;
          llvm::cantFail(CPU.write(Code, Instruction));
          std::vector<uint8_t> Body(std::begin(Context), std::end(Context));
          const llvm::ArrayRef<uint8_t> Edit =
              Mode == ModeSkip   ? llvm::ArrayRef(Skip)
              : Mode == ModeMask ? llvm::ArrayRef(Mask)
                                 : llvm::ArrayRef(Repair);
          Body.insert(Body.end(), Edit.begin(), Edit.end());
          Body.insert(Body.end(), std::begin(Return), std::end(Return));
          llvm::cantFail(CPU.write(Handler, Body));
          const auto Control =
              (simd::DefaultMXCSR & ~(O.Status << simd::MaskShift)) | Sticky;
          const auto FaultControl = Control | O.FaultStatus;
          llvm::cantFail(CPU.setReg(X64Register::MXCSR, Control));
          llvm::cantFail(CPU.setReg(X64Register::FPCW, SeedFPCW));
          llvm::cantFail(CPU.setReg(X64Register::FPSW, SeedFPSW));
          llvm::cantFail(CPU.setReg(X64Register::SP, Top - StackAlignment));
          llvm::cantFail(CPU.setReg(X64Register::CX, simd::Data));
          llvm::cantFail(CPU.setReg(X64Register::R8, simd::Data));
          llvm::cantFail(CPU.setReg(X64Register::FLAGS, simd::Flags));
          for (unsigned N = 0; N < x64::XmmCount; ++N)
            llvm::cantFail(
                CPU.setXmm(N, {simd::Sentinel + N, simd::Sentinel - N}));
          const uint64_t Upper = simd::Sentinel & ~uint64_t(UINT32_MAX);
          llvm::cantFail(CPU.setXmm(0, {Upper | O.Left, simd::Sentinel}));
          llvm::cantFail(CPU.setXmm(1, {Upper | O.Right, simd::Sentinel}));
          llvm::cantFail(
              CPU.writeInteger(simd::Data, O.Right, sizeof(uint32_t)));
          auto Exit = llvm::cantFail(CPU.runUntilExit(Code, simd::Timeout));
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
              << Exit.Diagnostic;
          auto Fault = CPU.takeRecoverableFault();
          ASSERT_TRUE(Fault);
          ASSERT_EQ(Fault->Interrupt, simd::Vector);
          ASSERT_EQ(llvm::cantFail(CPU.reg(X64Register::MXCSR)), FaultControl);
          auto Transfer = llvm::cantFail(
              Dispatch.beginFault(*Fault, Top - StackAlignment, 0));
          const auto Pointers = llvm::cantFail(CPU.reg(X64Register::CX));
          const auto Record =
              llvm::cantFail(CPU.readInteger(Pointers, sizeof(uint64_t)));
          const auto Saved = llvm::cantFail(
              CPU.readInteger(Pointers + sizeof(uint64_t), sizeof(uint64_t)));
          const auto Active =
              FaultControl & ~(FaultControl >> simd::MaskShift) & simd::Sticky;
          ASSERT_NE(Active, 0u);
          EXPECT_EQ(llvm::cantFail(CPU.readInteger(Record, sizeof(uint32_t))),
                    StatusCodes[Active - 1]);
          EXPECT_EQ(llvm::cantFail(CPU.readInteger(
                        Record + RecordArgumentsOffset + sizeof(uint64_t),
                        sizeof(uint64_t))),
                    FaultControl);
          // The native Windows assembly observations distinguish live handler
          // controls from the mutable saved CONTEXT. Both callbacks retain the
          // original controls even after the VEH repairs the context masks.
          for (auto PC : {Handler, ContinueHandler}) {
            ASSERT_EQ(Transfer.PC, PC);
            EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::MXCSR)),
                      FaultControl);
            EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FPCW)), SeedFPCW);
            EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FPSW)), SeedFPSW);
            Exit = llvm::cantFail(CPU.runUntilExit(Transfer.PC, simd::Timeout));
            ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
            ASSERT_TRUE(Dispatch.returning(
                Gate, llvm::cantFail(CPU.reg(X64Register::SP)), 0));
            Transfer = llvm::cantFail(
                Dispatch.returned(llvm::cantFail(CPU.reg(X64Register::AX))));
          }
          const auto ResultControl = Mode == ModeMask
                                         ? FaultControl | simd::DefaultMXCSR
                                         : FaultControl;
          for (auto Offset : {ContextMXCSROffset, ContextFXMXCSROffset})
            EXPECT_EQ(llvm::cantFail(
                          CPU.readInteger(Saved + Offset, sizeof(uint32_t))),
                      ResultControl);
          EXPECT_EQ(Transfer.PC,
                    Mode == ModeSkip ? Code + simd::FourBytes : Code);
          EXPECT_FALSE(Dispatch.activeAt(0));
          Exit = llvm::cantFail(CPU.runUntilExit(Transfer.PC, simd::Timeout));
          ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
          EXPECT_EQ(Faults, 1u);
          EXPECT_EQ(
              llvm::cantFail(CPU.xmm(0)),
              (RegisterValue{Upper | (Mode == ModeSkip   ? O.Left
                                      : Mode == ModeMask ? O.MaskedResult
                                                         : O.RepairedResult),
                             simd::Sentinel}));
          EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::MXCSR)),
                    ResultControl | (Mode == ModeMask ? O.MaskedStatus : 0));
          EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FLAGS)), simd::Flags);
          EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::SP)),
                    Top - StackAlignment);
        }
}
INSTANTIATE_TEST_SUITE_P(Native, WindowsSIMDExecution,
                         testing::Values(Parameter{ExecutionBackendKind::KVM},
                                         Parameter{ExecutionBackendKind::WHP}),
                         [](const auto &Info) {
                           return executionBackendName(Info.param.Kind);
                         });
} // namespace
} // namespace neverd::emulation
