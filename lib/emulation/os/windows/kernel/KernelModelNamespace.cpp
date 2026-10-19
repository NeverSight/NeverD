//===- KernelModelNamespace.cpp - Named objects and link lifetimes -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve the session namespace through the shared object-reference owner.
//===----------------------------------------------------------------------===//

#include "../WindowsUnicode.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error namespaceError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}

bool sameName(llvm::StringRef A, llvm::StringRef B, bool Fold) {
  return Fold ? A.equals_insensitive(B) : A == B;
}

std::string canonicalName(llvm::StringRef Name, bool Fold) {
  // The bounded driver environment has one global DOS-device map. It does
  // not infer per-logon-session maps from the current host or process.
  for (llvm::StringRef Alias : {"\\??", "\\DosDevices"})
    if (Name.size() >= Alias.size() &&
        sameName(Name.take_front(Alias.size()), Alias, Fold) &&
        (Name.size() == Alias.size() || Name[Alias.size()] == '\\'))
      return "\\GLOBAL??" + Name.drop_front(Alias.size()).str();
  return Name.str();
}

std::vector<uint8_t> nameBytes(llvm::StringRef Name, bool Terminate) {
  std::vector<uint8_t> Bytes((Name.size() + unsigned(Terminate)) * 2);
  for (size_t I = 0; I < Name.size(); ++I)
    Bytes[2 * I] = uint8_t(Name[I]);
  return Bytes;
}
} // namespace

llvm::Error KernelModel::initializeObjectNamespace() {
  for (llvm::StringRef Name :
       {"\\", "\\Driver", "\\Device", "\\FileSystem", "\\GLOBAL??"}) {
    auto Object = allocate(profile::ProcessTokenSize);
    if (!Object)
      return Object.takeError();
    ReferencedObjects.emplace(
        *Object, ObjectReferences{ObjectKind::Directory, 0, 0, Name.str()});
  }
  return llvm::Error::success();
}

std::string KernelModel::objectName(uint64_t Object) const {
  if (const auto Device = Devices.find(Object); Device != Devices.end()) {
    const auto &Name = Device->second.Name;
    return Device->second.DeletePending && !Name.empty()
               ? Name.substr(Name.rfind('\\'))
               : Name;
  }
  const auto &Name = ReferencedObjects.at(Object).Name;
  if (const auto Link = SymbolicLinks.find(Object);
      Link != SymbolicLinks.end() && !Link->second.Named)
    return Name.substr(Name.rfind('\\'));
  return Name;
}

uint64_t KernelModel::findSymbolicLink(llvm::StringRef Key) const {
  const auto Canonical = canonicalName(Key, true);
  for (const auto &[Object, Link] : SymbolicLinks)
    if (Link.Named &&
        sameName(canonicalName(ReferencedObjects.at(Object).Name, true),
                 Canonical, true))
      return Object;
  return 0;
}

const KernelModel::ObjectHandle *
KernelModel::findObjectHandle(uint64_t Handle) const {
  const auto Found = ObjectHandles.find(Handle);
  if (Found == ObjectHandles.end())
    return nullptr;
  if (Found->second.ScenarioOwned)
    return nullptr;
  const auto Context = executionProcessContext();
  const uint32_t ProcessID = Context ? Context->ProcessID : 0;
  if (!(Found->second.Attributes & ObjectKernelHandle) &&
      Found->second.ProcessID != ProcessID)
    return nullptr;
  return &Found->second;
}

std::optional<uint32_t> KernelModel::objectAccessMask(ObjectKind Kind) {
  switch (Kind) {
  case ObjectKind::File:
    return FileAllAccess;
  case ObjectKind::Key:
    return KeyAllAccess;
  case ObjectKind::Thread:
    return ThreadAllAccess;
  case ObjectKind::Process:
    return ProcessAllAccess;
  case ObjectKind::Directory:
    return DirectoryAllAccess;
  case ObjectKind::SymbolicLink:
    return SymbolicLinkAllAccess;
  default:
    return std::nullopt;
  }
}

