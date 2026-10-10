//===- WindowsProcess.cpp - Windows PE64 process continuations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../../runtime/RuntimeValues.h"
#include "WindowsNativeServices.h"
#include "WindowsProcessExceptions.h"
#include "WindowsProcessLoader.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <array>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace neverd::emulation::windows_process {
using namespace value;
namespace {
/// Reads one stopped process for an observer. It owns nothing and exposes no
/// operation that changes the CPU, guest memory or the loader.
class StoppedProcess final : public ProcessView {
public:
  StoppedProcess(ExecutionBackend &CPU, AddressSpace &Space,
                 const IntegerABI &ABI, const Program &Modules,
                 const Environment &Env,
                 const std::optional<Lifetime::Call> &Active,
                 const std::vector<uint64_t> &Initializers,
                 const ProcessResult &Result, const Services &OS,
                 ProcessStackView Stack)
      : CPU(CPU), Space(Space), ABI(ABI), Modules(Modules), Env(Env),
        Active(Active), Initializers(Initializers), Result(Result), OS(OS),
        Stack(Stack) {}
  GuestArchitecture architecture() const override { return CPU.architecture(); }
  llvm::Expected<RegisterValue> readRegister(CPURegister Register) override {
    return CPU.readRegister(Register);
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Bytes) override {
    return Space.snapshotBacking(Address, Bytes);
  }
  llvm::Expected<uint32_t> instructionSize(uint64_t Address) override {
    return CPU.instructionSize(Address);
  }
  llvm::Expected<std::vector<AddressMapping>> mappings() override {
    return Space.mappings();
  }
  std::vector<ProcessModuleView> modules() override {
    std::vector<ProcessModuleView> Result;
    auto Add = [&](size_t Index) {
      const auto &M = Modules.Modules[Index];
      if (resident(M))
        Result.push_back(
            {Modules.Identities[Index].Name, M.Loaded.Base, M.Loaded.Size,
             M.Loaded.Entry, Index == 0,
             M.System || M.Opaque || (!Index && !Modules.InputName.empty())});
    };
    Add(0);
    for (size_t Index : Modules.LoaderInitializationOrder)
      if (Index)
        Add(Index);
    return Result;
  }
  std::optional<ProcessModuleView> inputModule() override {
    auto Index = windows_process::inputModule(Modules);
    if (!Index)
      return std::nullopt;
    const auto &M = Modules.Modules[*Index];
    return ProcessModuleView{Modules.Identities[*Index].Name,
                             M.Loaded.Base,
                             M.Loaded.Size,
                             M.Loaded.Entry,
                             *Index == 0,
                             false};
  }
  llvm::Expected<std::optional<ProcessCallFrame>> callFrame() override {
    auto SP = CPU.readRegister(ABI.info().StackPointer);
    if (!SP)
      return SP.takeError();
    if (auto E = ABI.validateStackPointer((*SP)[0])) {
      llvm::consumeError(std::move(E));
      return std::nullopt;
    }
    if (ABI.info().Link == CPURegister::Invalid) {
      auto Readable =
          CPU.canAccess((*SP)[0], PointerSize, Read | UserAccessible);
      if (!Readable)
        return Readable.takeError();
      if (!*Readable)
        return std::nullopt;
    }
    auto Return = ABI.readReturnAddress(CPU, (*SP)[0]);
    if (!Return)
      return Return.takeError();
    auto Executable = CPU.canAccess(*Return, 1, Execute | UserAccessible);
    if (!Executable)
      return Executable.takeError();
    if (!*Executable)
      return std::nullopt;
    auto ReturnSP = ABI.returnStackPointer((*SP)[0]);
    if (!ReturnSP)
      return ReturnSP.takeError();
    ProcessCallFrame Frame{*Return, *ReturnSP, {}};
    for (size_t I = 0; I < Frame.Arguments.size(); ++I) {
      auto Argument = ABI.readArgument(CPU, (*SP)[0], I);
      if (!Argument)
        return Argument.takeError();
      Frame.Arguments[I] = *Argument;
    }
    return Frame;
  }
  std::vector<ProcessExportView> exports() override {
    std::vector<ProcessExportView> Result;
    for (const auto &Gate : Modules.Gates)
      Result.push_back({Gate.Gate, Gate.Module, Gate.Name, Gate.Ordinal});
    for (size_t Index = 0; Index < Modules.Modules.size(); ++Index) {
      const auto &M = Modules.Modules[Index];
      // System providers publish exactly their gates. A forwarder or hole has
      // no address of its own in the exporting image.
      if (!resident(M) || M.System)
        continue;
      for (const auto &Export : M.Loaded.Exports.Entries) {
        if (Export.Kind != PEExportKind::Address)
          continue;
        const uint64_t Address = M.Loaded.Base + Export.RVA;
        const auto &Module = Modules.Identities[Index].Name;
        if (Export.Names.empty())
          Result.push_back({Address, Module, {}, uint16_t(Export.Ordinal)});
        for (const auto &Name : Export.Names)
          Result.push_back({Address, Module, Name, uint16_t(Export.Ordinal)});
      }
    }
    return Result;
  }

