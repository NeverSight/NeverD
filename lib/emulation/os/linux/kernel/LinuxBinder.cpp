//===- LinuxBinder.cpp - Bounded LP64 Binder control and looper protocol --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Workload-owned protocol state. No application service or transaction reply
/// is supplied implicitly; unmodeled commands stop before consuming their
/// bytes.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

#include "neverd/emulation/Limits.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::binderIoctl(LinuxDeviceState &Device, uint32_t Request,
                        uint64_t Argument, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  constexpr uint32_t WriteRead = 0xc0306201, SetMaxThreads = 0x40046205;
  constexpr uint32_t ThreadExit = 0x40046208, Version = 0xc0046209;
  constexpr uint32_t Register = 0x630b, Enter = 0x630c, Exit = 0x630d;
  constexpr uint32_t Registered = 1, Entered = 2, Exited = 4, Invalid = 8;
  constexpr uint32_t Noop = 0x720c, Spawn = 0x720d;
  auto Accessible = [&](uint64_t Address, uint64_t Size,
                        unsigned Permission) -> llvm::Expected<bool> {
    if (!Size)
      return true;
    if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
      return false;
    return CPU.canAccess(Address, Size, Permission | UserAccessible);
  };
  if (!Device.BinderLoopers.contains(CurrentTask) &&
      Device.BinderLoopers.size() ==
          ::neverd::limits::emulation::
              kLinuxBinderLinuxFilesBinderIoctlDeviceBinderLoopersSizeLimit)
    return unsupported(Result, "Binder thread identity budget exhausted");
  // A newly allocated kernel binder_thread needs one return to userspace. Every
  // ioctl clears that flag, even a version query or an ordinary error return.
  auto [Thread, Fresh] = Device.BinderLoopers.emplace(CurrentTask, 0);
  if (Request == 0x40046207) { // BINDER_SET_CONTEXT_MGR ignores argument.
    for (const auto &Weak : DeviceDescriptions)
      if (auto Open = Weak.lock())
        if (Open->Options == Device.Options && Open->BinderManager)
          return Reply(uint64_t(0) - 16); // EBUSY
    if (Device.BinderNodes.size() ==
        ::neverd::limits::emulation::kMaxBinderNodesPerOpen)
      return unsupported(Result, "Binder node budget exhausted");
    if (Device.BinderNodes.contains(0))
      return unsupported(Result, "Binder context manager node already exists");
    auto Node = std::make_shared<BinderNode>();
    Node->Owner = binderOwner(Device);
    Node->LocalStrong = Node->LocalWeak = 1;
    Node->HasStrong = Node->HasWeak = true;
    Device.BinderNodes.emplace(0, std::move(Node));
    Device.BinderManager = true;
    return Reply(0);
  }
  if (Request == ThreadExit) {
    if (Device.BinderIncoming.contains(CurrentTask) ||
        Device.BinderOutgoing.contains(CurrentTask) ||
        !Device.BinderThreadWork[CurrentTask].empty())
      return unsupported(
          Result, "Binder thread exit with pending transactions is unmodeled");
    Device.BinderThreadWork.erase(CurrentTask);
    Device.BinderLoopers.erase(Thread);
    return Reply(0); // This request does not dereference its nominal argument.
  }
  if (Request == Version || Request == SetMaxThreads) {
    auto Allowed = Accessible(Argument, 4, Request == Version ? Write : Read);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return Reply(uint64_t(0) - InvalidArgument);
    uint8_t Bytes[4];
    if (Request == Version) {
      llvm::support::endian::write32le(Bytes, 8);
      if (auto E = CPU.write(Argument, Bytes))
        return std::move(E);
    } else {
      if (auto E = CPU.read(Argument, Bytes))
        return std::move(E);
      Device.BinderMaxThreads = llvm::support::endian::read32le(Bytes);
    }
    return Reply(0);
  }
  if (Request != WriteRead)
    return unsupported(Result, "Binder ioctl has no modeled control semantics");
  auto Readable = Accessible(Argument, 48, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return Reply(uint64_t(0) - BadAddress);
  uint8_t Record[48];
  if (auto E = CPU.read(Argument, Record))
    return std::move(E);
  auto Get = [&](unsigned At) {
    return llvm::support::endian::read64le(Record + At);
  };
  const uint64_t WriteSize = Get(0), WriteConsumed = Get(8),
                 WriteBuffer = Get(16);
  const uint64_t ReadSize = Get(24), ReadConsumed = Get(32),
                 ReadBuffer = Get(40);
  // Partial copy faults, malformed/resumed read buffers and unbounded command
  // streams require a broader kernel-copy model. Refuse them before effects.
  if (WriteSize > ::neverd::limits::emulation::
                      kLinuxBinderLinuxFilesBinderIoctlWriteSizeLimit ||
      WriteConsumed > WriteSize || (WriteSize - WriteConsumed) % 4 ||
      ReadConsumed || (ReadSize && ReadSize < 4))
    return unsupported(Result,
                       "Binder buffer sizes are outside the control subset");
  auto Writable = Accessible(Argument, sizeof(Record), Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return unsupported(Result, "Binder partial record copy-out is unmodeled");
  if (WriteConsumed > UINT64_MAX - WriteBuffer)
    return unsupported(Result, "Binder write buffer wraps");
  auto HasReturnError = [&] {
    const auto &Queue = Device.BinderThreadWork[CurrentTask];
    return std::any_of(Queue.begin(), Queue.end(), [](const BinderWork &Work) {
      return Work.Command == 0x7205 || Work.Command == 0x7211;
    });
  };
  // binder_thread_write stops at its pending return_error before touching any
  // further command bytes. Reading that error permits a later write to resume.
  const uint64_t CommandBytes =
      HasReturnError() ? 0 : WriteSize - WriteConsumed;
  Readable = Accessible(WriteBuffer + WriteConsumed, CommandBytes, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return unsupported(Result, "Binder partial command copy-in is unmodeled");
  std::vector<uint8_t> Commands(CommandBytes);
  if (!Commands.empty())
    if (auto E = CPU.read(WriteBuffer + WriteConsumed, Commands))
      return std::move(E);
  struct Decoded {
    uint32_t Command;
    size_t Offset, Size;
  };
  std::vector<Decoded> DecodedCommands;
  unsigned Transactions = 0;
  for (size_t I = 0; I < Commands.size();) {
    uint32_t Command = llvm::support::endian::read32le(Commands.data() + I);
    const size_t Size = Command == 0x40406300 || Command == 0x40406301   ? 64
                        : Command == 0x40086303                          ? 8
                        : Command >= 0x40046304 && Command <= 0x40046307 ? 4
                        : Command == 0x40106308 || Command == 0x40106309 ? 16
                                                                         : 0;
    if (!Size && Command != Register && Command != Enter && Command != Exit)
      return unsupported(
          Result, "Binder command requires an unmodeled object or protocol");
    if (Size > Commands.size() - I - 4)
      return unsupported(Result, "Binder transaction record is incomplete");
    Transactions += Size == 64;
    if (Transactions > 1)
      return unsupported(Result,
                         "Binder batch requires nested transaction semantics");
    DecodedCommands.push_back({Command, I + 4, Size});
    I += 4 + Size;
  }
  uint64_t Consumed = WriteConsumed;
  for (const auto &[Command, Offset, Size] : DecodedCommands) {
    Consumed = WriteConsumed + Offset + Size;
    if (Size) {
      auto Outcome =
          binderCommand(Device, Command,
                        llvm::ArrayRef(Commands).slice(Offset, Size), Result);
      if (!Outcome || !*Outcome)
        return Outcome;
      if (HasReturnError())
        break;
      continue;
    }
    auto &Flags = Thread->second;
    if (Command == Register) {
      if ((Flags & Entered) || !Device.BinderRequestedThreads)
        Flags |= Invalid;
      else {
        --Device.BinderRequestedThreads;
        ++Device.BinderStartedThreads;
      }
      Flags |= Registered;
    } else if (Command == Enter) {
      if (Flags & Registered)
        Flags |= Invalid;
      Flags |= Entered;
    } else {
      Flags |= Exited;
    }
  }
  llvm::support::endian::write64le(Record + 8, Consumed);
  uint64_t Status = 0;
  if (ReadSize) {
    Writable = Accessible(ReadBuffer, 4, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable) {
      Status = uint64_t(0) - BadAddress;
    } else {
      uint8_t Response[4];
      llvm::support::endian::write32le(Response, Noop);
      if (auto E = CPU.write(ReadBuffer, Response))
        return std::move(E);
      auto Work = binderReadWork(Device, ReadBuffer, ReadSize, Result);
      if (!Work || !*Work)
        return Work;
      if (**Work) {
        llvm::support::endian::write64le(Record + 32, **Work);
      } else if (!Fresh) {
        if (!(Device.OpenFlags & OpenNonBlock))
          return unsupported(
              Result, "Binder blocking read requires a peer or pending work");
        Status =
            uint64_t(0) - 11; // EAGAIN; the BR_NOOP write is still observable.
      } else {
        llvm::support::endian::write64le(Record + 32, 4);
      }
      if (!Status && !Device.BinderRequestedThreads &&
          Device.BinderMaxThreads > 0 &&
          Device.BinderStartedThreads < uint32_t(Device.BinderMaxThreads) &&
          (Thread->second & (Registered | Entered))) {
        ++Device.BinderRequestedThreads;
        llvm::support::endian::write32le(Response, Spawn);
        if (auto E = CPU.write(ReadBuffer, Response))
          return std::move(E);
      }
    }
  }
  if (auto E = CPU.write(Argument, Record))
    return std::move(E);
  return Reply(Status);
}
} // namespace neverd::emulation::linux_model
