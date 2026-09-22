//===- KernelFramework.cpp - KMDF binding and object lifetimes ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original guest model of the public KMDF 1.33 ABI. Bind layouts and function
/// indices follow Microsoft's fxldr.h, wdfglobals.h and wdffuncenum.h. Object
/// lifetime follows the documented cleanup/reference/destroy contract:
/// https://learn.microsoft.com/windows-hardware/drivers/wdf/framework-object-life-cycle
/// The model never overlays host WDF records or executes host callback
/// pointers.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

#include "WindowsKernelLayout.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace neverd::emulation {
namespace {
using namespace framework;
#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF: " + Message);
}

template <typename... Errors> llvm::Error joinedErrors(Errors... Values) {
  llvm::Error Result = llvm::Error::success();
  ((Result = llvm::joinErrors(std::move(Result), std::move(Values))), ...);
  return Result;
}

bool overlaps(uint64_t A, uint64_t Size, uint64_t B, uint64_t OtherSize) {
  return A < B + OtherSize && B < A + Size;
}
} // namespace

void KernelFramework::configure(uint64_t NewDriver, uint64_t NewRegistryPath,
                                std::string NewServiceName) {
  Driver = NewDriver;
  RegistryPath = NewRegistryPath;
  ServiceName = std::move(NewServiceName);
}

