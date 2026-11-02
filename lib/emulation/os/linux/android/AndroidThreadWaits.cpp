//===- AndroidThreadWaits.cpp - Guest waits and service completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidThreads.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
std::optional<size_t> GuestThreads::onceOwner(uint64_t Control) const {
  for (size_t I = 0; I < Threads.size(); ++I)
    for (const auto &Pending : Threads[I].Callbacks)
      if (const auto *Once =
              std::get_if<OnceCallback>(&Pending.Call.Continuation))
        if (Once->Control == Control)
          return I;
  return std::nullopt;
}
bool GuestThreads::onceActive(uint64_t Control) const {
  return onceOwner(Control).has_value();
}
BionicResult GuestThreads::waitOnce(uint64_t Control) {
  const auto Owner = onceOwner(Control);
  // A raw running value is not evidence of an initializer that can finish.
  // Same-thread recursion and externally forged controls remain unsupported.
  if (!Owner || *Owner == Current)
    return unsupported(diagnostic::OnceInProgress);
  assert(!current().Waiting && "running thread already has a pending wait");
  current().Waiting = Wait{Once{*Owner, Control}, std::nullopt};
  return std::optional<BionicValue>(GuestThreadWait{});
}
void GuestThreads::completeOnce(uint64_t Control) {
  // Bionic calls this only after the actual initializer returns and the
  // completion word has been written successfully to shared guest memory.
  for (auto &T : Threads)
    if (T.Waiting)
      if (auto *Once = std::get_if<GuestThreads::Once>(&T.Waiting->Operation))
        if (Once->Owner == Current && Once->Control == Control)
          Once->Ready = true;
}
void GuestThreads::waitMutex(uint64_t Address, uint16_t Attributes) {
  auto &Waiting = current().Waiting;
  if (Waiting) {
    auto &Mutex = std::get<GuestThreads::Mutex>(Waiting->Operation);
    assert(Mutex.Address == Address && Mutex.Attributes == Attributes);
    // A wake does not grant ownership. Preserve the suspended request and
    // its event when another thread acquires the lock before this one runs.
    Mutex.Ready = false;
    return;
  }
  Waiting = Wait{Mutex{Address, Attributes}, std::nullopt};
}

void GuestThreads::wakeMutex(uint64_t Address) {
  // FUTEX_WAKE(1) has no guest ordering guarantee. Choose one sleeper in the
  // same deterministic round-robin order used to schedule runnable threads.
  for (size_t Offset = 1; Offset <= Threads.size(); ++Offset) {
    auto &Waiting = Threads[(Current + Offset) % Threads.size()].Waiting;
    if (!Waiting)
      continue;
    auto *Mutex = std::get_if<GuestThreads::Mutex>(&Waiting->Operation);
    if (Mutex && Mutex->Address == Address && !Mutex->Ready) {
      Mutex->Ready = true;
      return;
    }
  }
}

bool GuestThreads::waitKernel(const std::optional<SleepContinuation> &Sleep) {
  auto &T = current();
  if (T.Kernel.Pending) {
    assert(!T.Waiting);
    T.Waiting = Wait{std::move(T.Kernel.Pending), std::nullopt};
    T.Waiting->Sleep = Sleep;
    return true;
  }
  return false;
}

void GuestThreads::waitNative(std::unique_ptr<NativeWait> Pending) {
  assert(!current().Waiting && !current().Kernel.Pending);
  current().Waiting = Wait{std::move(Pending), std::nullopt};
}

void GuestThreads::waitSemaphore(uint64_t Address, bool Shared) {
  auto &Waiting = current().Waiting;
  if (Waiting) {
    auto &S = std::get<Semaphore>(Waiting->Operation);
    assert(S.Address == Address && S.Shared == Shared);
    S.Ready = false;
    return;
  }
  Waiting = Wait{Semaphore{Address, Shared}, std::nullopt};
}

