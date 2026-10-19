//===- DriverExecutionRun.cpp - Retained WDM driver execution -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded WDM driver execution.
///
//===----------------------------------------------------------------------===//

#include "DriverExecutionRun.h"

#include "../../../arch/x86_64/X64Exception.h"
#include "../../../core/ExecutionDiagnostics.h"
#include "../../../runtime/RuntimeValues.h"
#include "../exception/X64SEH.h"
#include "../exception/X64SIMDException.h"
#include "../kernel/KernelException.h"
#include "../kernel/KernelExportRegistry.h"
#include "../kernel/KernelGuestPageTables.h"
#include "../kernel/KernelModel.h"
#include "../kernel/KernelReferenceImage.h"
#include "../kernel/WindowsKernelLayout.h"
#include "../process/WindowsProcessContext.h"
#include "DriverBootstrapModules.h"
#include "DriverContextCapture.h"
#include "DriverExceptionPublication.h"
#include "DriverGuardPublication.h"
#include "DriverImage.h"
#include "DriverImportPublication.h"
#include "DriverScenario.h"
#include "DriverSessionState.h"
#include "DriverTask.h"
#include "GuardControlFlow.h"
#include "KernelModuleImage.h"
#include "WindowsX64ExecutionPolicy.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ExecutionBudget.h"
#include "neverd/emulation/ExecutionSession.h"
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
  case GuestCallOwner::Avl:
    Prefix = AvlCallbackPhase;
    break;
  case GuestCallOwner::DriverService:
    Prefix = "callback:driver-service:";
    break;
  case GuestCallOwner::ThreadNotify:
    Prefix = ThreadNotifyCallbackPhase;
    break;
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
  case GuestCallOwner::ExecutiveCallback:
    Prefix = ExecutiveCallbackPhase;
    break;
  case GuestCallOwner::ProcessNotify:
    Prefix = ProcessNotifyCallbackPhase;
    break;
  case GuestCallOwner::RegistryCallback:
    Prefix = RegistryCallbackPhase;
    break;
  }
  return std::string(Prefix) + std::to_string(Token.ID);
}

} // namespace

