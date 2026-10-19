//===- KernelModelObjects.cpp - Object references and handles ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelAPINames.h"
#include "KernelModel.h"
#include "KernelObjectTypes.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error objectError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Error KernelModel::validateObjectOutput(uint64_t Address, uint32_t Size) {
  if (auto E = validateGuestAccess(Address, Size, true))
    return E;
  auto Writable = Memory.canAccess(Address, Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return objectError(
        "object output requires writable guest memory (write permissions)");
  return Memory.validateBacking(Address, Size);
}

bool KernelModel::matchesObjectType(ObjectKind Kind, uint64_t Type) const {
  const auto Found = ObjectTypes.find(Kind);
  return !Type || (Found != ObjectTypes.end() && Found->second == Type);
}

llvm::Expected<uint64_t> KernelModel::objectType(uint64_t Object) {
  const auto Found = ReferencedObjects.find(Object);
  if (Found == ReferencedObjects.end())
    return objectError("object type requires a known object");
  return objectType(Found->second.Kind);
}

llvm::Expected<uint64_t> KernelModel::objectType(ObjectKind Kind) {
  {
    std::optional<object_type::Kind> Type;
    switch (Kind) {
    case ObjectKind::Process:
      Type = object_type::Kind::Process;
      break;
    case ObjectKind::Thread:
      Type = object_type::Kind::Thread;
      break;
    case ObjectKind::File:
      Type = object_type::Kind::File;
      break;
    case ObjectKind::Driver:
      Type = object_type::Kind::Driver;
      break;
    default:
      break;
    }
    if (Type) {
      const auto Value = object_type::identity(*Type);
      ObjectTypes.try_emplace(Kind, Value);
      return Value;
    }
  }

  if (const auto Type = ObjectTypes.find(Kind); Type != ObjectTypes.end())
    return Type->second;
  auto Type = allocate(profile::ProcessTokenSize);
  if (!Type)
    return Type.takeError();
  ObjectTypes.emplace(Kind, *Type);
  return *Type;
}

llvm::Expected<uint64_t> KernelModel::referenceObject(uint64_t Object) {
  if (auto E = ExecutiveCallbacks.find(Object); E != ExecutiveCallbacks.end()) {
    if (E->second.PointerReferences == SIZE_MAX)
      return objectError("callback reference overflow");
    ++E->second.PointerReferences;
    return uint64_t(0);
  }
  auto Found = ReferencedObjects.find(Object);
  if (Found == ReferencedObjects.end())
    return objectError("object reference requires a known live object");
  if (const auto File = FileObjects.find(Object);
      File != FileObjects.end() && File->second.State == FileState::Closing)
    return objectError("cannot reference a file object during final CLOSE");
  ++Found->second.Pointers;
  // ObReferenceObject is a void macro; its underlying export's result is
  // reserved. Do not expose synthetic object-manager header counts.
  return uint64_t(0);
}