void GuestThreads::wakeSemaphore(uint64_t Address) {
  // API 28 wakes all sleepers after posting to a contended word. They race
  // for the one new token when each resumes; no ownership is assigned here.
  for (auto &T : Threads)
    if (T.Waiting)
      if (auto *S = std::get_if<Semaphore>(&T.Waiting->Operation))
        if (S->Address == Address)
          S->Ready = true;
}

bool GuestThreads::ready(const Wait &Pending) const {
  return std::visit(
      llvm::makeVisitor(
          [&](const Join &J) { return Threads[J.Target].Report.Finished; },
          [](const Once &O) { return O.Ready; },
          [](const Mutex &M) { return M.Ready; },
          [](const Semaphore &S) { return S.Ready; },
          [](const std::unique_ptr<linux_model::KernelWait> &K) {
            return K->ready();
          },
          [](const std::unique_ptr<NativeWait> &N) { return N->ready(); }),
      Pending.Operation);
}

std::optional<uint64_t> GuestThreads::nextWake() const {
  std::optional<uint64_t> Deadline;
  for (const auto &T : Threads) {
    if (!T.Waiting || T.Report.Finished)
      continue;
    std::optional<uint64_t> Wake;
    if (const auto *K = std::get_if<std::unique_ptr<linux_model::KernelWait>>(
            &T.Waiting->Operation))
      Wake = (*K)->nextWake();
    else if (const auto *N = std::get_if<std::unique_ptr<NativeWait>>(
                 &T.Waiting->Operation))
      Wake = (*N)->nextWake();
    if (Wake && (!Deadline || *Wake < *Deadline))
      Deadline = Wake;
  }
  return Deadline;
}

llvm::Error GuestThreads::prepareJoin(const Join &Pending) {
  if (!Pending.Output)
    return llvm::Error::success();
  if (auto E = access(Pending.Output, 8, Write))
    return E;
  return put64(Pending.Output, *Threads[Pending.Target].Report.ReturnValue);
}

BionicResult GuestThreads::resumeWait(Bionic &LibC) {
  const auto &Pending = *current().Waiting;
  assert(Pending.Request && "scheduled wait has no suspended service");
  // Resumption can fail before another guest instruction executes. Attribute
  // that stop to this service, not the previous thread's last instruction.
  Result.PC = Pending.Request->PC;
  return std::visit(llvm::makeVisitor(
                        [&](const Join &J) -> BionicResult {
                          if (auto E = prepareJoin(J))
                            return std::move(E);
                          return value(0);
                        },
                        [&](const Once &O) { return resumeOnce(O); },
                        [&](const Mutex &M) {
                          return LibC.resumeMutex(M.Address, M.Attributes);
                        },
                        [&](const Semaphore &S) {
                          return LibC.resumeSemaphore(S.Address, S.Shared);
                        },
                        [&](const std::unique_ptr<linux_model::KernelWait> &K)
                            -> BionicResult {
                          auto R = K->resume(Result);
                          if (!R)
                            return R.takeError();
                          if (Pending.Source == WaitSource::NativeCall)
                            return LibC.kernelResult(*R, Pending.Sleep);
                          if (!*R)
                            return std::optional<BionicValue>();
                          return value(**R);
                        },
                        [&](const std::unique_ptr<NativeWait> &N) {
                          return N->resume(LibC);
                        }),
                    Pending.Operation);
}

BionicResult GuestThreads::resumeOnce(const Once &Once) {
  // Recheck memory after suspension: a different thread may have unmapped
  // or modified it. Failure must leave the original import incomplete.
  if (auto E = access(Once.Control, once_abi::ControlBytes, Read,
                      once_abi::ControlBytes))
    return std::move(E);
  uint8_t Bytes[once_abi::ControlBytes];
  if (auto E = CPU.read(Once.Control, Bytes))
    return std::move(E);
  if (llvm::support::endian::read32le(Bytes) != once_abi::Complete)
    return failure(diagnostic::OnceWaitControl);
  return value(0);
}

