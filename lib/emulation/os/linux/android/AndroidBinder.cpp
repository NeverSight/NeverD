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

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace neverd::emulation::android_model {
namespace {
std::nullopt_t unsupported(ProcessResult &Result, llvm::StringRef Message) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Message.str();
  return std::nullopt;
}
} // namespace
class Bionic::BinderWait final : public NativeWait {
public:
  std::shared_ptr<linux_model::LinuxDeviceState> Device;
  std::shared_ptr<DexContinuation> Continuation;
  uint32_t FD;
  uint64_t Scratch;

  bool ready() const override {
    auto I = Device->BinderThreadWork.find(Continuation->Thread);
    return I != Device->BinderThreadWork.end() && !I->second.empty();
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
    return LibC.kernelService(linux_model::ServiceKind::Ioctl,
                              {0, 0, {Descriptor, 0xc0306201, Scratch}, {}});
  }
  BionicResult flush(Bionic &LibC) {
    auto &Frees = LibC.binderThread().PendingFrees;
    for (const auto &Free : Frees) {
      uint8_t Bytes[12];
      llvm::support::endian::write32le(Bytes, 0x40086303);
      llvm::support::endian::write64le(Bytes + 4, Free.Address);
      auto Sent = exchange(LibC, Free.FD, Free.Device, Bytes, false);
      if (!Sent)
        return Sent.takeError();
      if (!*Sent)
        return std::nullopt;
      if (**Sent)
        return unsupported(
            LibC.Result,
            "Android Binder buffer release returned an unmodeled errno");
    }
    Frees.clear();
    return value(0);
  }
  BionicResult run(Bionic &LibC) {
    while (ready()) {
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
        if (Command == 0x720c || Command == 0x7206)
          continue;
        if (Command == 0x7205 || Command == 0x7211) {
          Reply.status = Command == 0x7205 ? -32 : INT32_MIN + 2;
          Finished = true;
          break;
        }
        if (Command != 0x80407203)
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
        auto Release = [&LibC, Device = Device, FD = FD, Address] {
          auto &Thread = LibC.binderThread();
          Thread.UsesBinder = true;
          Thread.PendingFrees.push_back({Device, FD, Address});
        };
        if (Flags & 8) {
          if (auto E = LibC.access(Address, Length, Read))
            return std::move(E);
          Reply.status =
              int32_t(llvm::support::endian::read32le(Buffer->data()));
          Release();
        } else {
          Reply.bytes.emplace(Buffer->begin(), Buffer->end());
          Reply.release = std::move(Release);
          Reply.view = [&LibC, Device = Device, Address,
                        Length](std::span<const uint8_t> &View)
              -> mobile::dalvik::ExecutionResult {
            using mobile::dalvik::ExecutionStop;
            auto Bytes = LibC.Kernel.binderBuffer(*Device, Address, Length);
            if (!Bytes)
              return {ExecutionStop::Unsupported,
                      0,
                      {},
                      "Android Parcel Binder buffer mapping is no longer live"};
            auto Readable =
                LibC.CPU.canAccess(Address, Length, Read | UserAccessible);
            if (!Readable)
              return {ExecutionStop::Invalid,
                      0,
                      {},
                      llvm::toString(Readable.takeError())};
            if (!*Readable)
              return {ExecutionStop::Unsupported,
                      0,
                      {},
                      "Android Parcel Binder buffer is not readable"};
            View = std::span(Bytes->data(), Bytes->size());
            return {};
          };
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
  if (BinderConnection && BinderConnection != Device)
    return unsupported(Result,
                       "Android native Binder descriptor was closed and reused "
                       "or names a different ProcessState connection");
  if (!BinderConnection) {
    BinderConnection = Device;
    BinderFD = FD;
  }
  binderThread().UsesBinder = true;
  const auto &Payload = JNI.Heap->get(Request.data)->Bytes;
  auto Scratch = allocate(512 + Payload.size());
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
  uint8_t Command[68]{};
  llvm::support::endian::write32le(Command, 0x40406300);
  llvm::support::endian::write32le(Command + 4, Request.handle);
  llvm::support::endian::write32le(Command + 20, Request.code);
  llvm::support::endian::write32le(Command + 24, Request.flags | 0x10);
  llvm::support::endian::write64le(Command + 36, Payload.size());
  llvm::support::endian::write64le(Command + 52, *Scratch + 512);
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
