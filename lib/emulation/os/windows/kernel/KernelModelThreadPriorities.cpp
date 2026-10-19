//===- KernelModelThreadPriorities.cpp - Thread priority objects ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "KernelThreadPriorities.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {

uint64_t
KernelModel::resolvedThreadPriorityKey(uint64_t Key,
                                       const ExecutionContext *Paused) const {
  if (Version && Key == profile::StackBase) {
    const auto C = executionProcessContext();
    const auto PID =
        Paused ? uint32_t(Paused->UserMemory ? Paused->Process
                                             : windows::SystemProcessID)
        : C    ? C->CreatingProcessID
               : uint32_t(windows::SystemProcessID);
    for (const auto &[Object, T] : ThreadObjects)
      if (!T.System && !T.Exited && T.CallbackID == Key && T.ProcessID == PID)
        return Object;
  }
  return Key;
}

llvm::Expected<uint64_t> KernelModel::threadPriorityKey(uint64_t Object,
                                                        bool Changing) const {
  if (const auto Thread = ThreadObjects.find(Object);
      Thread != ThreadObjects.end()) {
    if (Changing && (Thread->second.Terminating || Thread->second.Exited))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     thread_priority::ExitedThread);
    return Version && !Thread->second.System &&
                   Thread->second.CallbackID == profile::StackBase
               ? Object
               : Thread->second.CallbackID;
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 thread_priority::UnknownThread);
}

llvm::Expected<uint64_t>
KernelModel::queryThreadPriority(uint64_t Object) const {
  if (const auto Thread = ThreadObjects.find(Object);
      Thread != ThreadObjects.end() && Thread->second.Exited)
    return uint64_t(Thread->second.ExitPriority);
  auto Key = threadPriorityKey(Object, false);
  if (!Key)
    return Key.takeError();
  return uint64_t(Scheduler.threadPriority(*Key));
}

llvm::Expected<uint64_t> KernelModel::setThreadPriority(uint64_t Object,
                                                        int32_t Priority) {
  auto Key = threadPriorityKey(Object, true);
  if (!Key)
    return Key.takeError();
  auto Previous = Scheduler.setThreadPriority(*Key, Priority);
  if (!Previous)
    return Previous.takeError();
  if (*Previous != Priority)
    if (auto View = NativeInitialThreadViews.find(Object);
        View != NativeInitialThreadViews.end())
      View->second.Readable = false;
  return uint64_t(*Previous);
}

} // namespace neverd::emulation