std::optional<unsigned>
KernelFramework::argumentCount(const KernelExportRegistry::Export &Export) {
  if (Export.Kind == KernelExportRegistry::ExportKind::FrameworkFunction) {
    if (Export.Name == FrameworkUnloadRoutine)
      return 1;
#define NEVERD_FRAMEWORK_API(Symbol, Count)                                    \
  if (Export.Name == #Symbol)                                                  \
    return Count;
#include "KernelFrameworkAPIs.def"
#undef NEVERD_FRAMEWORK_API
    return std::nullopt;
  }
  if (Export.Module == FrameworkLoaderProvider) {
#define NEVERD_FRAMEWORK_LOADER_API(Symbol, Count)                             \
  if (Export.Name == #Symbol)                                                  \
    return Count;
#include "KernelFrameworkLoaderAPIs.def"
#undef NEVERD_FRAMEWORK_LOADER_API
  }
  return std::nullopt;
}

llvm::Expected<uint64_t> KernelFramework::read(uint64_t Address,
                                               unsigned Width) {
  if (auto E = ValidateAccess(Address, Width, false))
    return E;
  return Memory.readInteger(Address, Width);
}

llvm::Error KernelFramework::writable(uint64_t Address, uint32_t Size) {
  if (!Address)
    return invalid("null output pointer");
  return ValidateAccess(Address, Size, true);
}

llvm::Expected<std::vector<uint8_t>>
KernelFramework::readRegistryPath(uint64_t Address) {
  auto Length = read(Address, 2);
  auto Maximum = read(Address + 2, 2);
  auto Buffer = read(Address + 8);
  if (!Length || !Maximum || !Buffer)
    return joinedErrors(Length.takeError(), Maximum.takeError(),
                        Buffer.takeError());
  if (!*Length || *Length % 2 || *Maximum < *Length ||
      *Length > MaxRegistryPathBytes)
    return invalid("invalid counted driver registry path");
  if (auto E = ValidateAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (auto E = Memory.read(*Buffer, Bytes))
    return E;
  return Bytes;
}

llvm::Expected<uint64_t> KernelFramework::allocate(uint64_t Size, bool Writable,
                                                   bool Opaque) {
  auto Address = AllocateStorage(Size);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return invalid("framework storage budget exhausted");
  if (auto E = Memory.write(*Address, std::vector<uint8_t>(Size)))
    return E;
  Regions.emplace(*Address, Region{Size, Writable, Opaque});
  return *Address;
}

llvm::Error KernelFramework::retire(uint64_t Address) {
  if (!Address)
    return llvm::Error::success();
  auto I = Regions.find(Address);
  if (I == Regions.end() || I->second.Freed)
    return invalid("lost framework allocation ownership");
  if (auto E = ReleaseStorage(Address, I->second.Size))
    return E;
  I->second.Freed = true;
  return llvm::Error::success();
}

llvm::Error KernelFramework::validateGuestAccess(uint64_t Address,
                                                 uint32_t Size,
                                                 bool IsWrite) const {
  if (Size > UINT64_MAX - Address)
    return invalid("overflowing framework memory access");
  for (const auto &[Base, Region] : Regions) {
    if (!Size || !overlaps(Address, Size, Base, Region.Size))
      continue;
    if (Region.Freed)
      return invalid("guest access to freed framework storage");
    if (Region.Opaque || (IsWrite && !Region.Writable))
      return invalid("guest access to opaque or read-only framework storage");
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelFramework::bind(llvm::ArrayRef<uint64_t> A) {
  if (A[0] != Driver)
    return invalid("binding requires this DriverEntry's driver object");
  auto Path = readRegistryPath(A[1]);
  auto EntryPath = readRegistryPath(RegistryPath);
  if (!Path || !EntryPath)
    return joinedErrors(Path.takeError(), EntryPath.takeError());
  if (*Path != *EntryPath)
    return invalid("binding registry path does not identify this driver");
  for (const auto &[Address, B] : Bindings)
    if (!B.Unbound)
      return invalid("driver already has a live framework binding");
  auto Size = read(A[2], 4);
  if (!Size)
    return Size.takeError();
  if (*Size != BindV1Size && *Size != BindV2Size)
    return invalid("unsupported WDF_BIND_INFO size");
  if (auto E = ValidateAccess(A[2], *Size, false))
    return E;
  auto Component = read(A[2] + BindComponent);
  auto Major = read(A[2] + BindMajor, 4);
  auto Minor = read(A[2] + BindMinor, 4);
  auto Build = read(A[2] + BindBuild, 4);
  auto Count = read(A[2] + BindCount, 4);
  auto TableSlot = read(A[2] + BindTable);
  auto Module = read(A[2] + BindModule);
  if (!Component || !Major || !Minor || !Build || !Count || !TableSlot ||
      !Module)
    return joinedErrors(Component.takeError(), Major.takeError(),
                        Minor.takeError(), Build.takeError(), Count.takeError(),
                        TableSlot.takeError(), Module.takeError());
  if (*Major != MajorVersion || *Minor != MinorVersion || *Build ||
      *Count != FunctionCount)
    return invalid(
        "unsupported framework version or function count; expected 1.33.0/458");
  if (*Module)
    return invalid("binding record already contains a module identity");
  for (size_t I = 0; I <= FrameworkComponent.size(); ++I) {
    auto Ch = read(*Component + 2 * I, 2);
    if (!Ch)
      return Ch.takeError();
    const uint64_t Expected =
        I == FrameworkComponent.size() ? 0 : uint8_t(FrameworkComponent[I]);
    if (*Ch != Expected)
      return invalid("unsupported framework component name");
  }
  uint64_t Higher = 0;
  if (*Size == BindV2Size) {
    auto MinimumSlot = read(A[2] + BindMinimum);
    auto HigherSlot = read(A[2] + BindHigher);
    if (!MinimumSlot || !HigherSlot)
      return joinedErrors(MinimumSlot.takeError(), HigherSlot.takeError());
    auto Minimum = read(*MinimumSlot, 4);
    if (!Minimum)
      return Minimum.takeError();
    if (*Minimum < 25 || *Minimum > MinorVersion)
      return invalid("unsupported minimum framework version");
    Higher = *HigherSlot;
    if (auto E = writable(Higher, 1))
      return E;
    // Count/structure globals are consulted only when the client is newer
    // than its provider. This profile provides the exact target version;
    // keep the client's already initialized unused availability data intact.
  }
  std::vector<std::pair<uint64_t, uint32_t>> Outputs{
      {A[3], 8}, {*TableSlot, 8}, {A[2] + BindModule, 8}};
  if (Higher)
    Outputs.emplace_back(Higher, 1);
  for (size_t I = 0; I < Outputs.size(); ++I) {
    const auto [Slot, Width] = Outputs[I];
    if (auto E = writable(Slot, Width))
      return E;
    if (Slot != A[2] + BindModule && overlaps(Slot, Width, A[2], *Size))
      return invalid("binding output overlaps its input record");
    for (size_t J = 0; J < I; ++J)
      if (overlaps(Slot, Width, Outputs[J].first, Outputs[J].second))
        return invalid("binding output slots overlap");
  }
  // Imports have already been bound. Reserve enough identities for the exact
  // function table and its eventual unload bridge before allocating anything.
  if (Exports.availableThunkCount() < FunctionCount + 1)
    return invalid("binding exceeds the remaining framework thunk capacity");
  auto Globals = allocate(GlobalsSize, false, false);
  if (!Globals)
    return Globals.takeError();
  auto Table = allocate(FunctionCount * 8, false, false);
  if (!Table)
    return joinedErrors(Table.takeError(), retire(*Globals));
  auto ModuleToken = allocate(HandleSize, false, true);
  if (!ModuleToken)
    return joinedErrors(ModuleToken.takeError(), retire(*Globals),
                        retire(*Table));
  struct Function {
    const char *Name;
    unsigned Index;
  };
  static constexpr Function Functions[] = {
#define NEVERD_FRAMEWORK_FUNCTION(Name, Index) {#Name, Index},
#include "KernelFrameworkFunctions.def"
#undef NEVERD_FRAMEWORK_FUNCTION
  };
  static_assert(std::size(Functions) == FunctionCount);
  static_assert(
      [] {
        for (size_t I = 0; I < std::size(Functions); ++I)
          if (Functions[I].Index != I)
            return false;
        return true;
      }(),
      "framework function inventory must cover every slot exactly once");
  for (const auto &Function : Functions) {
    auto Thunk = Exports.insertFrameworkFunction(*Globals, Function.Name);
    if (!Thunk)
      return Thunk.takeError();
    if (auto E = Memory.writeInteger(*Table + Function.Index * 8, *Thunk, 8))
      return E;
  }
  if (auto E = Memory.writeInteger(*TableSlot, *Table, 8))
    return E;
  if (auto E = Memory.writeInteger(A[3], *Globals, 8))
    return E;
  if (auto E = Memory.writeInteger(A[2] + BindModule, *ModuleToken, 8))
    return E;
  if (Higher)
    if (auto E = Memory.writeInteger(Higher, 0, 1))
      return E;
  Binding B;
  B.Info = A[2];
  B.Globals = *Globals;
  B.Table = *Table;
  B.Module = *ModuleToken;
  B.RegistryBytes = std::move(*Path);
  Bindings.emplace(*Globals, std::move(B));
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::unbind(llvm::ArrayRef<uint64_t> A) {
  auto I = Bindings.find(A[2]);
  if (I == Bindings.end() || I->second.Unbound || I->second.Info != A[1])
    return invalid("unbind requires the exact live binding and globals");
  auto &B = I->second;
  auto Path = readRegistryPath(A[0]);
  if (!Path)
    return Path.takeError();
  if (*Path != B.RegistryBytes)
    return invalid("unbind registry path does not identify this driver");
  if (B.Unbinding)
    return invalid("framework binding unregistration is already in progress");
  if (B.DriverHandle && !B.Unloaded) {
    // FxLibraryCommonUnregisterClient deletes a surviving driver directly.
    // This is the DriverEntry failure path: EvtDriverUnload is never invoked.
    std::vector<Step> Steps;
    if (auto E = planDelete(B.DriverHandle, Steps))
      return E;
    B.Unbinding = true;
    Steps.push_back({StepKind::FinishBindingUnbind, B.Globals});
    return start(std::move(Steps));
  }
  if (auto E = finishUnbind(B))
    return E;
  return 0;
}

llvm::Error KernelFramework::finishUnbind(Binding &B) {
  for (const auto &[Address, Init] : DeviceInits)
    if (Init.Binding == B.Globals)
      return invalid("unbind still owns an unconsumed device initializer");
  if (std::any_of(Objects.begin(), Objects.end(), [&](const auto &Entry) {
        return Entry.second.Binding == B.Globals;
      }))
    return invalid("unbind still owns live framework objects or references");
  for (uint64_t Address : {B.Globals, B.Table, B.Module, B.RegistryCopy})
    if (auto E = retire(Address))
      return E;
  B.Unbound = true;
  B.Unbinding = false;
  return llvm::Error::success();
}

llvm::Expected<KernelFramework::AttributeResult>
KernelFramework::attributes(uint64_t Address, AttributesUse Use) {
  // Match FxValidateObjectAttributes and wdfstatus.h. A documented guest
  // NTSTATUS is separate from an unsupported profile or invalid memory access.
  Attributes A;
  if (!Address) {
    if (Use == AttributesUse::AdditionalContext)
      return uint32_t(ParentNotSpecified);
    return A;
  }
  auto Size = read(Address, 4);
  if (!Size)
    return Size.takeError();
  if (*Size != AttributesSize)
    return uint32_t(InfoLengthMismatch);
  if (auto E = ValidateAccess(Address, AttributesSize, false))
    return E;
  auto Cleanup = read(Address + AttributesCleanup);
  auto Destroy = read(Address + AttributesDestroy);
  auto Execution = read(Address + AttributesExecution, 4);
  auto Synchronization = read(Address + AttributesSynchronization, 4);
  auto Parent = read(Address + AttributesParent);
  auto Override = read(Address + AttributesContextSize);
  auto Type = read(Address + AttributesContextType);
  if (!Cleanup || !Destroy || !Execution || !Synchronization || !Parent ||
      !Override || !Type)
    return joinedErrors(Cleanup.takeError(), Destroy.takeError(),
                        Execution.takeError(), Synchronization.takeError(),
                        Parent.takeError(), Override.takeError(),
                        Type.takeError());
  A.Cleanup = *Cleanup;
  A.Destroy = *Destroy;
  A.Parent = *Parent;
  if (*Type) {
    auto TypeSize = read(*Type, 4);
    if (!TypeSize)
      return TypeSize.takeError();
    if (*TypeSize != ContextTypeSize && *TypeSize != ContextTypeLegacySize)
      return uint32_t(InfoLengthMismatch);
    if (auto E = ValidateAccess(*Type, *TypeSize, false))
      return E;
    auto Name = read(*Type + ContextName);
    auto Bytes = read(*Type + ContextSize);
    if (!Name || !Bytes)
      return joinedErrors(Name.takeError(), Bytes.takeError());
    if (*Bytes && !*Name)
      return uint32_t(ObjectAttributesInvalid);
    A.Type = *Type;
    A.ContextSize = *Bytes;
  }
  if (*Override) {
    if (!*Type || *Override < A.ContextSize)
      return uint32_t(ObjectAttributesInvalid);
    A.ContextSize = *Override;
  }
  if (Use != AttributesUse::Object && *Parent)
    return uint32_t(ParentAssignmentNotAllowed);
  if (!*Execution || *Execution > ExecutionDispatch || !*Synchronization ||
      *Synchronization > SynchronizationNone)
    return uint32_t(ObjectAttributesInvalid);
  if (*Execution != ExecutionInherit && *Execution != ExecutionPassive)
    return invalid("unsupported framework object execution level");
  if (*Synchronization != SynchronizationInherit &&
      *Synchronization != SynchronizationNone)
    return invalid("unsupported framework object synchronization scope");
  if (A.ContextSize > MaxContextSize)
    return invalid("framework context exceeds the storage limit");
  return A;
}

llvm::Expected<uint64_t> KernelFramework::addContext(Object &O,
                                                     const Attributes &A) {
  if (auto I = O.Contexts.find(A.Type); I != O.Contexts.end())
    return I->second.Address;
  if (O.Contexts.size() >= MaxContextsPerObject)
    return invalid("framework context count exceeds the limit");
  uint64_t Storage = 0;
  if (A.ContextSize) {
    auto Allocation = allocate(A.ContextSize, true, false);
    if (!Allocation)
      return Allocation.takeError();
    Storage = *Allocation;
  } else if (A.Type) {
    // WDF permits typed zero-byte contexts and still returns a context address.
    // Preserve that identity without exposing any readable or writable byte.
    auto Identity = allocate(1, false, true);
    if (!Identity)
      return Identity.takeError();
    Storage = *Identity;
  }
  O.Contexts.emplace(A.Type,
                     Context{Storage, A.ContextSize, A.Cleanup, A.Destroy});
  O.ContextOrder.push_back(A.Type);
  return Storage;
}

llvm::Expected<uint64_t> KernelFramework::createObject(uint64_t Globals,
                                                       const Attributes &A,
                                                       bool IsDriver) {
  if (Objects.size() >= MaxObjects)
    return invalid("framework object limit exhausted");
  const uint64_t Parent = IsDriver   ? 0
                          : A.Parent ? A.Parent
                                     : Bindings.at(Globals).DriverHandle;
  if (!IsDriver) {
    auto I = Objects.find(Parent);
    if (I == Objects.end() || I->second.Binding != Globals ||
        I->second.Deleting)
      return invalid("object parent is not a live object in this binding");
  }
  auto Handle = allocate(HandleSize, false, true);
  if (!Handle)
    return Handle.takeError();
  Object O;
  O.Binding = Globals;
  O.Parent = Parent;
  O.Kind = IsDriver ? ObjectKind::Driver : ObjectKind::Generic;
  auto Context = addContext(O, A);
  if (!Context)
    return llvm::joinErrors(Context.takeError(), retire(*Handle));
  Objects.emplace(*Handle, std::move(O));
  if (Parent)
    Objects.at(Parent).Children.push_back(*Handle);
  return *Handle;
}

llvm::Expected<uint64_t>
KernelFramework::createDriver(Binding &B, llvm::ArrayRef<uint64_t> A) {
  if (A[1] != Driver)
    return invalid("WdfDriverCreate does not match the bound driver");
  auto SuppliedPath = readRegistryPath(A[2]);
  if (!SuppliedPath)
    return SuppliedPath.takeError();
  if (*SuppliedPath != B.RegistryBytes)
    return invalid("WdfDriverCreate registry path does not match its binding");
  auto Size = read(A[4], 4);
  if (!Size)
    return Size.takeError();
  if (*Size != DriverConfigSize)
    return InfoLengthMismatch;
  auto AddDevice = read(A[4] + DriverConfigAddDevice);
  auto Unload = read(A[4] + DriverConfigUnload);
  auto Flags = read(A[4] + DriverConfigFlags, 4);
  auto Tag = read(A[4] + DriverConfigTag, 4);
  if (!AddDevice || !Unload || !Flags || !Tag)
    return joinedErrors(AddDevice.takeError(), Unload.takeError(),
                        Flags.takeError(), Tag.takeError());
  if ((*Flags & DriverNonPnp) && *AddDevice)
    return InvalidParameter;
  if (*Flags != DriverNonPnp || !*Unload)
    return invalid("this framework lifecycle requires a non-PnP driver with "
                   "EvtDriverUnload");
  if (B.DriverHandle)
    return DriverInternalError;
  auto Validation = attributes(A[3], AttributesUse::Driver);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return *Status;
  const auto &Attrs = std::get<Attributes>(*Validation);
  if (A[5])
    if (auto E = writable(A[5], 8))
      return E;
  auto Handle = createObject(B.Globals, Attrs, true);
  if (!Handle)
    return Handle.takeError();
  auto Thunk =
      Exports.insertFrameworkFunction(B.Globals, FrameworkUnloadRoutine);
  if (!Thunk)
    return Thunk.takeError();
  auto Length = read(RegistryPath, 2);
  auto Buffer = read(RegistryPath + 8);
  if (!Length || !Buffer)
    return joinedErrors(Length.takeError(), Buffer.takeError());
  if (*Length % 2 || *Length > MaxRegistryPathBytes)
    return invalid("invalid counted driver registry path");
  if (auto E = ValidateAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Path(*Length + 2);
  if (auto E =
          Memory.read(*Buffer, llvm::MutableArrayRef(Path).take_front(*Length)))
    return E;
  auto Copy = allocate(Path.size(), false, false);
  if (!Copy)
    return Copy.takeError();
  if (auto E = Memory.write(*Copy, Path))
    return E;
  std::vector<uint8_t> Name(GlobalsNameSize);
  std::copy_n(ServiceName.begin(),
              std::min(ServiceName.size(), Name.size() - 1), Name.begin());
  uint32_t PoolTag = *Tag;
  if (!PoolTag || PoolTag == ForbiddenPoolTag) {
    auto TagName = llvm::StringRef(ServiceName);
    if (TagName.take_front(3).equals_insensitive("WDF"))
      TagName = TagName.drop_front(3);
    PoolTag = DefaultPoolTag;
    if (TagName.size() > 2) {
      PoolTag = 0;
      for (size_t I = 0; I < std::min<size_t>(4, TagName.size()); ++I)
        PoolTag |= uint32_t(uint8_t(TagName[I])) << (8 * I);
    }
  }
  for (auto [Offset, Value, Width] :
       {std::tuple<uint64_t, uint64_t, unsigned>{GlobalsDriver, *Handle, 8},
        {GlobalsFlags, *Flags, 4},
        {GlobalsTag, PoolTag, 4},
        {GlobalsDisplaceUnload, 1, 1}})
    if (auto E = Memory.writeInteger(B.Globals + Offset, Value, Width))
      return E;
  if (auto E = Memory.write(B.Globals + GlobalsName, Name))
    return E;
  if (auto E =
          Memory.writeInteger(Driver + windows::DriverUnloadOffset, *Thunk, 8))
    return E;
  if (A[5])
    if (auto E = Memory.writeInteger(A[5], *Handle, 8))
      return E;
  B.DriverHandle = *Handle;
  B.UnloadCallback = *Unload;
  B.RegistryCopy = *Copy;
  return 0;
}

llvm::Error KernelFramework::planDelete(uint64_t Handle,
                                        std::vector<Step> &Steps) {
  // Cancellation/draining of live queue requests needs an explicit schedule.
  // Detect that boundary before changing any ancestor or invoking cleanup.
  auto Preflight = [&](auto &&Self, uint64_t Current) -> llvm::Error {
    auto I = Objects.find(Current);
    if (I == Objects.end())
      return invalid("delete requires a live framework object");
    if (I->second.Kind == ObjectKind::Request) {
      const auto &R = Requests.at(Current);
      if (!R.Completed && !(Current == Handle && R.Completing))
        return invalid(
            "deletion with a live request requires queue cancellation "
            "or draining, which is not modeled");
      if (Current != Handle && I->second.InternalReferences)
        return invalid(
            "deletion with an outstanding cancellation callback requires "
            "asynchronous draining, which is not modeled");
    }
    for (uint64_t Child : I->second.Children)
      if (Objects.count(Child))
        if (auto E = Self(Self, Child))
          return E;
    return llvm::Error::success();
  };
  if (auto E = Preflight(Preflight, Handle))
    return E;
  std::vector<Step> Destruction;
  auto Visit = [&](auto &&Self, uint64_t Handle) -> llvm::Error {
    auto I = Objects.find(Handle);
    if (I == Objects.end())
      return invalid("delete requires a live framework object");
    auto &O = I->second;
    if (O.Deleting)
      return invalid("object deletion is already in progress");
    O.Deleting = true;
    for (uint64_t Child : O.Children)
      if (Objects.count(Child) && !Objects.at(Child).Deleting)
        if (auto E = Self(Self, Child))
          return E;
    for (uint64_t Type : O.ContextOrder)
      if (O.Contexts.at(Type).Cleanup)
        Steps.push_back(
            {StepKind::Callback, Handle, O.Contexts.at(Type).Cleanup});
    Steps.push_back({StepKind::Cleaned, Handle});
    Destruction.push_back({StepKind::TryDestroy, Handle});
    return llvm::Error::success();
  };
  if (auto E = Visit(Visit, Handle))
    return E;
  Steps.insert(Steps.end(), Destruction.begin(), Destruction.end());
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::advance(uint64_t Token) {
  auto I = Continuations.find(Token);
  if (I == Continuations.end() || PendingCall)
    return invalid("invalid framework callback continuation");
  auto &C = I->second;
  while (C.Index < C.Steps.size()) {
    const Step S = C.Steps[C.Index++];
    if (S.Kind == StepKind::Callback) {
      PendingCall = GuestCall{Token, S.PC, {S.Object}};
      return std::optional<uint64_t>{};
    }
    if (S.Kind == StepKind::DriverUnloaded) {
      Bindings.at(S.Object).Unloaded = true;
      continue;
    }
    if (S.Kind == StepKind::FinishBindingUnbind) {
      if (auto E = finishUnbind(Bindings.at(S.Object)))
        return E;
      continue;
    }
    if (S.Kind == StepKind::BeginDriverDelete) {
      std::vector<Step> Delete;
      if (auto E = planDelete(Bindings.at(S.Object).DriverHandle, Delete))
        return E;
      C.Steps.insert(C.Steps.begin() + C.Index, Delete.begin(), Delete.end());
      continue;
    }
    if (S.Kind == StepKind::CompleteRequest) {
      auto R = Requests.find(S.Object);
      if (R == Requests.end() || !R->second.Completing || R->second.Completed)
        return invalid("completion continuation lost its live request");
      if (auto E =
              RequestsHost.Complete(R->second.IRP, R->second.CompletionStatus,
                                    R->second.CompletionInformation))
        return E;
      R->second.Completed = true;
      R->second.Completing = false;
      R->second.Queue = 0;
      continue;
    }
    if (S.Kind == StepKind::CancelReturned) {
      auto Callback = CancelCallbacks.find(Token);
      auto R = Requests.find(S.Object);
      auto O = Objects.find(S.Object);
      if (Callback == CancelCallbacks.end() || Callback->second != S.Object ||
          R == Requests.end() ||
          R->second.Cancellation != CancelState::Delivered ||
          O == Objects.end() || !O->second.InternalReferences)
        return invalid("cancellation return lost its request reference");
      --O->second.InternalReferences;
      CancelCallbacks.erase(Callback);
      // Completion may already have run cleanup and retired the WDM IRP while
      // this callback was suspended. Reuse the normal guest destroy sequence
      // only after both framework and driver holds are gone.
      if (O->second.Cleaned && O->second.DestroyEligible &&
          !O->second.References && !O->second.InternalReferences)
        C.Steps.insert(C.Steps.begin() + C.Index,
                       {StepKind::TryDestroy, S.Object});
      continue;
    }
    auto OI = Objects.find(S.Object);
    if (OI == Objects.end())
      return invalid("callback sequence lost its object");
    auto &O = OI->second;
    if (S.Kind == StepKind::Cleaned) {
      O.Cleaned = true;
      continue;
    }
    if (S.Kind == StepKind::TryDestroy) {
      O.DestroyEligible = true;
      if (O.References || O.InternalReferences)
        continue;
      std::vector<Step> Destroy;
      for (uint64_t Type : O.ContextOrder)
        if (O.Contexts.at(Type).Destroy)
          Destroy.push_back(
              {StepKind::Callback, S.Object, O.Contexts.at(Type).Destroy});
      Destroy.push_back({StepKind::Destroy, S.Object});
      C.Steps.insert(C.Steps.begin() + C.Index, Destroy.begin(), Destroy.end());
      continue;
    }
    if (O.References || O.InternalReferences)
      return invalid("destroy callback retained an object reference");
    if (O.Kind == ObjectKind::Device) {
      for (const auto &[Handle, Queue] : Queues)
        if (Queue.Device == S.Object)
          return invalid("device deletion still owns a referenced queue");
      if (!DevicesHost.Delete)
        return invalid("framework device host is not configured");
      if (auto E = DevicesHost.Delete(Devices.at(S.Object).Wdm))
        return E;
      Devices.erase(S.Object);
    }
    if (O.Kind == ObjectKind::Queue)
      Queues.erase(S.Object);
    if (O.Kind == ObjectKind::Request)
      Requests.erase(S.Object);
    for (const auto &[Type, Context] : O.Contexts)
      if (auto E = retire(Context.Address))
        return E;
    if (auto E = retire(S.Object))
      return E;
    Objects.erase(OI);
  }
  Continuations.erase(I);
  return std::optional<uint64_t>{0};
}

llvm::Expected<uint64_t> KernelFramework::start(std::vector<Step> Steps) {
  const uint64_t Token = NextContinuation++;
  Continuations.emplace(Token, Continuation{std::move(Steps)});
  auto Result = advance(Token);
  if (!Result)
    return Result.takeError();
  return 0; // Deferred callback completion is latched by DriverSession.
}

std::optional<KernelFramework::GuestCall> KernelFramework::takeGuestCall() {
  return std::exchange(PendingCall, std::nullopt);
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::finishGuestCall(uint64_t Token, uint64_t) {
  auto Cancel = CancelCallbacks.find(Token);
  if (Cancel != CancelCallbacks.end() &&
      Requests.at(Cancel->second).Cancellation != CancelState::Delivered)
    return invalid("cancellation callback has not entered guest execution");
  return advance(Token);
}

bool KernelFramework::hasLiveBinding() const {
  return std::any_of(Bindings.begin(), Bindings.end(),
                     [](const auto &Entry) { return !Entry.second.Unbound; });
}

llvm::Expected<uint64_t>
KernelFramework::call(const KernelExportRegistry::Export &Export,
                      llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  auto Count = argumentCount(Export);
  if (!Count || *Count != A.size())
    return invalid("unsupported framework routine or argument count");
  if (IRQL)
    return invalid("current framework object callbacks require PASSIVE_LEVEL");
  if (Export.Kind != KernelExportRegistry::ExportKind::FrameworkFunction)
    return Export.Name == "WdfVersionBind" ? bind(A) : unbind(A);
  auto BI = Bindings.find(Export.Binding);
  if (BI == Bindings.end() || BI->second.Unbound)
    return invalid("table entry belongs to an unbound framework instance");
  auto &B = BI->second;
  if (Export.Name == FrameworkUnloadRoutine) {
    if (A[0] != Driver || !B.DriverHandle || B.Unloaded)
      return invalid("invalid or repeated framework driver unload");
    std::vector<Step> Steps{
        {StepKind::Callback, B.DriverHandle, B.UnloadCallback}};
    Steps.push_back({StepKind::BeginDriverDelete, B.Globals});
    Steps.push_back({StepKind::DriverUnloaded, B.Globals});
    return start(std::move(Steps));
  }
  if (A[0] != B.Globals)
    return invalid("framework function received another binding's globals");
  auto Control = callControl(Export.Name, B, A);
  if (!Control)
    return Control.takeError();
  if (*Control)
    return **Control;
  auto Queue = callQueue(Export.Name, B, A);
  if (!Queue)
    return Queue.takeError();
  if (*Queue)
    return **Queue;
  auto Request = callRequest(Export.Name, B, A);
  if (!Request)
    return Request.takeError();
  if (*Request)
    return **Request;
  if (Export.Name == "WdfDriverCreate")
    return createDriver(B, A);
  if (Export.Name == "WdfWdmDriverGetWdfDriverHandle") {
    if (A[1] != Driver || !Objects.count(B.DriverHandle))
      return invalid("WDM driver does not own a live framework driver");
    return B.DriverHandle;
  }
  if (Export.Name == "WdfObjectCreate") {
    auto Validation = attributes(A[1], AttributesUse::Object);
    if (!Validation)
      return Validation.takeError();
    if (const auto *Status = std::get_if<uint32_t>(&*Validation))
      return *Status;
    const auto &Attrs = std::get<Attributes>(*Validation);
    if (auto E = writable(A[2], 8))
      return E;
    auto Handle = createObject(B.Globals, Attrs, false);
    if (!Handle)
      return Handle.takeError();
    if (auto E = Memory.writeInteger(A[2], *Handle, 8))
      return E;
    return 0;
  }
  if (Export.Name == "WdfObjectContextGetObject") {
    for (const auto &[Handle, O] : Objects)
      if (O.Binding == B.Globals)
        for (const auto &[Type, Context] : O.Contexts)
          if (Context.Address && Context.Address == A[1])
            return Handle;
    return invalid("context pointer has no live framework owner");
  }
  auto OI = Objects.find(A[1]);
  if (OI == Objects.end() || OI->second.Binding != B.Globals)
    return invalid("invalid, foreign or deleted framework handle");
  auto &O = OI->second;
  if (Export.Name == "WdfDriverGetRegistryPath" ||
      Export.Name == "WdfDriverWdmGetDriverObject") {
    if (O.Kind != ObjectKind::Driver)
      return invalid("framework handle has the wrong object type");
    return Export.Name == "WdfDriverGetRegistryPath" ? B.RegistryCopy : Driver;
  }
  if (Export.Name == "WdfObjectGetTypedContextWorker") {
    auto TypeSize = read(A[2], 4);
    if (!TypeSize)
      return TypeSize.takeError();
    if (*TypeSize != ContextTypeSize && *TypeSize != ContextTypeLegacySize)
      return invalid("unsupported typed context record size");
    auto CI = O.Contexts.find(A[2]);
    return CI == O.Contexts.end() ? 0 : CI->second.Address;
  }
  if (Export.Name == "WdfObjectAllocateContext") {
    if (O.Deleting)
      return DeletePending;
    auto Validation = attributes(A[2], AttributesUse::AdditionalContext);
    if (!Validation)
      return Validation.takeError();
    if (const auto *Status = std::get_if<uint32_t>(&*Validation))
      return *Status;
    const auto &Attrs = std::get<Attributes>(*Validation);
    if (!Attrs.Type)
      return ObjectNameInvalid;
    if (A[3])
      if (auto E = writable(A[3], 8))
        return E;
    const bool Exists = O.Contexts.count(Attrs.Type);
    auto Context = addContext(O, Attrs);
    if (!Context)
      return Context.takeError();
    if (A[3])
      if (auto E = Memory.writeInteger(A[3], *Context, 8))
        return E;
    return Exists ? ObjectNameExists : 0;
  }
  if (Export.Name == "WdfObjectReferenceActual") {
    if (O.Cleaned)
      return invalid(
          "references after cleanup are outside this framework profile");
    if (O.References == UINT64_MAX)
      return invalid("framework reference count overflow");
    ++O.References;
    return 0;
  }
  if (Export.Name == "WdfObjectDereferenceActual") {
    if (!O.References)
      return invalid("framework reference count underflow");
    --O.References;
    if (O.Cleaned && O.DestroyEligible && !O.References &&
        !O.InternalReferences)
      return start({{StepKind::TryDestroy, A[1]}});
    return 0;
  }
  if (Export.Name == "WdfObjectDelete") {
    if (O.Kind == ObjectKind::Driver)
      return invalid("WDFDRIVER cannot be deleted by the driver");
    if (O.Kind == ObjectKind::Queue && Queues.at(A[1]).IsDefault)
      return invalid("the default queue cannot be deleted by the driver");
    if (O.Kind == ObjectKind::Request)
      return invalid("an incoming framework request is released by completion");
    std::vector<Step> Steps;
    if (auto E = planDelete(A[1], Steps))
      return E;
    return start(std::move(Steps));
  }
  return invalid("unhandled framework routine");
}
} // namespace neverd::emulation
