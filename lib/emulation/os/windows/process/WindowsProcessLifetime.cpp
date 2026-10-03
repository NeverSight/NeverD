//===- WindowsProcessLifetime.cpp - Module initialization and exit --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessLifetime.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation::windows_process {
using namespace value;
Lifetime::Lifetime(windows_process::Program &Program) : Program(Program) {
  for (size_t I : Program.AttachOrder) {
    Program.Modules[I].State = ModuleState::Initializing;
    Pending.push_back({moduleRef(Program, I), CallKind::TLS, DLLProcessAttach});
    Pending.push_back({moduleRef(Program, I), CallKind::DLL, DLLProcessAttach});
  }
  Pending.push_back({moduleRef(Program, 0), CallKind::TLS, DLLProcessAttach});
  Pending.push_back({moduleRef(Program, 0), CallKind::Entry, 0});
}
Lifetime::Lifetime(windows_process::Program &Program, Mode Kind,
                   llvm::ArrayRef<ModuleRef> Order,
                   std::optional<ModuleRef> Failed)
    : Program(Program), Kind(Kind) {
  for (auto Ref : Order) {
    auto &M = Program.Modules[Ref.Index];
    if (Kind == Mode::Load) {
      M.State = ModuleState::Initializing;
      Pending.push_back({Ref, CallKind::TLS, DLLProcessAttach});
      Pending.push_back({Ref, CallKind::DLL, DLLProcessAttach});
    } else {
      M.State = ModuleState::Detaching;
      if (M.Attached || Failed == Ref) {
        Pending.push_back({Ref, CallKind::TLS, DLLProcessDetach});
        Pending.push_back({Ref, CallKind::DLL, DLLProcessDetach});
      }
    }
  }
}
void Lifetime::advance() {
  ++Position;
  Callback = 0;
  CallbackArray.reset();
}
llvm::Expected<std::optional<Lifetime::Call>>
Lifetime::next(ExecutionBackend &CPU) {
  if (Running)
    return failure(text::Lifetime);
  while (Position < Pending.size()) {
    const auto &N = Pending[Position];
    if (!current(Program, N.Module))
      return failure(text::Lifetime);
    auto &Module = Program.Modules[N.Module.Index];
    const auto &M = Module.Loaded;
    // Once detach begins, ExitProcess from this callback must not notify the
    // same DLL again. Later DLLs remain attached until their own turn.
    if (detaching())
      Module.Attached = false;
    uint64_t Target = M.Entry;
    if (N.Kind == CallKind::TLS) {
      if (!CallbackArray) {
        uint64_t Array = 0;
        if (M.TLSCallbackPointer) {
          auto V = CPU.readInteger(M.TLSCallbackPointer, PointerSize);
          if (!V)
            return V.takeError();
          Array = *V;
        }
        CallbackArray = Array;
      }
      Target = 0;
      if (*CallbackArray) {
        if (Callback > MaxCallbacks || *CallbackArray >= UserLimit ||
            Callback * PointerSize + PointerSize > UserLimit - *CallbackArray)
          return failure(text::TLS);
        const uint64_t Slot = *CallbackArray + Callback * PointerSize;
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
          if (Callback == MaxCallbacks ||
              (M.Architecture == GuestArchitecture::AArch64 &&
               Target % DWordSize))
            return failure(text::TLS);
          auto Executable = CPU.canAccess(Target, 1, Execute | UserAccessible);
          if (!Executable)
            return Executable.takeError();
          if (!*Executable)
            return failure(text::TLS);
          ++Callback;
          if (!detaching())
            Module.State = ModuleState::Initializing;
        }
      }
    }
    if (!Target) {
      // A no-entry DLL receives startup TLS, but native process teardown
      // does not notify it. Only successful entry return completes attach.
      if (N.Kind == CallKind::DLL && !detaching())
        Module.State = ModuleState::Ready;
      advance();
      continue;
    }
    Running = true;
    if (N.Kind == CallKind::Entry)
      return std::optional<Call>({N.Kind, M.Entry, ReturnGate, {PEB}});
    if (N.Kind == CallKind::DLL && !detaching())
      Module.State = ModuleState::Initializing;
    return std::optional<Call>(
        {N.Kind,
         Target,
         detaching() ? DetachReturnGate : AttachReturnGate,
         {M.Base, N.Reason,
          N.Kind == CallKind::TLS ||
                  (Kind != Mode::Startup && Kind != Mode::Exit)
              ? 0
              : StartupReserved}});
  }
  return std::optional<Call>();
}
llvm::Error Lifetime::returned(uint64_t Value) {
  if (!Running || Position >= Pending.size())
    return failure(text::Lifetime);
  Running = false;
  const auto N = Pending[Position];
  if (!current(Program, N.Module))
    return failure(text::Lifetime);
  auto &M = Program.Modules[N.Module.Index];
  if (N.Kind == CallKind::TLS)
    return llvm::Error::success();
  advance();
  if (N.Kind == CallKind::Entry) {
    if (llvm::any_of(llvm::drop_begin(Program.Modules), resident))
      return failure(text::EntryThreadExit);
    return beginExit(uint32_t(Value), false);
  }
  if (!detaching()) {
    if (!uint32_t(Value)) {
      if (Kind == Mode::Startup)
        return beginExit(StatusDLLInitFailed, true);
      Failed = N.Module;
      Pending.clear();
      Position = 0;
      return llvm::Error::success();
    }
    M.Attached = true;
    M.State = ModuleState::Ready;
  } else
    M.Attached = false;
  return llvm::Error::success();
}
llvm::Error Lifetime::exit(uint32_t Status) { return beginExit(Status, false); }
llvm::Error Lifetime::beginExit(uint32_t Status, bool InitializationFailed) {
  if (Kind == Mode::Exit)
    return failure(text::ReentrantExit);
  ExitStatus = Status;
  Kind = Mode::Exit;
  Running = false;
  Position = 0;
  Callback = 0;
  CallbackArray.reset();
  Pending.clear();
  // Startup failure terminates without DLL or executable detach. Explicit
  // ExitProcess only detaches DLLs whose attach call has returned success.
  if (InitializationFailed)
    return llvm::Error::success();
  for (auto I = Program.LoaderInitializationOrder.rbegin();
       I != Program.LoaderInitializationOrder.rend(); ++I) {
    if (!resident(Program.Modules[*I]) || !Program.Modules[*I].Attached)
      continue;
    Pending.push_back(
        {moduleRef(Program, *I), CallKind::TLS, DLLProcessDetach});
    Pending.push_back(
        {moduleRef(Program, *I), CallKind::DLL, DLLProcessDetach});
  }
  Pending.push_back({moduleRef(Program, 0), CallKind::TLS, DLLProcessDetach});
  return llvm::Error::success();
}
} // namespace neverd::emulation::windows_process
