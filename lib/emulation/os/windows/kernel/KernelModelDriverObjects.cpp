//===- KernelModelDriverObjects.cpp - Named driver objects and references
//--===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Named driver objects and references.
///

#include "KernelModel.h"
#include "KernelObjectTypes.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
llvm::Error driverObjectError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "driver object namespace: " + Text);
}
} // namespace

llvm::Error KernelModel::initializeDriverNamespace(const DriverOptions &O) {
  auto OwnName = readObjectName(DriverObject + windows::DriverNameOffset);
  if (!OwnName)
    return OwnName.takeError();
  NamedDrivers.emplace(llvm::StringRef(*OwnName).lower(),
                       NamedDriverObject{*OwnName, DriverObject, true});
  DriverNamespaceKnown = O.DriverNamespace.has_value();
  if (!O.DriverNamespace)
    return llvm::Error::success();
  for (const auto &Name : O.DriverNamespace->EmptyDrivers) {
    auto Object = allocate(windows::DriverObjectSize);
    if (!Object)
      return Object.takeError();
    auto String = makeUnicodeString(Name);
    if (!String)
      return String.takeError();
    if (auto E = Memory.writeInteger(*Object, windows::DriverType, 2))
      return E;
    if (auto E = Memory.writeInteger(*Object + 2, windows::DriverObjectSize, 2))
      return E;
    // allocate zeroed the object. Only the explicitly empty device head and
    // type/size/name are observable; unprovided image/dispatch fields are not.
    std::array<uint8_t, 16> Bytes{};
    if (auto E = Memory.read(*String, Bytes))
      return E;
    if (auto E = Memory.write(*Object + windows::DriverNameOffset, Bytes))
      return E;
    NamedDriverNameRanges.emplace_back(*String, 16 + (Name.size() + 1) * 2);
    ReferencedObjects.emplace(*Object,
                              ObjectReferences{ObjectKind::Driver, 0, 0, Name});
    NamedDrivers.emplace(llvm::StringRef(Name).lower(),
                         NamedDriverObject{Name, *Object, false});
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::referenceDriverObjectByName(llvm::ArrayRef<uint64_t> A,
                                         llvm::StringRef Name) {
  using namespace windows;
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return driverObjectError(
        "ObReferenceObjectByName requires Windows 10.0.19041");
  if (A[2] || A[6] || uint8_t(A[5]) != KernelMode)
    return driverObjectError("name references require KernelMode without "
                             "access state or parse context");
  const auto Attributes = uint32_t(A[1]);
  if (Attributes & ~(ObjectKernelHandle | 0x40u))
    return driverObjectError("unsupported name reference attributes");
  bool KnownType = false;
  for (const auto &Entry : object_type::Entries)
    KnownType |= A[4] == object_type::identity(Entry.Type);
  if (!KnownType)
    return driverObjectError(
        "name references require a known explicit object type");
  APICallDetail =
      Name.str(); // Captured once; the session never rereads guest names.
  if (!validDriverObjectName(Name))
    return driverObjectError("only the driver object directory is modeled");
  const auto Found = NamedDrivers.find(llvm::StringRef(Name).lower());
  if (Found == NamedDrivers.end() && !DriverNamespaceKnown)
    return driverObjectError("lookup of " + Name +
                             " requires driver_namespace");
  if (auto E = objectBuffer(A[7], 8, true))
    return E;
  // The stopped caller observes ordinary RAM only. This makes native's
  // intermediate NULL/success stores unobservable and excludes MMIO effects.
  auto Storage = Memory.pinBacking(A[7], 8);
  if (!Storage)
    return Storage.takeError();
  auto Complete = [&](uint32_t Status,
                      uint64_t Object = 0) -> llvm::Expected<uint64_t> {
    if (auto E = Memory.writeInteger(A[7], Object, 8))
      return E;
    return Status;
  };
  // In the inspected 19041 entry, lookup failures clear Object, while early
  // name-capture/access-state failures precede the output store entirely.
  if (Found == NamedDrivers.end() ||
      (!(Attributes & 0x40u) && Found->second.Name != Name))
    return Complete(StatusObjectNameNotFound);
  if (A[4] != object_type::identity(object_type::Kind::Driver))
    return Complete(StatusObjectTypeMismatch);
  auto &Object = Found->second;
  if (ReferencedObjects.at(Object.Object).Pointers == SIZE_MAX)
    return driverObjectError("driver reference count overflow");
  // KernelMode bypasses the object's access check; no user ACL or generic
  // mapping observation is invented, and no handle is created by this call.
  auto Status = Complete(StatusSuccess, Object.Object);
  if (!Status)
    return Status.takeError();
  ++ReferencedObjects.at(Object.Object).Pointers;
  return *Status;
}

bool KernelModel::isNamedDriverObject(uint64_t Object) const {
  for (const auto &[Key, D] : NamedDrivers)
    if (D.Object == Object)
      return true;
  return false;
}
llvm::Expected<uint64_t>
KernelModel::guestDriverDispatch(uint64_t Owner, unsigned Major) const {
  if (Major >= profile::MajorFunctionCount)
    return driverObjectError("dispatch major exceeds DRIVER_OBJECT table");
  const std::array<bool, profile::MajorFunctionCount * 8> *Written = nullptr;
  std::shared_ptr<RAMInitializationWatch> Watch;
  if (Owner == DriverObject) {
    Written = &DispatchBytesWritten;
    Watch = DispatchInitialization;
  } else
    for (const auto &[Key, D] : NamedDrivers)
      if (D.Guest && D.Object == Owner) {
        Written = &D.DispatchWritten;
        Watch = D.DispatchInitialization;
        break;
      }
  if (!Written)
    return driverObjectError("dispatch requires a live guest driver owner");
  if (!anyInitialized(*Written, Watch, Major * 8, 8))
    return uint64_t(0);
  if (!initialized(*Written, Watch, Major * 8, 8))
    return driverObjectError(
        "driver left a partially initialized dispatch pointer");
  auto PC =
      Memory.readInteger(Owner + windows::DriverDispatchOffset + Major * 8, 8);
  if (!PC)
    return PC.takeError();
  if (!*PC)
    return driverObjectError(
        "driver explicitly registered a null dispatch callback");
  return *PC;
}

llvm::Error KernelModel::validateNamedDriverUnload() const {
  for (const auto &[Key, D] : NamedDrivers)
    if (ReferencedObjects.at(D.Object).Pointers)
      return driverObjectError(
          "unload returned with live driver pointer references");
  return llvm::Error::success();
}
llvm::Error KernelModel::validateNamedDriverAccess(uint64_t Address,
                                                   uint32_t Size,
                                                   bool IsWrite) const {
  using namespace windows;
  const uint64_t End = Address + Size;
  for (auto [Base, Length] : NamedDriverNameRanges)
    if (IsWrite && Address < Base + Length && Base < End)
      return driverObjectError("write to read-only driver name storage");
  for (const auto &[Key, D] : NamedDrivers) {
    if (D.Object == DriverObject || Address >= D.Object + DriverObjectSize ||
        D.Object >= End)
      continue;
    const auto Begin = std::max(Address, D.Object) - D.Object;
    const auto Last = std::min(End, D.Object + DriverObjectSize) - D.Object;
    for (auto Offset = Begin; Offset < Last; ++Offset) {
      if (D.Guest) {
        const bool Allowed =
            IsWrite ? ((Offset >= DriverFastIOOffset &&
                        Offset < DriverInitOffset) ||
                       Offset >= DriverStartIOOffset)
                    : (!(Offset >= DriverFlagsOffset &&
                         Offset < DriverFlagsOffset + 4) &&
                       (Offset < DriverDispatchOffset ||
                        initialized(D.DispatchWritten, D.DispatchInitialization,
                                    Offset - DriverDispatchOffset)));
        if (!Allowed)
          return driverObjectError(
              "access to opaque or uninitialized child driver field");
        continue;
      }
      if (IsWrite ||
          !(Offset < 4 ||
            (Offset >= DriverDeviceHead && Offset < DriverDeviceHead + 8) ||
            (Offset >= DriverNameOffset && Offset < DriverNameOffset + 16)))
        return driverObjectError(
            "access to an opaque or read-only driver field");
    }
  }
  for (const auto &[ID, S] : ServiceImages) {
    if (S.Phase == ServiceImageState::State::Unloaded)
      continue;
    if (Address < S.Extension + DriverExtensionSize && S.Extension < End)
      for (auto P = std::max(Address, S.Extension);
           P < std::min(End, S.Extension + DriverExtensionSize); ++P)
        if (P < S.Extension + DriverAddDeviceOffset ||
            P >= S.Extension + DriverAddDeviceOffset + 8)
          return driverObjectError(
              "access to opaque child driver extension field");
    if (IsWrite)
      for (size_t I = 3; I < S.Storage.size(); ++I) {
        const auto &[B, N] = S.Storage[I];
        if (Address < B + N && B < End)
          return driverObjectError(
              "write to read-only child service name storage");
      }
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
