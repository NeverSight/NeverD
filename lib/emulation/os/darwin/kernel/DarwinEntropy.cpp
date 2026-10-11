//===- DarwinEntropy.cpp - Exact per-call supplied bytes and copyout ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinEntropy.h"

#include "DarwinUserMemory.h"

namespace neverd::emulation::darwin_model {
using namespace value;
llvm::Expected<std::optional<ServiceResult>>
DarwinEntropy::handle(GuestMemory &Memory, const ProcessServiceEvent &Event,
                      ProcessResult &Result) {
  const auto Address = Event.Arguments[0], Count = Event.Arguments[1];
  // The native size is full-width. A zero copy needs no valid pointer.
  if (Count > EntropyReadLimit)
    return std::optional<ServiceResult>({InvalidArgument, true});
  if (!Count)
    return std::optional<ServiceResult>({0, false});
  auto unsupported = [&](const char *Reason) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Reason;
    return std::optional<ServiceResult>();
  };
  // Replay admission precedes copyout, including a wholly invalid address.
  if (!Options || !Options->EntropyReads)
    return unsupported(diagnostic::EntropyObservation);
  const auto &Reads = *Options->EntropyReads;
  if (Next == Reads.size())
    return unsupported(diagnostic::EntropyExhausted);
  if (Reads[Next].size() != Count)
    return unsupported(diagnostic::EntropyLength);
  auto Copied = copyUserMemory(Memory, Address, Reads[Next],
                               diagnostic::EntropyPartialOutput, Result);
  // Native generation precedes copyout: whole EFAULT consumes the admitted
  // record. Partial refusal and transport errors publish no replay advance.
  if (Copied && *Copied)
    ++Next;
  return Copied;
}
} // namespace neverd::emulation::darwin_model
