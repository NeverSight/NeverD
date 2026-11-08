//===- LinuxBinderTransactions.cpp - Workload-owned Binder message routing ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Synchronous protocol-8 transactions between guest-owned Binder opens. Guest
/// code produces every service reply. No Parcel interpretation, remote Android
/// service or implicit service result lives in this layer.
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxMemory.h"

#include "neverd/emulation/Limits.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
namespace {
constexpr uint32_t Reply = 0x40406301;
constexpr uint32_t FreeBuffer = 0x40086303;
constexpr uint32_t Received = 0x80407202, Replied = 0x80407203;
constexpr uint32_t Complete = 0x7206, Dead = 0x7205, Failed = 0x7211;
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
std::optional<BinderBufferView>
LinuxFiles::binderBuffer(const LinuxDeviceState &Device, uint64_t Address,
                         uint64_t Size) const {
  if (Memory)
    for (const auto &[Offset, Buffer] : Device.BinderBuffers)
      if (Buffer->Delivered && Buffer->Size == Size &&
          Memory->binderAddress(Device, Offset, Buffer->allocationSize()) ==
              Address)
        return BinderBufferView{
            Memory->deviceBytes(Device.BinderMapping).slice(Offset, Size),
            Buffer};
  return std::nullopt;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::binderCommand(LinuxDeviceState &Device, uint32_t Command,
                          llvm::ArrayRef<uint8_t> Data, ProcessResult &Result) {
  using Outcome = std::optional<uint64_t>;
  if ((Command >= 0x40046304 && Command <= 0x40046307) ||
      Command == 0x40106308 || Command == 0x40106309)
    return binderReferenceCommand(Device, Command, Data, Result);
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
                                I->second->allocationSize()) == Address) {
        if (!I->second->Delivered)
          return unsupported(Result, "Binder free requires a delivered buffer");
        if (!binderReleaseBuffer(Device, *I->second, Result))
          return std::nullopt;
        Device.BinderBuffers.erase(I);
        return Outcome(0);
      }
    return unsupported(Result,
                       "Binder free does not identify an owned live buffer");
  }
  const bool IsReply = Command == Reply;
  const uint32_t Flags = Get32(20);
  // TF_ACCEPT_FDS is legal on synchronous calls, but descriptor object
  // translation remains outside this bounded profile.
  const uint32_t AllowedFlags = IsReply ? 0x08 : 0x10;
  if ((Flags & ~AllowedFlags) ||
      ((Flags & 0x08) && (Get64(32) != 4 || Get64(40)))) {
    const auto Message =
        llvm::formatv("Binder transaction command {0:x} target {1:x} code "
                      "{2:x} flags {3:x} "
                      "data_size {4} offsets_size {5}: flags or status payload "
                      "are outside the synchronous profile",
                      Command, Get32(0), Get32(16), Flags, Get64(32), Get64(40))
            .str();
    return unsupported(Result, Message.c_str());
  }
  if (Get64(32) > ::neverd::limits::emulation::kMaxBinderTransactionBytes)
    return unsupported(Result, "Binder transaction payload budget exhausted");
  const uint64_t Size = Get64(32), OffsetsSize = Get64(40);
  if (OffsetsSize > ::neverd::limits::emulation::kMaxBinderTransactionBytes)
    return unsupported(Result, "Binder object table budget exhausted");
  auto &OwnQueue = Device.BinderThreadWork[CurrentTask];
  if (OwnQueue.size() >=
      ::neverd::limits::emulation::
          kLinuxBinderTransactionsLinuxFilesBinderCommandOwnQueueSizeLimit)
    return unsupported(Result, "Binder thread work budget exhausted");
  auto Error = [&](uint32_t Code) -> Outcome {
    BinderWork Work{Code};
    Work.ReturnError = true;
    OwnQueue.push_back(std::move(Work));
    return Outcome(0);
  };
  std::shared_ptr<LinuxDeviceState> Target, Sender;
  std::shared_ptr<BinderCall> Call;
  std::shared_ptr<BinderNode> TargetNode;
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
    Sender = binderOwner(Device);
    const auto Handle = Get32(0);
    if (Handle) {
      auto I = Device.BinderHandles.find(Handle);
      if (I == Device.BinderHandles.end() || !I->second.Strong)
        return Error(Failed);
      TargetNode = I->second.Node;
      Target = TargetNode->Owner.lock();
    } else {
      for (const auto &Weak : DeviceDescriptions)
        if (auto Open = Weak.lock();
            Open && Open->Options == Device.Options && Open->BinderManager) {
          Target = Open;
          TargetNode = Open->BinderNodes.at(0);
        }
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
  auto Buffer = std::make_shared<BinderBuffer>();
  Buffer->Size = Size;
  Buffer->Objects.resize(OffsetsSize / 8);
  const uint64_t Needed = std::max<uint64_t>(
      8, ((Size + 7) & ~uint64_t(7)) + ((OffsetsSize + 7) & ~uint64_t(7)));
  if (Target->BinderBuffers.size() >=
      ::neverd::limits::emulation::
          kLinuxBinderTransactionsLinuxFilesBinderCommandTargetBinderBuffersSizeLimit)
    return unsupported(Result, "Binder live buffer budget exhausted");
  uint64_t Offset = 0;
  for (const auto &[At, Buffer] : Target->BinderBuffers) {
    if (Needed <= At - Offset)
      break;
    Offset = At + Buffer->allocationSize();
  }
  if (Offset > Target->BinderMapping.Bytes.size() ||
      Needed > Target->BinderMapping.Bytes.size() - Offset ||
      !Memory->binderAddress(*Target, Offset, Needed))
    return unsupported(Result,
                       "Binder peer has no complete live reply arena capacity");
  auto Copy = [&](uint64_t Address, uint64_t Length)
      -> llvm::Expected<std::optional<std::vector<uint8_t>>> {
    if (Length &&
        (Address > Layout.UserLimit || Length > Layout.UserLimit - Address)) {
      unsupported(Result, "Binder payload address wraps or leaves userspace");
      return std::nullopt;
    }
    if (Length) {
      auto Readable = CPU.canAccess(Address, Length, Read | UserAccessible);
      if (!Readable)
        return Readable.takeError();
      if (!*Readable) {
        unsupported(
            Result,
            "Binder payload or object table copy-in is not fully readable");
        return std::nullopt;
      }
    }
    std::vector<uint8_t> Bytes(Length);
    if (Length)
      if (auto E = CPU.read(Address, Bytes))
        return std::move(E);
    return Bytes;
  };
  auto Payload = Copy(Get64(48), Size);
  if (!Payload)
    return Payload.takeError();
  if (!*Payload)
    return std::nullopt;
  auto Offsets = Copy(Get64(56), OffsetsSize);
  if (!Offsets)
    return Offsets.takeError();
  if (!*Offsets)
    return std::nullopt;
  for (size_t I = 0; I < Buffer->Objects.size(); ++I)
    Buffer->Objects[I] =
        llvm::support::endian::read64le((**Offsets).data() + I * 8);
  const auto Translation =
      OffsetsSize % 8
          ? BinderObjectTranslation::Rejected
          : binderTranslateObjects(Device, *Target, **Payload, *Buffer, Result);
  if (Translation == BinderObjectTranslation::Unsupported)
    return std::nullopt;
  if (TargetNode) {
    ++TargetNode->LocalStrong;
    Buffer->Target = TargetNode;
  }
  if (auto E =
          Memory->writeDeviceBytes(Target->BinderMapping, Offset, **Payload))
    return std::move(E);
  if (OffsetsSize)
    if (auto E = Memory->writeDeviceBytes(Target->BinderMapping,
                                          Offset + ((Size + 7) & ~uint64_t(7)),
                                          **Offsets))
      return std::move(E);
  if (Translation == BinderObjectTranslation::Rejected) {
    // The driver copied the bytes and translated a prefix, then released the
    // target and prefix references. The failed allocation is reusable without
    // clearing the mapped bytes and never becomes a delivered buffer lease.
    if (!binderReleaseBuffer(*Target, *Buffer, Result))
      return std::nullopt;
    if (!IsReply)
      return Error(Failed);
    Device.BinderIncoming.erase(CurrentTask);
    Target->BinderOutgoing.erase(Call->Thread);
    auto &Queue = Target->BinderThreadWork[Call->Thread];
    Queue.push_back({Complete});
    // reply_error is a single slot; a second failed call retires normally
    // without overwriting an unread failure from the previous call.
    if (std::none_of(Queue.begin(), Queue.end(), [](const BinderWork &Work) {
          return !Work.ReturnError &&
                 (Work.Command == Failed || Work.Command == Dead);
        }))
      Queue.push_back({Failed});
    return Error(Complete);
  }
  Target->BinderBuffers.emplace(Offset, std::move(Buffer));
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
  if (!Device.hasReadyBinderThreadWork(CurrentTask))
    return Outcome(0);
  // Kernel eligibility is fixed before draining this read. A thread with
  // private work cannot also take process work in that same read operation.
  auto &Queue = !Own.empty() ? Own : Device.BinderProcessWork;
  if (!Memory)
    return unsupported(Result,
                       "Binder delivery requires mapped buffer ownership");
  std::vector<uint8_t> Output(4);
  llvm::support::endian::write32le(Output.data(), 0x720c);
  size_t Delivered = 0;
  std::vector<uint8_t> NodeEvents;
  // The kernel reserves a full transaction record even for four-byte work.
  for (const auto &Work : Queue) {
    if (Size - Output.size() < 68)
      break;
    ++Delivered;
    if (Work.Node) {
      NodeEvents.push_back(binderNodeCommands(*Work.Node, Output));
      continue;
    }
    const size_t At = Output.size();
    const bool Transaction =
        Work.Command == Received || Work.Command == Replied;
    Output.resize(At + (Transaction ? 68 : 4));
    llvm::support::endian::write32le(Output.data() + At, Work.Command);
    if (!Transaction)
      continue;
    auto I = Device.BinderBuffers.find(Work.Offset);
    if (I == Device.BinderBuffers.end())
      return unsupported(Result, "Binder queued buffer ownership was lost");
    auto Buffer =
        Memory->binderAddress(Device, Work.Offset, I->second->allocationSize());
    if (!Buffer)
      return unsupported(Result, "Binder queued buffer mapping was retired");
    auto *Record = Output.data() + At + 4;
    if (const auto &Node = I->second->Target) {
      llvm::support::endian::write64le(Record, Node->Pointer);
      llvm::support::endian::write64le(Record + 8, Node->Cookie);
    }
    llvm::support::endian::write32le(Record + 16, Work.Code);
    llvm::support::endian::write32le(Record + 20, Work.Flags);
    llvm::support::endian::write32le(Record + 24, Work.SenderPID);
    llvm::support::endian::write32le(Record + 28, UserID);
    llvm::support::endian::write64le(Record + 32, I->second->Size);
    llvm::support::endian::write64le(Record + 40,
                                     I->second->Objects.size() * 8);
    llvm::support::endian::write64le(Record + 48, *Buffer);
    llvm::support::endian::write64le(
        Record + 56, *Buffer + ((I->second->Size + 7) & ~uint64_t(7)));
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
  size_t NodeIndex = 0;
  while (Delivered--) {
    auto &Work = Queue.front();
    if (Work.Node)
      binderCommitNode(Work.Node, NodeEvents[NodeIndex++]);
    else if (Work.Command == Received) {
      Device.BinderIncoming.emplace(CurrentTask, Work.Call);
      Device.BinderBuffers.at(Work.Offset)->Delivered = true;
    } else if (Work.Command == Replied) {
      Device.BinderOutgoing.erase(CurrentTask);
      Device.BinderBuffers.at(Work.Offset)->Delivered = true;
    }
    Queue.pop_front();
  }
  return Outcome(Output.size());
}
} // namespace neverd::emulation::linux_model