llvm::Expected<uint64_t>
KernelModel::referenceObjectByPointer(llvm::ArrayRef<uint64_t> A) {
  const auto Found = ReferencedObjects.find(A[0]);
  if (Found == ReferencedObjects.end())
    return objectError("pointer reference requires a known live object");
  const uint8_t Mode = A[3];
  if (Mode != windows::KernelMode && Mode != windows::UserMode)
    return objectError("invalid object reference access mode");
  // The target kernel validates a supplied type in both modes. DesiredAccess
  // does not perform an ACL check for a pointer already held by the caller.
  if ((!A[2] && Mode == windows::UserMode) ||
      !matchesObjectType(Found->second.Kind, A[2]))
    return windows::StatusObjectTypeMismatch;
  auto Referenced = referenceObject(A[0]);
  if (!Referenced)
    return Referenced.takeError();
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::dereferenceObject(uint64_t Object) {
  if (ExecutiveCallbacks.contains(Object))
    return dereferenceExecutiveCallback(Object);
  auto Found = ReferencedObjects.find(Object);
  if (Found == ReferencedObjects.end() || !Found->second.Pointers)
    return objectError(
        Found != ReferencedObjects.end() &&
                Found->second.Kind == ObjectKind::Thread
            ? "ObfDereferenceObject requires a referenced object (referenced "
              "thread)"
        : Found != ReferencedObjects.end() &&
                Found->second.Kind == ObjectKind::Process
            ? "ObfDereferenceObject requires a referenced object (referenced "
              "process)"
            : "ObfDereferenceObject requires a referenced object");
  if (const auto File = FileObjects.find(Object);
      File != FileObjects.end() && File->second.Asynchronous &&
      File->second.InternalIOHolds && !Found->second.Handles &&
      Found->second.Pointers == 1)
    return objectError("asynchronous final query release requires deferred "
                       "file deletion");
  if (const auto File = FileObjects.find(Object);
      File != FileObjects.end() && owesFileClose(File->second) &&
      !File->second.HandleCloseHolds && !File->second.InternalIOHolds &&
      !Found->second.Handles && Found->second.Pointers == 1) {
    if (CurrentIRQL != scheduler::PassiveLevel)
      return objectError("final file reference release requires PASSIVE_LEVEL");
    if (!executionProcessContext())
      return objectError(
          "final file reference release requires a known process context");
    if (PendingWdmCall || PendingWait || NextIRPCall == UINT64_MAX)
      return objectError(
          "final file reference release cannot start its continuation");
  }
  --Found->second.Pointers;
  if (Found->second.Kind == ObjectKind::Device) {
    if (auto E = retireDeviceIfUnreferenced(Object))
      return E;
  } else if (Found->second.Kind == ObjectKind::File) {
    if (auto E = NativeFiles.contains(Object)
                     ? retireNativeFile(Object)
                     : retireFileIfUnreferenced(Object))
      return E;
  } else
    retireObjectIfUnreferenced(Object);
  return uint64_t(0);
}

llvm::Expected<uint64_t>
KernelModel::referenceObjectByHandle(llvm::ArrayRef<uint64_t> A) {
  if (FileHandles.contains(A[0]))
    return referenceFile(A);
  const auto Handle = ObjectHandles.find(A[0]);
  if (Handle == ObjectHandles.end())
    return windows::StatusInvalidHandle;
  const auto &State = Handle->second;
  const auto Found = ReferencedObjects.find(State.Object);
  if (Found == ReferencedObjects.end())
    return objectError("object handle lost its live object");
  const uint8_t Mode = A[3];
  if (Mode != windows::KernelMode && Mode != windows::UserMode)
    return objectError("invalid object reference access mode");
  const bool Kernel = State.Attributes & windows::ObjectKernelHandle;
  if ((Kernel && Mode == windows::UserMode) || !findObjectHandle(A[0]))
    return windows::StatusInvalidHandle;
  if (!matchesObjectType(Found->second.Kind, A[2]))
    return windows::StatusObjectTypeMismatch;
  const auto ValidAccess = objectAccessMask(Found->second.Kind);
  if (!ValidAccess)
    return objectError("unsupported object handle class");
  if (uint32_t(A[1]) & ~*ValidAccess)
    return objectError("unsupported object reference access mask");
  if (Mode == windows::UserMode && (uint32_t(A[1]) & ~State.GrantedAccess))
    return windows::StatusAccessDenied;
  if (!A[4])
    return windows::StatusInvalidParameter;
  if (auto E = validateObjectOutput(A[4], 8))
    return E;
  if (A[5])
    if (auto E =
            validateObjectOutput(A[5], windows::ObjectHandleInformationSize))
      return E;
  if (auto E = Memory.writeInteger(A[4], State.Object, 8))
    return E;
  if (A[5]) {
    // OBJECT_HANDLE_INFORMATION reports public OBJ_* attributes and the
    // granted mask, not the kernel/user handle-table selection bit.
    if (auto E = Memory.writeInteger(
            A[5], State.Attributes & ~windows::ObjectKernelHandle, 4))
      return E;
    if (auto E = Memory.writeInteger(A[5] + 4, State.GrantedAccess, 4))
      return E;
  }
  ++Found->second.Pointers;
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::openObjectByPointer(llvm::ArrayRef<uint64_t> A) {
  if (NativeFiles.contains(A[0]))
    return openFilePointer(A);
  const auto Object = ReferencedObjects.find(A[0]);
  if (Object == ReferencedObjects.end())
    return objectError("object open requires a known live object");
  const uint8_t Mode = A[5];
  if (Mode != windows::KernelMode && Mode != windows::UserMode)
    return objectError("invalid object open access mode");
  if ((!A[4] && Mode == windows::UserMode) ||
      !matchesObjectType(Object->second.Kind, A[4])) {
    if (!A[6])
      return windows::StatusInvalidParameter;
    if (auto E = validateObjectOutput(A[6], 8))
      return E;
    // The target clears PHANDLE before its typed pointer-reference check.
    if (auto E = Memory.writeInteger(A[6], 0, 8))
      return E;
    return windows::StatusObjectTypeMismatch;
  }
  if (!objectAccessMask(Object->second.Kind))
    return objectError("unsupported object handle class");
  if (Object->second.Kind == ObjectKind::File) {
    const auto File = FileObjects.find(A[0]);
    if (File == FileObjects.end() || (File->second.State != FileState::Open &&
                                      File->second.State != FileState::Cleaned))
      return objectError(
          "file pointer open requires an initial successful handle");
    if (File->second.HandleCloseHolds ||
        std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
          const auto &Request = Entry.second;
          return Request.FileAddress == A[0] &&
                 (Request.Kind == DriverRequestKind::Cleanup ||
                  Request.Kind == DriverRequestKind::UnlockAll);
        }))
      return objectError(
          "file pointer open during a handle-close callback is unsupported");
    auto Flags = Memory.readInteger(A[0] + windows::FileFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    if (!(*Flags & windows::FileHandleCreated))
      return objectError("file pointer open requires FO_HANDLE_CREATED");
  }
  // ACLs, access-state records and inheritance need a declared security
  // environment. Refuse those requests without granting invented authority.
  if (Mode != windows::KernelMode || A[2] ||
      (uint32_t(A[1]) & ~windows::ObjectKernelHandle))
    return objectError("object open requires kernel mode, no access state and "
                       "only OBJ_KERNEL_HANDLE attributes");
  auto Access = mapObjectAccess(Object->second.Kind, uint32_t(A[3]));
  if (!Access)
    return Access.takeError();
  const auto Context = executionProcessContext();
  const uint32_t ProcessID = Context ? Context->ProcessID : 0;
  if (ProcessID != windows::SystemProcessID &&
      !(uint32_t(A[1]) & windows::ObjectKernelHandle))
    return objectError("object open outside the system process requires "
                       "OBJ_KERNEL_HANDLE");
  if (!A[6])
    return windows::StatusInvalidParameter;
  if (auto E = validateObjectOutput(A[6], 8))
    return E;
  if (Object->second.RegistryOwner)
    if (auto E = Object->second.RegistryOwner->checkHandleCapacity())
      return E;
  if (NextObjectHandle > profile::SystemThreadHandleLimit)
    return windows::StatusInsufficientResources;
  if (auto E = Memory.writeInteger(A[6], NextObjectHandle, 8))
    return E;
  retainObjectHandle(A[0], *Access, uint32_t(A[1]), ProcessID);
  return windows::StatusSuccess;
}

void KernelModel::retainObjectHandle(uint64_t Object, uint32_t Access,
                                     uint32_t Attributes, uint32_t ProcessID) {
  const uint64_t Handle = NextObjectHandle;
  NextObjectHandle += profile::SystemThreadHandleStride;
  retainReservedObjectHandle(Handle, Object, Access, Attributes, ProcessID);
}

void KernelModel::retainReservedObjectHandle(uint64_t Handle, uint64_t Object,
                                             uint32_t Access,
                                             uint32_t Attributes,
                                             uint32_t ProcessID) {
  if (Attributes & windows::ObjectKernelHandle)
    ProcessID = windows::SystemProcessID;
  ObjectHandles.emplace(Handle,
                        ObjectHandle{Object, Access, Attributes, ProcessID});
  ++ReferencedObjects.at(Object).Handles;
}

llvm::Expected<uint64_t> KernelModel::closeHandle(uint64_t Handle) {
  if (FileHandles.contains(Handle))
    return closeFile(Handle);
  if (auto Section = SectionHandles.find(Handle);
      Section != SectionHandles.end()) {
    if (!canAccessHandle(Section->second.Attributes, Section->second.ProcessID))
      return windows::StatusInvalidHandle;
    SectionHandles.erase(Section);
    return uint64_t(0);
  }
  const auto Found = ObjectHandles.find(Handle);
  if (Found == ObjectHandles.end())
    return windows::StatusInvalidHandle;
  if (!findObjectHandle(Handle))
    return windows::StatusInvalidHandle;
  const uint64_t Object = Found->second.Object;
  auto Reference = ReferencedObjects.find(Object);
  if (Reference == ReferencedObjects.end() || !Reference->second.Handles)
    return objectError("object handle lost its reference");
  if (Reference->second.Kind == ObjectKind::File) {
    if (auto E = closeFileHandle(Handle))
      return E;
    return windows::StatusSuccess;
  }
  --Reference->second.Handles;
  ObjectHandles.erase(Found);
  retireObjectIfUnreferenced(Object);
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::lookupProcess(uint64_t ID,
                                                    uint64_t Output) {
  const bool Known =
      Version ? (ID && ID <= UINT32_MAX && isLiveProcess(uint32_t(ID)))
              : ID && ID <= UINT32_MAX &&
                    (ID == windows::SystemProcessID ||
                     ProcessObjects.contains(uint32_t(ID)) ||
                     RequestorProcessIDs.contains(uint32_t(ID)) ||
                     (UserRequestContext && ID == CurrentUserProcessID));
  if (!Known || ((Version || LifecycleParents.contains(uint32_t(ID))) &&
                 ExitedUserProcesses.contains(uint32_t(ID))))
    return windows::StatusInvalidCID;
  if (!Output)
    return windows::StatusInvalidParameter;
  if (auto E = validateObjectOutput(Output, 8))
    return E;
  auto Object = processObject(uint32_t(ID));
  if (!Object)
    return Object.takeError();
  if (auto E = Memory.writeInteger(Output, *Object, 8))
    return E;
  ++ReferencedObjects.at(*Object).Pointers;
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::lookupThread(uint64_t ID,
                                                   uint64_t Output) {
  if (CurrentExecution && CurrentThreadKey) {
    auto Current = currentThreadObject();
    if (!Current)
      return Current.takeError();
    auto CID = threadClientID(*Current);
    if (!CID)
      return CID.takeError();
  }
  const auto Found =
      std::find_if(ThreadClientIDs.begin(), ThreadClientIDs.end(),
                   [&](const auto &Entry) { return Entry.second == ID; });
  if (Found == ThreadClientIDs.end() ||
      ((Version || ExitedUserProcesses.contains(
                       ThreadObjects.at(Found->first).ProcessID)) &&
       ThreadObjects.at(Found->first).Exited))
    return windows::StatusInvalidCID;
  if (!Output)
    return windows::StatusInvalidParameter;
  if (auto E = validateObjectOutput(Output, 8))
    return E;
  if (auto E = Memory.writeInteger(Output, Found->first, 8))
    return E;
  ++ReferencedObjects.at(Found->first).Pointers;
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::threadProcess(uint64_t Object) {
  const auto Thread = ThreadObjects.find(Object);
  if (Thread == ThreadObjects.end())
    return objectError("thread process requires a known thread object");
  if (!Thread->second.ProcessID)
    return objectError("thread has no modeled creating process identity");
  return processObject(Thread->second.ProcessID);
}

llvm::Expected<uint64_t> KernelModel::threadTerminating(uint64_t Object) const {
  const auto Thread = ThreadObjects.find(Object);
  if (Thread == ThreadObjects.end())
    return objectError(
        "thread termination query requires a known thread object");
  return uint64_t(Thread->second.Terminating || Thread->second.Exited);
}

void KernelModel::retireObjectIfUnreferenced(uint64_t Object) {
  const auto References = ReferencedObjects.find(Object);
  if (References == ReferencedObjects.end())
    return;
  // Making a permanent link temporary removes its name only at last handle
  // close. Pointer references keep the unnamed object and its target alive.
  if (auto Link = SymbolicLinks.find(Object); Link != SymbolicLinks.end()) {
    if (!Link->second.Permanent && !References->second.Handles)
      Link->second.Named = false;
    if (!Link->second.Named && !References->second.Pointers &&
        !References->second.Handles) {
      FreedRanges.emplace(Object, profile::ProcessTokenSize);
      SymbolicLinks.erase(Link);
      ReferencedObjects.erase(References);
    }
    return;
  }
  if (References->second.Pointers || References->second.Handles ||
      WaitReferences.contains(Object))
    return;
  if (References->second.RegistryOwner) {
    References->second.RegistryOwner->retireObject(Object);
    FreedRanges.emplace(Object, profile::ProcessTokenSize);
    ReferencedObjects.erase(References);
    return;
  }
  // Process and driver identities are retained by the session environment.
  if (References->second.Kind != ObjectKind::Thread)
    return;
  const auto Thread = ThreadObjects.find(Object);
  if (Thread == ThreadObjects.end() || !Thread->second.Exited)
    return;
  FreedRanges.emplace(Object,
                      Version ? ThreadObjectSize : profile::ProcessTokenSize);
  if (Thread->second.System)
    Scheduler.forgetThreadPriority(Thread->second.CallbackID);
  ThreadIdentities.erase(Object);
  NativeInitialThreadViews.erase(Object);
  ThreadClientIDs.erase(Object);
  ThreadObjects.erase(Thread);
  ReferencedObjects.erase(References);
}

void KernelModel::retireBorrowedThread(uint64_t Key) {
  ApcStates.erase(Key);
  SystemAffinityThreads.erase(Key);
  if (const auto Current = CurrentThreadObjects.find(Key);
      Current != CurrentThreadObjects.end()) {
    const uint64_t Object = Current->second;
    auto &Thread = ThreadObjects.at(Object);
    Thread.Exited = true;
    Thread.ExitPriority = Scheduler.threadPriority(Key);
    CurrentThreadObjects.erase(Current);
    retireObjectIfUnreferenced(Object);
  }
  // A reused stack key starts a new thread. Retained old objects use their
  // captured exit priority and cannot affect the new thread's scheduler state.
  Scheduler.forgetThreadPriority(Key);
}
} // namespace neverd::emulation
