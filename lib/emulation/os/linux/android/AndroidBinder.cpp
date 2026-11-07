//===- AndroidBinder.cpp - Native BinderProxy IPC bridge -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// API 28 libbinder transport through the shared Linux ioctl and scheduler.
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace neverd::emulation::android_model {
namespace {
std::nullopt_t unsupported(ProcessResult &Result, llvm::StringRef Message) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Message.str();
  return std::nullopt;
}
} // namespace
mobile::dalvik::ExecutionResult
Bionic::binderIdentity(mobile::dalvik::BinderIdentityOperation Operation,
                       uint64_t Address) {
  using mobile::dalvik::BinderIdentityOperation;
  using mobile::dalvik::ExecutionStop;
  if (Operation == BinderIdentityOperation::Allocate) {
    auto Storage = allocate(2 * sizeof(uint64_t));
    if (!Storage)
      return {
          ExecutionStop::Invalid, 0, {}, llvm::toString(Storage.takeError())};
    if (!*Storage)
      return {ExecutionStop::HeapLimit,
              0,
              {},
              "Android Binder opaque identity storage budget exhausted"};
    auto Protected = kernelService(linux_model::ServiceKind::Mprotect,
                                   {0, 0, {*Storage, PageSize, 0}, {}});
    if (!Protected)
      return {
          ExecutionStop::Invalid, 0, {}, llvm::toString(Protected.takeError())};
    if (!*Protected || **Protected)
      return {ExecutionStop::Unsupported,
              0,
              {},
              "Android Binder identity storage could not be made opaque"};
    auto Backing = CPU.pinBacking(*Storage, 2 * sizeof(uint64_t));
    if (!Backing)
      return {
          ExecutionStop::Invalid, 0, {}, llvm::toString(Backing.takeError())};
    BinderIdentities.try_emplace(*Storage, std::move(*Backing));
    return {ExecutionStop::Returned, *Storage};
  }
  if (auto E = opaqueHandle(Address, "Binder"))
    return {ExecutionStop::Unsupported, 0, {}, llvm::toString(std::move(E))};
  auto Current = CPU.pinBacking(Address, 2 * sizeof(uint64_t));
  if (!Current)
    return {ExecutionStop::Invalid, 0, {}, llvm::toString(Current.takeError())};
  if (!BinderIdentities.find(Address)->second.describesSameBytes(*Current))
    return {ExecutionStop::Unsupported,
            0,
            {},
            "Android Binder identity allocation was replaced"};
  if (Operation == BinderIdentityOperation::Release) {
    if (auto E = release(Address))
      return {ExecutionStop::Invalid, 0, {}, llvm::toString(std::move(E))};
    BinderIdentities.erase(Address);
  }
  return {};
}

