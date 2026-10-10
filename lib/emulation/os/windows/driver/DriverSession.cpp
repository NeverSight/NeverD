//===- DriverSession.cpp - Bounded WDM driver execution -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded WDM driver execution.
///
//===----------------------------------------------------------------------===//

#include "neverd/emulation/DriverSession.h"

#include "../../../arch/x86_64/X64Exception.h"
#include "../../../core/ExecutionDiagnostics.h"
#include "../../../runtime/RuntimeValues.h"
#include "../exception/X64SEH.h"
#include "../exception/X64SIMDException.h"
#include "../kernel/KernelException.h"
#include "../kernel/KernelExportRegistry.h"
#include "../kernel/KernelModel.h"
#include "../kernel/WindowsKernelLayout.h"
#include "DriverImage.h"
#include "DriverObservation.h"
#include "DriverScenario.h"
#include "GuardControlFlow.h"
#include "WindowsX64ExecutionPolicy.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ExecutionBudget.h"
#include "neverd/emulation/IntegerABI.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <memory>

namespace neverd::emulation {
namespace {
namespace session_diagnostic {
#define NEVERD_DRIVER_SESSION_DIAGNOSTIC(Name, Text)                           \
  constexpr char Name[] = Text;
#include "DriverSessionDiagnostics.def"
#undef NEVERD_DRIVER_SESSION_DIAGNOSTIC
} // namespace session_diagnostic
using namespace profile;
constexpr uint64_t ReturnSentinel = ThunkBase + ThunkSize - ThunkStride;

llvm::Error failure(const std::string &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}

std::string guestCallPhase(const GuestCallToken &Token) {
  const char *Prefix = InvalidCallbackPhase;
  switch (Token.Owner) {
  case GuestCallOwner::Framework:
    Prefix = FrameworkCallbackPhase;
    break;
  case GuestCallOwner::WDM:
    Prefix = WDMCallbackPhase;
    break;
  case GuestCallOwner::PoFx:
    Prefix = PoFxCallbackPhase;
    break;
  case GuestCallOwner::DMA:
    Prefix = DMACallbackPhase;
    break;
  case GuestCallOwner::Interrupt:
    Prefix = InterruptCallbackPhase;
    break;
  case GuestCallOwner::UsbIdle:
    Prefix = UsbIdleCallbackPhase;
    break;
  }
  return std::string(Prefix) + std::to_string(Token.ID);
}

} // namespace

static llvm::Expected<DriverResult> runDriver(const std::filesystem::path &Path,
                                              const DriverOptions &Options,
                                              ProcessObserver *Observer) {
  if (Options.Contract != ExecutionContract::Legacy &&
      Options.Contract != ExecutionContract::CheckedX64)
    return diagnostic::error(diagnostic::Contract);
  if (!Options.InstructionLimit || !Options.MemoryLimit ||
      !Options.EventLimit || !Options.TimeoutMilliseconds ||
      Options.TimeoutMilliseconds > MaxTimeoutMilliseconds ||
      Options.MemoryLimit > MaxMemoryLimit ||
      Options.EventLimit > MaxEventLimit)
    return failure(llvm::formatv(session_diagnostic::Limits, MaxMemoryLimit,
                                 MaxEventLimit, MaxTimeoutMilliseconds)
                       .str());
  if (auto E = validateDriverScenario(Options))
    return std::move(E);
  KernelExportRegistry Exports;
  if (auto E = Exports.initialize(Options))
    return std::move(E);
  auto Image = loadDriverImage(Path, Options.MemoryLimit, Options.LoadAddress);
  if (!Image)
    return Image.takeError();
  DriverResult Result;
  Result.Configuration = Options;
  Result.ImageBase = Image->Base;
  Result.PreferredImageBase = Image->PreferredBase;
  Result.SecurityCookieAddress = Image->SecurityCookieAddress;
  Result.Entry = Image->Entry;
  Result.PC = Image->Entry;
  if (Image->Imports.size() > (ThunkSize / ThunkStride) - 1)
    return failure(session_diagnostic::ImportLimit);
  auto Backend = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
  if (!Backend)
    return Backend.takeError();
  Result.SelectedBackend = Backend->Kind;
  Result.BackendSelectionReason = Backend->Reason;
  auto &CPU = *Backend->CPU;
  const auto ABI =
      llvm::cantFail(IntegerABI::get(IntegerCallingConvention::Win64));
  GuardControlFlow Guard(*Image);
  // Temporary writable image pages are private setup state. Final permissions
  // are applied before any guest instruction can run.
  for (const auto &Region : Image->Regions) {
    if (auto E = CPU.map(Region.Address, Region.Bytes.size(), Read | Write))
      return std::move(E);
    if (auto E = CPU.write(Region.Address, Region.Bytes))
      return std::move(E);
  }
  for (const auto &Import : Image->Imports) {
    auto Address = Exports.bindImport(Import);
    if (!Address)
      return Address.takeError();
    if (auto E = CPU.writeInteger(Import.Slot, *Address, PointerSize))
      return std::move(E);
  }
  if (Image->Guard.Enabled) {
    if (auto E = CPU.writeInteger(Image->Guard.CheckPointerAddress,
                                  GuardCheckThunk, PointerSize))
      return std::move(E);
    if (Image->Guard.DispatchPointerAddress)
      if (auto E = CPU.writeInteger(Image->Guard.DispatchPointerAddress,
                                    GuardDispatchThunk, PointerSize))
        return std::move(E);
    if (auto E = CPU.map(GuardThunkBase, PageSize, Read | Execute))
      return std::move(E);
  }
  for (const auto &Region : Image->Regions)
    if (auto E = CPU.protect(Region.Address, Region.Bytes.size(),
                             Region.Permissions))
      return std::move(E);
  if (auto E = CPU.map(ThunkBase, ThunkSize, Read | Write))
    return std::move(E);
  std::vector<uint8_t> TrapBytes(ThunkSize, ThunkTrapByte);
  if (auto E = CPU.write(ThunkBase, TrapBytes))
    return std::move(E);
  if (auto E = CPU.protect(ThunkBase, ThunkSize, Read | Execute))
    return std::move(E);
  if (auto E = CPU.map(StackBase, StackSize, Read | Write))
    return std::move(E);
  KernelModel Kernel(CPU, Result, &Exports);
  if (auto E = Kernel.initialize(*Image, Options))
    return std::move(E);
  DriverObservation Observation(Observer, CPU, *Image, Kernel, Exports, Result,
                                Path.filename().string());
  bool PendingObservation = false, PendingMemoryWrite = false;
  std::optional<std::string> ObserverFailure;
  auto ObservationFailed = [&](llvm::Error Error) {
    ObserverFailure = llvm::toString(std::move(Error));
    return failure(*ObserverFailure);
  };
  X64SEH Exceptions(
      Image->Exceptions, Image->PreferredBase, Image->Base, Image->Size,
      [&](uint64_t Address) -> llvm::Expected<uint64_t> {
        if (auto E = Kernel.validateGuestAccess(Address, PointerSize, false))
          return std::move(E);
        return CPU.readInteger(Address, PointerSize);
      },
      [&](uint64_t Address) { return CPU.executable(Address); },
      [&](uint64_t Address, llvm::MutableArrayRef<uint8_t> Bytes) {
        return CPU.fetch(Address, Bytes);
      },
      [&]() -> llvm::Expected<uint64_t> {
        if (!Image->SecurityCookieAddress)
          return llvm::createStringError(
              llvm::inconvertibleErrorCode(),
              session_diagnostic::MissingSecurityCookie);
        if (auto E = Kernel.validateGuestAccess(Image->SecurityCookieAddress,
                                                PointerSize, false))
          return std::move(E);
        return CPU.readInteger(Image->SecurityCookieAddress, PointerSize);
      });
  uint64_t ExpectedReturnSP = 0;
  uint64_t ActiveStackBase = 0;
  uint64_t ActiveStackSize = 0;
  WindowsX64ExecutionPolicy Policy;
  const auto BranchModel = CPU.x64BranchModel();
  if (!BranchModel)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   diagnostic::BranchModel);
  if (auto E = Policy.initialize(*BranchModel))
    return std::move(E);
  bool Stopped = false;
  std::optional<uint64_t> InvocationReturn;
  const KernelExportRegistry::Export *Pending = nullptr;
  uint64_t PendingGuard = 0;
  struct EnvironmentRead {
    WindowsX64ExecutionPolicy::Action Request;
    uint64_t NextPC;
  };
  std::optional<EnvironmentRead> PendingEnvironmentRead;
  bool ProcessorViewMapped = false;
  bool ProcessorReadAdmitted = false;
  auto RefreshProcessorView = [&]() -> llvm::Error {
    auto Thread = Kernel.currentThreadObject();
    if (!Thread)
      return Thread.takeError();
    std::array<uint8_t, PointerSize> Bytes;
    llvm::support::endian::write64le(Bytes.data(), *Thread);
    if (auto E = CPU.writeBacking(
            ProcessorEnvironmentBase + windows::GSCurrentThreadOffset, Bytes))
      return E;
    return CPU.setGSBase(ProcessorEnvironmentBase);
  };
  auto PrepareProcessorView = [&]() -> llvm::Error {
    if (auto E = CPU.map(ProcessorEnvironmentBase, PageSize, Read))
      return E;
    ProcessorViewMapped = true;
    return RefreshProcessorView();
  };
  auto Budget = ExecutionBudget::create(
      {Options.InstructionLimit, Options.EventLimit,
       Options.TimeoutMilliseconds * runtime::MicrosecondsPerMillisecond});
  if (!Budget)
    return Budget.takeError();
  auto &Resources = **Budget;
  auto Stop = [&](DriverStopReason Reason, const std::string &Diagnostic) {
    if (Stopped)
      return;
    Result.Stop = Reason;
    Result.Diagnostic = Diagnostic;
    Stopped = true;
    CPU.stop();
  };
  auto EventAvailable = [&]() {
    if (!Resources.hasEvents()) {
      Stop(DriverStopReason::EventLimit, runtime::EventLimit);
      return false;
    }
    return true;
  };
  auto RecordEvent = [&]() {
    if (!Resources.consumeEvents()) {
      Stop(DriverStopReason::EventLimit, runtime::EventLimit);
      return false;
    }
    return true;
  };
  uint64_t RunInstructions = 0, RunAllowance = UINT64_MAX;
  bool AdmissionBoundary = false;
  auto RecordInstruction = [&]() {
    if (!Resources.consumeInstructions()) {
      Stop(DriverStopReason::InstructionLimit, runtime::InstructionLimit);
      return false;
    }
    Result.Instructions = Resources.instructions();
    Observation.instructionAdmitted();
    ++RunInstructions;
    return true;
  };
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t Address, uint32_t Size) {
    ProcessorReadAdmitted = false;
    if (Stopped)
      return;
    Result.PC = Address;
    if (Observation.matches(Address)) {
      PendingObservation = true;
      CPU.stop();
      return;
    }
    auto StackPointer = CPU.reg(X64Register::SP);
    if (!StackPointer) {
      Stop(DriverStopReason::EngineError,
           llvm::toString(StackPointer.takeError()));
      return;
    }
    if (*StackPointer < ActiveStackBase ||
        *StackPointer >= ActiveStackBase + ActiveStackSize) {
      Stop(DriverStopReason::ModelError, session_diagnostic::StackRange);
      return;
    }
    if (Address == ReturnSentinel) {
      auto SP = CPU.reg(X64Register::SP);
      auto AX = CPU.reg(X64Register::AX);
      if (!SP || !AX) {
        std::string Error;
        if (!SP)
          Error += llvm::toString(SP.takeError());
        if (!AX)
          Error += llvm::toString(AX.takeError());
        Stop(DriverStopReason::EngineError, Error);
      } else if (*SP != ExpectedReturnSP) {
        Stop(DriverStopReason::ModelError, session_diagnostic::CallbackStack);
      } else {
        InvocationReturn = *AX;
        Stop(DriverStopReason::Returned, "");
      }
      return;
    }
    if (Options.Scheduling && RunInstructions >= RunAllowance) {
      AdmissionBoundary = true;
      CPU.stop();
      return;
    }
    if (Image->Guard.Enabled &&
        (Address == GuardCheckThunk || Address == GuardDispatchThunk)) {
      PendingGuard = Address;
      CPU.stop();
      return;
    }
    if (const auto *Export = Exports.lookup(Address)) {
      Pending = Export;
      CPU.stop();
      return;
    }
    if (!Resources.hasInstructions()) {
      Stop(DriverStopReason::InstructionLimit, runtime::InstructionLimit);
      return;
    }
    if (!Size || Size > MaxInstructionSize) {
      Stop(DriverStopReason::UnsupportedInstruction,
           session_diagnostic::InstructionExtent);
      return;
    }
    if (auto E = Kernel.validateGuestAccess(Address, Size, false)) {
      Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
      return;
    }
    std::array<uint8_t, MaxInstructionSize> Bytes{};
    if (auto E = CPU.fetch(
            Address, llvm::MutableArrayRef<uint8_t>(Bytes.data(), Size))) {
      Stop(DriverStopReason::MemoryFault, llvm::toString(std::move(E)));
      return;
    }
    auto Inspection =
        Policy.inspect(llvm::ArrayRef<uint8_t>(Bytes.data(), Size), Address);
    if (!Inspection) {
      Stop(DriverStopReason::UnsupportedInstruction,
           llvm::toString(Inspection.takeError()));
      return;
    }
    if (*Inspection &&
        (**Inspection).Source ==
            WindowsX64ExecutionPolicy::Action::Kind::ReadCurrentThread) {
      if (!ProcessorViewMapped) {
        // Materialize the processor field outside the running backend, then
        // retry this instruction once. It has not consumed its budget yet.
        PendingEnvironmentRead = EnvironmentRead{**Inspection, Address};
        CPU.stop();
        return;
      }
      ProcessorReadAdmitted = true;
      RecordInstruction();
      return;
    }
    if (!RecordInstruction())
      return;
    if (*Inspection) {
      if (Size > UINT64_MAX - Address) {
        Stop(DriverStopReason::MemoryFault,
             session_diagnostic::EnvironmentReadRange);
        return;
      }
      PendingEnvironmentRead = EnvironmentRead{**Inspection, Address + Size};
      CPU.stop();
    }
  };
  Hooks.Read = [&](uint64_t Address, uint32_t Size) {
    if (Stopped)
      return;
    if (ProcessorReadAdmitted && Size == PointerSize &&
        Address == ProcessorEnvironmentBase + windows::GSCurrentThreadOffset)
      return;
    if (KernelExportRegistry::overlapsThunk(Address, Size)) {
      Stop(DriverStopReason::UnsupportedAPI, session_diagnostic::ImportRead);
      return;
    }
    if (auto E = Kernel.validateGuestAccess(Address, Size, false))
      Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
  };
  Hooks.MemoryWritten = [&]() { PendingMemoryWrite = true; };
  Hooks.Write = [&](uint64_t Address, uint32_t Size, uint64_t Value) {
    if (Stopped)
      return;
    if (auto E = Kernel.validateGuestAccess(Address, Size, true)) {
      Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
      return;
    }
    if (Stopped || ((Address >= StackBase && Address < StackBase + StackSize) ||
                    (Address >= CallbackStackBase &&
                     Address < CallbackStackBase + MaxConcurrentCallbacks *
                                                       CallbackStackStride)))
      return;
    if (!EventAvailable())
      return;
    DriverMemoryWrite Event;
    Event.PC = Result.PC;
    Event.Phase = Result.Phase;
    Event.Address = Address;
    Event.Size = Size;
    if (Size && Size <= PointerSize)
      Event.Value = Size == PointerSize
                        ? Value
                        : Value & ((uint64_t(1) << (Size * 8)) - 1);
    if (RecordEvent())
      Result.Writes.push_back(std::move(Event));
  };
  Hooks.Fault = [&](uint64_t Address, uint32_t Size, const char *Access) {
    Stop(DriverStopReason::MemoryFault,
         std::string(session_diagnostic::GuestFaultPrefix) + Access +
             session_diagnostic::GuestFaultAddress + llvm::utohexstr(Address) +
             " (" + std::to_string(Size) +
             session_diagnostic::GuestFaultSizeSuffix);
  };
  Hooks.RecoverableFault = [&](const BackendFault &Fault) {
    if (!Stopped && Fault.Kind == BackendFaultKind::Interrupt &&
        Fault.Interrupt &&
        (exceptions::kernelX64ExceptionStatus(*Fault.Interrupt) ||
         (CPU.supportsSIMDExceptions() &&
          *Fault.Interrupt == unsigned(x64::ExceptionVector::SIMD))))
      return true;
    return !Stopped && Fault.Address && Fault.Size && Fault.Access &&
           (*Fault.Access == BackendAccessKind::Read ||
            *Fault.Access == BackendAccessKind::Write) &&
           Kernel.canCatchUserAccess(*Fault.Address, *Fault.Size);
  };
  Hooks.Interrupt = [&](uint32_t Number) {
    Stop(DriverStopReason::UnsupportedInstruction,
         session_diagnostic::CPUException + std::to_string(Number));
  };
  Hooks.InvalidInstruction = [&]() {
    auto PC = CPU.reg(X64Register::PC);
    if (PC)
      Result.PC = *PC;
    else
      llvm::consumeError(PC.takeError());
    Stop(DriverStopReason::UnsupportedInstruction,
         session_diagnostic::RejectedInstruction);
  };
  if (auto E = CPU.installHooks(std::move(Hooks)))
    return std::move(E);

  auto DeadlineExceeded = [&]() {
    if (Resources.remainingMicroseconds())
      return false;
    Stopped = false;
    Stop(DriverStopReason::Timeout, runtime::Timeout);
    return true;
  };
  auto ModelFailure = [&](llvm::Error E) {
    Stopped = false;
    Stop(CPU.hasMemoryFault() ? DriverStopReason::MemoryFault
                              : DriverStopReason::ModelError,
         llvm::toString(std::move(E)));
  };
  struct ExceptionExecution {
    X64SEH::Dispatch Dispatch;
    X64SEH::Exception Raised;
    X64SEH::Action Next;
    std::unique_ptr<BackendContext> Original;
    bool CanContinue = false;
  };
  struct TimeSlice {
    uint64_t Remaining;
    uint64_t ReadyOrder = 0;
  };
  struct Execution {
    uint64_t ID = 0; // Zero denotes the foreground driver invocation.
    uint64_t Base = 0;
    uint64_t Size = 0;
    uint64_t InitialSP = 0;
    uint64_t PC = 0;
    uint8_t EntryIRQL = 0;
    uint64_t ThreadKey = 0;
    std::shared_ptr<TimeSlice> Slice;
    std::optional<KernelModel::ExecutionContext> PausedModel;
    bool Boundary = false;
    std::string Phase;
    std::unique_ptr<BackendContext> Context;
    std::optional<KernelModel::Wait> Wait;
    std::optional<uint64_t> ResumeValue;
    std::optional<uint8_t> ResumeIRQL;
    size_t WaitEvent = 0;
    bool PrivateStack = false;
    GuestCallToken ReturnToken;
    std::optional<GuestCallToken> PendingEntry;
    uint64_t SynchronizationObject = 0;
    bool SynchronizationEntered = false;
    bool CallbackEntered = false;
    bool ObservationEntered = false;
    bool PowerManagedCallback = false;
    uint64_t RequestIRP = 0;
    std::optional<KernelGuestCall> ChildCall;
    bool ThreadTerminated = false;
    std::unique_ptr<ExceptionExecution> Exception;
    std::optional<X64SEH::ActionKind> ExceptionCallback;
    std::unique_ptr<Execution> Parent;
  };
  std::vector<std::unique_ptr<Execution>> Waiting;
  const Execution *ObservedFrame = nullptr;
  struct ClockCall {
    KernelGuestCall Call;
    std::optional<KernelModel::Wait> Wait;
    uint64_t ReadyOrder;
  };
  std::deque<ClockCall> ClockCalls;
  uint64_t AccountedInstructions = 0;
  auto NextExecutionDeadline = [&]() {
    auto Deadline = Kernel.nextEventTime();
    for (const auto &Frame : Waiting) {
      if (!Frame->Wait || !Frame->Wait->Deadline)
        continue;
      const auto Time = std::max(*Frame->Wait->Deadline, Kernel.now100ns());
      if (!Deadline || Time < *Deadline)
        Deadline = Time;
    }
    return Deadline;
  };
  std::array<bool, MaxConcurrentCallbacks> StackMapped{}, StackInUse{};
  auto NewExecution = [&](uint64_t PC, llvm::ArrayRef<uint64_t> Arguments,
                          const std::string &Phase, uint64_t ID,
                          bool Nested = false,
                          uint64_t PayloadSize =
                              0) -> llvm::Expected<std::unique_ptr<Execution>> {
    // A modeled bus cancel routine is an actual nested callback with a scoped
    // thunk identity. Its model validates the live cancel token, PDO and IRP;
    // ordinary exports remain invalid driver callback targets.
    const auto *Export = Nested ? Exports.lookup(PC) : nullptr;
    const bool ProviderCallback =
        Export &&
        Export->Kind == KernelExportRegistry::ExportKind::ProviderFunction;
    if (!CPU.executable(PC) ||
        (KernelExportRegistry::overlapsThunk(PC) && !ProviderCallback))
      return failure(session_diagnostic::CallbackCode);
    if (Arguments.size() > MaxCallbackArguments)
      return failure(session_diagnostic::CallbackArguments);
    auto Frame = std::make_unique<Execution>();
    Frame->ID = ID;
    Frame->PC = PC;
    Frame->EntryIRQL = Kernel.currentIRQL();
    Frame->Phase = Phase;
    Frame->Base = StackBase;
    Frame->Size = StackSize;
    Frame->PrivateStack = ID || Nested;
    if (Frame->PrivateStack) {
      auto Slot = std::find(StackInUse.begin(), StackInUse.end(), false);
      if (Slot == StackInUse.end())
        return failure(session_diagnostic::CallbackStackLimit);
      const size_t Index = Slot - StackInUse.begin();
      Frame->Base = CallbackStackBase + PageSize + Index * CallbackStackStride;
      Frame->Size = CallbackStackSize;
      if (!StackMapped[Index]) {
        if (auto E = CPU.map(Frame->Base, Frame->Size, Read | Write))
          return std::move(E);
        StackMapped[Index] = true;
      }
      StackInUse[Index] = true;
    }
    if (auto E = Kernel.activateStack(Frame->Base, Frame->Size))
      return std::move(E);
    auto Layout = ABI.prepareCall(CPU, Frame->Base, Frame->Size, ReturnSentinel,
                                  Arguments, PayloadSize);
    if (!Layout)
      return Layout.takeError();
    Frame->InitialSP = Layout->StackPointer;
    Frame->ThreadKey =
        ID ? ID : (Nested && Options.Scheduling ? Frame->Base : StackBase);
    if (Options.Scheduling) {
      Frame->Slice = std::make_shared<TimeSlice>(
          TimeSlice{Options.Scheduling->QuantumInstructions});
      auto Order = Kernel.issueReadyOrder();
      if (!Order)
        return Order.takeError();
      Frame->Slice->ReadyOrder = *Order;
    }
    if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
      return std::move(E);
    return Frame;
  };
  auto CaptureRegisters = [&](uint64_t PC,
                              bool CaptureSSE =
                                  false) -> llvm::Expected<X64SEH::Context> {
    X64SEH::Context Registers;
    static_assert(unsigned(X64Register::AX) == 0 &&
                  unsigned(X64Register::SP) == 4 &&
                  unsigned(X64Register::R15) + 1 == seh::RegisterCount);
    for (size_t I = 0; I < Registers.GPR.size(); ++I) {
      auto Value = CPU.reg(static_cast<X64Register>(I));
      if (!Value)
        return Value.takeError();
      Registers.GPR[I] = *Value;
    }
    for (size_t I = 0; I < Registers.Xmm.size(); ++I) {
      auto Value = CPU.xmm(seh::FirstNonvolatileXmm + I);
      if (!Value)
        return Value.takeError();
      Registers.Xmm[I] = *Value;
    }
    auto Flags = CPU.reg(X64Register::FLAGS);
    if (!Flags)
      return Flags.takeError();
    auto CS = CPU.reg(X64Register::CS);
    if (!CS)
      return CS.takeError();
    auto SS = CPU.reg(X64Register::SS);
    if (!SS)
      return SS.takeError();
    Registers.Flags = *Flags;
    Registers.CS = *CS;
    Registers.SS = *SS;
    Registers.PC = PC;
    if (CaptureSSE) {
      auto MXCSR = CPU.reg(X64Register::MXCSR);
      if (!MXCSR)
        return MXCSR.takeError();
      auto Mask = CPU.supportedControlBits(CPURegister::X64MXCSR);
      if (!Mask)
        return Mask.takeError();
      auto &SSE = Registers.SSE.emplace(X64SEH::SSEContext{});
      SSE.MXCSR = *MXCSR;
      SSE.MXCSRMask = (*Mask)[0];
      for (size_t I = 0; I < SSE.VolatileXmm.size(); ++I) {
        auto Value = CPU.xmm(I);
        if (!Value)
          return Value.takeError();
        SSE.VolatileXmm[I] = *Value;
      }
    }
    return Registers;
  };
  auto ApplyRegisters = [&](const X64SEH::Context &Registers) -> llvm::Error {
    if (Registers.SSE && !CPU.supportsSIMDExceptions() &&
        (Registers.SSE->MXCSR & exceptions::HandlerMXCSR) !=
            exceptions::HandlerMXCSR)
      return failure(exceptions::UnsupportedSIMDContinuation);
    for (size_t I = 0; I < Registers.GPR.size(); ++I)
      if (auto E = CPU.setReg(static_cast<X64Register>(I), Registers.GPR[I]))
        return E;
    for (size_t I = 0; I < Registers.Xmm.size(); ++I)
      if (auto E = CPU.setXmm(seh::FirstNonvolatileXmm + I, Registers.Xmm[I]))
        return E;
    if (const auto &SSE = Registers.SSE) {
      for (size_t I = 0; I < SSE->VolatileXmm.size(); ++I)
        if (auto E = CPU.setXmm(I, SSE->VolatileXmm[I]))
          return E;
      if (auto E = CPU.setReg(X64Register::MXCSR, SSE->MXCSR))
        return E;
    }
    return CPU.setReg(X64Register::FLAGS, Registers.Flags);
  };
  auto ApplyHandlerRegisters = [&](X64SEH::Context Registers) -> llvm::Error {
    if (Registers.SSE) {
      Registers.SSE->MXCSR = exceptions::HandlerMXCSR;
      Registers.Flags &= ~exceptions::DirectionFlag;
    }
    return ApplyRegisters(Registers);
  };
  auto BeginException =
      [&](Execution &Frame, X64SEH::Exception Raised, uint64_t ControlPC,
          bool CanContinue) -> llvm::Expected<std::optional<uint64_t>> {
    size_t NestedDepth = 0;
    for (const Execution *Ancestor = &Frame; Ancestor;
         Ancestor = Ancestor->Parent.get()) {
      if (!Ancestor->ExceptionCallback)
        continue;
      if (++NestedDepth > seh::MaxNestedExceptions)
        return failure(session_diagnostic::ExceptionDepth);
      if (!Raised.PreviousRecord)
        Raised.PreviousRecord =
            Ancestor->Base + Ancestor->Size - seh::RecordsSize;
    }
    auto Search = Raised.Registers;
    Search.PC = ControlPC;
    Search.FromReturnAddress = !CanContinue;
    auto State = std::make_unique<ExceptionExecution>();
    if (Frame.ExceptionCallback) {
      if (!Frame.Parent || !Frame.Parent->Exception)
        return failure(session_diagnostic::MissingSEHDispatch);
      const auto &Parent = *Frame.Parent->Exception;
      auto Nested =
          Exceptions.beginNested(Raised.Code, Search, {Frame.Base, Frame.Size},
                                 Parent.Dispatch, Parent.Next);
      if (!Nested)
        return Nested.takeError();
      State->Dispatch = std::move(*Nested);
    } else {
      State->Dispatch =
          Exceptions.begin(Raised.Code, Search, {Frame.Base, Frame.Size});
    }
    auto Next = Exceptions.advance(State->Dispatch);
    if (!Next)
      return Next.takeError();
    if (Next->Kind == X64SEH::ActionKind::Unhandled)
      return failure(session_diagnostic::UnhandledException +
                     llvm::utohexstr(Raised.Code));
    if (Next->Kind == X64SEH::ActionKind::Handler &&
        Next->Bounds.Base == Frame.Base) {
      if (auto E = ApplyHandlerRegisters(Next->State.Registers))
        return std::move(E);
      return std::optional<uint64_t>{Next->State.HandlerPC};
    }
    auto Context = CPU.saveContext();
    if (!Context)
      return Context.takeError();
    State->Original = std::move(*Context);
    State->Raised = std::move(Raised);
    State->Raised.Flags = Next->ExceptionFlags;
    State->Next = *Next;
    State->CanContinue = CanContinue;
    Frame.Exception = std::move(State);
    return std::optional<uint64_t>{};
  };
  auto RefreshWaiters = [&]() -> llvm::Error {
    for (auto &Frame : Waiting) {
      if (!Frame->Wait)
        continue;
      if (Frame->Wait->Type == KernelModel::Wait::Kind::PoFxActive ||
          Frame->Wait->Type == KernelModel::Wait::Kind::PoFxIdle) {
        if (auto Call = Kernel.takePoFxThreadCall(Frame->Wait->Thread)) {
          Frame->ChildCall = std::move(*Call);
          Frame->ResumeIRQL = Frame->Wait->IRQL;
          Frame->Wait.reset();
          if (Options.Scheduling) {
            auto Order = Kernel.issueReadyOrder();
            if (!Order)
              return Order.takeError();
            Frame->Slice->ReadyOrder = *Order;
          }
          continue;
        }
      }
      auto Status = Kernel.pollWait(*Frame->Wait);
      if (!Status)
        return Status.takeError();
      if (*Status) {
        if (Frame->Wait->Type !=
            KernelModel::Wait::Kind::InterruptSynchronization)
          Frame->ResumeIRQL = Frame->Wait->IRQL;
        const bool BeforeChild =
            Frame->Wait->Type ==
                KernelModel::Wait::Kind::InterruptSynchronization ||
            Frame->Wait->Type == KernelModel::Wait::Kind::FrameworkCallback;
        Frame->Wait.reset();
        if (Options.Scheduling) {
          auto Order = Kernel.issueReadyOrder();
          if (!Order)
            return Order.takeError();
          Frame->Slice->ReadyOrder = *Order;
        }
        if (!BeforeChild) {
          Frame->ResumeValue = **Status;
          Result.Calls[Frame->WaitEvent].Result = **Status;
        }
      }
    }
    return llvm::Error::success();
  };
  // The same checked projection supplies instruction environment reads and
  // the scheduler's next commit. Reading it must not consume the scheduler's
  // instruction accounting or give the current thread a fresh time slice.
  auto ExecutionTime = [&]() -> llvm::Expected<uint64_t> {
    if (!Options.Scheduling)
      return Kernel.now100ns();
    const uint64_t Count = Result.Instructions - AccountedInstructions;
    const uint64_t Unit = Options.Scheduling->InstructionTime100ns;
    if (Count > (UINT64_MAX - Kernel.now100ns()) / Unit)
      return failure(driver_scheduling::ClockOverflow);
    return Kernel.now100ns() + Count * Unit;
  };
  auto AdvanceClock = [&]() -> llvm::Error {
    if (!Options.Scheduling)
      return llvm::Error::success();
    auto Time = ExecutionTime();
    if (!Time)
      return Time.takeError();
    const uint64_t End = *Time;
    for (;;) {
      const auto Deadline = NextExecutionDeadline();
      const uint64_t Boundary = Deadline ? std::min(*Deadline, End) : End;
      if (auto E = Kernel.advanceExecutionTo100ns(Boundary))
        return E;
      // Time producers are independent invocations, never inline children of
      // the thread whose instructions advanced the clock.
      if (auto Call = Kernel.takeIndependentGuestCall()) {
        auto Order = Kernel.issueReadyOrder();
        if (!Order)
          return Order.takeError();
        ClockCalls.push_back({std::move(*Call), Kernel.takeWait(), *Order});
      }
      // Complete each wait at its deadline, even when the running thread
      // keeps the remainder of its quantum or one attempt spans more events.
      if (auto E = RefreshWaiters())
        return E;
      if (Boundary == End)
        break;
      if (auto Next = NextExecutionDeadline(); Next && *Next <= Boundary)
        return failure(driver_scheduling::StalledClock);
    }
    AccountedInstructions = Result.Instructions;
    return llvm::Error::success();
  };
  auto NextReadyThread = [&]() {
    auto Best = Kernel.nextPassiveThread();
    auto Consider = [&](KernelScheduler::ReadyThread Candidate) {
      if (!Best || Candidate.precedes(*Best))
        Best = Candidate;
    };
    if (!ClockCalls.empty())
      Consider(Kernel.readyThread(0, ClockCalls.front().ReadyOrder));
    for (const auto &Frame : Waiting)
      if (!Frame->Wait)
        Consider(
            Kernel.readyThread(Frame->ThreadKey, Frame->Slice->ReadyOrder));
    return Best;
  };
  auto HasHigherReadyThread = [&](const Execution &Frame) {
    const auto Best = NextReadyThread();
    return Best && Best->Priority > Kernel.threadPriority(Frame.ThreadKey);
  };
  auto RunExecution = [&](Execution &Frame) -> llvm::Error {
    const Execution *Owner = &Frame;
    while (Owner->ExceptionCallback && Owner->Parent)
      Owner = Owner->Parent.get();
    Kernel.enterExecution(Frame.Base, Frame.ThreadKey,
                          Owner->ReturnToken.ID
                              ? Owner->ReturnToken
                              : Kernel.scheduledGuestCall(Owner->ID));
    if (Frame.Context) {
      if (auto E = CPU.restoreContext(*Frame.Context))
        return E;
      if (Frame.ResumeValue) {
        if (auto E = CPU.setReg(X64Register::AX, *Frame.ResumeValue))
          return E;
        Frame.ResumeValue.reset();
      }
    }
    auto SaveBoundary = [&](uint64_t PC) -> llvm::Error {
      auto Context = CPU.saveContext();
      if (!Context)
        return Context.takeError();
      Frame.Context = std::move(*Context);
      Frame.PC = PC;
      Frame.Boundary = true;
      return llvm::Error::success();
    };
    // Idle advancement can select a fresh callback at the same deadline that
    // wakes a higher-priority thread. Recheck before callback synchronization
    // acquires a lock or raises IRQL, even before the first guest instruction.
    if (Options.Scheduling && Kernel.currentIRQL() < scheduler::DispatchLevel &&
        HasHigherReadyThread(Frame))
      return SaveBoundary(Frame.PC);
    if (Frame.SynchronizationObject && !Frame.SynchronizationEntered) {
      auto Entered = Kernel.beginFrameworkCallback(Frame.SynchronizationObject);
      if (!Entered)
        return Entered.takeError();
      if (!*Entered) {
        Frame.Wait = Kernel.takeWait();
        if (!Frame.Wait ||
            Frame.Wait->Type != KernelModel::Wait::Kind::FrameworkCallback)
          return failure(session_diagnostic::MissingFrameworkLockWait);
        auto Context = CPU.saveContext();
        if (!Context)
          return Context.takeError();
        Frame.Context = std::move(*Context);
        if (Options.Scheduling)
          Frame.PausedModel = Kernel.captureExecutionContext();
        if (Frame.ID)
          if (auto E = Kernel.suspendScheduled(Frame.ID))
            return E;
        return llvm::Error::success();
      }
      Frame.SynchronizationEntered = true;
      if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
        return E;
    }
    if (!Frame.CallbackEntered) {
      if (!Frame.ExceptionCallback) {
        auto Managed = Kernel.enterFrameworkCallback(
            Frame.ID, Frame.ReturnToken, Frame.RequestIRP);
        if (!Managed)
          return Managed.takeError();
        Frame.PowerManagedCallback |= *Managed;
      }
      Frame.CallbackEntered = true;
    }
    Kernel.setFrameworkCallbackContext(Frame.PowerManagedCallback);
    auto IRQL = CPU.reg(X64Register::CR8);
    if (!IRQL)
      return IRQL.takeError();
    if (*IRQL != Kernel.currentIRQL())
      return failure(session_diagnostic::SavedIRQL);
    if (ProcessorViewMapped)
      if (auto E = RefreshProcessorView())
        return E;
    Result.Phase = Frame.Phase;
    Result.PC = Frame.PC;
    ExpectedReturnSP = Frame.InitialSP + PointerSize;
    ActiveStackBase = Frame.Base;
    ActiveStackSize = Frame.Size;
    Stopped = false;
    InvocationReturn.reset();
    Frame.Boundary = false;
    RunInstructions = 0;
    AdmissionBoundary = false;
    RunAllowance = UINT64_MAX;
    if (Options.Scheduling) {
      const bool CanPreempt = Kernel.currentIRQL() < scheduler::DispatchLevel;
      if (Frame.Slice->Remaining || CanPreempt)
        RunAllowance = Frame.Slice->Remaining;
      if (CanPreempt &&
          (Kernel.hasQueuedPriorityCallback() || HasHigherReadyThread(Frame)))
        RunAllowance = 0;
      auto Deadline = NextExecutionDeadline();
      if (Deadline) {
        const uint64_t Distance =
            *Deadline > Kernel.now100ns() ? *Deadline - Kernel.now100ns() : 0;
        const uint64_t Unit = Options.Scheduling->InstructionTime100ns;
        const uint64_t Attempts = Distance / Unit + (Distance % Unit != 0);
        RunAllowance = std::min(RunAllowance, std::max(uint64_t(1), Attempts));
      }
    }
    bool Ran = false;
    uint64_t NextPC = Frame.PC;
    if (Observer) {
      if (auto E = CPU.setReg(X64Register::PC, NextPC))
        return E;
      // A time slice of the same invocation does not start a new code
      // generation. Nested callbacks and restored callers do establish a
      // new invocation boundary, as in the process environment.
      if (!Frame.ObservationEntered || ObservedFrame != &Frame) {
        if (auto E = Observation.entered(Frame.Phase == EntryPhase &&
                                             !Frame.ID && !Frame.Parent &&
                                             !Frame.ExceptionCallback,
                                         {Frame.Base, Frame.Size}))
          return ObservationFailed(std::move(E));
        Frame.ObservationEntered = true;
        ObservedFrame = &Frame;
      }
    }
    while (!Stopped) {
      if (Options.Scheduling && Ran)
        return SaveBoundary(NextPC);
      if (Observer) {
        if (auto E = CPU.setReg(X64Register::PC, NextPC))
          return E;
        if (auto E = Observation.resuming())
          return ObservationFailed(std::move(E));
      }
      const uint64_t Remaining = Resources.remainingMicroseconds();
      if (!Remaining) {
        Stop(DriverStopReason::Timeout, runtime::Timeout);
        break;
      }
      Pending = nullptr;
      PendingGuard = 0;
      PendingEnvironmentRead.reset();
      PendingObservation = PendingMemoryWrite = false;
      if (auto E = CPU.run(NextPC, Remaining)) {
        std::string Message = llvm::toString(std::move(E));
        if (!Stopped)
          Stop(CPU.hasMemoryFault()   ? DriverStopReason::MemoryFault
               : CPU.hasDeviceError() ? DriverStopReason::ModelError
                                      : DriverStopReason::EngineError,
               Message);
      }
      if (Stopped)
        break;
      if (PendingObservation) {
        auto Resume = Observation.watched(Result.PC);
        if (!Resume)
          return ObservationFailed(Resume.takeError());
        if (!*Resume) {
          Stop(DriverStopReason::Observer, "");
          break;
        }
        NextPC = Result.PC;
        continue;
      }
      Ran = true;
      if (AdmissionBoundary)
        return SaveBoundary(Result.PC);
      // An import observes time after all preceding machine instructions.
      // Its implementation and any nested continuation remain indivisible.
      if (Options.Scheduling && Pending && RunInstructions)
        return SaveBoundary(Result.PC);
      if (auto Fault = CPU.takeRecoverableFault()) {
        auto Status =
            Fault->Kind == BackendFaultKind::Interrupt && Fault->Interrupt
                ? exceptions::kernelX64ExceptionStatus(*Fault->Interrupt)
                : std::nullopt;
        const bool SIMDFault =
            Fault->Kind == BackendFaultKind::Interrupt && Fault->Interrupt &&
            *Fault->Interrupt == unsigned(x64::ExceptionVector::SIMD) &&
            CPU.supportsSIMDExceptions();
        if (CPU.fault() ||
            (!Status && !SIMDFault && (!Fault->Address || !Fault->Access)))
          return failure(exceptions::LostRecoverableContext);
        auto Registers = CaptureRegisters(Fault->PC, true);
        if (!Registers)
          return Registers.takeError();
        X64SEH::Exception Raised{
            *Registers,
            Status.value_or(exceptions::StatusAccessViolation),
            0,
            Fault->PC,
            {}};
        if (SIMDFault) {
          auto SIMD =
              windows_exception::x64SIMDException(Registers->SSE->MXCSR);
          if (!SIMD)
            return failure(exceptions::LostRecoverableContext);
          Raised.Code = SIMD->Code;
          Raised.Parameters.assign(SIMD->Parameters.begin(),
                                   SIMD->Parameters.end());
        } else if (!Status)
          Raised.Parameters = {
              *Fault->Access == BackendAccessKind::Write ? 1ULL : 0ULL,
              *Fault->Address};
        auto Resume = BeginException(Frame, std::move(Raised), Fault->PC, true);
        if (!Resume)
          return Resume.takeError();
        if (!*Resume)
          return llvm::Error::success();
        NextPC = **Resume;
        continue;
      }
      if (CPU.timedOut()) {
        Stop(DriverStopReason::Timeout, runtime::Timeout);
        break;
      }
      if (PendingEnvironmentRead) {
        const auto &Action = PendingEnvironmentRead->Request;
        if (Action.Source ==
                WindowsX64ExecutionPolicy::Action::Kind::ReadTimestamp ||
            Action.Source == WindowsX64ExecutionPolicy::Action::Kind::
                                 ReadTimestampAndProcessor) {
          // The modeled TSC shares the 10 MHz scheduler clock. Account for
          // this admitted instruction exactly once when sampling, retaining
          // its time-slice charge for the scheduler. Cooperative reads never
          // advance time. No host CPU state escapes.
          auto Timestamp = ExecutionTime();
          if (!Timestamp)
            return Timestamp.takeError();
          if (auto E = CPU.setReg(X64Register::AX, uint32_t(*Timestamp)))
            return E;
          if (auto E = CPU.setReg(X64Register::DX, *Timestamp >> 32))
            return E;
          if (Action.Source == WindowsX64ExecutionPolicy::Action::Kind::
                                   ReadTimestampAndProcessor)
            if (auto E = CPU.setReg(X64Register::CX, 0))
              return E;
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        if (Action.Source ==
            WindowsX64ExecutionPolicy::Action::Kind::ReadCurrentThread) {
          if (auto E = PrepareProcessorView())
            return E;
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        auto IRQL = CPU.reg(X64Register::CR8);
        if (!IRQL)
          return IRQL.takeError();
        if (*IRQL != Kernel.currentIRQL())
          return failure(session_diagnostic::CR8Read);
        // MOV only changes its destination and instruction pointer. Preserve
        // flags, other registers and the already counted instruction.
        if (!Action.Destination)
          return failure(session_diagnostic::IRQLDestination);
        if (auto E = CPU.setReg(*Action.Destination, *IRQL))
          return E;
        NextPC = PendingEnvironmentRead->NextPC;
        continue;
      }
      if (PendingGuard) {
        auto Target =
            CPU.reg(PendingGuard == GuardCheckThunk ? X64Register::CX
                                                    : X64Register::AX);
        if (!Target)
          return Target.takeError();
        if (Exports.lookup(*Target))
          if (auto E = Guard.registerExportTarget(*Target))
            return E;
        if (auto E = Guard.validateTarget(*Target))
          return E;
        if (PendingGuard == GuardCheckThunk) {
          auto SP = CPU.reg(X64Register::SP);
          if (!SP)
            return SP.takeError();
          if (auto E = Kernel.validateGuestAccess(*SP, PointerSize, false))
            return E;
          auto Return = CPU.readInteger(*SP, PointerSize);
          if (!Return)
            return Return.takeError();
          if (!CPU.executable(*Return))
            return failure(session_diagnostic::CFGReturnAddress);
          if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
            return E;
          NextPC = *Return;
        } else {
          NextPC = *Target;
        }
        continue;
      }
      if (PendingMemoryWrite && !Pending) {
        auto PC = CPU.reg(X64Register::PC);
        if (!PC)
          return PC.takeError();
        NextPC = *PC;
        continue;
      }
      if (!Pending) {
        Stop(DriverStopReason::EngineError, session_diagnostic::UnknownCPUExit);
        break;
      }
      if (auto E = Observation.exporting(*Pending))
        return ObservationFailed(std::move(E));
      auto ArgumentCount = KernelModel::argumentCount(*Pending);
      if (!ArgumentCount) {
        Stop(DriverStopReason::UnsupportedAPI,
             session_diagnostic::UnsupportedImport + Pending->Module + "!" +
                 Pending->Name);
        break;
      }
      if (!EventAvailable())
        break;
      DriverAPIEvent Event;
      Event.PC = Result.PC;
      Event.Phase = Result.Phase;
      Event.Name = Pending->Name;
      auto SP = CPU.reg(X64Register::SP);
      if (!SP)
        return SP.takeError();
      auto LastArgument =
          ABI.argumentLocation(*SP, MaxVariableAPIArguments - 1);
      if (!LastArgument) {
        Stop(DriverStopReason::ModelError,
             llvm::toString(LastArgument.takeError()));
        break;
      }
      unsigned Count = *ArgumentCount;
      if (Count > MaxAPIArguments) {
        Stop(DriverStopReason::EngineError,
             session_diagnostic::KernelArgumentContract);
        break;
      }
      if (auto E = Kernel.validateGuestAccess(*SP, 8, false)) {
        Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
        break;
      }
      auto ReadArgument = [&](unsigned I) -> llvm::Expected<uint64_t> {
        if (I >= MaxVariableAPIArguments)
          return failure(session_diagnostic::KernelArgumentLimit);
        auto Location = ABI.argumentLocation(*SP, I);
        if (!Location)
          return Location.takeError();
        if (Location->Register == CPURegister::Invalid)
          if (auto E = Kernel.validateGuestAccess(Location->Address,
                                                  ABI.info().WordSize, false))
            return std::move(E);
        return ABI.readArgument(CPU, *SP, I);
      };
      for (unsigned I = 0; I < Count; ++I) {
        auto Argument = ReadArgument(I);
        if (!Argument) {
          Stop(CPU.hasMemoryFault() ? DriverStopReason::MemoryFault
                                    : DriverStopReason::ModelError,
               llvm::toString(Argument.takeError()));
          break;
        }
        Event.Arguments.push_back(*Argument);
      }
      if (!RecordEvent())
        break;
      Result.Calls.push_back(std::move(Event));
      if (Stopped)
        break;
      auto ReturnPC = ABI.readReturnAddress(CPU, *SP);
      if (!ReturnPC) {
        Stop(DriverStopReason::MemoryFault,
             llvm::toString(ReturnPC.takeError()));
        break;
      }
      if (!CPU.executable(*ReturnPC)) {
        Stop(DriverStopReason::MemoryFault,
             session_diagnostic::KernelReturnAddress);
        break;
      }
      // Keep fixed arguments independent of the growing trace: a variadic read
      // can reallocate Event.Arguments while the model still holds ArrayRef.
      const std::vector<uint64_t> FixedArguments =
          Result.Calls.back().Arguments;
      auto ReadVariableArgument = [&](unsigned I) -> llvm::Expected<uint64_t> {
        auto &Arguments = Result.Calls.back().Arguments;
        if (I < Arguments.size())
          return Arguments[I];
        if (I != Arguments.size())
          return failure(session_diagnostic::VariadicArgumentOrder);
        auto Value = ReadArgument(I);
        if (!Value)
          return Value.takeError();
        Arguments.push_back(*Value);
        return *Value;
      };
      auto Value = Kernel.call(*Pending, FixedArguments, ReadVariableArgument);
      if (!Value) {
        std::optional<uint32_t> RaisedCode;
        auto OtherError = llvm::handleErrors(
            Value.takeError(), [&](const KernelGuestException &Exception) {
              RaisedCode = Exception.code();
            });
        if (!OtherError && RaisedCode) {
          const std::string Detail = session_diagnostic::RaisedException +
                                     llvm::utohexstr(*RaisedCode);
          Result.Calls.back().Detail = Detail;
          // A model-raised exception starts at a normal API stop. Never use
          // this path to reset the backend's retained execution fault.
          if (CPU.fault() || CPU.hasDeviceError())
            return failure(session_diagnostic::ExceptionCPU);
          auto Registers = CaptureRegisters(*ReturnPC);
          if (!Registers)
            return Registers.takeError();
          Registers->GPR[seh::StackRegister] = *SP + PointerSize;
          X64SEH::Exception Raised{*Registers, *RaisedCode, 0, *ReturnPC, {}};
          auto Resume =
              BeginException(Frame, std::move(Raised), *ReturnPC - 1, false);
          if (!Resume)
            return Resume.takeError();
          if (!*Resume)
            return llvm::Error::success();
          NextPC = **Resume;
          continue;
        }
        std::string Error = llvm::toString(std::move(OtherError));
        Result.Calls.back().Detail = Error;
        Stop(CPU.hasMemoryFault() ? DriverStopReason::MemoryFault
                                  : DriverStopReason::ModelError,
             Error);
        break;
      }
      // A native provider callback can release its cancel lock and invoke an
      // upper completion in one modeled call. Save the provider frame at its
      // new IRQL before entering that completion; IoCancelIrp's caller keeps
      // its own pre-cancel CPU context.
      if (Pending->Kind == KernelExportRegistry::ExportKind::ProviderFunction)
        if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
          return E;
      if (auto E = RefreshWaiters())
        return E;
      if (auto Status = Kernel.takeThreadTermination()) {
        if (Frame.Parent)
          return failure(session_diagnostic::NestedThreadTermination);
        Frame.ThreadTerminated = true;
        InvocationReturn = *Status;
        Result.Calls.back().Detail = session_diagnostic::ThreadTerminated;
        Stop(DriverStopReason::Returned, "");
        break;
      }
      if (auto Call = Kernel.takeGuestCall()) {
        Frame.ChildCall = std::move(*Call);
        // A framework call can publish both a nested cancellation callback
        // and a synchronous queue wait. The wait belongs to this caller, not
        // to the child callback's next imported function call.
        Frame.Wait = Kernel.takeWait();
        Frame.WaitEvent = Result.Calls.size() - 1;
        Frame.PC = *ReturnPC;
        if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
          return E;
        auto Context = CPU.saveContext();
        if (!Context)
          return Context.takeError();
        Frame.Context = std::move(*Context);
        return llvm::Error::success();
      }
      if (auto Wait = Kernel.takeWait()) {
        Frame.Wait = *Wait;
        Frame.WaitEvent = Result.Calls.size() - 1;
        Frame.PC = *ReturnPC;
        if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
          return E;
        auto Context = CPU.saveContext();
        if (!Context)
          return Context.takeError();
        Frame.Context = std::move(*Context);
        if (Options.Scheduling)
          Frame.PausedModel = Kernel.captureExecutionContext();
        if (Frame.ID)
          if (auto E = Kernel.suspendScheduled(Frame.ID))
            return E;
        return llvm::Error::success();
      }
      Result.Calls.back().Result = *Value;
      // Direct interrupt-lock APIs change the running frame's IRQL without a
      // new guest callback. The next instruction must observe that same CR8.
      if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
        return E;
      if (auto E = CPU.setReg(X64Register::AX, *Value))
        return E;
      if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
        return E;
      NextPC = *ReturnPC;
    }
    return llvm::Error::success();
  };
  auto Pump = [&](std::unique_ptr<Execution> Current,
                  const std::string &ParentPhase) -> llvm::Error {
    const bool Foreground = bool(Current) && !Current->ID;
    auto StartDetachedCall =
        [&](const KernelGuestCall &Call,
            std::optional<KernelModel::Wait> ClockWait = {},
            uint64_t ReadyOrder = 0) -> llvm::Error {
      auto Wait = ReadyOrder ? std::move(ClockWait) : Kernel.takeWait();
      if (Wait &&
          Wait->Type != KernelModel::Wait::Kind::InterruptSynchronization)
        return failure(session_diagnostic::DetachedCallbackWait);
      if (!Wait)
        if (auto E = Kernel.beginGuestCall(Call.Token))
          return E;
      auto Frame = NewExecution(Call.PC, Call.Arguments,
                                guestCallPhase(Call.Token), 0, true);
      if (!Frame)
        return Frame.takeError();
      (*Frame)->ReturnToken = Call.Token;
      if (ReadyOrder)
        (*Frame)->Slice->ReadyOrder = ReadyOrder;
      (*Frame)->SynchronizationObject = Call.SynchronizationObject;
      if (Wait) {
        auto Context = CPU.saveContext();
        if (!Context)
          return Context.takeError();
        (*Frame)->Context = std::move(*Context);
        (*Frame)->Wait = std::move(Wait);
        (*Frame)->PendingEntry = Call.Token;
        Waiting.push_back(std::move(*Frame));
        Current.reset();
      } else {
        Current = std::move(*Frame);
      }
      return llvm::Error::success();
    };
    auto ReadyDetachedCall = [&](const KernelGuestCall &Call) -> llvm::Error {
      if (!Options.Scheduling)
        return StartDetachedCall(Call);
      auto Order = Kernel.issueReadyOrder();
      if (!Order)
        return Order.takeError();
      ClockCalls.push_back({Call, Kernel.takeWait(), *Order});
      Current.reset();
      return llvm::Error::success();
    };
    auto CommitException = [&](X64SEH::Context Registers,
                               const BackendContext &Original,
                               uint64_t DestinationBase,
                               bool ContinueExecution = false) -> llvm::Error {
      const Execution *Destination = Current.get();
      while (Destination && Destination->Base != DestinationBase) {
        if (!Destination->ExceptionCallback)
          return failure(session_diagnostic::ExceptionCallbackBoundary);
        Destination = Destination->Parent.get();
      }
      if (!Destination)
        return failure(session_diagnostic::MissingExceptionStack);
      if (auto E = CPU.restoreContext(Original))
        return E;
      if (auto E = ContinueExecution ? ApplyRegisters(Registers)
                                     : ApplyHandlerRegisters(Registers))
        return E;
      auto Context = CPU.saveContext();
      if (!Context)
        return Context.takeError();
      while (Current->Base != DestinationBase) {
        Kernel.enterExecution(Current->Base, Current->ThreadKey);
        if (auto E = Kernel.validateExecutionReturn(Current->Base,
                                                    Current->EntryIRQL, true))
          return E;
        if (auto E = Kernel.retireStack(Current->Base, Current->Size))
          return E;
        StackInUse[(Current->Base - CallbackStackBase) / CallbackStackStride] =
            false;
        auto Parent = std::move(Current->Parent);
        Current = std::move(Parent);
      }
      Current->Context = std::move(*Context);
      Current->PC = Registers.PC;
      Current->Exception.reset();
      return llvm::Error::success();
    };
    auto PrepareExceptionCallback = [&](Execution &Child,
                                        Execution &Parent) -> llvm::Error {
      auto &State = *Parent.Exception;
      if (auto E = CPU.restoreContext(*State.Original))
        return E;
      if (auto E = ApplyHandlerRegisters(State.Next.State.Registers))
        return E;
      const uint64_t Storage = Child.Base + Child.Size - seh::RecordsSize;
      State.Raised.Flags = State.Next.ExceptionFlags;
      if (auto E = CPU.writeInteger(Storage + seh::ExceptionFlagsOffset,
                                    State.Raised.Flags, 4))
        return E;
      Child.PC = State.Next.State.HandlerPC;
      Child.ExceptionCallback = State.Next.Kind;
      Child.Context.reset();
      if (auto E =
              CPU.writeInteger(Child.InitialSP, ReturnSentinel, PointerSize))
        return E;
      if (auto E = CPU.setReg(X64Register::SP, Child.InitialSP))
        return E;
      if (auto E = CPU.setReg(X64Register::CX,
                              State.Next.Kind == X64SEH::ActionKind::Filter
                                  ? Storage + seh::ExceptionPointersOffset
                                  : 1))
        return E;
      return CPU.setReg(X64Register::DX, State.Next.State.EstablisherFrame);
    };
    while (!DeadlineExceeded()) {
      if (Current) {
        if (Current->PausedModel) {
          if (auto E = Kernel.restoreExecutionContext(*Current->PausedModel)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          Current->PausedModel.reset();
        }
        if (Current->ResumeIRQL) {
          if (auto E = Kernel.restoreWaitIRQL(*Current->ResumeIRQL)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          Current->ResumeIRQL.reset();
        }
        if (Current->PendingEntry) {
          if (auto E = Kernel.beginGuestCall(*Current->PendingEntry)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          Current->PendingEntry.reset();
        }
        if (!Current->ChildCall)
          if (auto E = RunExecution(*Current)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
        if (Options.Scheduling) {
          const uint64_t Count = Result.Instructions - AccountedInstructions;
          Current->Slice->Remaining -=
              std::min(Current->Slice->Remaining, Count);
          if (auto E = AdvanceClock()) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
        }
        if (Current->Boundary) {
          if (Kernel.currentIRQL() >= scheduler::DispatchLevel ||
              (Current->Slice->Remaining &&
               !Kernel.hasQueuedPriorityCallback() &&
               !HasHigherReadyThread(*Current)))
            continue;
          Current->PausedModel = Kernel.captureExecutionContext();
          if (auto E = Kernel.preemptScheduled(Current->ID)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          if (!Current->Slice->Remaining) {
            auto Order = Kernel.issueReadyOrder();
            if (!Order) {
              ModelFailure(Order.takeError());
              return llvm::Error::success();
            }
            Current->Slice->ReadyOrder = *Order;
            Current->Slice->Remaining = Options.Scheduling->QuantumInstructions;
          }
          Waiting.push_back(std::move(Current));
        }
        if (!Current)
          continue;
        if (Current->Exception) {
          auto &State = *Current->Exception;
          if (State.Next.Kind == X64SEH::ActionKind::Handler) {
            if (auto E =
                    CommitException(State.Next.State.Registers, *State.Original,
                                    State.Next.Bounds.Base)) {
              ModelFailure(std::move(E));
              return llvm::Error::success();
            }
            continue;
          }
          auto Child =
              NewExecution(State.Next.State.HandlerPC, {}, Current->Phase,
                           Current->ID, true, seh::RecordsSize);
          if (!Child) {
            ModelFailure(Child.takeError());
            return llvm::Error::success();
          }
          if (auto E = Kernel.inheritExecutionContext((*Child)->Base,
                                                      Current->Base)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          auto Records = X64SEH::encodeRecords(
              State.Raised, (*Child)->Base + (*Child)->Size - seh::RecordsSize);
          if (!Records) {
            ModelFailure(Records.takeError());
            return llvm::Error::success();
          }
          if (auto E =
                  CPU.write((*Child)->Base + (*Child)->Size - seh::RecordsSize,
                            *Records)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          if (auto E = PrepareExceptionCallback(**Child, *Current)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          (*Child)->PowerManagedCallback = Current->PowerManagedCallback;
          (*Child)->Slice = Current->Slice;
          (*Child)->ThreadKey = Current->ThreadKey;
          (*Child)->Parent = std::move(Current);
          Current = std::move(*Child);
          continue;
        }
        if (Current->ChildCall) {
          if (Current->Wait &&
              Current->Wait->Type ==
                  KernelModel::Wait::Kind::InterruptSynchronization) {
            if (Options.Scheduling)
              Current->PausedModel = Kernel.captureExecutionContext();
            if (Current->ID)
              if (auto E = Kernel.suspendScheduled(Current->ID)) {
                ModelFailure(std::move(E));
                return llvm::Error::success();
              }
            Waiting.push_back(std::move(Current));
            continue;
          }
          auto Call = std::move(*Current->ChildCall);
          Current->ChildCall.reset();
          if (Call.Token.Owner == GuestCallOwner::PoFx)
            Kernel.enterExecution(Current->Base, Current->ThreadKey);
          if (auto E = Kernel.beginGuestCall(Call.Token)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          auto Child =
              NewExecution(Call.PC, Call.Arguments, guestCallPhase(Call.Token),
                           Current->ID, true);
          if (!Child) {
            ModelFailure(Child.takeError());
            return llvm::Error::success();
          }
          (*Child)->ReturnToken = Call.Token;
          (*Child)->SynchronizationObject = Call.SynchronizationObject;
          (*Child)->PowerManagedCallback = Current->PowerManagedCallback;
          (*Child)->Slice = Current->Slice;
          (*Child)->ThreadKey = Current->ThreadKey;
          (*Child)->Parent = std::move(Current);
          Current = std::move(*Child);
          continue;
        }
        if (Current->Wait) {
          Waiting.push_back(std::move(Current));
        } else {
          if (Result.Stop != DriverStopReason::Returned)
            return llvm::Error::success();
          if (Current->SynchronizationEntered) {
            if (auto E = Kernel.finishFrameworkCallback(
                    Current->SynchronizationObject)) {
              ModelFailure(std::move(E));
              return llvm::Error::success();
            }
            Current->SynchronizationEntered = false;
            if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
              return E;
          }
          if (auto E = Kernel.validateExecutionReturn(
                  Current->Base, Current->EntryIRQL, bool(Current->Parent))) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          if (Current->ExceptionCallback) {
            auto &Parent = *Current->Parent;
            auto &State = *Parent.Exception;
            const uint64_t Storage =
                Current->Base + Current->Size - seh::RecordsSize;
            std::vector<uint8_t> Records;
            auto Advance = [&]() -> llvm::Expected<X64SEH::Action> {
              if (*Current->ExceptionCallback != X64SEH::ActionKind::Filter)
                return Exceptions.advance(State.Dispatch);
              Records.resize(seh::RecordsSize);
              if (auto E = CPU.read(Storage, Records))
                return std::move(E);
              return Exceptions.finishFilter(
                  State.Dispatch, static_cast<int32_t>(*InvocationReturn),
                  Records, State.Raised, Storage);
            };
            auto Next = Advance();
            if (!Next) {
              ModelFailure(Next.takeError());
              return llvm::Error::success();
            }
            State.Next = *Next;
            if (Next->Kind == X64SEH::ActionKind::Filter ||
                Next->Kind == X64SEH::ActionKind::Finally) {
              if (auto E = PrepareExceptionCallback(*Current, Parent)) {
                ModelFailure(std::move(E));
                return llvm::Error::success();
              }
              continue;
            }
            if (Next->Kind == X64SEH::ActionKind::Unhandled) {
              ModelFailure(failure(session_diagnostic::UnhandledException +
                                   llvm::utohexstr(State.Raised.Code)));
              return llvm::Error::success();
            }
            auto Registers = Next->State.Registers;
            if (Next->Kind == X64SEH::ActionKind::ContinueExecution) {
              if (!State.CanContinue) {
                ModelFailure(
                    failure(session_diagnostic::APIExceptionContinuation));
                return llvm::Error::success();
              }
              auto Restored = Exceptions.continuation(
                  Records, State.Raised, Storage, {Parent.Base, Parent.Size});
              if (!Restored) {
                ModelFailure(Restored.takeError());
                return llvm::Error::success();
              }
              Registers = *Restored;
            }
            const uint64_t DestinationBase =
                Next->Kind == X64SEH::ActionKind::ContinueExecution
                    ? Parent.Base
                    : Next->Bounds.Base;
            if (auto E = CommitException(
                    Registers, *State.Original, DestinationBase,
                    Next->Kind == X64SEH::ActionKind::ContinueExecution)) {
              ModelFailure(std::move(E));
              return llvm::Error::success();
            }
            continue;
          }
          if (auto E = Kernel.retireStack(Current->Base, Current->Size)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          if (Current->PrivateStack)
            StackInUse[(Current->Base - CallbackStackBase) /
                       CallbackStackStride] = false;
          if (Current->Parent) {
            auto Completion =
                Kernel.finishGuestCall(Current->ReturnToken, *InvocationReturn);
            if (!Completion) {
              ModelFailure(Completion.takeError());
              return llvm::Error::success();
            }
            auto Parent = std::move(Current->Parent);
            if (auto Wait = Kernel.takeWait()) {
              if (Parent->Wait) {
                ModelFailure(failure(session_diagnostic::CallbackReplacesWait));
                return llvm::Error::success();
              }
              Parent->Wait = std::move(Wait);
            }
            if (*Completion) {
              if (!Parent->Wait) {
                Parent->ResumeValue = **Completion;
                Result.Calls[Parent->WaitEvent].Result = **Completion;
              } else {
                Kernel.enterExecution(Parent->Base, Parent->ThreadKey);
                if (Options.Scheduling)
                  Parent->PausedModel = Kernel.captureExecutionContext();
                if (Parent->ID)
                  if (auto E = Kernel.suspendScheduled(Parent->ID)) {
                    ModelFailure(std::move(E));
                    return llvm::Error::success();
                  }
                Waiting.push_back(std::move(Parent));
                Current.reset();
                continue;
              }
            } else {
              Parent->ChildCall = Kernel.takeGuestCall();
              if (!Parent->ChildCall) {
                ModelFailure(failure(session_diagnostic::MissingModelCallback));
                return llvm::Error::success();
              }
              if (auto Wait = Kernel.takeWait()) {
                if (Parent->Wait) {
                  ModelFailure(
                      failure(session_diagnostic::CallbackReplacesWait));
                  return llvm::Error::success();
                }
                Parent->Wait = std::move(Wait);
              }
              Current = std::move(Parent);
              continue;
            }
            Current = std::move(Parent);
            continue;
          }
          if (Current->ReturnToken.ID) {
            auto Completion =
                Kernel.finishGuestCall(Current->ReturnToken, *InvocationReturn);
            if (!Completion) {
              ModelFailure(Completion.takeError());
              return llvm::Error::success();
            }
            if (!*Completion) {
              auto Call = Kernel.takeGuestCall();
              if (!Call) {
                ModelFailure(failure(session_diagnostic::MissingModelCallback));
                return llvm::Error::success();
              }
              if (auto E = ReadyDetachedCall(*Call)) {
                ModelFailure(std::move(E));
                return llvm::Error::success();
              }
              continue;
            }
            Current.reset();
            continue;
          }
          if (!Current->ID) {
            if (auto E = Kernel.flushFrameworkCallbackDestructions()) {
              ModelFailure(std::move(E));
              return llvm::Error::success();
            }
            return llvm::Error::success();
          }
          auto Continuation =
              Current->ThreadTerminated
                  ? llvm::Expected<std::optional<KernelGuestCall>>(
                        std::optional<KernelGuestCall>{})
                  : Kernel.continueScheduled(Current->ID, *InvocationReturn);
          if (!Continuation) {
            ModelFailure(Continuation.takeError());
            return llvm::Error::success();
          }
          if (*Continuation) {
            auto Wait = Kernel.takeWait();
            if (Wait && Wait->Type !=
                            KernelModel::Wait::Kind::InterruptSynchronization) {
              ModelFailure(failure(session_diagnostic::ScheduledCallbackWait));
              return llvm::Error::success();
            }
            if (!Wait)
              if (auto E = Kernel.beginGuestCall((**Continuation).Token)) {
                ModelFailure(std::move(E));
                return llvm::Error::success();
              }
            auto Frame =
                NewExecution((**Continuation).PC, (**Continuation).Arguments,
                             Current->Phase, Current->ID);
            if (!Frame) {
              ModelFailure(Frame.takeError());
              return llvm::Error::success();
            }
            (*Frame)->SynchronizationObject =
                (**Continuation).SynchronizationObject;
            (*Frame)->Slice = Current->Slice;
            (*Frame)->ThreadKey = Current->ThreadKey;
            if (Wait) {
              auto Context = CPU.saveContext();
              if (!Context) {
                ModelFailure(Context.takeError());
                return llvm::Error::success();
              }
              (*Frame)->Context = std::move(*Context);
              (*Frame)->Wait = std::move(Wait);
              (*Frame)->PendingEntry = (**Continuation).Token;
              if (Options.Scheduling) {
                Kernel.enterExecution((*Frame)->Base, (*Frame)->ThreadKey);
                (*Frame)->PausedModel = Kernel.captureExecutionContext();
              }
              if (auto E = Kernel.suspendScheduled(Current->ID)) {
                ModelFailure(std::move(E));
                return llvm::Error::success();
              }
              Waiting.push_back(std::move(*Frame));
              Current.reset();
            } else {
              Current = std::move(*Frame);
            }
            continue;
          }
          if (auto E = Kernel.finishScheduled(Current->ID)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          Current.reset();
        }
      }
      if (Options.Scheduling) {
        Kernel.enterForeground();
        if (auto E = Kernel.setUserRequestContext(false)) {
          ModelFailure(std::move(E));
          return llvm::Error::success();
        }
        if (auto E = AdvanceClock()) {
          ModelFailure(std::move(E));
          return llvm::Error::success();
        }
      }
      // Waiters retain independent stacks and CPU state. Ready passive frames
      // yield to interrupts, DPCs, cancellation and provider completions.
      if (auto E = RefreshWaiters()) {
        ModelFailure(std::move(E));
        return llvm::Error::success();
      }
      std::optional<size_t> Ready;
      auto Best =
          Options.Scheduling ? Kernel.nextPassiveThread() : std::nullopt;
      if (Options.Scheduling && !ClockCalls.empty() &&
          (!Best || Kernel.readyThread(0, ClockCalls.front().ReadyOrder)
                        .precedes(*Best)))
        Best = Kernel.readyThread(0, ClockCalls.front().ReadyOrder);
      for (size_t I = 0;
           I < Waiting.size() && !Kernel.hasQueuedPriorityCallback(); ++I) {
        if (Waiting[I]->Wait ||
            (Best && !Kernel
                          .readyThread(Waiting[I]->ThreadKey,
                                       Waiting[I]->Slice->ReadyOrder)
                          .precedes(*Best)))
          continue;
        Ready = I;
        if (!Options.Scheduling)
          break;
        Best = Kernel.readyThread(Waiting[I]->ThreadKey,
                                  Waiting[I]->Slice->ReadyOrder);
      }
      if (Ready) {
        Current = std::move(Waiting[*Ready]);
        Waiting.erase(Waiting.begin() + *Ready);
        if (Current->ID) {
          if (auto E = Kernel.resumeScheduled(Current->ID)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
        } else {
          Kernel.enterForeground();
        }
      }
      if (!Current && !ClockCalls.empty() &&
          !Kernel.hasQueuedPriorityCallback() && Best &&
          Best->ReadyOrder == ClockCalls.front().ReadyOrder) {
        auto Call = std::move(ClockCalls.front());
        ClockCalls.pop_front();
        if (auto E = StartDetachedCall(Call.Call, std::move(Call.Wait),
                                       Call.ReadyOrder)) {
          ModelFailure(std::move(E));
          return llvm::Error::success();
        }
      }
      if (Current)
        continue;
      if (auto Call = Kernel.takeGuestCall()) {
        if (auto E = ReadyDetachedCall(*Call)) {
          ModelFailure(std::move(E));
          return llvm::Error::success();
        }
        continue;
      }
      auto Next = Kernel.nextScheduled(false);
      if (!Next) {
        ModelFailure(Next.takeError());
        return llvm::Error::success();
      }
      if (auto E = RefreshWaiters()) {
        ModelFailure(std::move(E));
        return llvm::Error::success();
      }
      if (!*Next) {
        if (auto Call = Kernel.takeGuestCall()) {
          if (auto E = ReadyDetachedCall(*Call)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          continue;
        }
        if (std::any_of(Waiting.begin(), Waiting.end(),
                        [](const auto &Frame) { return !Frame->Wait; }))
          continue;
        if (!Foreground && Waiting.empty() &&
            !Kernel.requestPending(
                0, KernelModel::PendingRequestScope::ExcludePowerParked) &&
            !Kernel.hasPendingHardwareWork()) {
          Result.Phase = ParentPhase;
          Result.Stop = DriverStopReason::Returned;
          return llvm::Error::success();
        }
        std::optional<uint64_t> Bound = Kernel.nextEventTime();
        for (const auto &Frame : Waiting)
          if (Frame->Wait && Frame->Wait->Deadline &&
              (!Bound || *Frame->Wait->Deadline < *Bound))
            Bound = Frame->Wait->Deadline;
        if (!Bound) {
          ModelFailure(failure(session_diagnostic::StalledPending));
          return llvm::Error::success();
        }
        Next = Kernel.nextScheduled(true, Bound);
        if (!Next) {
          ModelFailure(Next.takeError());
          return llvm::Error::success();
        }
        // Timer expiration satisfies existing waits before its queued DPC can
        // rearm the timer and clear its signal.
        if (auto E = RefreshWaiters()) {
          ModelFailure(std::move(E));
          return llvm::Error::success();
        }
        if (!*Next) {
          if (auto Call = Kernel.takeGuestCall()) {
            if (auto E = ReadyDetachedCall(*Call)) {
              ModelFailure(std::move(E));
              return llvm::Error::success();
            }
          }
          continue;
        }
      }
      auto Frame =
          NewExecution((**Next).PC, (**Next).Arguments,
                       std::string(CallbackPhase) + std::to_string((**Next).ID),
                       (**Next).ID);
      if (!Frame) {
        ModelFailure(Frame.takeError());
        return llvm::Error::success();
      }
      (*Frame)->SynchronizationObject = (**Next).SynchronizationObject;
      if (Options.Scheduling)
        (*Frame)->Slice->ReadyOrder = (**Next).ReadyOrder;
      Current = std::move(*Frame);
    }
    return llvm::Error::success();
  };
  auto Invoke = [&](const KernelModel::Invocation &Invocation,
                    const std::string &Phase) -> llvm::Error {
    Kernel.enterForeground();
    std::vector<uint64_t> Arguments{Invocation.Argument0, Invocation.Argument1,
                                    Invocation.Argument2, Invocation.Argument3};
    Arguments.insert(Arguments.end(), Invocation.StackArguments.begin(),
                     Invocation.StackArguments.end());
    auto Frame = NewExecution(Invocation.PC, Arguments, Phase, 0);
    if (!Frame) {
      ModelFailure(Frame.takeError());
      return llvm::Error::success();
    }
    (*Frame)->SynchronizationObject = Invocation.SynchronizationObject;
    (*Frame)->RequestIRP = Invocation.IRP;
    return Pump(std::move(*Frame), Phase);
  };
  auto DrainCallbacks = [&]() -> llvm::Error {
    const std::string ParentPhase = Result.Phase;
    return Pump(nullptr, ParentPhase);
  };
  if (auto E =
          Invoke({Image->Entry, Kernel.driverObject(), Kernel.registryPath()},
                 EntryPhase))
    return std::move(E);
  if (Result.Stop == DriverStopReason::Returned)
    Result.NTStatus = InvocationReturn;
  if (Result.Stop == DriverStopReason::Returned) {
    if (auto E = Kernel.finishEntry())
      ModelFailure(std::move(E));
    else if (*Result.NTStatus && !(*Result.NTStatus & NTStatusFailureMask))
      ModelFailure(failure(session_diagnostic::InitializationStatus));
  }
  // Entry failure is a valid completed observation. No requests or unload are
  // delivered to a driver whose initialization did not succeed.
  if (Result.Stop == DriverStopReason::Returned && Result.NTStatus == 0) {
    if (auto E = DrainCallbacks())
      return std::move(E);
    if (Result.Stop == DriverStopReason::Returned)
      if (auto E = Kernel.preparePnpDevices())
        ModelFailure(std::move(E));
    for (const auto &Device : Options.PnpDevices) {
      if (Result.Stop != DriverStopReason::Returned)
        break;
      Result.Phase = std::string(AddDevicePhase) + Device.ID;
      if (DeadlineExceeded())
        break;
      auto Invocation = Kernel.beginAddDevice(Device.ID);
      if (!Invocation) {
        ModelFailure(Invocation.takeError());
        break;
      }
      if (auto E = Invoke(*Invocation, Result.Phase))
        return std::move(E);
      if (Result.Stop != DriverStopReason::Returned)
        break;
      if (auto E =
              Kernel.finishAddDevice(Device.ID, uint32_t(*InvocationReturn))) {
        ModelFailure(std::move(E));
        break;
      }
      if (auto E = DrainCallbacks())
        return std::move(E);
      if (Result.Stop == DriverStopReason::Returned)
        if (auto E = Kernel.finalizeAddDevice(Device.ID)) {
          ModelFailure(std::move(E));
          break;
        }
    }
    std::vector<uint64_t> BatchedIRPs;
    auto FinalizeBatch = [&](bool RequireCompleted = false) -> llvm::Error {
      std::vector<uint64_t> Remaining;
      for (uint64_t IRP : BatchedIRPs) {
        if (!RequireCompleted && Kernel.requestPending(IRP)) {
          Remaining.push_back(IRP);
          continue;
        }
        if (auto E = Kernel.finalizeRequest(IRP))
          return E;
      }
      BatchedIRPs = std::move(Remaining);
      return llvm::Error::success();
    };
    for (size_t Index = 0; Index < Options.Requests.size(); ++Index) {
      if (Result.Stop != DriverStopReason::Returned)
        break;
      Result.Phase = std::string(RequestPhase) + std::to_string(Index);
      if (DeadlineExceeded())
        break;
      auto Invocation = Kernel.beginRequest(Options.Requests[Index], Index);
      if (!Invocation) {
        ModelFailure(Invocation.takeError());
        break;
      }
      if (Invocation->PC) {
        if (auto E = Kernel.setUserRequestContext(
                Options.Requests[Index].Kind != DriverRequestKind::Pnp &&
                    Options.Requests[Index].Kind != DriverRequestKind::Power,
                Options.Requests[Index].RequestorProcessID)) {
          ModelFailure(std::move(E));
          break;
        }
        auto Invoked = Invoke(*Invocation, Result.Phase);
        if (auto E = Kernel.setUserRequestContext(false)) {
          ModelFailure(std::move(E));
          break;
        }
        if (Invoked)
          return std::move(Invoked);
        if (Result.Stop != DriverStopReason::Returned)
          break;
        if (Invocation->FrameworkCallerContext) {
          auto Continued =
              Kernel.continueFrameworkCallerContext(Invocation->IRP);
          if (!Continued) {
            ModelFailure(Continued.takeError());
            break;
          }
          Invocation->FrameworkDispatchStatus =
              Continued->FrameworkDispatchStatus;
          if (Continued->PC) {
            if (auto E = Invoke(*Continued, Result.Phase))
              return std::move(E);
            if (Result.Stop != DriverStopReason::Returned)
              break;
          }
        }
        const uint32_t DispatchStatus =
            Invocation->FrameworkDispatchStatus.value_or(
                uint32_t(*InvocationReturn));
        if (auto E =
                Kernel.recordDispatchReturn(Invocation->IRP, DispatchStatus)) {
          ModelFailure(std::move(E));
          break;
        }
      }
      if (Options.Requests[Index].UserUnmapAfterDispatch.value_or(false))
        if (auto E = Kernel.revokeRequestUserBuffers(Invocation->IRP)) {
          ModelFailure(std::move(E));
          break;
        }
      if (Options.Requests[Index].RequestorExitAfterDispatch.value_or(false))
        if (auto E = Kernel.exitRequestorProcess(Invocation->IRP)) {
          ModelFailure(std::move(E));
          break;
        }
      // A completed REMOVE may still have callbacks that released their final
      // remove-lock acquisition and then waited. Keep its captured route until
      // those real execution frames return, even if the packet is already dead.
      const auto &Input = Options.Requests[Index];
      const bool Removing =
          Input.Pnp && Input.Pnp->Minor == DevicePnpRequest::Remove;
      const bool Deferred = Removing || Kernel.requestPending(Invocation->IRP);
      if (Input.DeferCallbackDrain) {
        if (!Kernel.requestPending(Invocation->IRP) || Removing) {
          ModelFailure(failure(session_diagnostic::DeferredDrain));
          break;
        }
        BatchedIRPs.push_back(Invocation->IRP);
        continue;
      }
      if (!Deferred) {
        if (auto E = Kernel.finalizeRequest(Invocation->IRP)) {
          ModelFailure(std::move(E));
          break;
        }
      }
      if (auto E = DrainCallbacks())
        return std::move(E);
      if (Result.Stop != DriverStopReason::Returned)
        break;
      if (Removing) {
        if (auto E = Kernel.beginFrameworkRemoval(Invocation->IRP)) {
          ModelFailure(std::move(E));
          break;
        }
        if (auto E = DrainCallbacks())
          return std::move(E);
        if (Result.Stop != DriverStopReason::Returned)
          break;
      }
      if (Deferred) {
        if (auto E = Kernel.finalizeRequest(Invocation->IRP)) {
          ModelFailure(std::move(E));
          break;
        }
      }
      if (auto E = FinalizeBatch()) {
        ModelFailure(std::move(E));
        break;
      }
    }
    if (Result.Stop == DriverStopReason::Returned && !BatchedIRPs.empty()) {
      if (auto E = DrainCallbacks())
        return std::move(E);
      if (Result.Stop == DriverStopReason::Returned)
        if (auto E = FinalizeBatch(true))
          ModelFailure(std::move(E));
    }
    if (Result.Stop == DriverStopReason::Returned && Options.Unload &&
        !DeadlineExceeded()) {
      Result.Phase = UnloadPhase;
      auto Invocation = Kernel.beginUnload();
      if (!Invocation) {
        ModelFailure(Invocation.takeError());
      } else {
        if (auto E = Invoke(*Invocation, UnloadPhase))
          return std::move(E);
        if (Result.Stop == DriverStopReason::Returned) {
          if (auto E = Kernel.finishUnload())
            ModelFailure(std::move(E));
          else
            Result.UnloadCompleted = true;
        }
      }
    }
  }
  if (ObserverFailure)
    return failure(*ObserverFailure);
  if (auto E = Kernel.snapshot()) {
    Result.Diagnostic += (Result.Diagnostic.empty() ? "" : "; ") +
                         std::string(session_diagnostic::ObjectSnapshot) +
                         llvm::toString(std::move(E));
    if (Result.Stop == DriverStopReason::Returned) {
      Result.Stop = DriverStopReason::ModelError;
    }
  }
  if (Result.Stop == DriverStopReason::Returned)
    DeadlineExceeded();
  if (auto Fault = CPU.fault()) {
    DriverFault Observation;
    Observation.Kind = backendFaultKindName(Fault->Kind);
    Observation.PC = Fault->PC;
    Observation.Address = Fault->Address;
    Observation.Size = Fault->Size;
    Observation.Interrupt = Fault->Interrupt;
    Observation.ErrorCode = Fault->ErrorCode;
    if (Fault->Cause)
      Observation.Cause = backendFaultCauseName(*Fault->Cause);
    if (Fault->Access)
      Observation.Access = backendAccessKindName(*Fault->Access);
    Result.Fault = std::move(Observation);
  }
  return Result;
}

llvm::Expected<DriverResult> emulateDriver(const std::filesystem::path &Path,
                                           const DriverOptions &Options) {
  return runDriver(Path, Options, nullptr);
}

llvm::Expected<DriverResult> observeDriver(const std::filesystem::path &Path,
                                           const DriverOptions &Options,
                                           ProcessObserver &Observer) {
  return runDriver(Path, Options, &Observer);
}
} // namespace neverd::emulation
