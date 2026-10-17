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

bool GuestThreads::waitSleep() {
  auto &T = current();
  if (!T.Kernel.SleepDeadline)
    return false;
  assert(!T.Waiting && "running thread already has a pending wait");
  T.Waiting = Wait{Sleep{*T.Kernel.SleepDeadline}, std::nullopt};
  T.Kernel.SleepDeadline.reset();
  return true;
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
          [&](const Sleep &S) { return S.Deadline <= Clock.elapsed(); }),
      Pending.Operation);
}

std::optional<uint64_t> GuestThreads::nextWake() const {
  std::optional<uint64_t> Deadline;
  for (const auto &T : Threads) {
    if (!T.Waiting || T.Report.Finished)
      continue;
    const auto *S = std::get_if<Sleep>(&T.Waiting->Operation);
    if (S && (!Deadline || S->Deadline < *Deadline))
      Deadline = S->Deadline;
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
  return std::visit(
      llvm::makeVisitor(
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
          [&](const Sleep &) -> BionicResult { return value(0); }),
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
void GuestThreads::suspend(const ServiceRequest &Request, size_t Event,
                           WaitSource Source) {
  assert(current().Waiting && !current().Waiting->Request);
  current().Waiting->Request = Request;
  current().Waiting->Event = Event;
  current().Waiting->Source = Source;
}
} // namespace neverd::emulation::android_model