class Bionic::BinderWait final
    : public NativeWait,
      public std::enable_shared_from_this<BinderWait> {
public:
  std::shared_ptr<linux_model::LinuxDeviceState> Device;
  std::shared_ptr<DexContinuation> Continuation;
  uint32_t FD;
  uint64_t Scratch;
  bool Join = false, Dispatching = false, WaitInstalled = false;
  std::weak_ptr<DexContinuation> ReplyContinuation;
  uint64_t ReplyStorage = 0;
  std::optional<std::pair<uint32_t, uint32_t>> SavedIdentity;
  uint32_t SavedPolicy = 0, SavedFlags = 0;

  // The guest callback owns the same operation as its scheduler wait. Each
  // suspension installs at most one wrapper, including immediate Java bodies
  // that reach sendReply and return to the pool in the same host call.
  class SharedWait final : public NativeWait {
    std::shared_ptr<BinderWait> Operation;
    std::shared_ptr<DexContinuation> Reply;

  public:
    explicit SharedWait(std::shared_ptr<BinderWait> Operation)
        : Operation(std::move(Operation)),
          Reply(this->Operation->ReplyContinuation.lock()) {}
    bool ready() const override { return Operation->ready(); }
    std::optional<uint64_t> nextWake() const override { return std::nullopt; }
    BionicResult resume(Bionic &LibC) override {
      Operation->WaitInstalled = false;
      return Operation->drive(LibC);
    }
  };
  BionicResult drive(Bionic &LibC) {
    auto Executed = run(LibC);
    if (Executed && *Executed &&
        std::holds_alternative<GuestThreadWait>(**Executed) && !WaitInstalled &&
        (!Dispatching || !ReplyContinuation.expired())) {
      LibC.Threads->waitNative(
          std::make_unique<SharedWait>(shared_from_this()));
      WaitInstalled = true;
    }
    return Executed;
  }
  BionicResult pendingDerefs(Bionic &LibC) {
    auto &Thread = LibC.binderThread();
    using mobile::dalvik::BinderReferenceOperation;
    while (!Thread.PendingWeak.empty() || !Thread.PendingStrong.empty()) {
      for (const auto &[Pointer, Cookie] : Thread.PendingWeak) {
        auto Released = LibC.JNI.DexRuntime->binderReference(
            BinderReferenceOperation::ReleaseDeferredWeak, Pointer, Cookie);
        if (Released.stop != mobile::dalvik::ExecutionStop::Returned)
          return unsupported(LibC.Result, Released.diagnostic);
      }
      Thread.PendingWeak.clear();
      if (!Thread.PendingStrong.empty()) {
        const auto [Pointer, Cookie] = Thread.PendingStrong.front();
        auto Released = LibC.JNI.DexRuntime->binderReference(
            BinderReferenceOperation::ReleaseDeferredStrong, Pointer, Cookie);
        if (Released.stop != mobile::dalvik::ExecutionStop::Returned)
          return unsupported(LibC.Result, Released.diagnostic);
        Thread.PendingStrong.erase(Thread.PendingStrong.begin());
      }
    }
    return value(0);
  }

  bool ready() const override {
    return Device->hasReadyBinderThreadWork(Continuation->Thread);
  }
  std::optional<uint64_t> nextWake() const override { return std::nullopt; }
  BionicResult resume(Bionic &LibC) override { return run(LibC); }
  llvm::Expected<std::optional<uint64_t>>
  exchange(Bionic &LibC, uint32_t Descriptor,
           const std::shared_ptr<linux_model::LinuxDeviceState> &Expected,
           llvm::ArrayRef<uint8_t> Commands, bool Read) {
    auto Current = LibC.Kernel.binderConnection(Descriptor);
    if (!Current)
      return std::nullopt;
    if (Current != Expected) {
      unsupported(LibC.Result,
                  "Android native Binder descriptor was closed and reused");
      return std::nullopt;
    }
    if (!Commands.empty())
      if (auto E = LibC.CPU.write(Scratch + 64, Commands))
        return std::move(E);
    uint8_t Record[48]{};
    llvm::support::endian::write64le(Record, Commands.size());
    llvm::support::endian::write64le(Record + 16, Scratch + 64);
    llvm::support::endian::write64le(Record + 24, Read ? 128 : 0);
    llvm::support::endian::write64le(Record + 40, Scratch + 256);
    if (auto E = LibC.CPU.write(Scratch, Record))
      return std::move(E);
    auto Sent =
        LibC.kernelService(linux_model::ServiceKind::Ioctl,
                           {0, 0, {Descriptor, 0xc0306201, Scratch}, {}});
    if (!Sent || !*Sent || **Sent || Commands.empty())
      return Sent;
    uint8_t Consumed[8];
    if (auto E = LibC.CPU.read(Scratch + 8, Consumed))
      return std::move(E);
    if (llvm::support::endian::read64le(Consumed) != Commands.size())
      return unsupported(
          LibC.Result,
          "Android Binder pending driver error left commands unconsumed");
    return Sent;
  }
  BionicResult flush(Bionic &LibC) {
    auto &Commands = LibC.binderThread().PendingCommands;
    for (const auto &Command : Commands) {
      if (Command.Lease) {
        auto Current = LibC.Kernel.binderBuffer(*Command.Device, Command.Value,
                                                Command.Lease->Size);
        if (!Current || Current->Lease != Command.Lease)
          return unsupported(
              LibC.Result,
              "Android Binder release buffer lease is no longer live");
      }
      const bool Acknowledgment =
          Command.Command == 0x40106308 || Command.Command == 0x40106309;
      uint8_t Bytes[20];
      llvm::support::endian::write32le(Bytes, Command.Command);
      llvm::support::endian::write64le(Bytes + 4, Command.Value);
      llvm::support::endian::write64le(Bytes + 12, Command.Cookie);
      auto Sent = exchange(
          LibC, Command.FD, Command.Device,
          llvm::ArrayRef(Bytes).take_front(Acknowledgment                  ? 20
                                           : Command.Command == 0x40086303 ? 12
                                                                           : 8),
          false);
      if (!Sent)
        return Sent.takeError();
      if (!*Sent)
        return std::nullopt;
      if (**Sent)
        return unsupported(LibC.Result, "Android Binder reference or release "
                                        "command returned an unmodeled errno");
    }
    Commands.clear();
    return value(0);
  }
  BionicResult run(Bionic &LibC) {
    while (true) {
      if (Join && !Dispatching) {
        auto Released = pendingDerefs(LibC);
        if (!Released || !*Released)
          return Released;
      }
      // An exhausted input buffer flushes mOut even when the next read waits.
      // Returning a reply above that boundary leaves its queued ACKs intact.
      auto Flushed = flush(LibC);
      if (!Flushed || !*Flushed)
        return Flushed;
      if (!ready())
        break;
      auto ReadResult = exchange(LibC, FD, Device, {}, true);
      if (!ReadResult)
        return ReadResult.takeError();
      if (!*ReadResult)
        return std::nullopt;
      if (**ReadResult)
        return unsupported(
            LibC.Result,
            "Android Binder driver read returned an unmodeled errno");
      uint8_t Count[8], Bytes[128];
      if (auto E = LibC.CPU.read(Scratch + 32, Count))
        return std::move(E);
      const auto Size = llvm::support::endian::read64le(Count);
      if (auto E = LibC.CPU.read(Scratch + 256, Bytes))
        return std::move(E);
      mobile::dalvik::BinderReply Reply;
      bool Finished = false;
      for (uint64_t At = 0; At < Size;) {
        const uint32_t Command = llvm::support::endian::read32le(Bytes + At);
        At += 4;
        if ((Command == 0x7206 || Command == 0x7205 || Command == 0x7211) &&
            !ReplyContinuation.expired()) {
          // executeCommand ignores sendReply's status, then restores TLS and
          // destroys both native Parcels even when the caller has died.
          auto &Thread = LibC.binderThread();
          Thread.CallingIdentity = SavedIdentity;
          Thread.StrictModePolicy = SavedPolicy;
          Thread.LastTransactionFlags = SavedFlags;
          if (auto E = LibC.release(ReplyStorage))
            return std::move(E);
          ReplyStorage = 0;
          auto Completed = ReplyContinuation.lock();
          ReplyContinuation.reset();
          return LibC.resumeDex(std::move(Completed), {});
        }
        if (Command == 0x720c || Command == 0x7206)
          continue;
        if (Command >= 0x80107207 && Command <= 0x8010720a) {
          const uint64_t Pointer = llvm::support::endian::read64le(Bytes + At);
          const uint64_t Cookie =
              llvm::support::endian::read64le(Bytes + At + 8);
          At += 16;
          using mobile::dalvik::BinderReferenceOperation;
          const auto Operation =
              Command == 0x80107207   ? BinderReferenceOperation::AcquireWeak
              : Command == 0x80107208 ? BinderReferenceOperation::AcquireStrong
              : Command == 0x80107209
                  ? BinderReferenceOperation::DeferStrongRelease
                  : BinderReferenceOperation::DeferWeakRelease;
          auto Applied =
              LibC.JNI.DexRuntime->binderReference(Operation, Pointer, Cookie);
          if (Applied.stop != mobile::dalvik::ExecutionStop::Returned)
            return unsupported(LibC.Result, Applied.diagnostic);
          if (Command == 0x80107207 || Command == 0x80107208)
            LibC.binderThread().PendingCommands.push_back(
                {Device,
                 FD,
                 Command == 0x80107207 ? 0x40106308u : 0x40106309u,
                 Pointer,
                 {},
                 Cookie});
          else {
            auto &Thread = LibC.binderThread();
            (Command == 0x80107209 ? Thread.PendingStrong : Thread.PendingWeak)
                .emplace_back(Pointer, Cookie);
          }
          continue;
        }
        if (Command == 0x7205 || Command == 0x7211) {
          if (Join)
            return unsupported(
                LibC.Result,
                "Android Binder pool received an unsolicited reply failure");
          Reply.status = Command == 0x7205 ? -32 : INT32_MIN + 2;
          Finished = true;
          break;
        }
        const bool Incoming = Command == 0x80407202;
        if (!(Command == 0x80407203 && !Join) &&
            !(Join && !Dispatching && Incoming))
          return unsupported(
              LibC.Result,
              "Android Binder native incoming command dispatch is not modeled");
        const uint32_t Flags = llvm::support::endian::read32le(Bytes + At + 20);
        const auto Length = llvm::support::endian::read64le(Bytes + At + 32);
        const auto Address = llvm::support::endian::read64le(Bytes + At + 48);
        auto Buffer = LibC.Kernel.binderBuffer(*Device, Address, Length);
        if (!Buffer)
          return unsupported(
              LibC.Result,
              "Android Binder delivered buffer ownership was lost");
        auto Release = [&LibC, Device = Device, FD = FD, Address,
                        Lease = Buffer->Lease] {
          auto &Thread = LibC.binderThread();
          Thread.UsesBinder = true;
          Thread.PendingCommands.push_back(
              {Device, FD, 0x40086303, Address, Lease});
        };
        if (Flags & 8) {
          if (auto E = LibC.access(Address, Length, Read))
            return std::move(E);
          Reply.status =
              int32_t(llvm::support::endian::read32le(Buffer->Bytes.data()));
          Release();
        } else {
          const auto OffsetsAddress =
              llvm::support::endian::read64le(Bytes + At + 56);
          if (!Buffer->Lease->Objects.empty()) {
            if (auto E = LibC.access(OffsetsAddress,
                                     Buffer->Lease->Objects.size() * 8, Read))
              return std::move(E);
            for (auto Offset : Buffer->Lease->Objects)
              if (auto E = LibC.access(Address + Offset, 4, Read))
                return std::move(E);
          }
          Reply.objects = Buffer->Lease->Objects;
          Reply.fd = FD;
          Reply.bytes.emplace(Buffer->Bytes.begin(), Buffer->Bytes.end());
          Reply.release = std::move(Release);
          Reply.reallocate_objects =
              [&LibC, OffsetsAddress](
                  uint64_t Size) -> mobile::dalvik::ExecutionResult {
            using mobile::dalvik::ExecutionStop;
            // Preserve the received pointer even for zero offsets. Binder's
            // mmap storage is subject to the same ownership check as realloc.
            auto Reallocated = LibC.reallocate(OffsetsAddress, Size);
            if (!Reallocated)
              return {ExecutionStop::Invalid,
                      0,
                      {},
                      llvm::toString(Reallocated.takeError())};
            if (!*Reallocated)
              return {ExecutionStop::Thrown, 0, "java/lang/OutOfMemoryError"};
            return {
                ExecutionStop::Unsupported,
                0,
                {},
                "Android Parcel reallocated external object-table ownership "
                "is not modeled"};
          };
          Reply.write = [&LibC, Device = Device, Address, Length,
                         Lease = Buffer->Lease](uint64_t Offset,
                                                std::span<const uint8_t> Data)
              -> mobile::dalvik::ExecutionResult {
            using mobile::dalvik::ExecutionStop;
            auto Current = LibC.Kernel.binderBuffer(*Device, Address, Length);
            if (!Current || Current->Lease != Lease)
              return {ExecutionStop::Unsupported,
                      0,
                      {},
                      "Android Parcel Binder buffer mapping is no longer live"};
            if (auto E = LibC.access(Address + Offset, Data.size(), Write))
              return {
                  ExecutionStop::Invalid, 0, {}, llvm::toString(std::move(E))};
            if (auto E = LibC.CPU.write(Address + Offset,
                                        {Data.data(), Data.size()}))
              return {
                  ExecutionStop::Invalid, 0, {}, llvm::toString(std::move(E))};
            return {};
          };
          Reply.view = [&LibC, Device = Device, Address, Length, OffsetsAddress,
                        Lease = Buffer->Lease](std::span<const uint8_t> &View)
              -> mobile::dalvik::ExecutionResult {
            using mobile::dalvik::ExecutionStop;
            auto Bytes = LibC.Kernel.binderBuffer(*Device, Address, Length);
            if (!Bytes || Bytes->Lease != Lease)
              return {ExecutionStop::Unsupported,
                      0,
                      {},
                      "Android Parcel Binder buffer mapping is no longer live"};
            auto Readable =
                LibC.CPU.canAccess(Address, Length, Read | UserAccessible);
            const char *Diagnostic =
                "Android Parcel Binder buffer is not readable";
            if (Readable && *Readable && !Lease->Objects.empty()) {
              Diagnostic = "Android Parcel Binder object table is not readable";
              Readable =
                  LibC.CPU.canAccess(OffsetsAddress, Lease->Objects.size() * 8,
                                     Read | UserAccessible);
            }
            if (!Readable)
              return {ExecutionStop::Invalid,
                      0,
                      {},
                      llvm::toString(Readable.takeError())};
            if (!*Readable)
              return {ExecutionStop::Unsupported, 0, {}, Diagnostic};
            View = std::span(Bytes->Bytes.data(), Bytes->Bytes.size());
            return {};
          };
        }
        if (Incoming) {
          if (++LibC.BinderExecuting >= 15)
            return unsupported(
                LibC.Result,
                "Android Binder pool starvation timing is not modeled");
          mobile::dalvik::BinderIncoming Request;
          Request.pointer = llvm::support::endian::read64le(Bytes + At);
          Request.cookie = llvm::support::endian::read64le(Bytes + At + 8);
          Request.code = llvm::support::endian::read32le(Bytes + At + 16);
          Request.flags = Flags;
          Request.uid = llvm::support::endian::read32le(Bytes + At + 28);
          Request.data = std::move(Reply);
          auto &Thread = LibC.binderThread();
          SavedIdentity = Thread.CallingIdentity;
          SavedPolicy = Thread.StrictModePolicy;
          SavedFlags = Thread.LastTransactionFlags;
          Thread.CallingIdentity = std::make_pair(
              llvm::support::endian::read32le(Bytes + At + 24), Request.uid);
          Thread.LastTransactionFlags = Flags;
          Dispatching = true;
          auto Self = shared_from_this();
          LibC.BinderDispatches[Continuation->Thread] = Self;
          return LibC.runDex(
              [&LibC, Request = std::move(Request)](const auto &Admit,
                                                    uint64_t Stack) mutable {
                return LibC.JNI.DexRuntime->dispatchBinder(
                    std::move(Request), Admit, Stack, LibC.threadID());
              },
              [&LibC, Self](uint64_t) {
                --LibC.BinderExecuting;
                Self->Dispatching = false;
                LibC.BinderDispatches.erase(Self->Continuation->Thread);
                return Self->drive(LibC);
              });
        }
        Finished = true;
        break;
      }
      if (Finished) {
        if (auto E = LibC.release(Scratch))
          return std::move(E);
        mobile::dalvik::ExecutionResult Completed;
        Completed.binder_reply = std::move(Reply);
        return LibC.resumeDex(std::move(Continuation), std::move(Completed));
      }
    }
    if (!LibC.Threads || !LibC.Threads->enabled())
      return unsupported(
          LibC.Result,
          "Android Binder transaction requires a scheduled guest peer");
    return BionicValue(GuestThreadWait{});
  }
};

