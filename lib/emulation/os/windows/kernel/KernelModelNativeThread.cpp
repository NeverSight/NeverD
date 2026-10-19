//===- KernelModelNativeThread.cpp - Initial thread construction view -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Initial thread construction view.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"
#include "KernelReferenceThread.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Error
KernelModel::initializeNativeThreadView(uint64_t Object,
                                        const ThreadIdentity &Identity) {
  // One initial System foreground thread, before virtual time or asynchronous
  // scheduling starts. Other lifetimes retain their authenticated scalar view.
  if (!DebuggerReference || !hasKernelThreadConstruction(*DebuggerReference) ||
      !Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041 || CurrentThreadKey != profile::StackBase ||
      UserRequestContext || !ProcessAttachments.empty() || InstructionClock ||
      Scheduler.now100ns() || !Identity.StartAddress ||
      Identity.CreatingProcessID != uint32_t(windows::SystemProcessID) ||
      Scheduler.threadPriority(CurrentThreadKey) != 8 ||
      std::any_of(ThreadObjects.begin(), ThreadObjects.end(),
                  [](const auto &E) { return E.second.System; }) ||
      std::any_of(NativeInitialThreadViews.begin(),
                  NativeInitialThreadViews.end(),
                  [](const auto &Entry) { return Entry.second.Size != 0; }))
    return llvm::Error::success();
  auto Process = processObject(windows::SystemProcessID);
  if (!Process)
    return Process.takeError();
  // Reserve private OS identities so any pointer in the original body denotes
  // resident model-owned storage. Its contents never acquire guest visibility.
  constexpr auto Reserve =
      KernelReferenceThreadInput::AuxiliaryBytes + profile::PageSize;
  auto Auxiliary = allocate(Reserve);
  if (!Auxiliary)
    return Auxiliary.takeError();
  const auto Base =
      (*Auxiliary + profile::PageSize - 1) & ~uint64_t(profile::PageSize - 1);
  auto Constructed = constructKernelReferenceThread(
      *DebuggerReference,
      {Object, *Process, Identity.ID, *Identity.StartAddress,
       profile::StackBase, profile::StackSize, Base});
  if (!Constructed)
    return Constructed.takeError();
  // The session already owns this logical foreground CPU context. Consume the
  // validated dispatch handoff by installing its original initialized body;
  // no original caller return, native stack switch or host service is claimed.
  if (auto E = Memory.write(Object, Constructed->Body))
    return E;
  if (auto E = Memory.write(
          *Process + KernelReferenceThreadInput::ProcessThreadListHeadOffset,
          Constructed->ProcessThreadListHead))
    return E;
  NativeInitialThreadViews.insert_or_assign(
      Object, NativeInitialThreadView{*Auxiliary, Reserve});
  return llvm::Error::success();
}

bool KernelModel::isNativeThreadViewRead(uint64_t Object, uint64_t Address,
                                         uint32_t Size) const {
  const auto View = NativeInitialThreadViews.find(Object);
  if (View == NativeInitialThreadViews.end() || !View->second.Readable ||
      Address < Object || Size > ThreadObjectSize ||
      Address - Object > ThreadObjectSize - Size ||
      CurrentThreadKey != profile::StackBase || UserRequestContext ||
      !ProcessAttachments.empty() || InstructionClock || Scheduler.now100ns() ||
      std::any_of(ThreadObjects.begin(), ThreadObjects.end(),
                  [](const auto &E) { return E.second.System; }) ||
      Scheduler.threadPriority(CurrentThreadKey) != 8 ||
      ExitedUserProcesses.contains(windows::SystemProcessID))
    return false;
  if (const auto APC = ApcStates.find(CurrentThreadKey);
      APC != ApcStates.end() &&
      (APC->second.CriticalDepth || APC->second.GuardedDepth))
    return false;
  if (Dispatcher.ownsMutex(CurrentThreadKey))
    return false;
  const auto Current = CurrentThreadObjects.find(profile::StackBase);
  return Current != CurrentThreadObjects.end() && Current->second == Object;
}
} // namespace neverd::emulation