llvm::Expected<uint32_t> KernelModel::mapObjectAccess(ObjectKind Kind,
                                                      uint32_t Access) {
  const auto Valid = objectAccessMask(Kind);
  if (!Valid)
    return namespaceError("unsupported object handle class");
  if (Kind == ObjectKind::Thread) {
    const uint32_t Generic = Access & 0xf0000000;
    Access &= 0x0fffffff;
    if (Generic & 0x80000000)
      Access |= 0x20048;
    if (Generic & 0x40000000)
      Access |= 0x20437;
    if (Generic & 0x20000000)
      Access |= 0x121800;
    if (Generic & 0x10000000)
      Access |= *Valid;
  }
  if (Kind == ObjectKind::Directory || Kind == ObjectKind::SymbolicLink ||
      Kind == ObjectKind::Key || Kind == ObjectKind::File) {
    // WDK rights and the target kernel's generic mappings. The per-type valid
    // mask controls SYNCHRONIZE; MAXIMUM_ALLOWED is not a guessed grant.
    const bool Directory = Kind == ObjectKind::Directory;
    const bool Key = Kind == ObjectKind::Key;
    const bool File = Kind == ObjectKind::File;
    const uint32_t Read = File ? 0x120089
                               : 0x20000 | (Key         ? 0x19
                                            : Directory ? 3
                                                        : 1);
    const uint32_t Execute = File ? 0x1200a0 : Read;
    const uint32_t Write = File ? 0x120116
                                : 0x20000 | (Key         ? 6
                                             : Directory ? 12
                                                         : 0);
    const uint32_t Generic = Access & 0xf0000000;
    Access &= ~0xf0000000;
    if (Generic & 0x80000000)
      Access |= Read;
    if (Generic & 0x40000000)
      Access |= Write;
    if (Generic & 0x20000000)
      Access |= Execute;
    if (Generic & 0x10000000)
      Access |= *Valid;
  }
  if (Access & ~*Valid)
    return namespaceError("unsupported object open access mask");
  return Access;
}