BionicResult
Bionic::binderTransact(const mobile::dalvik::ExternalCall &Call,
                       std::shared_ptr<DexContinuation> Continuation) {
  const auto &Request = *Call.binder;
  const uint32_t FD = Request.fd;
  auto Device = Kernel.binderConnection(FD);
  if (!Device)
    return std::nullopt;
  if (BinderConnection && (BinderConnection != Device || BinderFD != FD))
    return unsupported(Result,
                       "Android native Binder descriptor was closed and reused "
                       "or names a different ProcessState connection");
  if (!BinderConnection) {
    BinderConnection = Device;
    BinderFD = FD;
  }
  binderThread().UsesBinder = true;
  const auto &Payload = Request.bytes;
  const auto OffsetsAt = (Payload.size() + 7) & ~uint64_t(7);
  auto Scratch = allocate(512 + OffsetsAt + Request.objects.size() * 8);
  if (!Scratch)
    return Scratch.takeError();
  if (!*Scratch)
    return unsupported(Result,
                       "Android Binder bridge storage budget exhausted");
  auto Pending = std::make_unique<BinderWait>();
  Pending->Device = Device;
  Pending->FD = FD;
  Pending->Scratch = *Scratch;
  Pending->Continuation = std::move(Continuation);
  auto Flushed = Pending->flush(*this);
  if (!Flushed)
    return Flushed.takeError();
  if (!*Flushed)
    return std::nullopt;
  if (auto E = CPU.write(*Scratch + 512, Payload))
    return std::move(E);
  if (!Request.objects.empty()) {
    std::vector<uint8_t> Offsets(Request.objects.size() * 8);
    for (size_t I = 0; I != Request.objects.size(); ++I)
      llvm::support::endian::write64le(Offsets.data() + I * 8,
                                       Request.objects[I]);
    if (auto E = CPU.write(*Scratch + 512 + OffsetsAt, Offsets))
      return std::move(E);
  }
  uint8_t Command[68]{};
  llvm::support::endian::write32le(Command, 0x40406300);
  llvm::support::endian::write32le(Command + 4, Request.handle);
  llvm::support::endian::write32le(Command + 20, Request.code);
  llvm::support::endian::write32le(Command + 24, Request.flags | 0x10);
  llvm::support::endian::write64le(Command + 36, Payload.size());
  llvm::support::endian::write64le(Command + 44, Request.objects.size() * 8);
  llvm::support::endian::write64le(Command + 52, *Scratch + 512);
  llvm::support::endian::write64le(Command + 60, *Scratch + 512 + OffsetsAt);
  auto Sent = Pending->exchange(*this, FD, Device, Command, false);
  if (!Sent)
    return Sent.takeError();
  if (!*Sent)
    return std::nullopt;
  if (**Sent)
    return unsupported(
        Result, "Android Binder driver write returned an unmodeled errno");
  auto Executed = Pending->run(*this);
  if (Executed && *Executed &&
      std::holds_alternative<GuestThreadWait>(**Executed))
    Threads->waitNative(std::move(Pending));
  return Executed;
}
BionicResult Bionic::binderJoin(const mobile::dalvik::ExternalCall &Call,
                                std::shared_ptr<DexContinuation> Continuation) {
  const auto FD = Call.binder->fd;
  auto Device = Kernel.binderConnection(FD);
  if (!Device)
    return std::nullopt;
  if (BinderConnection && (BinderConnection != Device || BinderFD != FD))
    return unsupported(
        Result,
        "Android Binder join requires its original ProcessState connection");
  BinderConnection = Device;
  BinderFD = FD;
  if (!Threads || !Threads->enabled())
    return unsupported(Result,
                       "Android Binder pool requires a scheduled guest peer");
  auto Scratch = allocate(512);
  if (!Scratch)
    return Scratch.takeError();
  if (!*Scratch)
    return unsupported(Result, "Android Binder pool storage budget exhausted");
  auto Pending = std::make_shared<BinderWait>();
  Pending->Join = true;
  Pending->Device = Device;
  Pending->FD = FD;
  Pending->Scratch = *Scratch;
  Pending->Continuation = std::move(Continuation);
  binderThread().UsesBinder = true;
  auto Flushed = Pending->flush(*this);
  if (!Flushed || !*Flushed)
    return Flushed;
  uint8_t Command[4];
  llvm::support::endian::write32le(Command, 0x630c); // BC_ENTER_LOOPER
  auto Sent = Pending->exchange(*this, FD, Device, Command, false);
  if (!Sent)
    return Sent.takeError();
  if (!*Sent)
    return std::nullopt;
  if (**Sent)
    return unsupported(Result,
                       "Android Binder pool entry returned an unmodeled errno");
  return Pending->drive(*this);
}