namespace {
class DriverExecutionEnvironment;
DriverTask runDriver(DriverExecutionEnvironment &);
class DriverExecutionEnvironment final : public ExecutionEnvironment {
public:
  explicit DriverExecutionEnvironment(std::unique_ptr<DriverSessionState> S,
                                      bool CallsEnabled)
      : State(std::move(S)), CallsEnabled(CallsEnabled) {}
  std::string name() const override { return "windows-driver"; }
  std::vector<std::string> stageNames() const override {
    return {"after-entry"};
  }
  llvm::Error initialize(ExecutionSession &S,
                         ModuleBindings &Modules) override {
    if (auto E = adoptDriverBootstrapModules(*State, Modules))
      return E;
    State->AllowImportPublication = true;
    Session = &S;
    Task = runDriver(*this);
    Task.bind(Control);
    return llvm::Error::success();
  }
  SessionModelHooks modelHooks() override {
    SessionModelHooks H;
    H.BeforeInstruction = [this](uint64_t PC, uint32_t N) {
      return Policy.BeforeInstruction(PC, N);
    };
    H.Read = [this](uint64_t A, uint32_t N) { Policy.Read(A, N); };
    H.Write = [this](uint64_t A, uint32_t N, uint64_t V) {
      Policy.Write(A, N, V);
    };
    H.Fault = [this](uint64_t A, uint32_t N, const char *Kind) {
      Policy.Fault(A, N, Kind);
    };
    H.Interrupt = [this](uint32_t N) { Policy.Interrupt(N); };
    H.InvalidInstruction = [this] { Policy.InvalidInstruction(); };
    return H;
  }
  bool observeInstruction(ExecutionBackend &, uint64_t, uint32_t) override {
    State->Result.Instructions = State->Budget->instructions();
    ++State->RunInstructions;
    return false;
  }
  bool admitGuestException(ExecutionBackend &, const BackendFault &F) override {
    return Recovery(F);
  }
  bool admitResourcePause(const ResourcePause &P) const override {
    const auto &F = P.Fault;
    if (State->Stopped || F.Kind != BackendFaultKind::UnmappedMemory ||
        !F.Address || !F.Size || !*F.Size || *F.Size > UINT32_MAX || !F.Access)
      return false;
    const auto A = *F.Address, N = *F.Size;
    if (A < profile::UserProbeLimit || N > UINT64_MAX - A ||
        (A > profile::CanonicalUserMax && A < profile::CanonicalKernelMin) ||
        (A <= profile::CanonicalUserMax &&
         N - 1 > profile::CanonicalUserMax - A) ||
        overlapsDriverReservedMemory(A, N) ||
        (State->Options.PageTables &&
         KernelGuestPageTables::overlapsReservedMemory(A, N)))
      return false;
    return State->Kernel->isModuleResourceCandidate(A, uint32_t(N));
  }
  llvm::Expected<ModuleMappingPlan>
  buildModule(const ModuleBinding &B, const modules::ModuleImage &I) override {
    auto Plan = planKernelGuestModule(B, I, State->Options.MemoryLimit);
    if (!Plan)
      return Plan.takeError();
    if (State->Options.PageTables &&
        KernelGuestPageTables::overlapsReservedMemory(Plan->Image.Base,
                                                      Plan->Size))
      return failure("kernel module overlaps reserved guest page-table memory");
    return Plan;
  }
  ModuleMappingFinalizer moduleMappingFinalizer() override {
    return [this](llvm::ArrayRef<ModuleInstance> Existing,
                  llvm::ArrayRef<ModuleMappingCandidate> Candidates) {
      return finalizeKernelGuestModules(Existing, Candidates, State->Exports);
    };
  }
  ModuleDependencyMatcher moduleDependencyMatcher() override {
    return [](llvm::StringRef Name, const ModuleBinding &B) {
      return (B.Scope.empty() || B.Scope == "kernel") &&
             KernelExportRegistry::matchesImportProvider(Name, B.GuestName);
    };
  }
  ModulePublicationPreparer modulePublicationPreparer() override {
    return [this](llvm::ArrayRef<ModuleInstance> Preview)
               -> llvm::Expected<std::unique_ptr<ModulePublication>> {
      for (const auto &I : Preview)
        if (I.Binding.Space != State->CPU->addressSpace())
          return failure(
              "module publication uses a foreign kernel address space");
      auto Imported = prepareDriverImportPublication(*State, Preview);
      if (!Imported)
        return Imported.takeError();
      auto Exceptions = prepareDriverExceptionPublication(*State, Preview,
                                                          std::move(*Imported));
      if (!Exceptions)
        return Exceptions.takeError();
      return prepareDriverGuardPublication(*State, Preview,
                                           std::move(*Exceptions));
    };
  }
  llvm::Error prepareExecution(ExecutionSession &,
                               ModuleBindings &Modules) override {
    if (State->ModuleImportsLinked)
      return llvm::Error::success();
    return Modules.refreshPublication(
        State->CPU->addressSpace(),
        [this](const ModuleBinding &B, const modules::ModuleImage &I) {
          return buildModule(B, I);
        });
  }
  llvm::Error prepareCPUEntry(ExecutionSession &) override {
    if (auto E = State->Kernel->synchronizeGuestPageTables()) {
      State->Stopped = true;
      State->Result.Stop = DriverStopReason::ModelError;
      State->Result.Diagnostic = llvm::toString(std::move(E));
      return failure(State->Result.Diagnostic);
    }
    return llvm::Error::success();
  }
  llvm::Expected<EnvironmentAction> next(ExecutionSession &,
                                         ModuleBindings &) override {
    if (AwaitingCPU)
      return failure("driver task is awaiting its CPU outcome");
    if (!Action)
      if (auto E = drive())
        return std::move(E);
    auto Next = std::move(*Action);
    Action.reset();
    AwaitingCPU = Next.Kind == EnvironmentActionKind::RunCPU;
    return Next;
  }
  llvm::Error afterCPUExit(ExecutionSession &,
                           const SessionExit &Exit) override {
    if (!AwaitingCPU)
      return failure("driver has no suspended CPU action");
    if (Exit.CPU && Exit.CPU->Kind == ExecutionExitKind::ResourcePause)
      return llvm::Error::success();
    // The generic call engine captures return registers/buffers before model
    // cleanup. No callback or restored caller may overwrite them here.
    if (Call && Exit.Kind == SessionExitKind::InvocationReturn)
      return llvm::Error::success();
    AwaitingCPU = false;
    Outcome.emplace(Exit);
    if (auto E = drive())
      return E;
    if ((Call ? CallTask.done() : Task.done()) && !Session->readyToRun() &&
        !State->CPU->fault())
      return failure(State->Result.Diagnostic);
    return llvm::Error::success();
  }
  llvm::Expected<SessionExit> takeCPUExit() {
    if (!Outcome)
      return failure("driver CPU outcome was lost");
    auto Exit = std::move(*Outcome);
    Outcome.reset();
    return Exit;
  }
  llvm::Error prepareInvocationSpace() {
    if (!CallsEnabled || CallSpaceReady)
      return llvm::Error::success();
    if (auto E = State->CPU->map(InvocationStackBase, InvocationStackSize,
                                 Read | Write))
      return E;
    if (auto E = State->CPU->map(InvocationReturnPC, PageSize, Read | Execute))
      return E;
    const uint8_t Trap = ThunkTrapByte;
    if (auto E = State->CPU->writeBacking(InvocationReturnPC,
                                          llvm::ArrayRef<uint8_t>(&Trap, 1)))
      return E;
    CallSpaceReady = true;
    return llvm::Error::success();
  }
  llvm::Expected<GuestInvocationPlan>
  planInvocation(ExecutionSession &, ModuleBindings &Modules,
                 const GuestInvocationRequest &Request, uint64_t Entry,
                 llvm::StringRef Stage) override {
    const auto Context = State->Kernel->captureExecutionContext();
    if (!CallsEnabled || !CallSpaceReady || !AtAfterEntry ||
        Stage != "after-entry" || Call || AwaitingCPU || Action || Outcome ||
        !PumpInvocation || Request.ABI != IntegerCallingConvention::Win64 ||
        Context.Execution != StackBase || Context.Thread != StackBase ||
        Context.IRQL || Context.UserMemory || Context.PowerManaged ||
        Context.Call.ID || State->Result.Stop != DriverStopReason::Returned ||
        State->Result.NTStatus != 0)
      return failure("driver invocation requires the stopped after-entry "
                     "foreground context and explicit Win64 ABI");
    bool Owned = Entry >= State->Image->Base &&
                 Entry - State->Image->Base < State->Image->Size;
    for (const auto &I : Modules.inspect())
      Owned |= I.mapped() && Entry >= I.Base && Entry - I.Base < I.Size;
    if (!Owned || !State->CPU->executable(Entry))
      return failure("driver invocation target is not executable code in the "
                     "current driver or published guest module");
    return GuestInvocationPlan{InvocationStackBase, InvocationStackSize,
                               InvocationReturnPC};
  }
  llvm::Error beginInvocation(ExecutionSession &,
                              const GuestInvocationFrame &Frame) override;
  llvm::Error finishInvocation(ExecutionSession &,
                               const GuestInvocationResult &) override;
  llvm::Error finishStoppedFailure(const ExecutionRunEvent &Event) {
    if (!OrchestrationFailure.empty())
      return failure(OrchestrationFailure);
    if (Task.done())
      return llvm::Error::success();
    if (AwaitingCPU) {
      AwaitingCPU = false;
      if (Event.Reason == ExecutionRunReason::Timeout)
        Outcome.emplace(SessionExit{SessionExitKind::Timeout, {}});
      else if (Event.Reason == ExecutionRunReason::InstructionLimit)
        Outcome.emplace(SessionExit{SessionExitKind::InstructionLimit, {}});
      else
        Outcome.emplace(failure(Event.Diagnostic.empty()
                                    ? "driver execution failed before CPU entry"
                                    : Event.Diagnostic));
    }
    if (auto E = drive())
      return E;
    if (!Task.done())
      return failure("terminal driver cleanup requested further CPU execution");
    return llvm::Error::success();
  }
  std::unique_ptr<DriverSessionState> State;
  ExecutionSession *Session = nullptr;
  SessionModelHooks Policy;
  std::function<bool(const BackendFault &)> Recovery;
  std::function<DriverTask(std::unique_ptr<DriverSessionState::Execution>)>
      PumpInvocation;
  bool AtAfterEntry = false;

private:
  struct CallState {
    GuestInvocationFrame Frame;
    KernelModel::ExecutionContext Context;
    std::string Phase, Diagnostic;
    uint64_t PC, ExpectedReturnSP, ActiveStackBase, ActiveStackSize;
    uint64_t RunInstructions, RunAllowance;
    bool Stopped, AdmissionBoundary, ProcessorReadAdmitted;
    std::optional<uint64_t> ReturnValue;
    bool ResultConsumed = false;
  };
  llvm::Error drive() {
    auto &ActiveTask = Call ? CallTask : Task;
    auto &ActiveControl = Call ? CallControl : Control;
    if (!ActiveTask.done()) {
      if (!ActiveControl.Leaf)
        return failure("driver task has no suspended continuation");
      ActiveControl.Action.reset();
      ActiveControl.Leaf.resume();
    }
    if (ActiveControl.Action) {
      Action = std::move(ActiveControl.Action);
      ActiveControl.Action.reset();
      return llvm::Error::success();
    }
    if (!ActiveTask.done())
      return failure("driver task suspended without an action");
    auto &Consumed = Call ? Call->ResultConsumed : ResultConsumed;
    if (!Consumed) {
      Consumed = true;
      if (auto E = ActiveTask.takeResult()) {
        OrchestrationFailure = llvm::toString(std::move(E));
        return failure(OrchestrationFailure);
      }
    }
    Action =
        EnvironmentAction{State->Result.Stop == DriverStopReason::Returned
                              ? EnvironmentActionKind::Completed
                              : EnvironmentActionKind::Failed,
                          0, State->Result.Phase, State->Result.Diagnostic};
    if (State->Result.Stop == DriverStopReason::InstructionLimit)
      Action->FailureReason = ExecutionRunReason::InstructionLimit;
    else if (State->Result.Stop == DriverStopReason::EventLimit)
      Action->FailureReason = ExecutionRunReason::EventLimit;
    else if (State->Result.Stop == DriverStopReason::Timeout)
      Action->FailureReason = ExecutionRunReason::Timeout;
    return llvm::Error::success();
  }
  bool AwaitingCPU = false, ResultConsumed = false;
  bool CallsEnabled = false, CallSpaceReady = false;
  std::optional<CallState> Call;
  std::string OrchestrationFailure;
  std::optional<llvm::Expected<SessionExit>> Outcome;
  std::optional<EnvironmentAction> Action;
  DriverTask::Control Control;
  DriverTask Task;
  DriverTask::Control CallControl;
  DriverTask CallTask;
};
llvm::Error
DriverExecutionEnvironment::beginInvocation(ExecutionSession &,
                                            const GuestInvocationFrame &Frame) {
  if (Call || !AtAfterEntry || Frame.StackBase != InvocationStackBase ||
      Frame.StackSize != InvocationStackSize ||
      Frame.ReturnPC != InvocationReturnPC)
    return failure(
        "driver invocation frame does not match its admitted private plan");
  auto &S = *State;
  const auto Context = S.Kernel->captureExecutionContext();
  Call.emplace(CallState{Frame, Context, S.Result.Phase, S.Result.Diagnostic,
                         S.Result.PC, S.ExpectedReturnSP, S.ActiveStackBase,
                         S.ActiveStackSize, S.RunInstructions, S.RunAllowance,
                         S.Stopped, S.AdmissionBoundary,
                         S.ProcessorReadAdmitted, S.InvocationReturn});
  // Entry's stack memory stays retired. Retain its stopped selector identity
  // so model restoration is validated without reviving entry-local pointers.
  S.Kernel->enterExecution(Context.Execution, Context.Thread, Context.Call);
  if (auto E = S.Kernel->activateStack(Frame.StackBase, Frame.StackSize))
    return E;
  if (auto E =
          S.Kernel->inheritExecutionContext(Frame.StackBase, Context.Execution))
    return E;
  auto Execution = std::make_unique<DriverSessionState::Execution>();
  Execution->Base = Frame.StackBase;
  Execution->Size = Frame.StackSize;
  Execution->InitialSP = Frame.StackPointer;
  Execution->PC = Frame.Entry;
  Execution->EntryIRQL = Context.IRQL;
  Execution->ThreadKey = Context.Thread;
  Execution->Phase = "invocation";
  if (S.Options.Scheduling) {
    Execution->Slice = std::make_shared<DriverSessionState::TimeSlice>(
        DriverSessionState::TimeSlice{
            S.Options.Scheduling->QuantumInstructions});
    auto Order = S.Kernel->issueReadyOrder();
    if (!Order)
      return Order.takeError();
    Execution->Slice->ReadyOrder = *Order;
  }
  // The generic engine already installed ABI arguments and payload. Adopting
  // this frame must never call NewExecution/ABI.prepareCall a second time.
  CallTask = PumpInvocation(std::move(Execution));
  CallTask.bind(CallControl);
  return llvm::Error::success();
}
llvm::Error DriverExecutionEnvironment::finishInvocation(
    ExecutionSession &, const GuestInvocationResult &Result) {
  if (!Call || !AwaitingCPU || !Session->hasInvocationReturn())
    return failure("driver has no pending invocation return");
  auto &S = *State;
  const auto Active = S.Kernel->captureExecutionContext();
  if (Active.Execution != Call->Frame.StackBase ||
      Active.Thread != Call->Context.Thread)
    return failure(
        "driver invocation returned from a foreign callback or thread");
  if (auto E = S.Kernel->validateExecutionReturn(Call->Frame.StackBase,
                                                 Call->Context.IRQL))
    return E;
  // Finish the same pump after the generic result capture. It retires the call
  // frame and validates model resources, but cannot execute another CPU action.
  S.Stopped = true;
  S.InvocationReturn = Result.Value;
  S.Result.Stop = DriverStopReason::Returned;
  S.Result.Diagnostic.clear();
  AwaitingCPU = false;
  Outcome.emplace(SessionExit{SessionExitKind::InvocationReturn, {}});
  if (auto E = drive())
    return E;
  if (!CallTask.done() || S.Result.Stop != DriverStopReason::Returned)
    return failure(S.Result.Diagnostic.empty()
                       ? "driver call cleanup requested further execution"
                       : S.Result.Diagnostic);
  if (auto E = S.Kernel->restoreExecutionContext(Call->Context))
    return E;
  S.Result.Phase = Call->Phase;
  S.Result.PC = Call->PC;
  S.Result.Diagnostic = Call->Diagnostic;
  S.ExpectedReturnSP = Call->ExpectedReturnSP;
  S.ActiveStackBase = Call->ActiveStackBase;
  S.ActiveStackSize = Call->ActiveStackSize;
  S.RunInstructions = Call->RunInstructions;
  S.RunAllowance = Call->RunAllowance;
  S.Stopped = Call->Stopped;
  S.AdmissionBoundary = Call->AdmissionBoundary;
  S.ProcessorReadAdmitted = Call->ProcessorReadAdmitted;
  S.InvocationReturn = Call->ReturnValue;
  Call.reset();
  CallTask = {};
  CallControl = {};
  Action.reset();
  Outcome.reset();
  return llvm::Error::success();
}
DriverTask runDriver(DriverExecutionEnvironment &Environment) {
  auto &State = Environment.State;
  auto *Session = Environment.Session;
  const auto &Options = State->Options;
  const auto ForegroundReadyPolicy = State->ForegroundReadyPolicy;
  auto &Exports = State->Exports;
  auto &Image = State->Image;
  auto &Reference = State->Reference;
  auto &Result = State->Result;
  auto &CPU = *State->CPU;
  const auto &ABI = State->ABI;
  auto &Guard = *State->Guard;
  auto &Kernel = *State->Kernel;
  // Publications append immutable image views while suspended. Reacquire the
  // current planner after each yield; never borrow the replaced host object.
  auto Exceptions = [&]() -> X64SEH & { return *State->Exceptions; };
  auto &Policy = State->Policy;
  auto &CPUID = State->CPUID;
  auto &ExpectedReturnSP = State->ExpectedReturnSP;
  auto &ActiveStackBase = State->ActiveStackBase;
  auto &ActiveStackSize = State->ActiveStackSize;
  auto &Stopped = State->Stopped;
  auto &InvocationReturn = State->InvocationReturn;
  auto &Pending = State->Pending;
  auto &PendingGuard = State->PendingGuard;
  using EnvironmentRead = DriverSessionState::EnvironmentRead;
  auto &PendingEnvironmentRead = State->PendingEnvironmentRead;
  auto &ProcessorViewMapped = State->ProcessorViewMapped;
  auto &ProcessorReadAdmitted = State->ProcessorReadAdmitted;
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
  auto &Resources = *State->Budget;
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
  auto &RunInstructions = State->RunInstructions;
  auto &RunAllowance = State->RunAllowance;
  auto &AdmissionBoundary = State->AdmissionBoundary;
  SessionModelHooks Hooks;
  Hooks.BeforeInstruction = [&](uint64_t Address,
                                uint32_t Size) -> InstructionAdmission {
    ProcessorReadAdmitted = false;
    if (Stopped)
      return InstructionAdmission::Stop;
    Result.PC = Address;
    auto StackPointer = CPU.reg(X64Register::SP);
    if (!StackPointer) {
      Stop(DriverStopReason::EngineError,
           llvm::toString(StackPointer.takeError()));
      return InstructionAdmission::Stop;
    }
    if (*StackPointer < ActiveStackBase ||
        *StackPointer >= ActiveStackBase + ActiveStackSize) {
      Stop(DriverStopReason::ModelError, session_diagnostic::StackRange);
      return InstructionAdmission::Stop;
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
      return InstructionAdmission::Stop;
    }
    if (Options.Scheduling && RunInstructions >= RunAllowance) {
      AdmissionBoundary = true;
      CPU.stop();
      return InstructionAdmission::Stop;
    }
    if (Address == GuardCheckThunk || Address == GuardDispatchThunk) {
      PendingGuard = Address;
      CPU.stop();
      return InstructionAdmission::Stop;
    }
    if (const auto *Export = Exports.lookup(Address)) {
      if (!KernelModel::argumentCount(*Export)) {
        Stop(DriverStopReason::UnsupportedAPI,
             session_diagnostic::UnsupportedImport + Export->Module + "!" +
                 Export->Name);
        return InstructionAdmission::Stop;
      }
      Pending = Export;
      CPU.stop();
      return InstructionAdmission::Stop;
    }
    const bool ReferenceCode = Reference && Reference->contains(Address);
    if (ReferenceCode && !Kernel.isSystemThreadEntry(Address)) {
      Stop(DriverStopReason::UnsupportedAPI,
           "execution inside reference kernel code requires an OS model");
      return InstructionAdmission::Stop;
    }
    if (!Resources.hasInstructions()) {
      Stop(DriverStopReason::InstructionLimit, runtime::InstructionLimit);
      return InstructionAdmission::Stop;
    }
    if (!Size || Size > MaxInstructionSize) {
      Stop(DriverStopReason::UnsupportedInstruction,
           session_diagnostic::InstructionExtent);
      return InstructionAdmission::Stop;
    }
    if (!ReferenceCode)
      if (auto E = Kernel.validateGuestAccess(Address, Size, false)) {
        Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
        return InstructionAdmission::Stop;
      }
    std::array<uint8_t, MaxInstructionSize> Bytes{};
    if (auto E = CPU.fetch(
            Address, llvm::MutableArrayRef<uint8_t>(Bytes.data(), Size))) {
      Stop(DriverStopReason::MemoryFault, llvm::toString(std::move(E)));
      return InstructionAdmission::Stop;
    }
    if (ReferenceCode) {
      const auto LiveBytes = llvm::ArrayRef<uint8_t>(Bytes.data(), Size);
      if (!Reference->matchesCode(Address, LiveBytes)) {
        Stop(DriverStopReason::ModelError,
             "reference thread entry differs from authenticated code");
        return InstructionAdmission::Stop;
      }
      auto Transfer = Policy.isReferenceThreadTransfer(LiveBytes, Address);
      if (!Transfer) {
        Stop(DriverStopReason::UnsupportedInstruction,
             llvm::toString(Transfer.takeError()));
        return InstructionAdmission::Stop;
      }
      if (!*Transfer) {
        Stop(DriverStopReason::UnsupportedAPI,
             "execution inside reference kernel code requires an OS model");
        return InstructionAdmission::Stop;
      }
      // Execute and account for the actual instruction below. This permission
      // is only an authenticated code fetch; it grants no guest data access.
    }
    auto Inspection =
        Policy.inspect(llvm::ArrayRef<uint8_t>(Bytes.data(), Size), Address);
    if (!Inspection) {
      Stop(DriverStopReason::UnsupportedInstruction,
           llvm::toString(Inspection.takeError()));
      return InstructionAdmission::Stop;
    }
    if (*Inspection &&
        (**Inspection).Source ==
            WindowsX64ExecutionPolicy::Action::Kind::ReadCurrentThread) {
      if (!ProcessorViewMapped) {
        // Materialize the processor field outside the running backend, then
        // retry this instruction once. It has not consumed its budget yet.
        PendingEnvironmentRead = EnvironmentRead{**Inspection, Address};
        CPU.stop();
        return InstructionAdmission::Stop;
      }
      ProcessorReadAdmitted = true;
      return InstructionAdmission::Execute;
    }
    // CPUID and MSR admission depend on live input registers. Defer accounting
    // until the stopped-CPU lookup confirms that the query has a model.
    const bool DeferredQuery =
        *Inspection &&
        ((**Inspection).Source ==
             WindowsX64ExecutionPolicy::Action::Kind::ReadCPUID ||
         (**Inspection).Source ==
             WindowsX64ExecutionPolicy::Action::Kind::ReadMSR);
    if (*Inspection) {
      if (Size > UINT64_MAX - Address) {
        Stop(DriverStopReason::MemoryFault,
             session_diagnostic::EnvironmentReadRange);
        return InstructionAdmission::Stop;
      }
      PendingEnvironmentRead = EnvironmentRead{**Inspection, Address + Size};
      return DeferredQuery ? InstructionAdmission::Defer
                           : InstructionAdmission::Emulate;
    }
    return InstructionAdmission::Execute;
  };
  Hooks.Read = [&](uint64_t Address, uint32_t Size) {
    if (Stopped)
      return;
    if (ProcessorReadAdmitted && Size == PointerSize &&
        Address == ProcessorEnvironmentBase + windows::GSCurrentThreadOffset)
      return;
    if (Address < ThunkBase + ThunkSize &&
        (Address >= ThunkBase || Size > ThunkBase - Address)) {
      const uint64_t Slot =
          Address < ThunkBase
              ? ThunkBase
              : ThunkBase + (Address - ThunkBase) / ThunkStride * ThunkStride;
      const auto *Import = Exports.lookup(Slot);
      Stop(DriverStopReason::UnsupportedAPI,
           llvm::formatv(session_diagnostic::ImportReadDetail,
                         session_diagnostic::ImportRead,
                         Import ? Import->Module : "<unbound>",
                         Import ? Import->Name : "<unbound>",
                         llvm::utohexstr(Address), Size)
               .str());
      return;
    }
    if (auto E = Kernel.validateGuestAccess(Address, Size, false))
      Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
  };
  Hooks.Write = [&](uint64_t Address, uint32_t Size, uint64_t Value) {
    if (Stopped)
      return;
    if (auto E = Kernel.validateGuestCPUWrite(Address, Size)) {
      Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
      return;
    }
    if (Stopped || ((Address >= StackBase && Address < StackBase + StackSize) ||
                    (Address >= InvocationStackBase &&
                     Address < InvocationStackBase + InvocationStackSize) ||
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
  auto Recovery = [&](const BackendFault &Fault) {
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
  Environment.Policy = std::move(Hooks);
  Environment.Recovery = std::move(Recovery);
  auto RecordInstruction = [&]() {
    auto Admitted = Session->admitDeferredInstruction();
    if (!Admitted) {
      Stop(DriverStopReason::EngineError, llvm::toString(Admitted.takeError()));
      return false;
    }
    if (*Admitted) {
      Stop(**Admitted == SessionExitKind::Timeout
               ? DriverStopReason::Timeout
               : DriverStopReason::InstructionLimit,
           **Admitted == SessionExitKind::Timeout ? runtime::Timeout
                                                  : runtime::InstructionLimit);
      return false;
    }
    return true;
  };

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
  using ExceptionExecution = DriverSessionState::ExceptionExecution;
  using TimeSlice = DriverSessionState::TimeSlice;
  using Execution = DriverSessionState::Execution;
  using ClockCall = DriverSessionState::ClockCall;
  auto &Waiting = State->Waiting;
  auto &ClockCalls = State->ClockCalls;
  auto &AccountedInstructions = State->AccountedInstructions;
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
  auto &StackMapped = State->StackMapped;
  auto &StackInUse = State->StackInUse;
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
    const bool ServiceThread = Kernel.isServiceThreadEntry(PC);
    if (!CPU.executable(PC) ||
        ((PC >= ThunkBase && PC < ThunkBase + ThunkSize) && !ProviderCallback &&
         !ServiceThread))
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
    if (Kernel.isServiceImagePC(ControlPC))
      return failure("driver services: child exception dispatch requires a "
                     "multi-image unwind owner");
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
      auto Nested = Exceptions().beginNested(Raised.Code, Search,
                                             {Frame.Base, Frame.Size},
                                             Parent.Dispatch, Parent.Next);
      if (!Nested)
        return Nested.takeError();
      State->Dispatch = std::move(*Nested);
    } else {
      State->Dispatch =
          Exceptions().begin(Raised.Code, Search, {Frame.Base, Frame.Size});
    }
    auto Next = Exceptions().advance(State->Dispatch);
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
  auto ReadyExecution = [&](const Execution &Frame) {
    return Kernel.readyThread(Frame.ThreadKey, Frame.Slice->ReadyOrder,
                              Frame.PausedModel ? &*Frame.PausedModel
                                                : nullptr);
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
        Consider(ReadyExecution(*Frame));
    return Best;
  };
  auto HasHigherReadyThread = [&](const Execution &Frame) {
    const auto Best = NextReadyThread();
    return Best && Best->Priority > Kernel.threadPriority(Frame.ThreadKey);
  };
  auto RunExecution = [&](Execution &Frame) -> DriverTask {
    const Execution *Owner = &Frame;
    while (Owner->ExceptionCallback && Owner->Parent)
      Owner = Owner->Parent.get();
    Kernel.enterExecution(Frame.Base, Frame.ThreadKey,
                          Owner->ReturnToken.ID
                              ? Owner->ReturnToken
                              : Kernel.scheduledGuestCall(Owner->ID));
    if (Frame.Context) {
      if (auto E = CPU.restoreContext(*Frame.Context))
        co_return E;
      if (Frame.ResumeValue) {
        if (auto E = CPU.setReg(X64Register::AX, *Frame.ResumeValue))
          co_return E;
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
      co_return SaveBoundary(Frame.PC);
    if (Frame.SynchronizationObject && !Frame.SynchronizationEntered) {
      auto Entered = Kernel.beginFrameworkCallback(Frame.SynchronizationObject);
      if (!Entered)
        co_return Entered.takeError();
      if (!*Entered) {
        Frame.Wait = Kernel.takeWait();
        if (!Frame.Wait ||
            Frame.Wait->Type != KernelModel::Wait::Kind::FrameworkCallback)
          co_return failure(session_diagnostic::MissingFrameworkLockWait);
        auto Context = CPU.saveContext();
        if (!Context)
          co_return Context.takeError();
        Frame.Context = std::move(*Context);
        if (Options.Scheduling)
          Frame.PausedModel = Kernel.captureExecutionContext();
        if (Frame.ID)
          if (auto E = Kernel.suspendScheduled(Frame.ID))
            co_return E;
        co_return llvm::Error::success();
      }
      Frame.SynchronizationEntered = true;
      if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
        co_return E;
    }
    if (!Frame.CallbackEntered) {
      if (!Frame.ExceptionCallback) {
        auto Managed = Kernel.enterFrameworkCallback(
            Frame.ID, Frame.ReturnToken, Frame.RequestIRP);
        if (!Managed)
          co_return Managed.takeError();
        Frame.PowerManagedCallback |= *Managed;
      }
      Frame.CallbackEntered = true;
    }
    Kernel.setFrameworkCallbackContext(Frame.PowerManagedCallback);
    auto IRQL = CPU.reg(X64Register::CR8);
    if (!IRQL)
      co_return IRQL.takeError();
    if (*IRQL != Kernel.currentIRQL())
      co_return failure(session_diagnostic::SavedIRQL);
    if (ProcessorViewMapped)
      if (auto E = RefreshProcessorView())
        co_return E;
    Result.Phase = Frame.Phase;
    Result.PC = Frame.PC;
    ExpectedReturnSP = Frame.InitialSP + PointerSize;
    ActiveStackBase = Frame.Base;
    ActiveStackSize = Frame.Size;
    Stopped = false;
    InvocationReturn.reset();
    Frame.Boundary = false;
    // An exit-notification traversal finishes before the nonreturning
    // PsTerminateSystemThread operation commits. Never resume its caller PC.
    if (auto Status = Kernel.takeThreadTermination()) {
      if (Frame.Parent)
        co_return failure(session_diagnostic::NestedThreadTermination);
      Frame.ThreadTerminated = true;
      InvocationReturn = *Status;
      Result.Calls[Frame.WaitEvent].Detail =
          session_diagnostic::ThreadTerminated;
      Stop(DriverStopReason::Returned, "");
      co_return llvm::Error::success();
    }
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
    while (!Stopped) {
      if (Options.Scheduling && Ran)
        co_return SaveBoundary(NextPC);
      const uint64_t Remaining = Resources.remainingMicroseconds();
      if (!Remaining) {
        Stop(DriverStopReason::Timeout, runtime::Timeout);
        break;
      }
      Pending = nullptr;
      PendingGuard = 0;
      PendingEnvironmentRead.reset();
      co_yield EnvironmentAction{
          EnvironmentActionKind::RunCPU, NextPC, Result.Phase, {}};
      auto Exit = Environment.takeCPUExit();
      std::string Message;
      if (!Exit)
        Message = llvm::toString(Exit.takeError());
      else if (Exit->CPU) {
        if (Exit->CPU->Kind == ExecutionExitKind::UnsupportedOperation)
          Message =
              llvm::toString(llvm::make_error<UnsupportedExecutionError>());
        else if (Exit->CPU->Kind == ExecutionExitKind::ServiceRequest)
          Message = diagnostic::PendingService;
        else if (Exit->CPU->Kind == ExecutionExitKind::ResourcePause)
          Message = diagnostic::ResourcePausePending;
        else
          Message = Exit->CPU->Diagnostic;
      }
      if (!Message.empty() && !Stopped)
        Stop(CPU.hasMemoryFault()   ? DriverStopReason::MemoryFault
             : CPU.hasDeviceError() ? DriverStopReason::ModelError
                                    : DriverStopReason::EngineError,
             Message);
      if (Exit && !Stopped) {
        if (Exit->Kind == SessionExitKind::Timeout)
          Stop(DriverStopReason::Timeout, runtime::Timeout);
        else if (Exit->Kind == SessionExitKind::InstructionLimit)
          Stop(DriverStopReason::InstructionLimit, runtime::InstructionLimit);
      }
      Ran = true;
      if (Stopped)
        break;
      if (Exit && !Pending && !PendingGuard && !PendingEnvironmentRead &&
          !AdmissionBoundary &&
          (Exit->Kind == SessionExitKind::Quantum ||
           (Exit->Kind == SessionExitKind::CPU && Exit->CPU &&
            Exit->CPU->Kind == ExecutionExitKind::Stopped))) {
        auto PC = CPU.reg(X64Register::PC);
        if (!PC)
          co_return PC.takeError();
        NextPC = *PC;
        continue;
      }
      if (AdmissionBoundary)
        co_return SaveBoundary(Result.PC);
      // An import observes time after all preceding machine instructions.
      // Its implementation and any nested continuation remain indivisible.
      if (Options.Scheduling && Pending && RunInstructions)
        co_return SaveBoundary(Result.PC);
      std::optional<BackendFault> Fault;
      if (Exit && Exit->CPU &&
          Exit->CPU->Kind == ExecutionExitKind::RecoverableFault) {
        auto OwnedFault = Session->takeRecoverableFault();
        if (!OwnedFault)
          co_return OwnedFault.takeError();
        Fault = std::move(*OwnedFault);
      }
      if (Fault) {
        auto Repaired = Kernel.repairProcessImageWriteFault(*Fault);
        if (!Repaired) {
          Stop(DriverStopReason::ModelError,
               llvm::toString(Repaired.takeError()));
          break;
        }
        if (*Repaired) {
          // The protected-page access had no instruction effect. Resolve its
          // original image page, then execute the same instruction again.
          NextPC = Fault->PC;
          continue;
        }
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
          co_return failure(exceptions::LostRecoverableContext);
        auto Registers = CaptureRegisters(Fault->PC, true);
        if (!Registers)
          co_return Registers.takeError();
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
            co_return failure(exceptions::LostRecoverableContext);
          Raised.Code = SIMD->Code;
          Raised.Parameters.assign(SIMD->Parameters.begin(),
                                   SIMD->Parameters.end());
        } else if (!Status)
          Raised.Parameters = {
              *Fault->Access == BackendAccessKind::Write ? 1ULL : 0ULL,
              *Fault->Address};
        auto Resume = BeginException(Frame, std::move(Raised), Fault->PC, true);
        if (!Resume)
          co_return Resume.takeError();
        if (!*Resume)
          co_return llvm::Error::success();
        NextPC = **Resume;
        continue;
      }
      if (CPU.timedOut()) {
        Stop(DriverStopReason::Timeout, runtime::Timeout);
        break;
      }
      if (PendingEnvironmentRead) {
        const auto &Action = PendingEnvironmentRead->Request;
        if (Action.Source == WindowsX64ExecutionPolicy::Action::Kind::ReadMSR) {
          auto Index = CPU.reg(X64Register::CX);
          if (!Index)
            co_return Index.takeError();
          auto Value = Kernel.readModelSpecificRegister(uint32_t(*Index));
          if (!Value) {
            Stop(DriverStopReason::UnsupportedInstruction,
                 llvm::toString(Value.takeError()));
            break;
          }
          if (!RecordInstruction())
            break;
          Kernel.recordUnpackEnvironmentRead();
          auto Before = CPU.saveContext();
          if (!Before)
            co_return Before.takeError();
          if (auto E = CPU.setReg(X64Register::AX, uint32_t(*Value)))
            co_return llvm::joinErrors(std::move(E),
                                       CPU.restoreContext(**Before));
          if (auto E = CPU.setReg(X64Register::DX, uint32_t(*Value >> 32)))
            co_return llvm::joinErrors(std::move(E),
                                       CPU.restoreContext(**Before));
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        if (Action.Source == WindowsX64ExecutionPolicy::Action::Kind::ReadCR4) {
          Kernel.recordUnpackEnvironmentRead();
          if (!Action.Destination)
            co_return failure("CR4 read has no full-width destination");
          if (auto E = CPU.setReg(*Action.Destination, Kernel.guestCR4()))
            co_return E;
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        if (Action.Source == WindowsX64ExecutionPolicy::Action::Kind::ReadCR3) {
          Kernel.recordUnpackEnvironmentRead();
          auto Root = Kernel.guestCR3();
          if (!Root)
            co_return Root.takeError();
          if (!Action.Destination)
            co_return failure("CR3 read has no full-width destination");
          if (auto E = CPU.setReg(*Action.Destination, *Root))
            co_return E;
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        if (Action.Source ==
            WindowsX64ExecutionPolicy::Action::Kind::ReadCPUID) {
          auto Leaf = CPU.reg(X64Register::AX);
          if (!Leaf)
            co_return Leaf.takeError();
          auto Subleaf = CPU.reg(X64Register::CX);
          if (!Subleaf)
            co_return Subleaf.takeError();
          const auto Query = CPUID->query(uint32_t(*Leaf), uint32_t(*Subleaf));
          if (!Query) {
            Stop(DriverStopReason::UnsupportedInstruction,
                 "unmodeled CPUID query: EAX=0x" +
                     llvm::utohexstr(uint32_t(*Leaf)) + " ECX=0x" +
                     llvm::utohexstr(uint32_t(*Subleaf)));
            break;
          }
          if (!RecordInstruction())
            break;
          Kernel.recordUnpackEnvironmentRead();
          // CPUID writes four zero-extended 32-bit results and preserves flags.
          // Publish the register set atomically at the stopped CPU boundary.
          auto Before = CPU.saveContext();
          if (!Before)
            co_return Before.takeError();
          constexpr X64Register Destinations[] = {
              X64Register::AX, X64Register::BX, X64Register::CX,
              X64Register::DX};
          for (unsigned I = 0; I < 4; ++I)
            if (auto E = CPU.setReg(Destinations[I], (*Query)[I]))
              co_return llvm::joinErrors(std::move(E),
                                         CPU.restoreContext(**Before));
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        if (Action.Source ==
            WindowsX64ExecutionPolicy::Action::Kind::ReadDisabledShadowStack) {
          // This fixed profile disables CR4.CET. RDSSPD/Q preserve all state,
          // including the upper destination bits, after normal accounting.
          Kernel.recordUnpackEnvironmentRead();
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        if (Action.Source ==
                WindowsX64ExecutionPolicy::Action::Kind::ReadTimestamp ||
            Action.Source == WindowsX64ExecutionPolicy::Action::Kind::
                                 ReadTimestampAndProcessor) {
          auto Timestamp = ExecutionTime();
          if (!Timestamp)
            co_return Timestamp.takeError();
          Kernel.recordUnpackEnvironmentRead();
          if (auto E = CPU.setReg(X64Register::AX, uint32_t(*Timestamp)))
            co_return E;
          if (auto E = CPU.setReg(X64Register::DX, *Timestamp >> 32))
            co_return E;
          if (Action.Source == WindowsX64ExecutionPolicy::Action::Kind::
                                   ReadTimestampAndProcessor)
            if (auto E = CPU.setReg(X64Register::CX, ProcessorNumber))
              co_return E;
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        if (Action.Source ==
            WindowsX64ExecutionPolicy::Action::Kind::ReadCurrentThread) {
          if (auto E = PrepareProcessorView())
            co_return E;
          NextPC = PendingEnvironmentRead->NextPC;
          continue;
        }
        if (Action.Source != WindowsX64ExecutionPolicy::Action::Kind::ReadIRQL)
          co_return failure("initializer-only CPU action in driver execution");
        auto IRQL = CPU.reg(X64Register::CR8);
        if (!IRQL)
          co_return IRQL.takeError();
        if (*IRQL != Kernel.currentIRQL())
          co_return failure(session_diagnostic::CR8Read);
        // MOV only changes its destination and instruction pointer. Preserve
        // flags, other registers and the already counted instruction.
        if (!Action.Destination)
          co_return failure(session_diagnostic::IRQLDestination);
        if (auto E = CPU.setReg(*Action.Destination, *IRQL))
          co_return E;
        NextPC = PendingEnvironmentRead->NextPC;
        continue;
      }
      if (PendingGuard) {
        auto Target =
            CPU.reg(PendingGuard == GuardCheckThunk ? X64Register::CX
                                                    : X64Register::AX);
        if (!Target)
          co_return Target.takeError();
        if (Exports.lookup(*Target))
          if (auto E = Guard.registerExportTarget(*Target))
            co_return E;
        if (auto E = Guard.validateTarget(*Target))
          co_return E;
        if (PendingGuard == GuardCheckThunk) {
          auto SP = CPU.reg(X64Register::SP);
          if (!SP)
            co_return SP.takeError();
          if (auto E = Kernel.validateGuestAccess(*SP, PointerSize, false))
            co_return E;
          auto Return = CPU.readInteger(*SP, PointerSize);
          if (!Return)
            co_return Return.takeError();
          if (!CPU.executable(*Return))
            co_return failure(session_diagnostic::CFGReturnAddress);
          if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
            co_return E;
          NextPC = *Return;
        } else {
          NextPC = *Target;
        }
        continue;
      }
      if (!Pending) {
        Stop(DriverStopReason::EngineError, session_diagnostic::UnknownCPUExit);
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
        co_return SP.takeError();
      auto LastArgument =
          ABI.argumentLocation(*SP, MaxVariableAPIArguments - 1);
      if (!LastArgument) {
        Stop(DriverStopReason::ModelError,
             llvm::toString(LastArgument.takeError()));
        break;
      }
      unsigned Count = *KernelModel::argumentCount(*Pending);
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
      Result.Calls.back().ReturnAddress = *ReturnPC;
      if (auto E = Kernel.validateServiceAPI(*ReturnPC, Pending->Name)) {
        Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
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
      auto ReadContext = [&] { return collectDriverContext(CPU); };
      auto SetContextFlags = [&](uint64_t Flags) {
        return CPU.setReg(X64Register::FLAGS, Flags);
      };
      const KernelContextCapture Context{ReadContext, SetContextFlags};
      auto Value =
          Kernel.call(*Pending, FixedArguments, ReadVariableArgument, &Context);
      if (!Kernel.callDetail().empty())
        Result.Calls.back().Detail = Kernel.callDetail();
      if (!Value) {
        std::optional<uint32_t> RaisedCode;
        std::optional<DriverBugCheck> BugCheck;
        auto OtherError = llvm::handleErrors(
            Value.takeError(),
            [&](const KernelGuestException &Exception) {
              RaisedCode = Exception.code();
            },
            [&](const KernelGuestBugCheck &Request) {
              BugCheck = Request.request();
            });
        if (!OtherError && BugCheck) {
          Result.BugCheck = *BugCheck;
          const std::string Detail =
              "guest requested bug check 0x" + llvm::utohexstr(BugCheck->Code);
          Result.Calls.back().Detail = Detail;
          Stop(DriverStopReason::BugCheck, Detail);
          break;
        }
        if (!OtherError && RaisedCode) {
          const std::string Detail = session_diagnostic::RaisedException +
                                     llvm::utohexstr(*RaisedCode);
          Result.Calls.back().Detail = Detail;
          // A model-raised exception starts at a normal API stop. Never use
          // this path to reset the backend's retained execution fault.
          if (CPU.fault() || CPU.hasDeviceError())
            co_return failure(session_diagnostic::ExceptionCPU);
          auto Registers = CaptureRegisters(*ReturnPC);
          if (!Registers)
            co_return Registers.takeError();
          Registers->GPR[seh::StackRegister] = *SP + PointerSize;
          X64SEH::Exception Raised{*Registers, *RaisedCode, 0, *ReturnPC, {}};
          auto Resume =
              BeginException(Frame, std::move(Raised), *ReturnPC - 1, false);
          if (!Resume)
            co_return Resume.takeError();
          if (!*Resume)
            co_return llvm::Error::success();
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
          co_return E;
      if (auto E = RefreshWaiters())
        co_return E;
      if (auto Status = Kernel.takeThreadTermination()) {
        if (Frame.Parent)
          co_return failure(session_diagnostic::NestedThreadTermination);
        Frame.ThreadTerminated = true;
        InvocationReturn = *Status;
        Result.Calls.back().Detail = session_diagnostic::ThreadTerminated;
        Stop(DriverStopReason::Returned, "");
        break;
      }
      if (auto Call = Kernel.takeGuestCall()) {
        Frame.DeferredThreadTermination =
            Kernel.hasDeferredThreadTermination(Call->Token);
        Frame.ChildCall = std::move(*Call);
        // A framework call can publish both a nested cancellation callback
        // and a synchronous queue wait. The wait belongs to this caller, not
        // to the child callback's next imported function call.
        Frame.Wait = Kernel.takeWait();
        Frame.WaitEvent = Result.Calls.size() - 1;
        Frame.PC = *ReturnPC;
        if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
          co_return E;
        auto Context = CPU.saveContext();
        if (!Context)
          co_return Context.takeError();
        Frame.Context = std::move(*Context);
        co_return llvm::Error::success();
      }
      if (auto Wait = Kernel.takeWait()) {
        Frame.Wait = *Wait;
        Frame.WaitEvent = Result.Calls.size() - 1;
        Frame.PC = *ReturnPC;
        if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
          co_return E;
        auto Context = CPU.saveContext();
        if (!Context)
          co_return Context.takeError();
        Frame.Context = std::move(*Context);
        if (Options.Scheduling)
          Frame.PausedModel = Kernel.captureExecutionContext();
        if (Frame.ID)
          if (auto E = Kernel.suspendScheduled(Frame.ID))
            co_return E;
        co_return llvm::Error::success();
      }
      Result.Calls.back().Result = *Value;
      // Direct interrupt-lock APIs change the running frame's IRQL without a
      // new guest callback. The next instruction must observe that same CR8.
      if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
        co_return E;
      if (auto E = CPU.setReg(X64Register::AX, *Value))
        co_return E;
      if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
        co_return E;
      NextPC = *ReturnPC;
    }
    co_return llvm::Error::success();
  };
  auto Pump = [&](std::unique_ptr<Execution> Current,
                  const std::string &ParentPhase,
                  bool ForegroundBoundary = false, uint64_t RequiredIRP = 0,
                  std::optional<uint64_t> IdleUntil100ns = {}) -> DriverTask {
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
            co_return llvm::Error::success();
          }
          Current->PausedModel.reset();
        }
        if (Current->ResumeIRQL) {
          if (auto E = Kernel.restoreWaitIRQL(*Current->ResumeIRQL)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
          Current->ResumeIRQL.reset();
        }
        if (Current->PendingEntry) {
          if (auto E = Kernel.beginGuestCall(*Current->PendingEntry)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
          Current->PendingEntry.reset();
        }
        if (!Current->ChildCall)
          if (auto E = co_await RunExecution(*Current)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
        if (Options.Scheduling) {
          const uint64_t Count = Result.Instructions - AccountedInstructions;
          Current->Slice->Remaining -=
              std::min(Current->Slice->Remaining, Count);
          if (auto E = AdvanceClock()) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
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
            co_return llvm::Error::success();
          }
          if (!Current->Slice->Remaining) {
            auto Order = Kernel.issueReadyOrder();
            if (!Order) {
              ModelFailure(Order.takeError());
              co_return llvm::Error::success();
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
              co_return llvm::Error::success();
            }
            continue;
          }
          auto Child =
              NewExecution(State.Next.State.HandlerPC, {}, Current->Phase,
                           Current->ID, true, seh::RecordsSize);
          if (!Child) {
            ModelFailure(Child.takeError());
            co_return llvm::Error::success();
          }
          if (auto E = Kernel.inheritExecutionContext((*Child)->Base,
                                                      Current->Base)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
          auto Records = X64SEH::encodeRecords(
              State.Raised, (*Child)->Base + (*Child)->Size - seh::RecordsSize);
          if (!Records) {
            ModelFailure(Records.takeError());
            co_return llvm::Error::success();
          }
          if (auto E =
                  CPU.write((*Child)->Base + (*Child)->Size - seh::RecordsSize,
                            *Records)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
          if (auto E = PrepareExceptionCallback(**Child, *Current)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
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
                co_return llvm::Error::success();
              }
            Waiting.push_back(std::move(Current));
            continue;
          }
          auto Call = std::move(*Current->ChildCall);
          Current->ChildCall.reset();
          // A returned callback can prepare the next stage before its parent
          // executes another instruction. Restore that parent's typed scope
          // before capturing the new child's process and memory authority.
          Kernel.enterExecution(Current->Base, Current->ThreadKey,
                                Current->ReturnToken);
          if (auto E = Kernel.beginGuestCall(Call.Token)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
          auto Child =
              NewExecution(Call.PC, Call.Arguments, guestCallPhase(Call.Token),
                           Current->ID, true);
          if (!Child) {
            ModelFailure(Child.takeError());
            co_return llvm::Error::success();
          }
          if (auto E = Kernel.inheritExecutionContext((*Child)->Base,
                                                      Current->Base)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
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
            co_return llvm::Error::success();
          if (Current->SynchronizationEntered) {
            if (auto E = Kernel.finishFrameworkCallback(
                    Current->SynchronizationObject)) {
              ModelFailure(std::move(E));
              co_return llvm::Error::success();
            }
            Current->SynchronizationEntered = false;
            if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
              co_return E;
          }
          if (auto E = Kernel.validateExecutionReturn(
                  Current->Base, Current->EntryIRQL, bool(Current->Parent))) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
          if (Current->ExceptionCallback) {
            auto &Parent = *Current->Parent;
            auto &State = *Parent.Exception;
            const uint64_t Storage =
                Current->Base + Current->Size - seh::RecordsSize;
            std::vector<uint8_t> Records;
            auto Advance = [&]() -> llvm::Expected<X64SEH::Action> {
              if (*Current->ExceptionCallback != X64SEH::ActionKind::Filter)
                return Exceptions().advance(State.Dispatch);
              Records.resize(seh::RecordsSize);
              if (auto E = CPU.read(Storage, Records))
                return std::move(E);
              return Exceptions().finishFilter(
                  State.Dispatch, static_cast<int32_t>(*InvocationReturn),
                  Records, State.Raised, Storage);
            };
            auto Next = Advance();
            if (!Next) {
              ModelFailure(Next.takeError());
              co_return llvm::Error::success();
            }
            State.Next = *Next;
            if (Next->Kind == X64SEH::ActionKind::Filter ||
                Next->Kind == X64SEH::ActionKind::Finally) {
              if (auto E = PrepareExceptionCallback(*Current, Parent)) {
                ModelFailure(std::move(E));
                co_return llvm::Error::success();
              }
              continue;
            }
            if (Next->Kind == X64SEH::ActionKind::Unhandled) {
              ModelFailure(failure(session_diagnostic::UnhandledException +
                                   llvm::utohexstr(State.Raised.Code)));
              co_return llvm::Error::success();
            }
            auto Registers = Next->State.Registers;
            if (Next->Kind == X64SEH::ActionKind::ContinueExecution) {
              if (!State.CanContinue) {
                ModelFailure(
                    failure(session_diagnostic::APIExceptionContinuation));
                co_return llvm::Error::success();
              }
              auto Restored = Exceptions().continuation(
                  Records, State.Raised, Storage, {Parent.Base, Parent.Size});
              if (!Restored) {
                ModelFailure(Restored.takeError());
                co_return llvm::Error::success();
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
              co_return llvm::Error::success();
            }
            continue;
          }
          if (auto E = Kernel.retireStack(Current->Base, Current->Size)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
          if (Current->PrivateStack)
            StackInUse[(Current->Base - CallbackStackBase) /
                       CallbackStackStride] = false;
          if (Current->Parent) {
            auto Completion =
                Kernel.finishGuestCall(Current->ReturnToken, *InvocationReturn);
            if (!Completion) {
              ModelFailure(Completion.takeError());
              co_return llvm::Error::success();
            }
            auto Parent = std::move(Current->Parent);
            if (auto Wait = Kernel.takeWait()) {
              if (Parent->Wait) {
                ModelFailure(failure(session_diagnostic::CallbackReplacesWait));
                co_return llvm::Error::success();
              }
              Parent->Wait = std::move(Wait);
            }
            if (*Completion) {
              if (!Parent->Wait) {
                if (!Parent->DeferredThreadTermination) {
                  Parent->ResumeValue = **Completion;
                  Result.Calls[Parent->WaitEvent].Result = **Completion;
                }
              } else {
                Kernel.enterExecution(Parent->Base, Parent->ThreadKey);
                if (Options.Scheduling)
                  Parent->PausedModel = Kernel.captureExecutionContext();
                if (Parent->ID)
                  if (auto E = Kernel.suspendScheduled(Parent->ID)) {
                    ModelFailure(std::move(E));
                    co_return llvm::Error::success();
                  }
                Waiting.push_back(std::move(Parent));
                Current.reset();
                continue;
              }
            } else {
              Parent->ChildCall = Kernel.takeGuestCall();
              if (!Parent->ChildCall) {
                ModelFailure(failure(session_diagnostic::MissingModelCallback));
                co_return llvm::Error::success();
              }
              if (auto Wait = Kernel.takeWait()) {
                if (Parent->Wait) {
                  ModelFailure(
                      failure(session_diagnostic::CallbackReplacesWait));
                  co_return llvm::Error::success();
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
              co_return llvm::Error::success();
            }
            if (!*Completion) {
              auto Call = Kernel.takeGuestCall();
              if (!Call) {
                ModelFailure(failure(session_diagnostic::MissingModelCallback));
                co_return llvm::Error::success();
              }
              // A process traversal retains its actor thread, foreground
              // stack, and time slice across all registered routines.
              if ((Current->ReturnToken.Owner ==
                       GuestCallOwner::ProcessNotify ||
                   Current->ReturnToken.Owner ==
                       GuestCallOwner::RegistryCallback)) {
                if (auto E = Kernel.beginGuestCall(Call->Token)) {
                  ModelFailure(std::move(E));
                  co_return llvm::Error::success();
                }
                const bool RegistryStage = Current->ReturnToken.Owner ==
                                           GuestCallOwner::RegistryCallback;
                auto Frame =
                    NewExecution(Call->PC, Call->Arguments,
                                 guestCallPhase(Call->Token), 0, RegistryStage);
                if (!Frame) {
                  ModelFailure(Frame.takeError());
                  co_return llvm::Error::success();
                }
                (*Frame)->ReturnToken = Call->Token;
                if (RegistryStage)
                  if (auto E = Kernel.inheritExecutionContext((*Frame)->Base,
                                                              StackBase)) {
                    ModelFailure(std::move(E));
                    co_return llvm::Error::success();
                  }
                (*Frame)->ThreadKey = Current->ThreadKey;
                (*Frame)->Slice = Current->Slice;
                Current = std::move(*Frame);
                continue;
              }
              if (auto E = ReadyDetachedCall(*Call)) {
                ModelFailure(std::move(E));
                co_return llvm::Error::success();
              }
              continue;
            }
            if ((Current->ReturnToken.Owner == GuestCallOwner::ProcessNotify ||
                 Current->ReturnToken.Owner ==
                     GuestCallOwner::RegistryCallback)) {
              InvocationReturn = **Completion;
              if (auto E = Kernel.flushFrameworkCallbackDestructions()) {
                ModelFailure(std::move(E));
                co_return llvm::Error::success();
              }
              co_return llvm::Error::success();
            }
            Current.reset();
            continue;
          }
          if (!Current->ID) {
            if (auto E = Kernel.flushFrameworkCallbackDestructions()) {
              ModelFailure(std::move(E));
              co_return llvm::Error::success();
            }
            co_return llvm::Error::success();
          }
          auto Continuation =
              Current->ThreadTerminated
                  ? llvm::Expected<std::optional<KernelGuestCall>>(
                        std::optional<KernelGuestCall>{})
                  : Kernel.continueScheduled(Current->ID, *InvocationReturn);
          if (!Continuation) {
            ModelFailure(Continuation.takeError());
            co_return llvm::Error::success();
          }
          if (*Continuation) {
            auto Wait = Kernel.takeWait();
            if (Wait && Wait->Type !=
                            KernelModel::Wait::Kind::InterruptSynchronization) {
              ModelFailure(failure(session_diagnostic::ScheduledCallbackWait));
              co_return llvm::Error::success();
            }
            if (!Wait)
              if (auto E = Kernel.beginGuestCall((**Continuation).Token)) {
                ModelFailure(std::move(E));
                co_return llvm::Error::success();
              }
            auto Frame =
                NewExecution((**Continuation).PC, (**Continuation).Arguments,
                             Current->Phase, Current->ID);
            if (!Frame) {
              ModelFailure(Frame.takeError());
              co_return llvm::Error::success();
            }
            (*Frame)->SynchronizationObject =
                (**Continuation).SynchronizationObject;
            (*Frame)->Slice = Current->Slice;
            (*Frame)->ThreadKey = Current->ThreadKey;
            if (Wait) {
              auto Context = CPU.saveContext();
              if (!Context) {
                ModelFailure(Context.takeError());
                co_return llvm::Error::success();
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
                co_return llvm::Error::success();
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
            co_return llvm::Error::success();
          }
          Current.reset();
        }
      }
      if (Options.Scheduling) {
        Kernel.enterForeground();
        if (auto E = Kernel.setUserRequestContext(false)) {
          ModelFailure(std::move(E));
          co_return llvm::Error::success();
        }
        if (auto E = AdvanceClock()) {
          ModelFailure(std::move(E));
          co_return llvm::Error::success();
        }
      }
      // Waiters retain independent stacks and CPU state. Ready passive frames
      // yield to interrupts, DPCs, cancellation and provider completions.
      if (auto E = RefreshWaiters()) {
        ModelFailure(std::move(E));
        co_return llvm::Error::success();
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
            (Best && !ReadyExecution(*Waiting[I]).precedes(*Best)))
          continue;
        Ready = I;
        if (!Options.Scheduling)
          break;
        Best = ReadyExecution(*Waiting[I]);
      }
      if (Ready) {
        Current = std::move(Waiting[*Ready]);
        Waiting.erase(Waiting.begin() + *Ready);
        if (Current->ID) {
          if (auto E = Kernel.resumeScheduled(Current->ID)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
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
          co_return llvm::Error::success();
        }
      }
      if (Current)
        continue;
      if (auto Call = Kernel.takeGuestCall()) {
        if (auto E = ReadyDetachedCall(*Call)) {
          ModelFailure(std::move(E));
          co_return llvm::Error::success();
        }
        continue;
      }
      auto Next = Kernel.nextScheduled(false);
      if (!Next) {
        ModelFailure(Next.takeError());
        co_return llvm::Error::success();
      }
      if (auto E = RefreshWaiters()) {
        ModelFailure(std::move(E));
        co_return llvm::Error::success();
      }
      if (!*Next) {
        if (auto Call = Kernel.takeGuestCall()) {
          if (auto E = ReadyDetachedCall(*Call)) {
            ModelFailure(std::move(E));
            co_return llvm::Error::success();
          }
          continue;
        }
        if (std::any_of(Waiting.begin(), Waiting.end(),
                        [](const auto &Frame) { return !Frame->Wait; }))
          continue;
        if (IdleUntil100ns && Kernel.now100ns() >= *IdleUntil100ns) {
          Result.Phase = ParentPhase;
          Result.Stop = DriverStopReason::Returned;
          co_return llvm::Error::success();
        }
        // A declared next foreground action may run while ordinary system
        // threads are parked. Retain their real stacks, waits and references.
        // Required IRPs and other callback kinds still drain normally.
        if (!IdleUntil100ns && !Foreground && ForegroundReadyPolicy &&
            ForegroundBoundary && !Kernel.hasForegroundBlockingCallbacks() &&
            !Kernel.hasPendingHardwareWork() &&
            !Kernel.requestPending(
                RequiredIRP,
                KernelModel::PendingRequestScope::ExcludePowerParked) &&
            std::all_of(Waiting.begin(), Waiting.end(), [&](const auto &Frame) {
              return Frame->Wait && !Frame->Parent && !Frame->ReturnToken.ID &&
                     Kernel.isSuspendedSystemThread(Frame->ID);
            })) {
          Result.Phase = ParentPhase;
          Result.Stop = DriverStopReason::Returned;
          co_return llvm::Error::success();
        }
        if (!IdleUntil100ns && !Foreground && Waiting.empty() &&
            !Kernel.requestPending(
                0, KernelModel::PendingRequestScope::ExcludePowerParked) &&
            !Kernel.hasPendingHardwareWork()) {
          Result.Phase = ParentPhase;
          Result.Stop = DriverStopReason::Returned;
          co_return llvm::Error::success();
        }
        std::optional<uint64_t> Bound = Kernel.nextEventTime();
        for (const auto &Frame : Waiting)
          if (Frame->Wait && Frame->Wait->Deadline &&
              (!Bound || *Frame->Wait->Deadline < *Bound))
            Bound = Frame->Wait->Deadline;
        if (IdleUntil100ns && (!Bound || *IdleUntil100ns < *Bound))
          Bound = IdleUntil100ns;
        if (!Bound) {
          ModelFailure(failure(session_diagnostic::StalledPending));
          co_return llvm::Error::success();
        }
        Next = Kernel.nextScheduled(true, Bound);
        if (!Next) {
          ModelFailure(Next.takeError());
          co_return llvm::Error::success();
        }
        // Timer expiration satisfies existing waits before its queued DPC can
        // rearm the timer and clear its signal.
        if (auto E = RefreshWaiters()) {
          ModelFailure(std::move(E));
          co_return llvm::Error::success();
        }
        if (!*Next) {
          if (auto Call = Kernel.takeGuestCall()) {
            if (auto E = ReadyDetachedCall(*Call)) {
              ModelFailure(std::move(E));
              co_return llvm::Error::success();
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
        co_return llvm::Error::success();
      }
      (*Frame)->SynchronizationObject = (**Next).SynchronizationObject;
      if (Options.Scheduling)
        (*Frame)->Slice->ReadyOrder = (**Next).ReadyOrder;
      Current = std::move(*Frame);
    }
    co_return llvm::Error::success();
  };
  Environment.PumpInvocation =
      [&](std::unique_ptr<Execution> Frame) -> DriverTask {
    co_return co_await Pump(std::move(Frame), "invocation");
  };
  auto Invoke = [&](const KernelModel::Invocation &Invocation,
                    const std::string &Phase) -> DriverTask {
    Kernel.enterForeground();
    std::vector<uint64_t> Arguments{Invocation.Argument0, Invocation.Argument1,
                                    Invocation.Argument2, Invocation.Argument3};
    Arguments.insert(Arguments.end(), Invocation.StackArguments.begin(),
                     Invocation.StackArguments.end());
    auto Frame = NewExecution(Invocation.PC, Arguments, Phase, 0);
    if (!Frame) {
      ModelFailure(Frame.takeError());
      co_return llvm::Error::success();
    }
    (*Frame)->SynchronizationObject = Invocation.SynchronizationObject;
    (*Frame)->RequestIRP = Invocation.IRP;
    co_return co_await Pump(std::move(*Frame), Phase);
  };
  auto DrainCallbacks = [&](bool ForegroundBoundary = false,
                            uint64_t RequiredIRP = 0) -> DriverTask {
    const std::string ParentPhase = Result.Phase;
    co_return co_await Pump(nullptr, ParentPhase, ForegroundBoundary,
                            RequiredIRP);
  };
  auto &RegistryEventCursor = State->RegistryEventCursor;
  auto &ProcessEventCursor = State->ProcessEventCursor;
  auto DeliverProcessEvents = [&](size_t Boundary) -> DriverTask {
    while (ProcessEventCursor < Options.ProcessEvents.size() &&
           Options.ProcessEvents[ProcessEventCursor].BeforeRequest ==
               Boundary &&
           Result.Stop == DriverStopReason::Returned) {
      Kernel.enterForeground();
      Kernel.enterExecution(StackBase, StackBase);
      const auto &Event = Options.ProcessEvents[ProcessEventCursor];
      const uint32_t Actor = Event.Type == DriverProcessEvent::Kind::Create
                                 ? Event.CreatorProcessID
                                 : Event.ProcessID;
      if (auto E = Kernel.setUserRequestContext(
              Actor != windows::SystemProcessID, Actor)) {
        ModelFailure(std::move(E));
        break;
      }
      auto Call = Kernel.beginProcessEvent(Event, ProcessEventCursor);
      if (!Call) {
        ModelFailure(Call.takeError());
        break;
      }
      if (*Call) {
        Result.Phase = guestCallPhase((**Call).Token);
        if (auto E = Kernel.beginGuestCall((**Call).Token)) {
          ModelFailure(std::move(E));
          break;
        }
        auto Frame =
            NewExecution((**Call).PC, (**Call).Arguments, Result.Phase, 0);
        if (!Frame) {
          ModelFailure(Frame.takeError());
          break;
        }
        (*Frame)->ReturnToken = (**Call).Token;
        if (auto E = co_await Pump(std::move(*Frame), Result.Phase))
          co_return E;
      }
      if (Result.Stop == DriverStopReason::Returned)
        if (auto E = Kernel.completeProcessEvent())
          ModelFailure(std::move(E));
      if (auto E = Kernel.setUserRequestContext(false))
        ModelFailure(std::move(E));
      if (Result.Stop != DriverStopReason::Returned)
        break;
      ++ProcessEventCursor;
      if (auto E = co_await DrainCallbacks(
              Boundary < Options.Requests.size() ||
              ProcessEventCursor < Options.ProcessEvents.size() ||
              RegistryEventCursor < Options.RegistryEvents.size() ||
              Options.Unload))
        co_return E;
    }
    co_return llvm::Error::success();
  };
  auto DeliverRegistryEvents = [&](size_t Boundary) -> DriverTask {
    while (RegistryEventCursor < Options.RegistryEvents.size() &&
           Options.RegistryEvents[RegistryEventCursor].BeforeRequest ==
               Boundary &&
           Result.Stop == DriverStopReason::Returned) {
      Result.Phase = "registry_event:" + std::to_string(RegistryEventCursor);
      if (DeadlineExceeded())
        break;
      Kernel.enterForeground();
      Kernel.enterExecution(StackBase, StackBase);
      const auto Actor =
          Options.RegistryEvents[RegistryEventCursor].ActorProcessID;
      if (auto E = Kernel.setUserRequestContext(
              Actor != windows::SystemProcessID, Actor)) {
        ModelFailure(std::move(E));
        break;
      }
      auto Status = Kernel.beginRegistryEvent(
          Options.RegistryEvents[RegistryEventCursor], RegistryEventCursor);
      if (!Status) {
        ModelFailure(Status.takeError());
        break;
      }
      if (auto Call = Kernel.takeGuestCall()) {
        if (auto E = Kernel.beginGuestCall(Call->Token)) {
          ModelFailure(std::move(E));
          break;
        }
        auto Frame = NewExecution(Call->PC, Call->Arguments,
                                  guestCallPhase(Call->Token), 0, true);
        if (!Frame) {
          ModelFailure(Frame.takeError());
          break;
        }
        (*Frame)->ReturnToken = Call->Token;
        (*Frame)->ThreadKey = StackBase;
        if (auto E =
                Kernel.inheritExecutionContext((*Frame)->Base, StackBase)) {
          ModelFailure(std::move(E));
          break;
        }
        if (auto E = co_await Pump(std::move(*Frame), Result.Phase))
          co_return E;
        if (Result.Stop != DriverStopReason::Returned)
          break;
        *Status = *InvocationReturn;
      }
      Kernel.enterExecution(StackBase, StackBase);
      if (auto E = Kernel.finishRegistryEvent(*Status)) {
        ModelFailure(std::move(E));
        break;
      }
      if (auto E = Kernel.setUserRequestContext(false)) {
        ModelFailure(std::move(E));
        break;
      }
      ++RegistryEventCursor;
      if (auto E = co_await DrainCallbacks(Boundary < Options.Requests.size() ||
                                           RegistryEventCursor <
                                               Options.RegistryEvents.size() ||
                                           Options.Unload))
        co_return E;
    }
    co_return llvm::Error::success();
  };
  if (auto E = co_await Invoke(
          {Image->Entry, Kernel.driverObject(), Kernel.registryPath()},
          EntryPhase))
    co_return std::move(E);
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
    if (auto E = Environment.prepareInvocationSpace())
      co_return E;
    Environment.AtAfterEntry = true;
    co_yield EnvironmentAction{
        EnvironmentActionKind::StageBoundary, 0, "after-entry", {}};
    Environment.AtAfterEntry = false;
    if (auto E = co_await DrainCallbacks(
            !Options.Requests.empty() || !Options.ProcessEvents.empty() ||
            !Options.RegistryEvents.empty() || Options.Unload))
      co_return std::move(E);
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
      if (auto E = co_await Invoke(*Invocation, Result.Phase))
        co_return std::move(E);
      if (Result.Stop != DriverStopReason::Returned)
        break;
      if (auto E =
              Kernel.finishAddDevice(Device.ID, uint32_t(*InvocationReturn))) {
        ModelFailure(std::move(E));
        break;
      }
      if (auto E = co_await DrainCallbacks())
        co_return std::move(E);
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
      if (auto E = co_await DeliverProcessEvents(Index))
        co_return std::move(E);
      if (Result.Stop == DriverStopReason::Returned &&
          Options.Requests[Index].DelayBefore100ns) {
        const auto Duration = Options.Requests[Index].DelayBefore100ns;
        if (Duration > UINT64_MAX - Kernel.now100ns()) {
          ModelFailure(failure("request delay overflows virtual time"));
          break;
        }
        const auto Start = Kernel.now100ns();
        Result.RequestDelays.push_back({Index, Duration, Start, {}});
        const auto Phase = "request_delay:" + std::to_string(Index);
        if (auto E = co_await Pump(nullptr, Phase, true, 0, Start + Duration))
          co_return std::move(E);
        if (Result.Stop != DriverStopReason::Returned)
          break;
        Result.RequestDelays.back().CompletedAt100ns = Kernel.now100ns();
        Kernel.enterForeground();
        Kernel.enterExecution(StackBase, StackBase);
        if (auto E = Kernel.setUserRequestContext(false)) {
          ModelFailure(std::move(E));
          break;
        }
      }
      if (auto E = co_await DeliverRegistryEvents(Index))
        co_return std::move(E);
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
        auto Invoked = co_await Invoke(*Invocation, Result.Phase);
        if (auto E = Kernel.setUserRequestContext(false)) {
          ModelFailure(std::move(E));
          break;
        }
        if (Invoked)
          co_return std::move(Invoked);
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
            if (auto E = co_await Invoke(*Continued, Result.Phase))
              co_return std::move(E);
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
      if (auto E = co_await DrainCallbacks(
              !Removing &&
                  (Index + 1 < Options.Requests.size() ||
                   (ProcessEventCursor < Options.ProcessEvents.size() ||
                    RegistryEventCursor < Options.RegistryEvents.size()) ||
                   Options.Unload),
              Invocation->IRP))
        co_return std::move(E);
      if (Result.Stop != DriverStopReason::Returned)
        break;
      if (Removing) {
        if (auto E = Kernel.beginFrameworkRemoval(Invocation->IRP)) {
          ModelFailure(std::move(E));
          break;
        }
        if (auto E = co_await DrainCallbacks())
          co_return std::move(E);
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
      if (auto E = co_await DrainCallbacks(
              Options.Unload ||
              (ProcessEventCursor < Options.ProcessEvents.size() ||
               RegistryEventCursor < Options.RegistryEvents.size())))
        co_return std::move(E);
      if (Result.Stop == DriverStopReason::Returned)
        if (auto E = FinalizeBatch(true))
          ModelFailure(std::move(E));
    }
    if (Result.Stop == DriverStopReason::Returned)
      if (auto E = co_await DeliverProcessEvents(Options.Requests.size()))
        co_return std::move(E);
    if (Result.Stop == DriverStopReason::Returned)
      if (auto E = co_await DeliverRegistryEvents(Options.Requests.size()))
        co_return std::move(E);
    if (Result.Stop == DriverStopReason::Returned && Options.Unload &&
        !DeadlineExceeded()) {
      Result.Phase = UnloadPhase;
      auto Invocation = Kernel.beginUnload();
      if (!Invocation) {
        ModelFailure(Invocation.takeError());
      } else {
        if (auto E = co_await Invoke(*Invocation, UnloadPhase))
          co_return std::move(E);
        if (Result.Stop == DriverStopReason::Returned) {
          if (auto E = Kernel.finishUnload())
            ModelFailure(std::move(E));
          else
            Result.UnloadCompleted = true;
        }
      }
    }
  }
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
  co_return llvm::Error::success();
}
} // namespace
llvm::Error
PreparedDriverExecution::finishStoppedFailure(const ExecutionRunEvent &Event) {
  if (!Run || !FinishFailure)
    return failure("driver execution owner has been released");
  return FinishFailure(Event);
}
llvm::Expected<PreparedDriverExecution> createDriverExecutionRun(
    const std::filesystem::path &Path, const DriverOptions &Options,
    ExecutionRunOptions Control, std::shared_ptr<modules::ModuleSet> Sources,
    DriverSessionState::ResourceInputs Inputs, bool LegacyImportBindings) {
  if (Control.Limits.Instructions != Options.InstructionLimit ||
      Control.Limits.Events != Options.EventLimit ||
      Control.Limits.TimeoutMicroseconds / runtime::MicrosecondsPerMillisecond +
              (Control.Limits.TimeoutMicroseconds %
                   runtime::MicrosecondsPerMillisecond !=
               0) !=
          Options.TimeoutMilliseconds)
    return failure("driver and execution limits must match");
  Inputs.LinkModuleImports = !LegacyImportBindings;
  auto Created = DriverSessionState::create(Path, Options, Control.TimePolicy,
                                            Control.Limits.TimeoutMicroseconds,
                                            std::move(Inputs));
  if (!Created)
    return Created.takeError();
  auto State = std::move(*Created);
  auto CPU = std::move(State->UnattachedCPU);
  auto Budget = State->Budget;
  const bool CallsEnabled =
      llvm::is_contained(Control.PauseStages, "after-entry");
  auto Environment = std::make_unique<DriverExecutionEnvironment>(
      std::move(State), CallsEnabled);
  auto *View = Environment.get();
  if (!Sources) {
    auto Empty = modules::ModuleSet::create({});
    if (!Empty)
      return Empty.takeError();
    Sources = std::move(*Empty);
  }
  auto Run = ExecutionRun::create(std::move(CPU), std::move(Sources),
                                  std::move(Environment), std::move(Control),
                                  std::move(Budget));
  if (!Run)
    return Run.takeError();
  PreparedDriverExecution Prepared;
  Prepared.Run = std::move(*Run);
  Prepared.State = View->State.get();
  Prepared.FinishFailure = [View](const ExecutionRunEvent &E) {
    return View->finishStoppedFailure(E);
  };
  return Prepared;
}
} // namespace neverd::emulation