llvm::Expected<KernelModel::ObjectLookup>
KernelModel::lookupNamedObject(llvm::StringRef Name, uint64_t Root,
                               uint32_t Attributes, bool OpenLink,
                               bool ParseDevice, unsigned LinkDepth) {
  if (LinkDepth >= 16)
    return namespaceError("symbolic-link traversal exceeds its bounded depth");
  std::string Path = Name.str();
  const bool Fold = Attributes & ObjectCaseInsensitive;
  if (Root) {
    const auto Handle = findObjectHandle(Root);
    if (!Handle)
      return ObjectLookup{StatusInvalidHandle};
    const auto &State = ReferencedObjects.at(Handle->Object);
    if (State.Kind != ObjectKind::Directory)
      return ObjectLookup{StatusObjectTypeMismatch};
    if (Name.starts_with("\\"))
      return ObjectLookup{StatusObjectPathSyntaxBad};
    Path = State.Name;
    if (!Name.empty()) {
      if (Path != "\\")
        Path += '\\';
      Path += Name;
    }
  } else if (!Name.starts_with("\\")) {
    return ObjectLookup{StatusObjectPathSyntaxBad};
  }
  Path = canonicalName(Path, Fold);
  if (llvm::StringRef(Path).equals_insensitive("\\Registry") ||
      llvm::StringRef(Path).starts_with_insensitive("\\Registry\\"))
    return namespaceError("registry name parsing requires a registry open API");
  if (Path.size() >
      profile::MaxDeviceNameSize + (ParseDevice ? MaxFileSystemPathBytes : 0))
    return namespaceError("object path exceeds the bounded namespace size");
  if (Path.size() > 1 && ((!ParseDevice && Path.back() == '\\') ||
                          llvm::StringRef(Path).contains("\\\\")))
    return ObjectLookup{StatusObjectNameInvalid};
  auto Find = [&](llvm::StringRef Candidate) -> uint64_t {
    for (const auto &[Object, State] : ReferencedObjects) {
      if (const auto Link = SymbolicLinks.find(Object);
          Link != SymbolicLinks.end() && !Link->second.Named)
        continue;
      if (const auto Device = Devices.find(Object);
          Device != Devices.end() && Device->second.DeletePending)
        continue;
      if (sameName(canonicalName(objectName(Object), Fold), Candidate, Fold))
        return Object;
    }
    return 0;
  };
  size_t End = 1;
  while (true) {
    End = Path.find('\\', End);
    const bool Last = End == std::string::npos;
    const auto Prefix =
        llvm::StringRef(Path).take_front(Last ? Path.size() : End);
    const auto Object = Find(Prefix);
    if (!Object)
      return ObjectLookup{Last ? StatusObjectNameNotFound
                               : StatusObjectPathNotFound};
    if (ParseDevice && ReferencedObjects.at(Object).Kind == ObjectKind::Device)
      return ObjectLookup{StatusSuccess, Object, Last ? "" : Path.substr(End)};
    if (ParseDevice)
      if (const auto Link = SymbolicLinks.find(Object);
          Link != SymbolicLinks.end() && (!Last || !OpenLink))
        return lookupNamedObject(Link->second.Target +
                                     (Last ? "" : Path.substr(End)),
                                 0, Attributes, false, true, LinkDepth + 1);
    if (Last) {
      if (const auto Link = SymbolicLinks.find(Object);
          Link != SymbolicLinks.end() && !OpenLink)
        return lookupNamedObject(Link->second.Target, 0, Attributes, false,
                                 ParseDevice, LinkDepth + 1);
      return ObjectLookup{StatusSuccess, Object};
    }
    if (ReferencedObjects.at(Object).Kind != ObjectKind::Directory)
      return namespaceError("namespace traversal through a device or parse "
                            "object is unsupported");
    ++End;
  }
}

llvm::Expected<uint64_t>
KernelModel::referenceObjectByName(llvm::ArrayRef<uint64_t> A) {
  if (!A[0])
    return StatusObjectNameInvalid;
  auto Name = readObjectName(A[0]);
  if (!Name)
    return Name.takeError();
  if (Name->empty())
    return StatusObjectNameInvalid;
  if (Version && llvm::StringRef(*Name).starts_with_insensitive("\\Driver\\"))
    return referenceDriverObjectByName(A, *Name);
  if (uint8_t(A[5]) != KernelMode || A[2] || A[6] || !A[4] ||
      (uint32_t(A[1]) & ~ObjectCaseInsensitive))
    return namespaceError(
        "named reference requires kernel mode, an object type, "
        "no access state or parse context and only OBJ_CASE_INSENSITIVE");
  // Driver/device security and nonzero DesiredAccess require the explicit
  // security environment. The ordinary kernel identity lookup uses zero.
  if (uint32_t(A[3]))
    return namespaceError("named reference security access is unsupported");
  if (auto E = validateObjectOutput(A[7], 8))
    return E;
  const auto LinkType = ObjectTypes.find(ObjectKind::SymbolicLink);
  const bool OpenLink =
      LinkType != ObjectTypes.end() && A[4] == LinkType->second;
  auto Found = lookupNamedObject(*Name, 0, uint32_t(A[1]), OpenLink);
  if (!Found)
    return Found.takeError();
  uint64_t Status = Found->Status;
  if (!Status &&
      !matchesObjectType(ReferencedObjects.at(Found->Object).Kind, A[4]))
    Status = StatusObjectTypeMismatch;
  if (auto E = Memory.writeInteger(A[7], Status ? 0 : Found->Object, 8))
    return E;
  if (!Status)
    ++ReferencedObjects.at(Found->Object).Pointers;
  return Status;
}