llvm::Error GuestThreads::completeWait(uint64_t Value) {
  const auto &Pending = *current().Waiting;
  if (Pending.Request) {
    if (auto E = linux_model::returnService(CPU, *Pending.Request, Value))
      return E;
    switch (Pending.Source) {
    case WaitSource::NativeCall:
      Result.NativeCalls[Pending.Event].Result = Value;
      break;
    case WaitSource::KernelService:
      Result.Services[Pending.Event].Result = Value;
      break;
    }
  }
  if (const auto *J = std::get_if<Join>(&Pending.Operation))
    if (auto E = retire(J->Target))
      return E;
  current().Waiting.reset();
  return llvm::Error::success();
}

llvm::Expected<bool>
GuestThreads::interruptWait(const LinuxSignalAction &Action) {
  auto &Pending = *current().Waiting;
  assert(Pending.Request && "interrupted wait has no suspended service");
  Result.PC = Pending.Request->PC;
  auto *K =
      std::get_if<std::unique_ptr<linux_model::KernelWait>>(&Pending.Operation);
  if (!K) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = linux_model::SignalWaitUnsupported;
    return false;
  }
  if ((*K)->restartOnSignal(Action)) {
    // No result was installed while suspended: registers still hold the
    // original trap/import arguments. Put that PC in the mutable signal frame.
    // A handler may redirect it, change arguments, close/reuse the FD, or issue
    // a nested wait. Re-execute only the trap, never the caller's earlier code.
    if (auto E =
            CPU.writeRegister(CPURegister::AArch64PC, {Pending.Request->PC, 0}))
      return std::move(E);
    current().Waiting.reset();
    // This attempt did not return. A re-entered trap has its own event; do not
    // manufacture a completed result for the abandoned attempt.
    return true;
  }
  auto Interrupted = (*K)->interrupt(Result);
  if (!Interrupted)
    return Interrupted.takeError();
  if (!*Interrupted)
    return false;
  if (Pending.Source == WaitSource::KernelService) {
    if (auto E = completeWait(**Interrupted))
      return std::move(E);
  } else {
    // The modeled import's post-trap PC is its libc return boundary. Save the
    // raw kernel result in the signal frame; errno conversion belongs after
    // rt_sigreturn, not before entering the handler.
    if (auto E =
            linux_model::returnService(CPU, *Pending.Request, **Interrupted))
      return std::move(E);
    current().SignalReturns.push_back({*Pending.Request, Pending.Event,
                                       Signals.frameDepth(id()),
                                       Pending.Sleep});
    current().Waiting.reset();
  }
  return true;
}

llvm::Error GuestThreads::resumeSignal(Bionic &LibC) {
  auto &Returns = current().SignalReturns;
  if (Returns.empty() || Signals.frameDepth(id()) != Returns.back().Depth)
    return llvm::Error::success();
  const auto Pending = Returns.back();
  Returns.pop_back();
  auto PC = CPU.readRegister(CPURegister::AArch64PC);
  if (!PC)
    return PC.takeError();
  // A ucontext redirect can deliberately bypass the modeled wrapper. Do not
  // force its old return value/PC/errno onto the new guest continuation, or
  // claim that this native call returned normally.
  if ((*PC)[0] != Pending.Request.NextPC)
    return llvm::Error::success();
  auto Raw = CPU.readRegister(CPURegister::AArch64X0);
  if (!Raw)
    return Raw.takeError();
  auto Adapted = LibC.kernelResult((*Raw)[0], Pending.Sleep);
  if (!Adapted)
    return Adapted.takeError();
  assert(*Adapted && std::holds_alternative<uint64_t>(**Adapted));
  const auto Value = std::get<uint64_t>(**Adapted);
  if (auto E = CPU.writeRegister(CPURegister::AArch64X0, {Value, 0}))
    return E;
  Result.NativeCalls[Pending.Event].Result = Value;
  return llvm::Error::success();
}

void GuestThreads::suspend(const ServiceRequest &Request, size_t Event,
                           WaitSource Source) {
  assert(current().Waiting && !current().Waiting->Request);
  current().Waiting->Request = Request;
  current().Waiting->Event = Event;
  current().Waiting->Source = Source;
}
} // namespace neverd::emulation::android_model
