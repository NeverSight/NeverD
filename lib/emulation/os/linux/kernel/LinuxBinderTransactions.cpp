//===- LinuxBinderTransactions.cpp - Workload-owned Binder message routing ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Synchronous, object-free protocol-8 transactions to a guest-owned context
/// manager. Guest code produces every service reply. No Parcel interpretation,
/// remote Android service or implicit service result lives in this layer.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxMemory.h"

#include "neverd/emulation/Limits.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::linux_model {
namespace {
constexpr uint32_t Reply = 0x40406301;
constexpr uint32_t FreeBuffer = 0x40086303;
constexpr uint32_t Received = 0x80407202, Replied = 0x80407203;
constexpr uint32_t Complete = 0x7206, Dead = 0x7205, Failed = 0x7211;
uint64_t allocationSize(uint64_t Size) {
  return std::max<uint64_t>(8, (Size + 7) & ~uint64_t(7));
}
} // namespace

std::shared_ptr<LinuxDeviceState>
LinuxFiles::binderConnection(uint32_t FD, ProcessResult &Result) const {
  auto Mapping = mappingDevice(FD);
  if (auto Device = std::get_if<std::shared_ptr<LinuxDeviceState>>(&Mapping);
      Device && *Device && (*Device)->Options->Binder)
    return *Device;
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic =
      "Android native Binder binding requires a live Binder descriptor";
  return {};
}
std::optional<llvm::ArrayRef<uint8_t>>
LinuxFiles::binderBuffer(const LinuxDeviceState &Device, uint64_t Address,
                         uint64_t Size) const {
  if (Memory)
    for (const auto &[Offset, Buffer] : Device.BinderBuffers)
      if (Buffer.Delivered && Buffer.Size == Size &&
          Memory->binderAddress(Device, Offset, allocationSize(Size)) ==
              Address)
        return Memory->deviceBytes(Device.BinderMapping).slice(Offset, Size);
  return std::nullopt;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::binderCommand(LinuxDeviceState &Device, uint32_t Command,
                          llvm::ArrayRef<uint8_t> Data, ProcessResult &Result) {
  using Outcome = std::optional<uint64_t>;
  if (!Memory)
    return unsupported(Result,
                       "Binder transactions require mapped buffer ownership");
  auto Get64 = [&](unsigned At) {
    return llvm::support::endian::read64le(Data.data() + At);
  };
  auto Get32 = [&](unsigned At) {
    return llvm::support::endian::read32le(Data.data() + At);
  };
  if (Command == FreeBuffer) {
    const uint64_t Address = Get64(0);
    for (auto I = Device.BinderBuffers.begin(); I != Device.BinderBuffers.end();
         ++I)
      if (Memory->binderAddress(Device, I->first,
                                allocationSize(I->second.Size)) == Address) {
        if (!I->second.Delivered)
          return unsupported(Result, "Binder free requires a delivered buffer");
        Device.BinderBuffers.erase(I);
        return Outcome(0);
      }
    return unsupported(Result,
                       "Binder free does not identify an owned live buffer");
  }
  const bool IsReply = Command == Reply;
  const uint32_t Flags = Get32(20);
  // libbinder adds TF_ACCEPT_FDS to ordinary calls, including object-free
  // ones. It grants no object translation here: nonempty offsets still stop.
  // TF_STATUS_CODE carries a guest-produced status_t, not a model result.
  const uint32_t AllowedFlags = IsReply ? 0x08 : 0x10;
  if ((Flags & ~AllowedFlags) || Get64(40) ||
      ((Flags & 0x08) && Get64(32) != 4)) {
    auto Message =
        llvm::formatv(
            "Binder transaction command {0:x} target {1:x} code {2:x} flags "
            "{3:x} "
            "data_size {4} offsets_size {5}: flags, objects or status payload "
            "are outside the object-free profile",
            Command, Get32(0), Get32(16), Flags, Get64(32), Get64(40))
            .str();
    return unsupported(Result, Message.c_str());
  }
  if (Get64(32) > ::neverd::limits::emulation::kMaxBinderTransactionBytes)
    return unsupported(Result, "Binder transaction payload budget exhausted");
  auto &OwnQueue = Device.BinderThreadWork[CurrentTask];
  if (OwnQueue.size() >=
      ::neverd::limits::emulation::
          kLinuxBinderTransactionsLinuxFilesBinderCommandOwnQueueSizeLimit)
    return unsupported(Result, "Binder thread work budget exhausted");
  auto Error = [&](uint32_t Code) -> Outcome {
    OwnQueue.push_back({Code});
    return Outcome(0);
  };
  std::shared_ptr<LinuxDeviceState> Target, Sender;
  std::shared_ptr<BinderCall> Call;
  if (IsReply) {
    auto I = Device.BinderIncoming.find(CurrentTask);
    if (I == Device.BinderIncoming.end())
      return Error(Failed);
    Call = I->second;
    Target = Call->Sender.lock();
    if (!Target || !Target->BinderOutgoing.contains(Call->Thread) ||
        Target->BinderOutgoing.at(Call->Thread) != Call)
      return unsupported(Result,
                         "Binder reply requires its live original caller");
    if (Target->BinderThreadWork[Call->Thread].size() >
        ::neverd::limits::emulation::
            kLinuxBinderTransactionsLinuxFilesBinderCommandTargetBinderThreadWorkCallThreadSizeLimit)
      return unsupported(Result, "Binder caller work budget exhausted");
  } else {
    if (Device.BinderIncoming.contains(CurrentTask) ||
        Device.BinderOutgoing.contains(CurrentTask))
      return unsupported(Result,
                         "Binder nested transactions are outside this profile");
    if (Get32(0))
      return Error(Failed); // No translated handles exist in this profile.
    for (const auto &Weak : DeviceDescriptions)
      if (auto Open = Weak.lock()) {
        if (Open.get() == &Device)
          Sender = Open;
        if (Open->Options == Device.Options && Open->BinderManager)
          Target = Open;
      }
    if (!Target)
      return Error(Dead);
    if (Target.get() == &Device)
      return Error(Failed);
    if (!Sender)
      return unsupported(Result, "Binder sender open identity is unavailable");
    if (Target->BinderProcessWork.size() >=
        ::neverd::limits::emulation::
            kLinuxBinderTransactionsLinuxFilesBinderCommandTargetBinderProcessWorkSizeLimit)
      return unsupported(Result, "Binder process work budget exhausted");
    Call = std::make_shared<BinderCall>();
    Call->Sender = Sender;
    Call->Thread = CurrentTask;
  }
  const uint64_t Size = Get64(32), Needed = allocationSize(Size);
  if (Target->BinderBuffers.size() >=
      ::neverd::limits::emulation::
          kLinuxBinderTransactionsLinuxFilesBinderCommandTargetBinderBuffersSizeLimit)
    return unsupported(Result, "Binder live buffer budget exhausted");
  uint64_t Offset = 0;
  for (const auto &[At, Buffer] : Target->BinderBuffers) {
    if (Needed <= At - Offset)
      break;
    Offset = At + allocationSize(Buffer.Size);
  }
  if (Offset > Target->BinderMapping.Bytes.size() ||
      Needed > Target->BinderMapping.Bytes.size() - Offset ||
      !Memory->binderAddress(*Target, Offset, Needed))
    return unsupported(Result,
                       "Binder peer has no complete live reply arena capacity");
  const uint64_t Address = Get64(48);
  if (Size && (Address > Layout.UserLimit || Size > Layout.UserLimit - Address))
    return unsupported(Result,
                       "Binder payload address wraps or leaves userspace");
  if (Size) {
    auto Readable = CPU.canAccess(Address, Size, Read | UserAccessible);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return unsupported(Result,
                         "Binder payload copy-in is not fully readable");
  }
  std::vector<uint8_t> Payload(Size);
  if (Size)
    if (auto E = CPU.read(Address, Payload))
      return std::move(E);
  if (auto E = Memory->writeDeviceBytes(Target->BinderMapping, Offset, Payload))
    return std::move(E);
  Target->BinderBuffers.emplace(Offset, BinderBuffer{Size, false});
  BinderWork Work{
      IsReply ? Replied : Received,       Get32(16), Flags,
      IsReply ? 0u : uint32_t(ProcessID), Offset,    IsReply ? nullptr : Call};
  if (IsReply) {
    auto &Queue = Target->BinderThreadWork[Call->Thread];
    Queue.push_back({Complete});
    Queue.push_back(std::move(Work));
    Device.BinderIncoming.erase(CurrentTask);
    // The caller remains busy until BR_REPLY is delivered to that thread.
    OwnQueue.push_back({Complete});
  } else {
    Device.BinderOutgoing.emplace(CurrentTask, Call);
    Target->BinderProcessWork.push_back(std::move(Work));
  }
  return Outcome(0);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::binderReadWork(LinuxDeviceState &Device, uint64_t Address,
                           uint64_t Size, ProcessResult &Result) {
  using Outcome = std::optional<uint64_t>;
  auto &Own = Device.BinderThreadWork[CurrentTask];
  // Kernel eligibility is fixed before draining this read. A thread with
  // private work cannot also take process work in that same read operation.
  const bool ProcessEligible = Own.empty() &&
                               !Device.BinderOutgoing.contains(CurrentTask) &&
                               !Device.BinderIncoming.contains(CurrentTask) &&
                               (Device.BinderLoopers.at(CurrentTask) & 3);
  auto &Queue = !Own.empty() ? Own : Device.BinderProcessWork;
  if (Queue.empty() || (Own.empty() && !ProcessEligible))
    return Outcome(0);
  if (!Memory)
    return unsupported(Result,
                       "Binder delivery requires mapped buffer ownership");
  std::vector<uint8_t> Output(4);
  llvm::support::endian::write32le(Output.data(), 0x720c);
  size_t Delivered = 0;
  // The kernel reserves a full transaction record even for four-byte work.
  for (const auto &Work : Queue) {
    if (Size - Output.size() < 68)
      break;
    const size_t At = Output.size();
    const bool Transaction =
        Work.Command == Received || Work.Command == Replied;
    Output.resize(At + (Transaction ? 68 : 4));
    llvm::support::endian::write32le(Output.data() + At, Work.Command);
    ++Delivered;
    if (!Transaction)
      continue;
    auto I = Device.BinderBuffers.find(Work.Offset);
    if (I == Device.BinderBuffers.end())
      return unsupported(Result, "Binder queued buffer ownership was lost");
    auto Buffer = Memory->binderAddress(Device, Work.Offset,
                                        allocationSize(I->second.Size));
    if (!Buffer)
      return unsupported(Result, "Binder queued buffer mapping was retired");
    auto *Record = Output.data() + At + 4;
    llvm::support::endian::write32le(Record + 16, Work.Code);
    llvm::support::endian::write32le(Record + 20, Work.Flags);
    llvm::support::endian::write32le(Record + 24, Work.SenderPID);
    llvm::support::endian::write32le(Record + 28, UserID);
    llvm::support::endian::write64le(Record + 32, I->second.Size);
    llvm::support::endian::write64le(Record + 48, *Buffer);
    llvm::support::endian::write64le(
        Record + 56, *Buffer + ((I->second.Size + 7) & ~uint64_t(7)));
    break; // A delivered request/reply returns control to native service code.
  }
  if (Address > Layout.UserLimit || Output.size() > Layout.UserLimit - Address)
    return unsupported(Result, "Binder delivery output leaves userspace");
  auto Writable = CPU.canAccess(Address, Output.size(), Write | UserAccessible);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return unsupported(Result, "Binder partial work copy-out is unmodeled");
  if (auto E = CPU.write(Address, Output))
    return std::move(E);
  while (Delivered--) {
    auto &Work = Queue.front();
    if (Work.Command == Received) {
      Device.BinderIncoming.emplace(CurrentTask, Work.Call);
      Device.BinderBuffers.at(Work.Offset).Delivered = true;
    } else if (Work.Command == Replied) {
      Device.BinderOutgoing.erase(CurrentTask);
      Device.BinderBuffers.at(Work.Offset).Delivered = true;
    }
    Queue.pop_front();
  }
  return Outcome(Output.size());
}
} // namespace neverd::emulation::linux_model
