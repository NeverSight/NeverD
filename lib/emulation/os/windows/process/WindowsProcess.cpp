//===- WindowsProcess.cpp - Windows PE64 process continuations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../../runtime/RuntimeValues.h"
#include "WindowsProcessLoader.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::windows_process {
using namespace value;
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options) {
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
  for (uint64_t Gate : {ReturnGate, AttachReturnGate, DetachReturnGate})
    if (auto E = (*Space)->write(Gate, Trap))
      return std::move(E);
  for (const auto &Gate : Program->Gates)
    if (auto E = (*Space)->write(Gate.Gate, Trap))
      return std::move(E);
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
  auto Backend =
      createExecutionBackend(Options.Backend,
                             X64 ? ExecutionContract::CheckedUserX64
                                 : ExecutionContract::CheckedUserAArch64,
                             *Space, Loaded->Architecture);
  if (!Backend)
    return Backend.takeError();
  if (auto E = Backend->CPU->writeRegister(
          X64 ? CPURegister::X64GSBase : CPURegister::AArch64X18, {TEB, 0}))
    return std::move(E);
  auto Session = ExecutionSession::create(std::move(Backend->CPU), Resources);
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
              *Resources);
  Lifetime Life(*Program);
  Loader Modules(*Program, Virtual, **Space, *Env, CPU, *Resources);
  std::optional<Lifetime::Call> Active;
  uint64_t ExpectedSP = 0, ExpectedGate = 0;
  uint64_t RootStackPointer = StackTop;
  struct Continuation {
    Loader::Operation Operation;
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
    return Pending.empty() ? Life : *Pending.back().Operation.Notifications;
  };
  auto Prepare = [&]() -> llvm::Expected<bool> {
    while (true) {
      auto Next = CurrentLife().next(CPU);
      if (!Next)
        return Next.takeError();
      Active = std::move(*Next);
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
        return true;
      }
      if (Pending.empty())
        return false;
      auto &C = Pending.back();
      if (auto E = Modules.complete(C.Operation))
        return std::move(E);
      if (C.Operation.Notifications)
        continue;
      if (auto E = CPU.restoreContext(*C.Context))
        return std::move(E);
      if (C.Operation.Error)
        if (auto E = CPU.writeInteger(TEB + TebLastError, C.Operation.Error,
                                      DWordSize))
          return std::move(E);
      if (auto E = Return(C.StackPointer, C.Event, C.Operation.Value))
        return std::move(E);
      Active = std::move(C.Active);
      ExpectedSP = C.ExpectedSP;
      ExpectedGate = C.ExpectedGate;
      Pending.pop_back();
      return true;
    }
  };
  auto Prepared = Prepare();
  if (!Prepared)
    return Prepared.takeError();
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
    if (Exit->Kind == SessionExitKind::Quantum)
      continue;
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
    if (!Result.LastCPUExit ||
        Result.LastCPUExit->Kind != ExecutionExitKind::ServiceRequest) {
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
    auto Request = (*Session)->takeServiceRequest();
    if (!Request) {
      Failed(Request.takeError());
      break;
    }
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
    if (Request->PC == ExpectedGate && (*SP)[0] == ExpectedSP) {
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
      if (I.Gate == Request->PC) {
        Import = &I;
        break;
      }
    if (!Import) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = text::Service;
      break;
    }
    const uint64_t StackPointer = (*SP)[0];
    if (auto E = ABI->validateStackPointer(StackPointer)) {
      Failed(std::move(E));
      break;
    }
    // User accessibility is an OS call-boundary requirement in addition to
    // the ABI's generic trusted-memory checks. Preflight before API effects.
    if (Import->Target->Returns && X64)
      if (auto E = Validate(StackPointer, PointerSize, Read)) {
        Failed(std::move(E));
        break;
      }
    NativeCallEvent Event{Request->PC, Import->Target->Name};
    Event.Module = Import->Module;
    Event.ArgumentCount = Import->Target->Arguments;
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
      auto V = ABI->readArgument(CPU, StackPointer, I);
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
    if (V->Request) {
      if (Pending.size() >= MaxLoaderDepth) {
        Failed(failure(text::ModuleBudget));
        break;
      }
      auto Operation = Modules.begin(*V->Request);
      if (!Operation) {
        Failed(Operation.takeError());
        break;
      }
      if (Operation->Notifications) {
        auto Context = CPU.saveContext();
        if (!Context) {
          Failed(Context.takeError());
          break;
        }
        Pending.push_back({std::move(*Operation), std::move(*Context),
                           StackPointer, ExpectedSP, ExpectedGate, EventIndex,
                           std::move(Active)});
        auto More = Prepare();
        if (!More) {
          Failed(More.takeError());
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
