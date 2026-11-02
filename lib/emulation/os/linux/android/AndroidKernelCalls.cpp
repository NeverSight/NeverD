//===- AndroidKernelCalls.cpp - Bionic to Linux service boundary ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include <algorithm>

namespace neverd::emulation::android_model {
llvm::Expected<std::optional<uint64_t>>
Bionic::kernelService(linux_model::ServiceKind Kind,
                      const ProcessServiceEvent &Event) {
  return Kernel.handle(Kind, Event, Threads ? Threads->kernel() : nullptr);
}

BionicResult Bionic::kernelCall(std::optional<linux_model::ServiceKind> Kind,
                                const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const bool IsSyscall = !Kind;
  // API 28 Bionic rejects flags before calling the three-argument syscall.
  if (Kind == linux_model::ServiceKind::FaccessAt && uint32_t(A[3])) {
    if (auto E = setErrno(linux_model::InvalidArgument))
      return std::move(E);
    return value(UINT64_MAX);
  }
  ProcessServiceEvent Event{Call.PC, IsSyscall ? A[0] : 0, {}, std::nullopt};
  Event.ReturnAddress = Call.ReturnAddress;
  // AArch64 Bionic syscall(number, ...) shifts x1..x6 into the six
  // kernel argument registers. The shared Linux table owns the number.
  std::copy_n(A.begin() + (IsSyscall ? 1 : 0), Event.Arguments.size(),
              Event.Arguments.begin());
  // Raw Linux service semantics are shared. Bionic alone owns errno/-1.
  auto *Thread = Threads ? Threads->kernel() : nullptr;
  auto Returned =
      Kind ? kernelService(*Kind, Event) : Kernel.handle(Event, Thread);
  if (!Returned)
    return Returned.takeError();
  return kernelResult(*Returned);
}

BionicResult
Bionic::kernelResult(std::optional<uint64_t> Returned,
                     const std::optional<SleepContinuation> &Sleep) {
  if (!Returned && Threads && Threads->waitKernel(Sleep))
    return std::optional<BionicValue>(GuestThreadWait{});
  if (!Returned)
    return std::optional<BionicValue>();
  if (*Returned >= uint64_t(0) - 4095) {
    if (auto E = setErrno(uint64_t(0) - *Returned))
      return std::move(E);
    Returned = UINT64_MAX;
  }
  if (Sleep)
    return sleepResult(*Returned, *Sleep);
  return value(*Returned);
}

BionicResult Bionic::checkedRead(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (A[2] > linux_model::MaxSignedIOSize)
    return failure(diagnostic::FortifiedReadCount);
  if (A[2] > A[3])
    return failure(diagnostic::FortifiedReadBounds);
  // The supplied object bound is checked before descriptor lookup or user
  // access. The ordinary read owner retains cursors, waits and libc errno.
  return kernelCall(linux_model::ServiceKind::Read, Call);
}

BionicResult Bionic::usleep(const NativeCallEvent &Call) {
  // Bionic's useconds_t is unsigned 32-bit, including values >= one second.
  const uint64_t Nanoseconds = uint64_t(uint32_t(Call.Arguments[0])) * 1000;
  const LinuxTimespec Duration{int64_t(Nanoseconds / 1000000000),
                               int64_t(Nanoseconds % 1000000000)};
  return kernelResult(linux_model::sleepRelative(
      CPU, Layout, Duration, Kernel.clock(),
      Threads ? Threads->kernel() : nullptr, Result));
}

BionicResult Bionic::sleep(const NativeCallEvent &Call) {
  const uint32_t Seconds = uint32_t(Call.Arguments[0]);
  // API 28 attempts only INT_MAX seconds and returns the unattempted excess,
  // including when the inner nanosleep succeeds or is interrupted.
  const auto Attempted = std::min<uint32_t>(Seconds, INT32_MAX);
  SleepContinuation Continuation{Seconds, Seconds - Attempted,
                                 std::make_shared<LinuxTimespec>()};
  auto Returned = linux_model::sleepRelative(
      CPU, Layout, {Attempted, 0}, Kernel.clock(),
      Threads ? Threads->kernel() : nullptr, Result, Continuation.Remaining);
  return kernelResult(Returned, Continuation);
}

BionicResult Bionic::sleepResult(uint64_t Returned,
                                 const SleepContinuation &Continuation) {
  // nanosleep returns int. A mutable signal context can supply arbitrary
  // upper bits, or a non-kernel-error value whose low word is still -1.
  if (uint32_t(Returned) != UINT32_MAX)
    return value(Continuation.Excess);
  auto Error = errnoValue();
  if (!Error)
    return Error.takeError();
  if (*Error != linux_model::InterruptedCall)
    return value(Continuation.Seconds);
  const auto &Left = *Continuation.Remaining;
  return value(Continuation.Excess + Left.Seconds + (Left.Nanoseconds != 0));
}

BionicResult Bionic::getcwd(const NativeCallEvent &Call) {
  const uint64_t Original = Call.Arguments[0], Requested = Call.Arguments[1];
  if (Original && !Requested) {
    if (auto E = setErrno(linux_model::InvalidArgument))
      return std::move(E);
    return value(0);
  }
  uint64_t Buffer = Original;
  const uint64_t Size = !Original && !Requested ? PageSize : Requested;
  if (!Buffer) {
    auto Allocated = allocate(Size, Options.Android->MallocFillByte);
    if (!Allocated)
      return Allocated.takeError();
    Buffer = *Allocated;
    if (!Buffer)
      return value(0);
  }
  ProcessServiceEvent Event{Call.PC, 0, {Buffer, Size, 0, 0, 0, 0}, {}};
  Event.ReturnAddress = Call.ReturnAddress;
  auto Returned = kernelService(linux_model::ServiceKind::Getcwd, Event);
  if (!Returned)
    return Returned.takeError();
  if (!*Returned)
    return std::optional<BionicValue>();
  if (**Returned >= uint64_t(0) - 4095) {
    if (!Original)
      if (auto E = release(Buffer))
        return std::move(E);
    if (auto E = setErrno(uint64_t(0) - **Returned))
      return std::move(E);
    return value(0);
  }
  if (!Original && !Requested) {
    // API 28 allocates a page for the kernel, then duplicates the exact string
    // before releasing it. Keep both allocation lifetimes and failure paths.
    std::vector<uint8_t> Bytes(**Returned);
    if (auto E = CPU.read(Buffer, Bytes))
      return std::move(E);
    auto Exact = allocate(Bytes.size(), Options.Android->MallocFillByte);
    if (!Exact)
      return Exact.takeError();
    if (*Exact)
      if (auto E = CPU.write(*Exact, Bytes))
        return std::move(E);
    if (auto E = release(Buffer))
      return std::move(E);
    Buffer = *Exact;
  }
  return value(Buffer);
}

BionicResult Bionic::socketTransfer(const NativeCallEvent &Call) {
  auto Adapted = Call;
  Adapted.Arguments[4] = Adapted.Arguments[5] = 0;
  return kernelCall(Call.Name == symbol::Send
                        ? linux_model::ServiceKind::SendTo
                        : linux_model::ServiceKind::ReceiveFrom,
                    Adapted);
}

BionicResult Bionic::syscall(const NativeCallEvent &Call) {
  return kernelCall(std::nullopt, Call);
}

BionicResult Bionic::pollCall(const NativeCallEvent &Call) {
  using linux_model::ServiceKind;
  auto Adapted = Call;
  if (Call.Name == symbol::EpollPwait || Call.Name == symbol::EpollPwait64) {
    Adapted.Arguments[5] = linux_model::SignalMaskSize;
    return kernelCall(ServiceKind::EpollPwait, Adapted);
  }
  ProcessServiceEvent Event{Call.PC, 0, {}, std::nullopt};
  std::copy_n(Call.Arguments.begin(), Event.Arguments.size(),
              Event.Arguments.begin());
  Event.ReturnAddress = Call.ReturnAddress;
  Event.Arguments[4] = linux_model::SignalMaskSize;
  auto R = Kernel.poll(Call.Name == symbol::Poll ? ServiceKind::Poll
                                                 : ServiceKind::PPoll,
                       Event, Threads ? Threads->kernel() : nullptr, true);
  if (!R)
    return R.takeError();
  return kernelResult(*R);
}

} // namespace neverd::emulation::android_model