BionicResult
Bionic::binderSendReply(const mobile::dalvik::ExternalCall &Call,
                        std::shared_ptr<DexContinuation> Continuation) {
  const auto Active = BinderDispatches.find(threadID());
  auto Pending =
      Active == BinderDispatches.end() ? nullptr : Active->second.lock();
  if (!Pending || !Pending->ReplyContinuation.expired())
    return failure("Binder reply has no active incoming transaction");
  const auto &Reply = *Call.response;
  const auto Size = Reply.status ? 4 : Reply.bytes.size();
  const auto Objects = Reply.status ? 0 : Reply.objects.size();
  const auto OffsetsAt = (Size + 7) & ~uint64_t(7);
  auto Storage = allocate(std::max<uint64_t>(8, OffsetsAt + Objects * 8));
  if (!Storage)
    return Storage.takeError();
  if (!*Storage)
    return unsupported(Result,
                       "Android Binder response storage budget exhausted");
  Pending->ReplyStorage = *Storage;
  if (Reply.status) {
    uint8_t Status[4];
    llvm::support::endian::write32le(Status, uint32_t(Reply.status));
    if (auto E = CPU.write(*Storage, Status))
      return std::move(E);
  } else if (auto E = CPU.write(*Storage, Reply.bytes))
    return std::move(E);
  if (Objects) {
    std::vector<uint8_t> Offsets(Objects * 8);
    for (size_t I = 0; I != Objects; ++I)
      llvm::support::endian::write64le(Offsets.data() + I * 8,
                                       Reply.objects[I]);
    if (auto E = CPU.write(*Storage + OffsetsAt, Offsets))
      return std::move(E);
  }
  auto Flushed = Pending->flush(*this);
  if (!Flushed || !*Flushed)
    return Flushed;
  uint8_t Command[68]{};
  llvm::support::endian::write32le(Command, 0x40406301); // BC_REPLY
  llvm::support::endian::write32le(Command + 24, Reply.status ? 8 : 0);
  llvm::support::endian::write64le(Command + 36, Size);
  llvm::support::endian::write64le(Command + 44, Objects * 8);
  llvm::support::endian::write64le(Command + 52, *Storage);
  llvm::support::endian::write64le(Command + 60, *Storage + OffsetsAt);
  auto Sent =
      Pending->exchange(*this, Pending->FD, Pending->Device, Command, false);
  if (!Sent)
    return Sent.takeError();
  if (!*Sent)
    return std::nullopt;
  if (**Sent)
    return unsupported(
        Result, "Android Binder reply write returned an unmodeled errno");
  Pending->ReplyContinuation = Continuation;
  return Pending->drive(*this);
}
BionicResult Bionic::binderThreadExit() {
  auto &Thread = binderThread();
  if (!Thread.UsesBinder)
    return value(0);
  BinderWait Exit;
  auto Scratch = allocate(512);
  if (!Scratch)
    return Scratch.takeError();
  if (!*Scratch)
    return unsupported(Result,
                       "Android Binder thread exit storage budget exhausted");
  Exit.Scratch = *Scratch;
  auto Flushed = Exit.flush(*this);
  if (!Flushed)
    return Flushed.takeError();
  if (!*Flushed)
    return std::nullopt;
  if (Kernel.binderConnection(BinderFD) != BinderConnection)
    return unsupported(
        Result, "Android Binder thread exit requires its original connection");
  auto Exited = kernelService(linux_model::ServiceKind::Ioctl,
                              {0, 0, {BinderFD, 0x40046208, 0}, {}});
  if (!Exited)
    return Exited.takeError();
  if (!*Exited)
    return std::nullopt;
  if (**Exited)
    return unsupported(
        Result, "Android Binder thread exit returned an unmodeled errno");
  if (auto E = release(*Scratch))
    return std::move(E);
  Thread.UsesBinder = false;
  return value(0);
}
} // namespace neverd::emulation::android_model
