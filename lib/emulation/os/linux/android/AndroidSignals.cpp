//===- AndroidSignals.cpp - Bionic LP64 signal action translation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../kernel/LinuxUserMemory.h"
#include "AndroidInternal.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
namespace {
// API 28 Bionic reserves real-time signals 32 through 35 for its runtime.
constexpr uint64_t ReservedSignals = uint64_t(0xf) << 31;
// The four-byte flags field precedes four bytes of padding in the LP64 ABI.
constexpr unsigned ActionSize = 32;
} // namespace

bool Bionic::abortMask() {
  // API 28 blocks every blockable signal except SIGABRT, rather than just
  // unblocking that signal. Reuse the kernel's mask and thread-pending state.
  const uint64_t Mask =
      ~(uint64_t(1) << (linux_model::SignalAbort - 1)) & ~ReservedSignals;
  auto Applied = Kernel.signals().mask(threadID(), 2, Mask, false, Result);
  if (!Applied)
    return false;
  assert(!Applied->Status);
  return true;
}

BionicResult Bionic::abortCall(const NativeCallEvent &) {
  if (!abortMask())
    return std::optional<BionicValue>();
  // A previously blocked SIGABRT can run as sigprocmask returns, before the
  // first tgkill. Retain that boundary without claiming an import return.
  return std::optional<BionicValue>(GuestAbort{});
}

llvm::Expected<bool> Bionic::continueAbort(bool ResetHandler) {
  if (ResetHandler) {
    const LinuxSignalAction Default{0, 0x10000000, 0, 0}; // SA_RESTART
    auto Applied =
        Kernel.signalAction(linux_model::SignalAbort, &Default, false);
    if (!Applied)
      return false;
    assert(!Applied->Status);
    if (!abortMask())
      return false;
  }
  auto Sent =
      Kernel.signals().sendThread(linux_model::SignalAbort, threadID(), Result);
  if (!Sent)
    return false;
  assert(!*Sent);
  // The first send may enter a guest handler. After a normal handler return,
  // the second has a known default and terminates the whole guest process.
  return Kernel.deliverSignals(threadID());
}

BionicResult Bionic::signalAction(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  LinuxSignalAction NewAction;
  if (A[1]) {
    if (auto E = access(A[1], ActionSize, Read))
      return std::move(E);
    uint8_t Bytes[ActionSize];
    if (auto E = CPU.read(A[1], Bytes))
      return std::move(E);
    NewAction.Flags =
        uint64_t(int64_t(int32_t(llvm::support::endian::read32le(Bytes))));
    NewAction.Handler = llvm::support::endian::read64le(Bytes + 8);
    NewAction.Mask =
        llvm::support::endian::read64le(Bytes + 16) & ~ReservedSignals;
    NewAction.Restorer = llvm::support::endian::read64le(Bytes + 24);
  }
  auto Applied = Kernel.signalAction(int32_t(uint32_t(A[0])),
                                     A[1] ? &NewAction : nullptr, A[2] != 0);
  if (!Applied)
    return std::optional<BionicValue>();
  if (Applied->Status) {
    if (auto E = setErrno(uint64_t(0) - Applied->Status))
      return std::move(E);
    // API 28's LP64 wrapper copies an uninitialized kernel temporary after
    // failure. Its bytes cannot be replaced with zeros or the previous action.
    if (A[2]) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::SignalIndeterminateOutput;
      return std::optional<BionicValue>();
    }
    return value(UINT64_MAX);
  }
  if (A[2]) {
    const auto &Old = Applied->Previous;
    struct Field {
      unsigned Offset, Size;
      uint64_t Value;
    };
    const Field Fields[] = {{0, 4, Old.Flags},
                            {8, 8, Old.Handler},
                            {16, 8, Old.Mask},
                            {24, 8, Old.Restorer}};
    for (const auto &F : Fields) {
      if (A[2] > UINT64_MAX - F.Offset)
        return failure(diagnostic::GuestPointer);
      const uint64_t Address = A[2] + F.Offset;
      if (auto E = access(Address, F.Size, Write))
        return std::move(E);
      uint8_t Bytes[8];
      llvm::support::endian::write64le(Bytes, F.Value);
      if (auto E = CPU.write(Address, llvm::ArrayRef(Bytes).take_front(F.Size)))
        return std::move(E);
    }
  }
  return value(0);
}