  bool programInvocation() const override { return Active && Active->Input; }
  std::vector<uint64_t> completedInitializers() const override {
    return Initializers;
  }
  std::optional<ProcessStackView> stack() const override { return Stack; }
  std::optional<std::vector<ProcessHeapAllocationView>>
  heapAllocations() const override {
    return OS.heapAllocations();
  }
  std::optional<std::vector<uint64_t>> encodedPointers() const override {
    return OS.encodedPointers();
  }
  llvm::Expected<std::optional<ProcessDynamicThreadLocalState>>
  dynamicThreadLocalState() override {
    auto State = OS.dynamicThreadLocalState();
    if (!State)
      return State.takeError();
    return std::optional(*State);
  }
  std::optional<uint64_t> nativeCallCount() const override {
    return Result.NativeCalls.size();
  }
  llvm::Expected<std::shared_ptr<const ProcessRuntimeState>>
  runtimeState(bool IncludeBacking) override {
    return OS.runtimeState(IncludeBacking);
  }
  bool watchedMemoryUnchanged() const override { return MemoryUnchanged; }
  void setWatchedMemoryUnchanged(bool Unchanged) {
    MemoryUnchanged = Unchanged;
  }
  llvm::Expected<std::optional<std::vector<uint8_t>>>
  threadLocalMemory() override {
    auto Index = windows_process::inputModule(Modules);
    if (!Index)
      return failure(text::TLS);
    std::vector<uint8_t> Bytes(Modules.Modules[*Index].Loaded.TLSSize);
    if (!Bytes.empty()) {
      const auto Block = Env.TLS.find(*Index);
      if (Block == Env.TLS.end() || Bytes.size() > Block->second.Size)
        return failure(text::TLS);
      if (auto E = Space.snapshotBacking(Block->second.Address, Bytes))
        return std::move(E);
    }
    return std::move(Bytes);
  }

private:
  ExecutionBackend &CPU;
  AddressSpace &Space;
  const IntegerABI &ABI;
  const Program &Modules;
  const Environment &Env;
  const std::optional<Lifetime::Call> &Active;
  const std::vector<uint64_t> &Initializers;
  const ProcessResult &Result;
  const Services &OS;
  ProcessStackView Stack;
  bool MemoryUnchanged = false;
};
} // namespace

llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options,
                                         ProcessObserver *Observer) {
  if (Options.Android || !Options.Limits.Instructions ||
      !Options.Limits.Events || !Options.Limits.TimeoutMicroseconds ||
      !Options.MemoryLimit || !Options.StackSize || !Options.OutputLimit ||
      !Options.InstructionQuantum || Options.StackSize % PageSize ||
      Options.StackSize > MaxStackSize ||
      Options.StackSize >= Options.MemoryLimit ||
      Options.OutputLimit > Options.MemoryLimit)
    return failure(text::Limits);
  auto Budget = ExecutionBudget::create(Options.Limits);
  if (!Budget)
    return Budget.takeError();
  std::shared_ptr<ExecutionBudget> Resources(std::move(*Budget));
  const uint64_t StackBase = StackTop - Options.StackSize;
  auto Physical = PhysicalMemory::create(Options.MemoryLimit);
  if (!Physical)
    return Physical.takeError();
  auto Space = AddressSpace::create(*Physical, Options.MemoryLimit);
  if (!Space)
    return Space.takeError();
  VirtualMemory Virtual(**Space, Options);
  auto Program = loadProgram(Path, Options, *Resources, Virtual);
  if (!Program)
    return Program.takeError();
  auto *Loaded = &Program->Modules.front().Loaded;
  for (const auto &Module : Program->Modules)
    for (const auto &Region : Module.Loaded.Regions) {
      if (auto E = (*Space)->map(Region.Address, Region.Bytes.size(),
                                 Read | Write | UserAccessible))
        return std::move(E);
      if (auto E = (*Space)->write(Region.Address, Region.Bytes))
        return std::move(E);
    }
  if (auto E = (*Space)->map(StackBase, Options.StackSize,
                             Read | Write | UserAccessible))
    return std::move(E);
  if (auto E = (*Space)->map(GateBase, GateSize, Read | Write | UserAccessible))
    return std::move(E);
  std::vector<uint8_t> Trap;
  const bool X64 = Loaded->Architecture == GuestArchitecture::X64;
  if (X64)
    Trap.assign(std::begin(X64Service), std::end(X64Service));
  else {
    Trap.resize(DWordSize);
    llvm::support::endian::write32le(Trap.data(), ArmServiceInstruction);
  }
  for (uint64_t Gate : {ReturnGate, AttachReturnGate, DetachReturnGate,
                        ExceptionReturnGate, ExceptionDispatchGate})
    if (auto E = (*Space)->write(Gate, Trap))
      return std::move(E);
  if (Program->DeferUnmodeled) {
    // Every opaque entry is the same service trap; its address is its
    // identity. Filling the region once lets later lookups bind without
    // touching guest memory.
    std::vector<uint8_t> Entries(OpaqueGateSize);
    for (uint64_t Offset = 0; Offset < OpaqueGateSize; Offset += GateStride)
      std::copy(Trap.begin(), Trap.end(), Entries.begin() + Offset);
    if (auto E = (*Space)->write(OpaqueGateBase, Entries))
      return std::move(E);
  }
  for (const auto &Module : Program->Modules)
    for (const auto &Import : Module.Loaded.Imports)
      if (auto E =
              (*Space)->writeInteger(Import.Slot, Import.Gate, PointerSize))
        return std::move(E);
  auto Env = prepareEnvironment(**Space, *Program, Options);
  if (!Env)
    return Env.takeError();
  if (auto E = (*Space)->protect(GateBase, GateSize,
                                 Read | Execute | UserAccessible))
    return std::move(E);
  for (const auto &Module : Program->Modules)
    for (const auto &Region : Module.Loaded.Regions)
      if (auto E = (*Space)->protect(Region.Address, Region.Bytes.size(),
                                     Region.Permissions))
        return std::move(E);
  for (auto &M : Program->Modules)
    M.State = ModuleState::Ready;
  auto ABI = IntegerABI::get(X64 ? IntegerCallingConvention::Win64
                                 : IntegerCallingConvention::AAPCS64);
  if (!ABI)
    return ABI.takeError();
  if (!Resources->remainingMicroseconds())
    return failure(text::ModuleTimeout);
  const ExecutionContract Checked = X64 ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedUserAArch64;
  const ExecutionContract Contract = Options.Contract.value_or(Checked);
  if (Contract != Checked &&
      !(X64 && Contract == ExecutionContract::DirectUserX64))
    return failure(text::Contract);
  auto Backend = createExecutionBackend(Options.Backend, Contract, *Space,
                                        Loaded->Architecture);
  if (!Backend)
    return Backend.takeError();
  if (auto E = Backend->CPU->writeRegister(
          X64 ? CPURegister::X64GSBase : CPURegister::AArch64X18, {TEB, 0}))
    return std::move(E);
  ExceptionDispatcher Exceptions(*Backend->CPU, *ABI, StackBase, &*Program,
                                 Resources.get());
  auto Session = ExecutionSession::create(
      std::move(Backend->CPU), Resources,
      [&](const BackendFault &Fault) { return Exceptions.accepts(Fault); });
  if (!Session)
    return Session.takeError();
  auto &CPU = (*Session)->cpu();
  const CPURegister PCRegister =
      X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
  ProcessResult Result{ProcessProfile::WindowsPE64, Loaded->Architecture,
                       Backend->Kind, Backend->Reason};
  Result.Entry = Loaded->Entry;
  Result.InitializersEnabled = true;
  Services OS(CPU, **Space, *Loaded, *Env, Options, Result, Virtual, *Program,
              *Resources, Exceptions);
  Lifetime Life(*Program);
  Loader Modules(*Program, Virtual, **Space, *Env, CPU, *Resources);
  std::optional<Lifetime::Call> Active;
  std::vector<uint64_t> Initializers;
  uint64_t ExpectedSP = 0, ExpectedGate = 0;
  uint64_t RootStackPointer = StackTop;
  std::optional<FLSCleanup> ExitCleanup;
  bool ExitCleanupDone = false;
  struct Continuation {
    using Work = std::variant<Loader::Operation, FLSCleanup>;
    Work Operation;
    std::unique_ptr<BackendContext> Context;
    uint64_t StackPointer, ExpectedSP, ExpectedGate;
    size_t Event;
    std::optional<Lifetime::Call> Active;
  };
  std::vector<Continuation> Pending;
  auto Validate = [&](uint64_t Address, uint64_t Size,
                      unsigned Rights) -> llvm::Error {
    auto Access = CPU.canAccess(Address, Size, Rights | UserAccessible);
    if (!Access)
      return Access.takeError();
    return *Access ? llvm::Error::success() : failure(text::Return);
  };
  auto Return = [&](uint64_t StackPointer, size_t Event,
                    uint64_t Value) -> llvm::Error {
    Result.NativeCalls[Event].Result = Value;
    // Guest callbacks and API outputs may alter the live return slot. CPU
    // restoration preserves memory writes and precedes the ARM64 LR read.
    if (X64)
      if (auto E = Validate(StackPointer, PointerSize, Read))
        return E;
    auto ReturnPC = ABI->readReturnAddress(CPU, StackPointer);
    if (!ReturnPC)
      return ReturnPC.takeError();
    if (auto E = Validate(*ReturnPC, 1, Execute))
      return E;
    auto ReturnSP = ABI->returnStackPointer(StackPointer);
    if (!ReturnSP)
      return ReturnSP.takeError();
    if (auto E = CPU.writeRegister(ABI->info().Result, {Value, 0}))
      return E;
    if (auto E = CPU.writeRegister(ABI->info().StackPointer, {*ReturnSP, 0}))
      return E;
    Result.PC = *ReturnPC;
    return CPU.writeRegister(PCRegister, {Result.PC, 0});
  };
  auto CurrentLife = [&]() -> Lifetime & {
    return Pending.empty()
               ? Life
               : *std::get<Loader::Operation>(Pending.back().Operation)
                      .Notifications;
  };
  auto CurrentCleanup = [&]() -> FLSCleanup * {
    return Pending.empty() ? (ExitCleanup ? &*ExitCleanup : nullptr)
                           : std::get_if<FLSCleanup>(&Pending.back().Operation);
  };
  StoppedProcess Stopped(CPU, **Space, *ABI, *Program, *Env, Active,
                         Initializers, Result, OS,
                         {StackBase, Options.StackSize});
  NativeEntryEvidence NativeEntries(X64 ? llvm::ArrayRef(Program->Gates)
                                        : llvm::ArrayRef<Import>{});
  const auto NativeWatches = NativeEntries.watches();
  std::vector<ExecutionWatch> ObserverWatches;
  auto SetWatches = [&](std::vector<ExecutionWatch> Watches) {
    ObserverWatches = std::move(Watches);
    auto Combined = ObserverWatches;
    // Keep an already ordered observer set ordered. Appending the lower
    // system-image range would force a full sort at every watched instruction.
    // The session still validates and normalizes arbitrary caller ordering.
    if (!NativeWatches.empty()) {
      auto Position = llvm::find_if(Combined, [&](const ExecutionWatch &W) {
        return W.Address >= NativeWatches.front().Address;
      });
      Combined.insert(Position, NativeWatches.begin(), NativeWatches.end());
    }
    return (*Session)->watchExecution(std::move(Combined));
  };
  bool Observing = false;
  auto Invoking = [&]() -> llvm::Error {
    if (!Observing || !Observer)
      return llvm::Error::success();
    auto Watches = Observer->invoking(Stopped);
    if (!Watches)
      return Watches.takeError();
    if (*Watches)
      if (auto E = SetWatches(std::move(**Watches)))
        return E;
    return (*Session)->watchMemoryWrites(Observer->writeWatches());
  };
  auto Resume = [&](uint64_t Value, uint32_t Error) -> llvm::Error {
    auto &C = Pending.back();
    if (auto E = CPU.restoreContext(*C.Context))
      return E;
    if (Error)
      if (auto E = CPU.writeInteger(TEB + TebLastError, Error, DWordSize))
        return E;
    if (auto E = Return(C.StackPointer, C.Event, Value))
      return E;
    Active = std::move(C.Active);
    ExpectedSP = C.ExpectedSP;
    ExpectedGate = C.ExpectedGate;
    Pending.pop_back();
    return Invoking();
  };
  auto Prepare = [&]() -> llvm::Expected<bool> {
    while (true) {
      // Root thread cleanup precedes DLL/TLS process-detach notifications.
      // The services owner retains the sweep boundary across guest calls.
      if (Pending.empty() && Life.exitsNormally() && !ExitCleanup &&
          !ExitCleanupDone) {
        auto Next = OS.exitCleanup();
        if (!Next)
          return Next.takeError();
        ExitCleanup = std::move(*Next);
        ExitCleanupDone = !ExitCleanup;
      }
      if (const auto *Cleanup = CurrentCleanup()) {
        auto Executable =
            CPU.canAccess(Cleanup->Function, 1, Execute | UserAccessible);
        if (!Executable)
          return Executable.takeError();
        if (!*Executable || (!X64 && Cleanup->Function % DWordSize))
          return failure(text::FLSCallback);
        Active = Lifetime::Call{Lifetime::CallKind::FLS,
                                Cleanup->Function,
                                AttachReturnGate,
                                {Cleanup->Argument},
                                false};
      } else {
        auto Next = CurrentLife().next(CPU);
        if (!Next)
          return Next.takeError();
        Active = std::move(*Next);
      }
      if (Active) {
        const uint64_t Top =
            (Pending.empty() ? RootStackPointer : Pending.back().StackPointer) &
            ~(ABI->info().StackAlignment - 1);
        if (Top <= StackBase || Top > StackTop)
          return failure(text::Return);
        Result.PC = Active->PC;
        ExpectedGate = Active->ReturnGate;
        auto Frame = ABI->prepareCall(CPU, StackBase, Top - StackBase,
                                      ExpectedGate, Active->Arguments);
        if (!Frame)
          return Frame.takeError();
        ExpectedSP = Frame->ReturnStackPointer;
        if (auto E = CPU.writeRegister(PCRegister, {Result.PC, 0}))
          return std::move(E);
        if (auto E = Invoking())
          return std::move(E);
        return true;
      }
      if (Pending.empty())
        return false;
      auto &Operation = std::get<Loader::Operation>(Pending.back().Operation);
      if (auto E = Modules.complete(Operation))
        return std::move(E);
      if (Operation.Notifications)
        continue;
      if (auto E = Resume(Operation.Value, Operation.Error))
        return std::move(E);
      return true;
    }
  };
  auto BeginContinuation = [&](Continuation::Work Work, uint64_t StackPointer,
                               size_t Event) -> llvm::Error {
    auto Context = CPU.saveContext();
    if (!Context)
      return Context.takeError();
    Pending.push_back({std::move(Work), std::move(*Context), StackPointer,
                       ExpectedSP, ExpectedGate, Event, std::move(Active)});
    auto More = Prepare();
    if (!More)
      return More.takeError();
    return *More ? llvm::Error::success() : failure(text::Return);
  };
  auto Prepared = Prepare();
  if (!Prepared)
    return Prepared.takeError();
  if (Observer) {
    auto Watches = Observer->started(Stopped);
    if (!Watches)
      return Watches.takeError();
    if (auto E = SetWatches(std::move(*Watches)))
      return std::move(E);
  } else if (auto E = SetWatches({})) {
    return std::move(E);
  }
  Observing = true;
  auto Failed = [&](llvm::Error E) {
    Result.Stop = ProcessStopReason::RuntimeFailure;
    Result.ExitStatus.reset();
    Result.Diagnostic = llvm::toString(std::move(E));
  };
  auto Complete = [&]() {
    Result.Stop = ProcessStopReason::Exited;
    Result.ExitStatus = Life.exitStatus();
  };
  while (true) {
    if (Observer) {
      auto Watches = Observer->resuming(Stopped);
      if (!Watches) {
        Failed(Watches.takeError());
        break;
      }
      if (*Watches)
        if (auto E = SetWatches(std::move(**Watches))) {
          Failed(std::move(E));
          break;
        }
      if (auto E = (*Session)->watchMemoryWrites(Observer->writeWatches())) {
        Failed(std::move(E));
        break;
      }
    }
    // OS services, exceptions and invocation preparation can change stopped
    // memory. Only a pure execution-watch handoff proves the preceding write
    // watches stayed untouched; committed writes stop the session first.
    Stopped.setWatchedMemoryUnchanged(false);
    auto Exit = (*Session)->run(Result.PC, Options.InstructionQuantum);
    if (!Exit) {
      Failed(Exit.takeError());
      break;
    }
    Result.LastCPUExit = std::move(Exit->CPU);
    auto PC = CPU.readRegister(PCRegister);
    if (!PC) {
      Failed(PC.takeError());
      break;
    }
    Result.PC = (*PC)[0];
    if (Exit->Kind == SessionExitKind::Quantum ||
        Exit->Kind == SessionExitKind::MemoryWriteWatch)
      continue;
    if (Exit->Kind == SessionExitKind::ExecutionWatch) {
      Stopped.setWatchedMemoryUnchanged(true);
      // The watched instruction has not executed. Internal native-entry
      // watches are independent of the caller's observation contract.
      if (auto E = NativeEntries.watched(CPU, Result.PC)) {
        Failed(std::move(E));
        break;
      }
      const auto ContainsPC = [&](const ExecutionWatch &W) {
        return Result.PC >= W.Address && Result.PC - W.Address < W.Size;
      };
      // The installed set is exactly the union of both owners. Outside the
      // small native range, the observer necessarily requested this stop;
      // scanning its entire image-watch list again would be quadratic work.
      if (!Observer || (llvm::any_of(NativeWatches, ContainsPC) &&
                        !llvm::any_of(ObserverWatches, ContainsPC)))
        continue;
      auto Next = Observer->watched(Stopped, Result.PC);
      if (!Next) {
        Failed(Next.takeError());
        break;
      }
      if (!*Next) {
        Result.Stop = ProcessStopReason::Observer;
        Result.Diagnostic = runtime::ObserverStop;
        break;
      }
      if (auto E = SetWatches(std::move(**Next))) {
        Failed(std::move(E));
        break;
      }
      continue;
    }
    if (Exit->Kind == SessionExitKind::InstructionLimit) {
      Result.Stop = ProcessStopReason::InstructionLimit;
      Result.Diagnostic = runtime::InstructionLimit;
      break;
    }
    if (Exit->Kind == SessionExitKind::Timeout) {
      Result.Stop = ProcessStopReason::Timeout;
      Result.Diagnostic = runtime::Timeout;
      break;
    }
    const bool Fault =
        Result.LastCPUExit &&
        Result.LastCPUExit->Kind == ExecutionExitKind::RecoverableFault;
    if (!Result.LastCPUExit ||
        (Result.LastCPUExit->Kind != ExecutionExitKind::ServiceRequest &&
         !Fault)) {
      Result.Stop = ProcessStopReason::CPUFailure;
      Result.Diagnostic =
          Result.LastCPUExit ? Result.LastCPUExit->Diagnostic : text::CPUExit;
      break;
    }
    if (!Resources->consumeEvents()) {
      Result.Stop = ProcessStopReason::EventLimit;
      Result.Diagnostic = runtime::EventLimit;
      break;
    }
    if (Fault) {
      NativeEntries.invalidate();
      auto Raised = (*Session)->takeRecoverableFault();
      if (!Raised) {
        Failed(Raised.takeError());
        break;
      }
      auto SP = CPU.readRegister(ABI->info().StackPointer);
      if (!SP) {
        Failed(SP.takeError());
        break;
      }
      auto Transfer = Exceptions.beginFault(*Raised, (*SP)[0], Pending.size());
      if (!Transfer) {
        Failed(Transfer.takeError());
        break;
      }
      Result.PC = Transfer->PC;
      continue;
    }
    auto Request = (*Session)->takeServiceRequest();
    if (!Request) {
      Failed(Request.takeError());
      break;
    }
    const bool EnteredNativeExport = NativeEntries.take(Request->PC);
    auto SP = CPU.readRegister(ABI->info().StackPointer);
    if (!SP) {
      Failed(SP.takeError());
      break;
    }
    if (Request->Kind != (X64 ? ServiceRequestKind::X64Syscall
                              : ServiceRequestKind::AArch64SVC) ||
        Request->Immediate) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = text::Service;
      break;
    }
    if (Exceptions.returning(Request->PC, (*SP)[0], Pending.size())) {
      auto V = CPU.readRegister(ABI->info().Result);
      if (!V) {
        Failed(V.takeError());
        break;
      }
      auto Transfer = Exceptions.returned(uint32_t((*V)[0]));
      if (!Transfer) {
        Failed(Transfer.takeError());
        break;
      }
      Result.PC = Transfer->PC;
      if (Transfer->CompletedEvent)
        Result.NativeCalls[*Transfer->CompletedEvent].Result = 0;
      continue;
    }
    if (!Exceptions.activeAt(Pending.size()) && Request->PC == ExpectedGate &&
        (*SP)[0] == ExpectedSP) {
      if (auto *Cleanup = CurrentCleanup()) {
        auto Completed = OS.complete(*Cleanup);
        if (!Completed) {
          Failed(Completed.takeError());
          break;
        }
        if (*Completed && !Pending.empty()) {
          if (auto E = Resume(1, 0)) {
            Failed(std::move(E));
            break;
          }
          continue;
        }
        if (*Completed)
          ExitCleanup.reset();
      } else {
        auto V = CPU.readRegister(ABI->info().Result);
        if (!V) {
          Failed(V.takeError());
          break;
        }
        if (Active->Kind == Lifetime::CallKind::Entry)
          Result.ReturnValue = (*V)[0];
        if (auto E = CurrentLife().returned((*V)[0])) {
          Failed(std::move(E));
          break;
        }
        auto Input = inputModule(*Program);
        if (Active->Kind == Lifetime::CallKind::TLS && Input &&
            Active->Arguments[0] == Program->Modules[*Input].Loaded.Base &&
            Active->Arguments[1] == DLLProcessAttach)
          Initializers.push_back(Active->PC);
      }
      auto More = Prepare();
      if (!More) {
        Failed(More.takeError());
        break;
      }
      if (!*More) {
        Complete();
        break;
      }
      continue;
    }
    const Import *Import = nullptr;
    for (const auto &I : Program->Gates)
      if (I.Gate == Request->PC ||
          (I.Module == text::NTDLL &&
           Request->PC == I.Gate + NativeSyscallOffset &&
           nativeServiceNumber(I.Name))) {
        Import = &I;
        break;
      }
    const struct Import *Export = Import;
    struct Import NativeImport{};
    std::optional<uint64_t> DirectServiceNumber;
    const bool NativeCall =
        X64 && (!Import || (Request->PC == Import->Gate + NativeSyscallOffset &&
                            nativeServiceNumber(Import->Name)));
    if (NativeCall) {
      auto Number = CPU.reg(X64Register::AX);
      if (!Number) {
        Failed(Number.takeError());
        break;
      }
      // Witness the complete prologue and the actual declared number. Equal
      // RCX/R10 values alone cannot distinguish a call from an interior jump.
      if (Export && (!EnteredNativeExport ||
                     nativeServiceNumber(Export->Name) != *Number))
        Export = nullptr;
      Import = nullptr;
      if (const auto *Native = findNativeService(*Number)) {
        NativeImport.Module = text::NTDLL;
        NativeImport.Name = Native->Name;
        NativeImport.Gate = Request->PC;
        NativeImport.Target = findService(text::NTDLL, Native->Name);
        Import = &NativeImport;
        if (!Export)
          DirectServiceNumber = *Number;
      }
    }
    if (!Import) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = text::Service;
      // No model service has this identity. Keep the stopped inputs in the
      // diagnostic; never guess a Windows-version-specific number mapping.
      if (X64) {
        const std::pair<const char *, CPURegister> Registers[] = {
            {"rax", CPURegister::X64AX}, {"r10", CPURegister::X64R10},
            {"rdx", CPURegister::X64DX}, {"r8", CPURegister::X64R8},
            {"r9", CPURegister::X64R9},  {"rsp", CPURegister::X64SP}};
        for (const auto &[Name, Register] : Registers) {
          auto Value = CPU.readRegister(Register);
          if (!Value) {
            llvm::consumeError(Value.takeError());
            continue;
          }
          Result.Diagnostic +=
              std::string("; ") + Name + "=0x" + llvm::utohexstr((*Value)[0]);
        }
      }
      break;
    }
    if (Observing && Observer && Export) {
      // This is call-boundary metadata, not an API result or signature.
      // Read only committed RAM/registers; a missing return cannot authorize
      // an import repair and does not change an opaque export's outcome.
      std::optional<uint64_t> ReturnAddress;
      if (ABI->info().Link != CPURegister::Invalid) {
        auto Link = Stopped.readRegister(ABI->info().Link);
        if (!Link)
          llvm::consumeError(Link.takeError());
        else
          ReturnAddress = (*Link)[0];
      } else {
        std::array<uint8_t, PointerSize> Bytes{};
        if (auto E = Stopped.read((*SP)[0], Bytes))
          llvm::consumeError(std::move(E));
        else
          ReturnAddress = llvm::support::endian::read64le(Bytes.data());
      }
      if (auto E = Observer->exporting(
              Stopped,
              {Export->Gate, Export->Module, Export->Name, Export->Ordinal},
              ReturnAddress)) {
        Failed(std::move(E));
        break;
      }
    }
    if (!Import->Target) {
      // The guest was free to resolve and store this address; only calling
      // it requires behavior the model does not have.
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic =
          (text::OpaqueEntry + Import->Module + text::ImportSeparator +
           (Import->Ordinal
                ? text::OpaqueOrdinal + llvm::Twine(*Import->Ordinal)
                : llvm::Twine(Import->Name)))
              .str();
      break;
    }
    const uint64_t StackPointer = (*SP)[0];
    if (auto E = ABI->validateStackPointer(StackPointer)) {
      Failed(std::move(E));
      break;
    }
    // User accessibility is an OS call-boundary requirement in addition to
    // the ABI's generic trusted-memory checks. Preflight before API effects.
    if (Import->Target->Returns && X64 && !NativeCall)
      if (auto E = Validate(StackPointer, PointerSize, Read)) {
        Failed(std::move(E));
        break;
      }
    NativeCallEvent Event{Request->PC, Import->Target->Name};
    Event.Module = Import->Module;
    Event.DirectServiceNumber = DirectServiceNumber;
    Event.ArgumentCount = Import->Target->Arguments;
    if (X64 && Export) {
      auto Ret = (*Space)->readInteger(StackPointer, PointerSize);
      if (!Ret)
        llvm::consumeError(Ret.takeError());
      else
        Event.ReturnAddress = *Ret;
    }
    bool Invalid = false;
    for (unsigned I = 0; I < Event.ArgumentCount; ++I) {
      auto Location = ABI->argumentLocation(StackPointer, I);
      if (!Location) {
        Failed(Location.takeError());
        Invalid = true;
        break;
      }
      if (Location->Register == CPURegister::Invalid)
        if (auto E = Validate(Location->Address, PointerSize, Read)) {
          Failed(std::move(E));
          Invalid = true;
          break;
        }
      auto V = NativeCall ? readNativeServiceArgument(CPU, StackPointer, I)
                          : ABI->readArgument(CPU, StackPointer, I);
      if (!V) {
        Failed(V.takeError());
        Invalid = true;
        break;
      }
      Event.Arguments[I] = *V;
    }
    if (Invalid)
      break;
    const size_t EventIndex = Result.NativeCalls.size();
    Result.NativeCalls.push_back(Event);
    auto V = OS.invoke(*Import->Target, Event);
    if (!V) {
      Failed(V.takeError());
      break;
    }
    if (V->Raised) {
      // RaiseException resumes through its modeled executable RET. Its CONTEXT
      // therefore points inside the provider, preserving the original live
      // return slot and ARM64 LR when a handler elects to continue.
      if (auto E = CPU.writeRegister(PCRegister, {Request->NextPC, 0})) {
        Failed(std::move(E));
        break;
      }
      V->Raised->Address = Request->NextPC;
      auto Transfer = Exceptions.begin(std::move(*V->Raised), StackPointer,
                                       Pending.size(), EventIndex);
      if (!Transfer) {
        Failed(Transfer.takeError());
        break;
      }
      Result.PC = Transfer->PC;
      continue;
    }
    // Loader and service callbacks share one bounded continuation stack.
    // Check before a loader request can acquire references or map modules.
    if ((V->Cleanup || V->Request) && Pending.size() >= MaxContinuationDepth) {
      Failed(failure(text::ModuleBudget));
      break;
    }
    if (V->Cleanup) {
      if (auto E = BeginContinuation(*V->Cleanup, StackPointer, EventIndex)) {
        Failed(std::move(E));
        break;
      }
      continue;
    }
    if (V->Request) {
      // Loader operations during termination do not share the ordinary
      // load/unload contract. Stop before acquiring or releasing references.
      if (ExitCleanup) {
        Result.Stop = ProcessStopReason::UnsupportedService;
        Result.Diagnostic = text::FLSExitLoader;
        break;
      }
      auto Operation = Modules.begin(*V->Request);
      if (!Operation) {
        Failed(Operation.takeError());
        break;
      }
      if (Operation->Notifications) {
        if (auto E = BeginContinuation(std::move(*Operation), StackPointer,
                                       EventIndex)) {
          Failed(std::move(E));
          break;
        }
        continue;
      }
      if (Operation->Error)
        if (auto E = CPU.writeInteger(TEB + TebLastError, Operation->Error,
                                      DWordSize)) {
          Failed(std::move(E));
          break;
        }
      V->Value = Operation->Value;
    }
    if (!V->Value) {
      if (Result.Stop != ProcessStopReason::Exited)
        break;
      const uint32_t Status = *Result.ExitStatus;
      Result.ExitStatus.reset();
      Pending.clear();
      Exceptions.abandon();
      // Process-detach callbacks may still observe the exiting caller's
      // frame. Abandon its continuation without overwriting that storage.
      RootStackPointer = StackPointer;
      if (auto E = Life.exit(Status)) {
        Failed(std::move(E));
        break;
      }
      auto More = Prepare();
      if (!More) {
        Failed(More.takeError());
        break;
      }
      if (*More)
        continue;
      Complete();
      break;
    }
    if (NativeCall) {
      if (auto E = returnNativeService(CPU, *Request, *V->Value)) {
        Failed(std::move(E));
        break;
      }
      Result.NativeCalls[EventIndex].Result = *V->Value;
      Result.PC = Request->NextPC;
      continue;
    }
    if (auto E = Return(StackPointer, EventIndex, *V->Value)) {
      Failed(std::move(E));
      break;
    }
  }
  Result.Instructions = Resources->instructions();
  Result.Events = Resources->events();
  return Result;
}
} // namespace neverd::emulation::windows_process
