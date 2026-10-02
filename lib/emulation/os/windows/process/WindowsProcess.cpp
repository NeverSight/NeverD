//===- WindowsProcess.cpp - Windows PE64 process continuations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../../runtime/RuntimeValues.h"
#include "WindowsProcessModules.h"

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
  const auto FileName = Path.filename().u8string();
  auto Env = prepareEnvironment(
      **Space, *Loaded, Options,
      llvm::StringRef(reinterpret_cast<const char *>(FileName.data()),
                      FileName.size()),
      Program->Identities, Program->InitializationOrder);
  if (!Env)
    return Env.takeError();
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
  if (auto E = (*Space)->protect(GateBase, GateSize,
                                 Read | Execute | UserAccessible))
    return std::move(E);
  for (const auto &Module : Program->Modules)
    for (const auto &Region : Module.Loaded.Regions)
      if (auto E = (*Space)->protect(Region.Address, Region.Bytes.size(),
                                     Region.Permissions))
        return std::move(E);
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
  Services OS(CPU, **Space, *Loaded, *Env, Options, Result, Virtual,
              Program->Identities);
  enum class Phase { Attach, Entry, Detach };
  Phase Current = Phase::Attach;
  size_t Callback = 0;
  uint64_t ExpectedSP = 0, ExpectedGate = 0;
  std::optional<uint32_t> ExitStatus;
  uint64_t CallbackArray = 0;
  auto ReadCallbackArray = [&]() -> llvm::Error {
    if (!Loaded->TLSCallbackPointer) {
      CallbackArray = 0;
      return llvm::Error::success();
    }
    auto V = CPU.readInteger(Loaded->TLSCallbackPointer, PointerSize);
    if (!V)
      return V.takeError();
    CallbackArray = *V;
    return llvm::Error::success();
  };
  auto Prepare = [&]() -> llvm::Expected<bool> {
    uint64_t Target = 0;
    if (Current != Phase::Entry && CallbackArray) {
      if (Callback > MaxCallbacks || CallbackArray >= UserLimit ||
          Callback * PointerSize + PointerSize > UserLimit - CallbackArray)
        return failure(text::TLS);
      const uint64_t Slot = CallbackArray + Callback * PointerSize;
      auto Access = CPU.canAccess(Slot, PointerSize, Read | UserAccessible);
      if (!Access)
        return Access.takeError();
      if (!*Access)
        return failure(text::TLS);
      auto V = CPU.readInteger(Slot, PointerSize);
      if (!V)
        return V.takeError();
      Target = *V;
      if (Target) {
        if (Callback == MaxCallbacks || (!X64 && Target % DWordSize))
          return failure(text::TLS);
        auto Executable = CPU.canAccess(Target, 1, Execute | UserAccessible);
        if (!Executable)
          return Executable.takeError();
        if (!*Executable)
          return failure(text::TLS);
        ++Callback;
      }
    }
    if (Current != Phase::Entry && !Target) {
      if (Current == Phase::Detach)
        return false;
      Current = Phase::Entry;
    }
    std::vector<uint64_t> Arguments;
    if (Current == Phase::Entry) {
      Result.PC = Loaded->Entry;
      ExpectedGate = ReturnGate;
      Arguments = {PEB};
    } else {
      Result.PC = Target;
      ExpectedGate =
          Current == Phase::Attach ? AttachReturnGate : DetachReturnGate;
      Arguments = {
          Loaded->Base,
          Current == Phase::Attach ? DLLProcessAttach : DLLProcessDetach, 0};
    }
    auto Frame = ABI->prepareCall(CPU, StackBase, Options.StackSize,
                                  ExpectedGate, Arguments);
    if (!Frame)
      return Frame.takeError();
    ExpectedSP = Frame->ReturnStackPointer;
    if (auto E = CPU.writeRegister(PCRegister, {Result.PC, 0}))
      return std::move(E);
    return true;
  };
  if (auto E = ReadCallbackArray())
    return std::move(E);
  auto Prepared = Prepare();
  if (!Prepared)
    return Prepared.takeError();
  auto Failed = [&](llvm::Error E) {
    Result.Stop = ProcessStopReason::RuntimeFailure;
    Result.ExitStatus.reset();
    Result.Diagnostic = llvm::toString(std::move(E));
  };
  auto StartExit = [&](uint32_t Status) -> llvm::Expected<bool> {
    if (Current == Phase::Detach)
      return failure(text::ReentrantExit);
    ExitStatus = Status;
    Result.ExitStatus.reset();
    Current = Phase::Detach;
    Callback = 0;
    if (auto E = ReadCallbackArray())
      return std::move(E);
    return Prepare();
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
      if (Current == Phase::Entry) {
        auto V = CPU.readRegister(ABI->info().Result);
        if (!V) {
          Failed(V.takeError());
          break;
        }
        Result.ReturnValue = (*V)[0];
        auto More = StartExit(uint32_t((*V)[0]));
        if (!More) {
          Failed(More.takeError());
          break;
        }
        if (!*More) {
          Result.Stop = ProcessStopReason::Exited;
          Result.ExitStatus = ExitStatus;
          break;
        }
      } else {
        auto More = Prepare();
        if (!More) {
          Failed(More.takeError());
          break;
        }
        if (!*More) {
          Result.Stop = ProcessStopReason::Exited;
          Result.ExitStatus = ExitStatus;
          break;
        }
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
    auto Validate = [&](uint64_t Address, uint64_t Size,
                        unsigned Rights) -> llvm::Error {
      auto Access = CPU.canAccess(Address, Size, Rights | UserAccessible);
      if (!Access)
        return Access.takeError();
      return *Access ? llvm::Error::success() : failure(text::Return);
    };
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
    Result.NativeCalls.push_back(Event);
    auto V = OS.invoke(*Import->Target, Event);
    if (!V) {
      Failed(V.takeError());
      break;
    }
    if (!*V) {
      if (Result.Stop != ProcessStopReason::Exited)
        break;
      auto More = StartExit(*Result.ExitStatus);
      if (!More) {
        Failed(More.takeError());
        break;
      }
      if (*More)
        continue;
      Result.ExitStatus = ExitStatus;
      break;
    }
    Result.NativeCalls.back().Result = **V;
    // Consume the live return slot after the API, which may have legally
    // written through an aliased output pointer into the caller's stack.
    auto ReturnPC = ABI->readReturnAddress(CPU, StackPointer);
    if (!ReturnPC) {
      Failed(ReturnPC.takeError());
      break;
    }
    if (auto E = Validate(*ReturnPC, 1, Execute)) {
      Failed(std::move(E));
      break;
    }
    auto ReturnSP = ABI->returnStackPointer(StackPointer);
    if (!ReturnSP) {
      Failed(ReturnSP.takeError());
      break;
    }
    if (auto E = CPU.writeRegister(ABI->info().Result, {**V, 0})) {
      Failed(std::move(E));
      break;
    }
    if (auto E = CPU.writeRegister(ABI->info().StackPointer, {*ReturnSP, 0})) {
      Failed(std::move(E));
      break;
    }
    Result.PC = *ReturnPC;
    if (auto E = CPU.writeRegister(PCRegister, {Result.PC, 0})) {
      Failed(std::move(E));
      break;
    }
  }
  Result.Instructions = Resources->instructions();
  Result.Events = Resources->events();
  return Result;
}
} // namespace neverd::emulation::windows_process
