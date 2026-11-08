//===- LinuxDeviceState.h - Shared device open lifetime --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Descriptor aliases and VMAs retain the same device open description.
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXDEVICESTATE_H
#define NEVERD_EMULATION_OS_LINUX_LINUXDEVICESTATE_H
#include "neverd/emulation/LinuxDeviceOptions.h"
#include "neverd/emulation/MemoryView.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace neverd::emulation::linux_model {
struct LinuxDeviceState;
struct BinderNode {
  std::weak_ptr<LinuxDeviceState> Owner;
  uint64_t Pointer = 0, Cookie = 0;
  uint32_t InternalStrong = 0, References = 0;
  uint32_t LocalStrong = 0, LocalWeak = 0;
  bool HasStrong = false, HasWeak = false;
  bool PendingStrong = false, PendingWeak = false, Queued = false;
  std::optional<uint32_t> QueuedThread;
};
struct BinderReference {
  std::shared_ptr<BinderNode> Node;
  uint32_t Strong = 0, Weak = 0;
};
struct BinderCall {
  std::weak_ptr<LinuxDeviceState> Sender;
  uint32_t Thread = 0;
};
struct BinderBuffer {
  uint64_t Size = 0;
  bool Delivered = false;
  std::vector<uint64_t> Objects;
  // A remote object holds a strong handle; a returned local object holds a
  // local node reference. The transaction target has its own local reference.
  struct Object {
    std::shared_ptr<BinderNode> Node;
    std::optional<uint32_t> Handle;
  };
  std::vector<Object> References;
  std::shared_ptr<BinderNode> Target;
  uint64_t allocationSize() const {
    return std::max<uint64_t>(8,
                              ((Size + 7) & ~uint64_t(7)) + Objects.size() * 8);
  }
};
struct BinderBufferView {
  llvm::ArrayRef<uint8_t> Bytes;
  std::shared_ptr<const BinderBuffer> Lease;
};
struct BinderWork {
  uint32_t Command = 0, Code = 0, Flags = 0, SenderPID = 0;
  uint64_t Offset = 0;
  std::shared_ptr<BinderCall> Call;
  // Command zero is queued kernel node-reference work, not a wire command.
  std::shared_ptr<BinderNode> Node;
  // The driver's return_error slot stops command writes until it is read.
  // A failed reply uses this slot for the server's BR_TRANSACTION_COMPLETE;
  // the caller's separate reply_error slot does not stop its writes.
  bool ReturnError = false;
};
struct GPUAllocation {
  MemoryView Bytes;
  uint64_t Flags;
  uint32_t ID = 0; // Zero identifies a legacy user-address import.
  uint64_t UserAddress = 0;
  bool PendingFree = false;
};
struct KGSLProcess {
  std::map<uint64_t, GPUAllocation> Allocations;
  uint64_t NextAddress = 0x10000000;
};
struct LinuxDeviceState {
  explicit LinuxDeviceState(const LinuxDeviceOptions *Options)
      : Options(Options) {}
  ~LinuxDeviceState();
  LinuxDeviceState(const LinuxDeviceState &) = delete;
  const LinuxDeviceOptions *Options;
  size_t NextIoctl = 0;
  std::set<uint32_t> Contexts;
  std::map<uint32_t, uint32_t> QueuedTimestamps;
  std::shared_ptr<KGSLProcess> GPU;
  uint32_t OpenFlags = 0;
  std::map<uint32_t, uint32_t> BinderLoopers;
  int32_t BinderMaxThreads = 0;
  uint32_t BinderRequestedThreads = 0;
  uint32_t BinderStartedThreads = 0;
  bool BinderManager = false;
  LinuxDeviceMapping BinderMapping;
  std::map<uint64_t, std::shared_ptr<BinderBuffer>> BinderBuffers;
  std::map<uint64_t, std::shared_ptr<BinderNode>> BinderNodes;
  std::map<uint32_t, BinderReference> BinderHandles;
  llvm::DenseMap<const BinderNode *, uint32_t> BinderHandlesByNode;
  std::deque<BinderWork> BinderProcessWork;
  std::map<uint32_t, std::deque<BinderWork>> BinderThreadWork;
  std::map<uint32_t, std::shared_ptr<BinderCall>> BinderIncoming,
      BinderOutgoing;

  bool hasReadyBinderThreadWork(uint32_t Thread) const;

  bool hasPendingBinderWork() const {
    if (!BinderIncoming.empty() || !BinderOutgoing.empty() ||
        !BinderProcessWork.empty())
      return true;
    for (const auto &[Thread, Work] : BinderThreadWork)
      if (!Work.empty())
        return true;
    return false;
  }
};
} // namespace neverd::emulation::linux_model
#endif