BionicResult Bionic::signalMask(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  std::optional<uint64_t> Set;
  if (A[1]) {
    if (auto E = access(A[1], 8, Read))
      return std::move(E);
    uint8_t Bytes[8];
    if (auto E = CPU.read(A[1], Bytes))
      return std::move(E);
    Set = llvm::support::endian::read64le(Bytes) & ~ReservedSignals;
  }
  const bool Classic = Call.Name == symbol::Sigprocmask;
  // The classic LP64 wrapper asks the kernel for a local old-set temporary
  // even when its caller omits the output. The 64 variant forwards null.
  auto Applied = Kernel.signals().mask(threadID(), int32_t(uint32_t(A[0])), Set,
                                       Classic || A[2], Result);
  if (!Applied)
    return std::optional<BionicValue>();
  if (Applied->Status)
    return kernelResult(Applied->Status);
  if (!A[2])
    return value(0);
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, Applied->Previous);
  if (Classic) {
    if (auto E = access(A[2], 8, Write))
      return std::move(E);
    if (auto E = CPU.write(A[2], Bytes))
      return std::move(E);
    return value(0);
  }
  auto Stored = linux_model::writeUserMemory(CPU, Layout, A[2], Bytes);
  if (!Stored)
    return Stored.takeError();
  if (*Stored == linux_model::UserWriteResult::Stored)
    return value(0);
  if (*Stored == linux_model::UserWriteResult::BadAddress)
    return kernelResult(uint64_t(0) - linux_model::BadAddress);
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = linux_model::SignalPartialOutput;
  return std::optional<BionicValue>();
}

BionicResult Bionic::signalPending(const NativeCallEvent &Call) {
  if (Call.Name == symbol::Sigpending64) {
    auto Adapted = Call;
    Adapted.Arguments[1] = linux_model::SignalMaskSize;
    return kernelCall(linux_model::ServiceKind::SignalPending, Adapted);
  }
  if (auto E = access(Call.Arguments[0], 8, Read))
    return std::move(E);
  uint8_t Bytes[8];
  if (auto E = CPU.read(Call.Arguments[0], Bytes))
    return std::move(E);
  auto Mask = Kernel.signals().pending(threadID(), Result);
  if (!Mask)
    return std::optional<BionicValue>();
  if (auto E = access(Call.Arguments[0], 8, Write))
    return std::move(E);
  llvm::support::endian::write64le(Bytes, *Mask);
  if (auto E = CPU.write(Call.Arguments[0], Bytes))
    return std::move(E);
  return value(0);
}

BionicResult Bionic::signalHandler(const NativeCallEvent &Call) {
  LinuxSignalAction Action{Call.Arguments[1], 0x10000000, 0, 0};
  auto Applied =
      Kernel.signalAction(int32_t(uint32_t(Call.Arguments[0])), &Action, true);
  if (!Applied)
    return std::optional<BionicValue>();
  if (Applied->Status)
    return kernelResult(Applied->Status);
  return value(Applied->Previous.Handler);
}

BionicResult Bionic::signalSet(const NativeCallEvent &Call) {
  const auto Name = llvm::StringRef(Call.Name);
  const bool Empty = Name.starts_with("sigemptyset");
  const bool Fill = Name.starts_with("sigfillset");
  const bool Member = Name.starts_with("sigismember");
  const int32_t Signal = int32_t(uint32_t(Call.Arguments[1]));
  if (!Call.Arguments[0] || (!Empty && !Fill && (Signal <= 0 || Signal > 64))) {
    if (auto E = setErrno(linux_model::InvalidArgument))
      return std::move(E);
    return value(UINT64_MAX);
  }
  uint64_t Mask = Fill ? UINT64_MAX : 0;
  uint8_t Bytes[8];
  if (!Empty && !Fill) {
    if (auto E = access(Call.Arguments[0], 8, Read))
      return std::move(E);
    if (auto E = CPU.read(Call.Arguments[0], Bytes))
      return std::move(E);
    Mask = llvm::support::endian::read64le(Bytes);
    const uint64_t Bit = uint64_t(1) << (Signal - 1);
    if (Member)
      return value(bool(Mask & Bit));
    Mask = Name.starts_with("sigaddset") ? Mask | Bit : Mask & ~Bit;
  }
  if (auto E = access(Call.Arguments[0], 8, Write))
    return std::move(E);
  llvm::support::endian::write64le(Bytes, Mask);
  if (auto E = CPU.write(Call.Arguments[0], Bytes))
    return std::move(E);
  return value(0);
}
} // namespace neverd::emulation::android_model
