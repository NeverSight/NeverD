//===- LinuxIoctlTrace.cpp - Bounded ioctl memory observations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observes explicit guest RAM ranges without supplying device semantics.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::tracedIoctl(const ProcessServiceEvent &Event,
                        ProcessResult &Result) {
  const uint32_t FD = Event.Arguments[0], Request = Event.Arguments[1];
  const uint64_t Argument = Event.Arguments[2];
  if (!Options->IoctlTrace)
    return deviceIoctl(FD, Request, Argument, Result);
  const auto &Config = *Options->IoctlTrace;
  auto Ranges = Config.Requests.find(Request);
  if (!Config.IncludeUnlisted && Ranges == Config.Requests.end())
    return deviceIoctl(FD, Request, Argument, Result);
  if (Result.IoctlTrace.size() == Config.EventLimit) {
    Result.IoctlTraceTruncated = true;
    return deviceIoctl(FD, Request, Argument, Result);
  }
  ProcessIoctlEvent Trace;
  Trace.PC = Event.PC;
  Trace.FD = FD;
  Trace.Request = Request;
  Trace.Argument = Argument;
  Trace.ThreadID = CurrentTask;
  Trace.ReturnAddress = Event.ReturnAddress;
  // Charge attempted reads as well as successful reads, including pointer
  // cells. Neither unreadability nor evidence exhaustion changes the guest.
  auto Capture = [&](uint64_t Address, uint64_t Size,
                     std::vector<uint8_t> &Bytes) -> std::string {
    if (Size > Config.ByteLimit - IoctlTraceBytes) {
      Result.IoctlTraceTruncated = true;
      return "byte_limit";
    }
    IoctlTraceBytes += Size;
    if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
      return "unreadable";
    auto Access = CPU.canAccess(Address, Size, Read | UserAccessible);
    if (!Access) {
      llvm::consumeError(Access.takeError());
      return "backend_error";
    }
    if (!*Access)
      return "unreadable";
    Bytes.resize(Size);
    if (auto E = CPU.read(Address, Bytes)) {
      llvm::consumeError(std::move(E));
      Bytes.clear();
      return "backend_error";
    }
    return "complete";
  };
  if (Ranges != Config.Requests.end()) {
    for (const auto &Range : Ranges->second) {
      auto &M = Trace.Memory.emplace_back();
      M.Size = Range.Size;
      uint64_t Base = Argument;
      if (Range.PointerOffset) {
        if (*Range.PointerOffset > UINT64_MAX - Base) {
          M.BeforeStatus = "address_overflow";
          continue;
        }
        std::vector<uint8_t> Pointer;
        M.BeforeStatus = Capture(Base + *Range.PointerOffset, 8, Pointer);
        if (M.BeforeStatus != "complete")
          continue;
        Base = 0;
        for (unsigned N = 0; N != 8; ++N)
          Base |= uint64_t(Pointer[N]) << (N * 8);
      }
      if (Range.Offset > UINT64_MAX - Base) {
        M.BeforeStatus = "address_overflow";
        continue;
      }
      M.Address = Base + Range.Offset;
      M.BeforeStatus = Capture(*M.Address, M.Size, M.Before);
    }
  }
  auto Returned = deviceIoctl(FD, Request, Argument, Result);
  if (Returned)
    Trace.Result = *Returned;
  for (auto &M : Trace.Memory)
    M.AfterStatus = M.Address ? Capture(*M.Address, M.Size, M.After)
                              : "unresolved_entry_address";
  Result.IoctlTrace.push_back(std::move(Trace));
  return Returned;
}
} // namespace neverd::emulation::linux_model
