//===- DarwinPoll.cpp - Immediate Darwin descriptor readiness ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Independently authored bounded projection of the ABI documented in
// docs/darwin-emulation.md. Guest descriptors never become host descriptors.
#include "DarwinFiles.h"
#include "DarwinUserMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
} // namespace

llvm::Expected<std::optional<ServiceResult>>
DarwinFiles::poll(const ProcessServiceEvent &Event,
                  const std::optional<DarwinSystemOptions> &System,
                  ProcessResult &Result) {
  const auto &A = Event.Arguments;
  const uint32_t Count = uint32_t(A[1]);
  if (Count > PollOpenMax)
    return returned(InvalidArgument, true);
  if (Count) {
    if (!System)
      return unsupported(Result, diagnostic::PollNoFile);
    const auto Limit = System->ResourceLimits.find(ResourceLimitNoFile);
    if (Limit == System->ResourceLimits.end())
      return unsupported(Result, diagnostic::PollNoFile);
    if (Count > Limit->second.Current) {
      if (Count > PollFDSetSize)
        return returned(InvalidArgument, true);
      if (!System->Credentials)
        return unsupported(Result, diagnostic::PollCredentials);
      if (System->Credentials->EffectiveUID)
        return returned(InvalidArgument, true);
    }
  }
  if (int32_t(A[2]))
    return unsupported(Result, diagnostic::PollWait);
  // The native empty array needs no copyin/copyout address access.
  if (!Count)
    return returned(0);

  const uint64_t Size = uint64_t(Count) * 8;
  auto Prefix = userMemoryPrefix(Memory, A[0], Size, Read);
  if (!Prefix)
    return Prefix.takeError();
  if (*Prefix != Size)
    return returned(BadAddress, true);
  std::vector<uint8_t> Bytes(Size);
  if (auto E = Memory.read(A[0], Bytes))
    return std::move(E);
  const auto Events = [&](uint32_t Index) {
    return llvm::support::endian::read16le(Bytes.data() + Index * 8 + 4);
  };
  const auto SetReady = [&](uint32_t Index, uint16_t Bits) {
    auto *Out = Bytes.data() + Index * 8 + 6;
    llvm::support::endian::write16le(Out, llvm::support::endian::read16le(Out) |
                                              Bits);
  };
  // Each fresh native poll registers a numeric-FD/filter key. Later rows
  // replace only that filter's udata; dup aliases have distinct numeric keys.
  struct Filters {
    std::optional<uint32_t> Read, Write;
  };
  std::map<uint32_t, Filters> Registered;
  for (uint32_t I = 0; I != Count; ++I) {
    const auto FD = llvm::support::endian::read32le(Bytes.data() + I * 8);
    const uint16_t Requested = Events(I);
    llvm::support::endian::write16le(Bytes.data() + I * 8 + 6, 0);
    if (int32_t(FD) < 0 ||
        !(Requested & (PollReadEvents | PollWriteEvents | PollVNodeEvents)))
      continue;
    auto Found = Descriptors.find(FD);
    if (Found == Descriptors.end()) {
      SetReady(I, PollInvalid);
      continue;
    }
    // Model-constructed regular descriptions explicitly have successful
    // ordinary filter attachment, no revocation and no MAC/provider refusal.
    // Kind alone is not proof of these facts for an arbitrary native vnode.
    if (Found->second.Open->Type != Kind::File)
      return unsupported(Result, diagnostic::PollObject);
    if (Requested & (PollOutOfBandEvents | PollVNodeEvents))
      return unsupported(Result, diagnostic::PollEvents);
    auto &Entry = Registered[FD];
    if (Requested & PollReadEvents)
      Entry.Read = I;
    if (Requested & PollWriteEvents)
      Entry.Write = I;
  }
  for (const auto &[FD, Entry] : Registered) {
    if (Entry.Read)
      SetReady(*Entry.Read, Events(*Entry.Read) & PollOrdinaryReadEvents);
    if (Entry.Write)
      SetReady(*Entry.Write, Events(*Entry.Write) & PollWriteEvents);
  }
  uint32_t Ready = 0;
  for (uint32_t I = 0; I != Count; ++I)
    Ready += llvm::support::endian::read16le(Bytes.data() + I * 8 + 6) != 0;
  // Darwin copies the complete input snapshot back, including fd/events.
  // The shared owner refuses an individually partial writable destination.
  auto Copied = copyUserMemory(Memory, A[0], Bytes,
                               diagnostic::PollPartialOutput, Result);
  if (!Copied || !*Copied || (**Copied).Error)
    return Copied;
  return returned(Ready);
}
} // namespace neverd::emulation::darwin_model