llvm::Expected<uint64_t>
KernelModel::openNamedObject(llvm::ArrayRef<uint64_t> A, ObjectKind Kind) {
  if (auto E = validateGuestAccess(A[2], ObjectAttributesSize, false))
    return E;
  std::array<uint8_t, ObjectAttributesSize> Record;
  if (auto E = Memory.read(A[2], Record))
    return E;
  using namespace llvm::support::endian;
  if (read32le(Record.data()) != ObjectAttributesSize)
    return StatusInvalidParameter;
  const uint32_t Flags = read32le(Record.data() + ObjectFlagsOffset);
  if ((Flags & ~(ObjectCaseInsensitive | ObjectKernelHandle)) ||
      read64le(Record.data() + ObjectSecurityOffset) ||
      read64le(Record.data() + ObjectQualityOffset))
    return namespaceError("namespace open supports only case and kernel-handle "
                          "attributes, without security or QoS records");
  const auto NameAddress = read64le(Record.data() + ObjectNameOffset);
  if (!NameAddress)
    return StatusObjectNameInvalid;
  auto Name = readObjectName(NameAddress);
  if (!Name)
    return Name.takeError();
  auto Access = mapObjectAccess(Kind, uint32_t(A[1]));
  if (!Access)
    return Access.takeError();
  const auto Context = executionProcessContext();
  const uint32_t ProcessID = Context ? Context->ProcessID : 0;
  if (ProcessID != SystemProcessID && !(Flags & ObjectKernelHandle))
    return namespaceError(
        "namespace open outside the system process requires OBJ_KERNEL_HANDLE");
  if (Kind == ObjectKind::SymbolicLink &&
      (llvm::StringRef(*Name).equals_insensitive("\\Device\\PhysicalMemory") ||
       NamedSections.contains(llvm::StringRef(*Name).lower()))) {
    if (auto E = validateObjectOutput(A[0], 8))
      return E;
    if (auto E = Memory.writeInteger(A[0], 0, 8))
      return E;
    return StatusObjectTypeMismatch;
  }
  auto Found =
      lookupNamedObject(*Name, read64le(Record.data() + ObjectRootOffset),
                        Flags, Kind == ObjectKind::SymbolicLink);
  if (!Found)
    return Found.takeError();
  const bool Native = Version || FiniteFileSystem || SectionNamespace;
  if (Native)
    if (auto E = validateObjectOutput(A[0], 8))
      return E;
  auto Failed = [&](uint64_t Status) -> llvm::Expected<uint64_t> {
    if (Native)
      if (auto E = Memory.writeInteger(A[0], 0, 8))
        return E;
    return Status;
  };
  if (Found->Status)
    return Failed(Found->Status);
  if (ReferencedObjects.at(Found->Object).Kind != Kind)
    return Failed(StatusObjectTypeMismatch);
  if (auto E = validateObjectOutput(A[0], 8))
    return E;
  if (NextObjectHandle > profile::SystemThreadHandleLimit ||
      (Native && Kind == ObjectKind::SymbolicLink &&
       std::count_if(
           ObjectHandles.begin(), ObjectHandles.end(), [&](const auto &H) {
             return ReferencedObjects.at(H.second.Object).Kind == Kind;
           }) >= 256))
    return Failed(StatusInsufficientResources);
  if (auto E = Memory.writeInteger(A[0], NextObjectHandle, 8))
    return E;
  retainObjectHandle(Found->Object, *Access, Flags & ObjectKernelHandle,
                     ProcessID);
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::queryObjectName(uint64_t Object, uint64_t Buffer, uint32_t Length,
                             uint64_t ReturnLength, bool RequiredLength) {
  if (!ReferencedObjects.contains(Object))
    return namespaceError("name query requires a known live object");
  const auto &References = ReferencedObjects.at(Object);
  if (References.Kind == ObjectKind::File)
    return namespaceError(
        "file object name query requires a filesystem provider");
  if (References.RegistryOwner)
    return References.RegistryOwner->queryObjectName(
        *this, Object, Buffer, Length, ReturnLength, RequiredLength);
  const auto Name = objectName(Object);
  const uint32_t Required = 16 + (Name.empty() ? 0 : (Name.size() + 1) * 2);
  if (RequiredLength || ReturnLength)
    if (auto E = validateObjectOutput(ReturnLength, 4))
      return E;
  if (Length >= Required)
    if (auto E = validateObjectOutput(Buffer, Required))
      return E;
  if (RequiredLength && ReturnLength)
    if (auto E = Memory.writeInteger(ReturnLength, Required, 4))
      return E;
  if (Length >= Required) {
    std::vector<uint8_t> Bytes(Required);
    if (!Name.empty()) {
      llvm::support::endian::write16le(Bytes.data(), Name.size() * 2);
      llvm::support::endian::write16le(Bytes.data() + 2, (Name.size() + 1) * 2);
      llvm::support::endian::write64le(Bytes.data() + 8, Buffer + 16);
      auto Text = nameBytes(Name, true);
      std::copy(Text.begin(), Text.end(), Bytes.begin() + 16);
    }
    if (auto E = Memory.write(Buffer, Bytes))
      return E;
  }
  if (!RequiredLength && ReturnLength)
    if (auto E = Memory.writeInteger(ReturnLength, Required, 4))
      return E;
  if (Length < Required)
    return StatusInfoLengthMismatch;
  return StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::queryObject(llvm::ArrayRef<uint64_t> A) {
  if (SectionHandles.contains(A[0]))
    return querySectionObject(A);
  const auto Handle = findObjectHandle(A[0]);
  if (!Handle)
    return StatusInvalidHandle;
  if (uint32_t(A[1]) != ObjectNameInformation)
    return namespaceError("unsupported object information class");
  return queryObjectName(Handle->Object, A[2], uint32_t(A[3]), A[4], false);
}

llvm::Expected<uint64_t>
KernelModel::querySymbolicLink(llvm::ArrayRef<uint64_t> A) {
  // Zw enters with KernelMode: handle type and process scope are checked,
  // while granted rights do not restrict this kernel reference operation.
  if (auto E = validateGuestAccess(A[1], 16, false))
    return E;
  std::array<uint8_t, 16> String;
  if (auto E = Memory.read(A[1], String))
    return E;
  const auto Handle = findObjectHandle(A[0]);
  if (!Handle)
    return StatusInvalidHandle;
  const auto Link = SymbolicLinks.find(Handle->Object);
  if (Link == SymbolicLinks.end())
    return StatusObjectTypeMismatch;
  using namespace llvm::support::endian;
  const auto Maximum = read16le(String.data() + 2);
  const auto Buffer = read64le(String.data() + 8);
  auto Target = windows_unicode::bytes(Link->second.Target, false);
  if (!Target)
    return Target.takeError();
  const auto Text =
      llvm::ArrayRef<uint8_t>(Link->second.TargetBytes)
          .take_front(A[2] ? Link->second.TargetBytes.size() : Target->size());
  if (A[2])
    if (auto E = validateObjectOutput(A[2], 4))
      return E;
  if (Maximum >= Text.size()) {
    if (auto E = validateObjectOutput(Buffer, Text.size()))
      return E;
    if (auto E = validateObjectOutput(A[1], 2))
      return E;
    if (auto E = Memory.write(Buffer, Text))
      return E;
    if (auto E = Memory.writeInteger(A[1], Target->size(), 2))
      return E;
  }
  if (A[2])
    if (auto E = Memory.writeInteger(A[2], Text.size(), 4))
      return E;
  return Maximum < Text.size() ? StatusBufferTooSmall : StatusSuccess;
}
} // namespace neverd::emulation
