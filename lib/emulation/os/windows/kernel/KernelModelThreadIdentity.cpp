//===- KernelModelThreadIdentity.cpp - Thread identity and native fields --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Thread identity and native fields.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"
#include "KernelReferenceThread.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error identityError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Error KernelModel::publishThreadIdentity(uint64_t Object,
                                               ThreadIdentity Identity) {
  // Modeled System threads are born without a win32k association. Borrowed
  // user threads need an explicit observation; their zero-filled allocation
  // is not evidence that the original thread had no GUI state.
  if (!Identity.Win32Thread &&
      Identity.CreatingProcessID == uint32_t(windows::SystemProcessID))
    Identity.Win32Thread = 0;
  if (DebuggerReference && DebuggerReference->debugger()) {
    const auto &Facts = *DebuggerReference->debugger();
    auto Put = [&](uint32_t Offset, uint64_t Value) -> llvm::Error {
      return Offset ? Memory.writeInteger(Object + Offset, Value, 8)
                    : llvm::Error::success();
    };
    if (auto E = Put(Facts.ThreadIDOffset, Identity.ID))
      return E;
    if (Identity.Win32Thread)
      if (auto E = Put(Facts.ThreadWin32ThreadOffset, *Identity.Win32Thread))
        return E;
    if (Identity.StartAddress)
      if (auto E = Put(Facts.ThreadWin32StartOffset, *Identity.StartAddress))
        return E;
    if (Identity.CreatingProcessID) {
      if (auto E =
              Put(Facts.ThreadProcessIDOffset, *Identity.CreatingProcessID))
        return E;
      if (Facts.ThreadProcessOffset) {
        auto Process = processObject(*Identity.CreatingProcessID);
        if (!Process)
          return Process.takeError();
        if (auto E = Put(Facts.ThreadProcessOffset, *Process))
          return E;
      }
    }
  }
  if (auto E = initializeNativeThreadView(Object, Identity))
    return E;
  ThreadIdentities.emplace(Object, std::move(Identity));
  return llvm::Error::success();
}

bool KernelModel::isThreadIdentityRead(uint64_t Object, uint64_t Address,
                                       uint32_t Size) const {
  if (!DebuggerReference || !DebuggerReference->debugger() || Address < Object)
    return false;
  const auto &Facts = *DebuggerReference->debugger();
  const auto &Thread = ThreadIdentities.at(Object);
  if (isNativeThreadViewRead(Object, Address, Size))
    return true;
  // Each byte must belong to an authenticated field with a modeled value.
  // Adjacent CID members may be read together; gaps never become readable.
  for (uint64_t Byte = Address - Object; Byte < Address - Object + Size;
       ++Byte) {
    auto In = [&](uint32_t Offset) {
      return Offset && Byte >= Offset && Byte - Offset < 8;
    };
    if (In(Facts.ThreadIDOffset) ||
        (Thread.Win32Thread && In(Facts.ThreadWin32ThreadOffset)) ||
        (Thread.StartAddress && In(Facts.ThreadWin32StartOffset)) ||
        (Thread.CreatingProcessID &&
         (In(Facts.ThreadProcessIDOffset) || In(Facts.ThreadProcessOffset))))
      continue;
    return false;
  }
  return true;
}

llvm::Expected<uint64_t> KernelModel::threadProcessID(uint64_t Object) const {
  const auto Thread = ThreadObjects.find(Object);
  if (Thread == ThreadObjects.end())
    return identityError("thread process query requires a known thread object");
  if (!Thread->second.ProcessID)
    return identityError("thread has no modeled creating process identity");
  return uint64_t(Thread->second.ProcessID);
}

llvm::Expected<uint64_t> KernelModel::threadProcessObject(uint64_t Object) {
  auto ID = threadProcessID(Object);
  if (!ID)
    return ID.takeError();
  return processObject(uint32_t(*ID));
}

llvm::Expected<uint64_t> KernelModel::threadID(uint64_t Object) const {
  const auto Identity = ThreadIdentities.find(Object);
  if (Identity == ThreadIdentities.end())
    return identityError("PsGetThreadId requires a known thread object");
  return Identity->second.ID;
}

llvm::Expected<uint64_t> KernelModel::currentThreadID() {
  auto Object = currentThreadObject();
  if (!Object)
    return Object.takeError();
  return threadID(*Object);
}

llvm::Expected<uint64_t> KernelModel::threadWin32Thread(uint64_t Object) const {
  const auto Thread = ThreadIdentities.find(Object);
  if (Thread == ThreadIdentities.end())
    return identityError("Win32 association requires a known thread object");
  if (!Thread->second.Win32Thread)
    return identityError("borrowed thread Win32 association is unspecified");
  return *Thread->second.Win32Thread;
}

llvm::Expected<uint64_t> KernelModel::currentThreadWin32Thread() {
  auto Object = currentThreadObject();
  if (!Object)
    return Object.takeError();
  return threadWin32Thread(*Object);
}

llvm::Expected<uint64_t> KernelModel::setThreadWin32Thread(uint64_t Object,
                                                           uint64_t Value,
                                                           uint64_t Expected) {
  const auto Thread = ThreadIdentities.find(Object);
  if (Thread == ThreadIdentities.end())
    return identityError("Win32 association requires a known thread object");
  // Original PsSetThreadWin32Thread exchanges nonzero values and only clears
  // when the third argument matches. A conditional clear cannot resolve an
  // unknown borrowed association. The routine has no return-value contract.
  if (!Value) {
    if (!Thread->second.Win32Thread)
      return identityError("borrowed thread Win32 association is unspecified");
    if (*Thread->second.Win32Thread != Expected)
      return uint64_t(0);
  }
  if (DebuggerReference && DebuggerReference->debugger()) {
    const auto Offset = DebuggerReference->debugger()->ThreadWin32ThreadOffset;
    if (Offset)
      if (auto E = Memory.writeInteger(Object + Offset, Value, 8))
        return E;
  }
  Thread->second.Win32Thread = Value;
  return uint64_t(0);
}

llvm::Expected<uint64_t>
KernelModel::isThreadTerminating(uint64_t Object) const {
  return threadTerminating(Object);
}

} // namespace neverd::emulation
