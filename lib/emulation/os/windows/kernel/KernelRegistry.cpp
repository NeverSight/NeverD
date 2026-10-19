//===- KernelRegistry.cpp - Concrete NT registry semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original implementation of the Microsoft WDK Zw*Key contracts. References:
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatekey
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-zwqueryvaluekey
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-zwdeletekey
/// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ne-wdm-_key_value_information_class
/// https://learn.microsoft.com/windows/win32/sysinfo/writing-and-deleting-registry-data
/// The explicit scenario tree grants the supported access-mask bits. This is
/// a concrete capability and virtual commit model; native hive formats,
/// privileges, ACL evaluation and host registry I/O remain unsupported.
///
//===----------------------------------------------------------------------===//

#include "KernelRegistry.h"

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

namespace neverd::emulation {
namespace {
#define NEVERD_REGISTRY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelRegistryValues.def"
#undef NEVERD_REGISTRY_VALUE
#define NEVERD_REGISTRY_STRING(Name, Value)                                    \
  constexpr llvm::StringLiteral Name(Value);
#include "KernelRegistryStrings.def"
#undef NEVERD_REGISTRY_STRING

enum class API {
  Unknown,
#define NEVERD_KERNEL_REGISTRY_API(Name, Count) Name,
#include "KernelRegistryAPIs.def"
#undef NEVERD_KERNEL_REGISTRY_API
};

struct APIDescriptor {
  API Function;
  unsigned ArgumentCount;
};

const APIDescriptor *lookupAPI(llvm::StringRef Name) {
  static const llvm::StringMap<APIDescriptor> APIs{
#define NEVERD_KERNEL_REGISTRY_API(Name, Count) {#Name, {API::Name, Count}},
#include "KernelRegistryAPIs.def"
#undef NEVERD_KERNEL_REGISTRY_API
  };
  auto I = APIs.find(Name);
  return I == APIs.end() ? nullptr : &I->second;
}

llvm::Error registryError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

std::string folded(llvm::StringRef Text) {
  std::string Result = Text.str();
  for (char &C : Result)
    if (C >= 'A' && C <= 'Z')
      C += 'a' - 'A';
  return Result;
}

bool asciiName(llvm::StringRef Text) {
  return llvm::all_of(Text, [](char C) { return C && llvm::isASCII(C); });
}

bool atOrBelow(llvm::StringRef Path, llvm::StringRef Parent) {
  return Path == Parent ||
         (Path.starts_with(Parent) &&
          Path.drop_front(Parent.size()).starts_with(Separator));
}

bool validPath(llvm::StringRef Path) {
  const std::string Folded = folded(Path);
  return asciiName(Path) && !Path.ends_with(Separator) &&
         !Path.contains(EmptyComponent) &&
         (atOrBelow(Folded, MachineRoot) || atOrBelow(Folded, UserRoot));
}

std::string parentPath(llvm::StringRef Path) {
  return Path.take_front(Path.rfind(Separator)).str();
}

bool rootPath(llvm::StringRef Path) {
  return Path == MachineRoot || Path == UserRoot;
}

llvm::Error validateValue(const DriverRegistryValue &Value) {
  if (!asciiName(Value.Name) || Value.Name.size() > MaxRegistryValueNameLength)
    return registryError(
        "registry value name must be bounded ASCII without NUL");
  if (Value.Type > MaxValueType || Value.Type == LinkValueType)
    return registryError(
        "unsupported registry value type (symbolic links are not modeled)");
  if (Value.Data.size() > MaxRegistryValueBytes)
    return registryError("registry value data exceeds the byte limit");
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint8_t>> readBytes(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Address,
                                               uint32_t Size) {
  if (auto E = Model.validateGuestAccess(Address, Size, false))
    return E;
  std::vector<uint8_t> Bytes(Size);
  if (Size)
    if (auto E = Memory.read(Address, Bytes))
      return E;
  return Bytes;
}

llvm::Error validateOutput(const KernelModel &Model, GuestMemory &Memory,
                           uint64_t Address, uint32_t Size) {
  if (!Size)
    return llvm::Error::success();
  if (auto E = Model.validateGuestAccess(Address, Size, true))
    return E;
  auto Accessible = Memory.canAccess(Address, Size, Write);
  if (!Accessible)
    return Accessible.takeError();
  if (!*Accessible)
    return registryError("registry output requires writable ordinary RAM");
  return Memory.validateBacking(Address, Size);
}

llvm::Expected<std::string> readName(const KernelModel &Model,
                                     GuestMemory &Memory, uint64_t Address,
                                     size_t Limit) {
  if (!Address)
    return registryError("registry UNICODE_STRING pointer is null");
  auto Record = readBytes(Model, Memory, Address, UnicodeSize);
  if (!Record)
    return Record.takeError();
  const uint16_t Length = llvm::support::endian::read16le(Record->data());
  const uint16_t Maximum =
      llvm::support::endian::read16le(Record->data() + UnicodeMaximumOffset);
  const uint64_t Buffer =
      llvm::support::endian::read64le(Record->data() + UnicodeBufferOffset);
  if ((Length & 1) || (Maximum & 1) || Length > Maximum || Length / 2 > Limit ||
      (Length && !Buffer))
    return registryError("invalid or over-limit registry UNICODE_STRING");
  auto Bytes = readBytes(Model, Memory, Buffer, Length);
  if (!Bytes)
    return Bytes.takeError();
  std::string Result;
  for (size_t I = 0; I < Bytes->size(); I += 2) {
    if ((*Bytes)[I + 1] || !(*Bytes)[I] || !llvm::isASCII(char((*Bytes)[I])))
      return registryError(
          "registry names require ASCII; Unicode case folding is unsupported");
    Result.push_back((*Bytes)[I]);
  }
  return Result;
}

void store32(std::vector<uint8_t> &Bytes, size_t Offset, uint32_t Value) {
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}

void storeName(std::vector<uint8_t> &Bytes, size_t Offset,
               llvm::StringRef Name) {
  for (size_t I = 0; I < Name.size(); ++I)
    Bytes[Offset + 2 * I] = Name[I];
}
} // namespace

llvm::Error validateDriverRegistry(
    const std::optional<std::vector<DriverRegistryKey>> &Registry,
    std::optional<uint64_t> SystemTime100ns) {
  if (SystemTime100ns && (!Registry || *SystemTime100ns > INT64_MAX))
    return registryError("registry system time requires an explicit registry "
                         "and a nonnegative signed 64-bit Windows time");
  if (!Registry)
    return llvm::Error::success();
  if (Registry->size() > MaxRegistryKeys)
    return registryError("registry key count exceeds the limit");
  std::set<std::string> ExplicitPaths;
  std::set<std::string> AllPaths;
  size_t Values = 0;
  size_t Bytes = 0;
  for (const DriverRegistryKey &Key : *Registry) {
    if (Key.LastWriteTime100ns && *Key.LastWriteTime100ns > INT64_MAX)
      return registryError(
          "registry last-write time exceeds the signed 64-bit limit");
    if (Key.Information && Key.LastWriteTime100ns &&
        Key.Information->LastWriteTime != *Key.LastWriteTime100ns)
      return registryError("registry last-write observations disagree");
    if (Key.Information && !Key.Information->ClassName.empty())
      return registryError("registry information currently requires an "
                           "explicit empty class_name");
    if (Key.Path.size() > MaxRegistryPathLength || !validPath(Key.Path))
      return registryError(
          "registry path must be bounded ASCII under \\Registry\\Machine or "
          "\\Registry\\User without empty components");
    std::string Path = folded(Key.Path);
    if (!ExplicitPaths.insert(Path).second)
      return registryError("duplicate case-insensitive registry key path");
    while (true) {
      AllPaths.insert(Path);
      if (AllPaths.size() > MaxRegistryKeys)
        return registryError(
            "registry key count including ancestors exceeds the limit");
      if (rootPath(Path))
        break;
      Path = parentPath(Path);
    }
    std::set<std::string> Names;
    for (const DriverRegistryValue &Value : Key.Values) {
      if (auto E = validateValue(Value))
        return E;
      if (!Names.insert(folded(Value.Name)).second)
        return registryError("duplicate case-insensitive registry value name");
      if (++Values > MaxRegistryValues)
        return registryError("registry value count exceeds the limit");
      Bytes += Value.Data.size();
      if (Bytes > MaxRegistryTotalBytes)
        return registryError("registry total value bytes exceed the limit");
    }
  }
  for (const auto &Key : *Registry) {
    if (!Key.SubkeyOrder)
      continue;
    const auto Parent = folded(Key.Path);
    std::set<std::string> Children, Seen;
    for (const auto &Path : AllPaths)
      if (Path != Parent && parentPath(Path) == Parent)
        Children.insert(Path);
    if (Key.SubkeyOrder->size() != Children.size())
      return registryError("registry subkey_order must contain every immediate "
                           "child exactly once");
    for (const auto &Name : *Key.SubkeyOrder) {
      if (Name.empty() || !asciiName(Name) ||
          Name.find('\\') != std::string::npos ||
          Name.size() > MaxRegistryPathLength)
        return registryError(
            "registry subkey_order requires bounded immediate child names");
      const auto Path = Parent + "\\" + folded(Name);
      if (!Children.contains(Path) || !Seen.insert(Path).second)
        return registryError(
            "registry subkey_order has an unknown or duplicate child");
    }
  }
  return llvm::Error::success();
}

llvm::Error KernelRegistry::initialize(
    const std::optional<std::vector<DriverRegistryKey>> &Registry,
    std::optional<uint64_t> InitialSystemTime100ns,
    const std::optional<std::vector<DriverRegistryHive>> &Mounts) {
  if (auto E = validateDriverRegistry(Registry, InitialSystemTime100ns))
    return E;
  if (!KeyBodies.empty())
    return registryError("registry reset requires released key objects");
  if (auto E = validateDriverRegistryHiveMounts(Registry, Mounts))
    return E;
  Configured = Registry.has_value();
  Keys.clear();
  Hives.reset();
  NextKeyIdentity = 1;
  ValueCount = 0;
  DataBytes = 0;
  SystemTime100ns = InitialSystemTime100ns;
  if (!Registry)
    return llvm::Error::success();
  for (const DriverRegistryKey &Input : *Registry) {
    std::string Path = Input.Path;
    while (true) {
      const std::string Folded = folded(Path);
      auto Inserted = Keys.try_emplace(
          Folded, std::make_shared<Key>(
                      Key{Path, {}, {}, false, false, SystemTime100ns}));
      if (Inserted.second)
        Inserted.first->second->Identity = NextKeyIdentity++;
      if (rootPath(Folded))
        break;
      Path = parentPath(Path);
    }
    Key &Entry = *Keys.at(folded(Input.Path));
    // Prefer the explicit key spelling over an implicit ancestor spelling.
    Entry.Path = Input.Path;
    Entry.LastWriteTime100ns =
        Input.Information
            ? std::optional<uint64_t>(Input.Information->LastWriteTime)
        : Input.LastWriteTime100ns ? Input.LastWriteTime100ns
                                   : SystemTime100ns;
    Entry.Information = Input.Information;
    ObservedEnumeration |=
        Input.Information.has_value() || Input.SubkeyOrder.has_value();
    Entry.SubkeyOrder = Input.SubkeyOrder;
    for (const DriverRegistryValue &Value : Input.Values) {
      Entry.Values.emplace(folded(Value.Name), Value);
      Entry.ValueOrder.push_back(folded(Value.Name));
      Entry.MaxValueNameLength =
          std::max(Entry.MaxValueNameLength, uint32_t(Value.Name.size() * 2));
      Entry.MaxValueDataLength =
          std::max(Entry.MaxValueDataLength, uint32_t(Value.Data.size()));
      ++ValueCount;
      DataBytes += Value.Data.size();
    }
  }
  // Folded paths sort parents before children. Retain one spelling per
  // component, including when input paths spell an existing ancestor
  // differently.
  for (auto &[Path, Data] : Keys) {
    auto &Entry = *Data;
    auto Parent = Keys.find(parentPath(Path));
    if (Parent != Keys.end()) {
      Entry.Path =
          Parent->second->Path + Entry.Path.substr(Entry.Path.rfind(Separator));
      Parent->second->MaxNameLength =
          std::max(Parent->second->MaxNameLength,
                   uint32_t((Path.size() - Parent->first.size() - 1) * 2));
    }
  }
  if (Mounts) {
    Hives.emplace();
    for (const auto &H : *Mounts)
      Hives->push_back({H.Path, 0, commitSnapshot(folded(H.Path))});
  }
  return llvm::Error::success();
}

DriverRegistryKey KernelRegistry::keySnapshot(const Key &Entry) {
  DriverRegistryKey Output{Entry.Path, {}};
  Output.Information = Entry.Information;
  Output.SubkeyOrder = Entry.SubkeyOrder;
  Output.LastWriteTime100ns = Entry.LastWriteTime100ns;
  if (Output.Information && Entry.LastWriteTime100ns)
    Output.Information->LastWriteTime = *Entry.LastWriteTime100ns;
  for (const auto &Name : Entry.ValueOrder)
    Output.Values.push_back(Entry.Values.at(Name));
  return Output;
}
llvm::Expected<std::optional<uint64_t>>
KernelRegistry::providerKey(llvm::StringRef Path) const {
  if (!Configured)
    return registryError("registry provider is not configured");
  if (!validPath(Path))
    return registryError("invalid registry provider key path");
  auto K = Keys.find(folded(Path));
  if (K == Keys.end() || K->second->Deleted)
    return std::optional<uint64_t>{};
  return std::optional<uint64_t>{K->second->Identity};
}
llvm::Expected<KernelRegistry::ProviderValue>
KernelRegistry::providerValue(uint64_t Identity, llvm::StringRef Name) const {
  if (!Configured || !Identity || !asciiName(Name) ||
      Name.size() > MaxRegistryValueNameLength)
    return registryError("invalid registry provider value request");
  for (const auto &[Path, K] : Keys)
    if (K->Identity == Identity) {
      if (K->Deleted)
        return ProviderValue{uint32_t(KeyDeleted), {}};
      auto V = K->Values.find(folded(Name));
      if (V == K->Values.end())
        return ProviderValue{uint32_t(ObjectNameNotFound), {}};
      return ProviderValue{0, V->second};
    }
  return ProviderValue{uint32_t(KeyDeleted), {}};
}
std::vector<DriverRegistryKey>
KernelRegistry::commitSnapshot(llvm::StringRef Root) const {
  std::vector<DriverRegistryKey> Result;
  for (const auto &[Path, Entry] : Keys)
    if (!Entry->Deleted && !Entry->Volatile && atOrBelow(Path, Root))
      Result.push_back(keySnapshot(*Entry));
  return Result;
}
llvm::Expected<uint64_t> KernelRegistry::flush(const Key &Entry) {
  if (!Hives)
    return registryError("ZwFlushKey requires explicit registry_hives storage");
  for (auto &H : *Hives) {
    const auto Root = folded(H.Path);
    if (!atOrBelow(folded(Entry.Path), Root))
      continue;
    if (H.FlushCount == UINT64_MAX)
      return registryError("registry hive flush observation count exhausted");
    auto Committed = commitSnapshot(Root);
    // Publish the whole hive atomically. Unflushed live state and volatile
    // keys never become durable merely because a handle was closed.
    H.Committed = std::move(Committed);
    ++H.FlushCount;
    return Success;
  }
  return registryError("ZwFlushKey target has no declared hive mount");
}

std::optional<std::vector<DriverRegistryKey>> KernelRegistry::snapshot() const {
  if (!Configured)
    return std::nullopt;
  std::vector<DriverRegistryKey> Result;
  for (const auto &[Path, Data] : Keys) {
    const auto &Entry = *Data;
    if (Entry.Deleted)
      continue;
    DriverRegistryKey Output{Entry.Path, {}};
    Output.Information = Entry.Information;
    Output.SubkeyOrder = Entry.SubkeyOrder;
    Output.LastWriteTime100ns = Entry.LastWriteTime100ns;
    for (const auto &Name : Entry.ValueOrder)
      Output.Values.push_back(Entry.Values.at(Name));
    Result.push_back(std::move(Output));
  }
  return Result;
}

llvm::Expected<std::optional<uint64_t>>
KernelRegistry::mutationTime(const KernelModel &Model) const {
  if (!SystemTime100ns)
    return std::optional<uint64_t>();
  if (Model.now100ns() > uint64_t(INT64_MAX) - *SystemTime100ns)
    return registryError(
        "registry system time exceeds the signed 64-bit limit");
  return std::optional<uint64_t>(*SystemTime100ns + Model.now100ns());
}

bool KernelRegistry::hasOpenHandles() const {
  return llvm::any_of(Owner.ObjectHandles, [&](const auto &Handle) {
    return KeyBodies.contains(Handle.second.Object);
  });
}

llvm::Error KernelRegistry::checkHandleCapacity() const {
  const auto Count =
      llvm::count_if(Owner.ObjectHandles, [&](const auto &Handle) {
        return KeyBodies.contains(Handle.second.Object);
      });
  if (Count >= MaxRegistryHandles)
    return registryError("registry handle count exceeds the limit");
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelRegistry::queryObjectName(KernelModel &Model, uint64_t Object,
                                uint64_t Buffer, uint32_t Length,
                                uint64_t ReturnLength, bool RequiredLength) {
  const auto &Entry = *KeyBodies.at(Object);
  if (Entry.Deleted) {
    // NtQueryObject initializes its local result length and publishes it even
    // on a failed name procedure; ObQueryNameString leaves it untouched.
    if (!RequiredLength && ReturnLength) {
      if (auto E = Model.validateObjectOutput(ReturnLength, 4))
        return E;
      if (auto E = Memory.writeInteger(ReturnLength, 0, 4))
        return E;
    }
    return KeyDeleted;
  }
  const uint32_t Required = 18 + Entry.Path.size() * 2;
  const uint32_t Written = Length >= 18 ? std::min(Length, Required) - 18 : 0;
  if (RequiredLength || ReturnLength)
    if (auto E = Model.validateObjectOutput(ReturnLength, 4))
      return E;
  if (Length >= 18) {
    // The target key-name routine preserves UNICODE_STRING padding, reports
    // MaximumLength equal to Length, and may return a truncated odd byte count.
    for (auto [Offset, Size] :
         {std::pair(0u, 4u), {8u, 8u}, {16u, Written + 2 - (Written & 1)}})
      if (auto E = Model.validateObjectOutput(Buffer + Offset, Size))
        return E;
  }
  if (Length >= Required)
    if (auto E = Model.validateObjectOutput(Buffer + 4, 4))
      return E;
  if (RequiredLength && ReturnLength)
    if (auto E = Memory.writeInteger(ReturnLength, Required, 4))
      return E;
  if (Length >= 18) {
    std::vector<uint8_t> Text(Entry.Path.size() * 2);
    storeName(Text, 0, Entry.Path);
    if (Written)
      if (auto E = Memory.write(
              Buffer + 16, llvm::ArrayRef<uint8_t>(Text).take_front(Written)))
        return E;
    if (auto E = Memory.writeInteger(Buffer + 16 + (Written & ~1u), 0, 2))
      return E;
    if (auto E = Memory.writeInteger(Buffer, Written, 2))
      return E;
    if (auto E = Memory.writeInteger(Buffer + 2, Written, 2))
      return E;
    if (auto E = Memory.writeInteger(Buffer + 8, Buffer + 16, 8))
      return E;
  }
  if (Length >= Required)
    if (auto E = Memory.writeInteger(Buffer + 4, 0, 4))
      return E;
  if (!RequiredLength && ReturnLength)
    if (auto E = Memory.writeInteger(ReturnLength, Required, 4))
      return E;
  return Length < Required ? windows::StatusInfoLengthMismatch : Success;
}

llvm::Expected<uint64_t> KernelRegistry::open(KernelModel &Model,
                                              llvm::ArrayRef<uint64_t> A,
                                              bool Create) {
  auto Access =
      Model.mapObjectAccess(KernelModel::ObjectKind::Key, uint32_t(A[1]));
  if (!Access)
    return Access.takeError();
  if (Create && (uint32_t(A[3]) || A[4] || (uint32_t(A[5]) & ~VolatileOption)))
    return registryError(
        "unsupported registry title, class, or creation options");
  auto Object = readBytes(Model, Memory, A[2], windows::ObjectAttributesSize);
  if (!Object)
    return Object.takeError();
  if (llvm::support::endian::read32le(Object->data()) !=
      windows::ObjectAttributesSize)
    return InvalidParameter;
  const uint64_t Root = llvm::support::endian::read64le(
      Object->data() + windows::ObjectRootOffset);
  const uint64_t Name = llvm::support::endian::read64le(
      Object->data() + windows::ObjectNameOffset);
  const uint32_t Flags = llvm::support::endian::read32le(
      Object->data() + windows::ObjectFlagsOffset);
  if ((Flags & ~(CaseInsensitive | windows::ObjectKernelHandle)) ||
      llvm::support::endian::read64le(Object->data() +
                                      windows::ObjectSecurityOffset) ||
      llvm::support::endian::read64le(Object->data() +
                                      windows::ObjectQualityOffset))
    return registryError(
        "unsupported registry object attributes or security policy");
  auto Text = readName(Model, Memory, Name, MaxRegistryPathLength);
  if (!Text)
    return Text.takeError();
  std::string Path = *Text;
  const KernelModel::ObjectHandle *RootHandle = nullptr;
  std::string RootPath;
  if (Root) {
    RootHandle = Model.findObjectHandle(Root);
    if (!RootHandle)
      return InvalidHandle;
    const auto Body = KeyBodies.find(RootHandle->Object);
    if (Body == KeyBodies.end())
      return windows::StatusObjectTypeMismatch;
    const Key &Parent = *Body->second;
    RootPath = folded(Parent.Path);
    if (Parent.Deleted)
      return KeyDeleted;
    if (llvm::StringRef(Path).starts_with(Separator))
      return InvalidParameter;
    Path =
        Parent.Path + (Path.empty() ? std::string() : Separator.str() + Path);
  }
  if (Path.size() > MaxRegistryPathLength)
    return registryError("registry path exceeds the length limit");
  if (!validPath(Path))
    return registryError("unsupported registry path outside the configured "
                         "namespace or with empty components");
  // OBJ_CASE_INSENSITIVE opts into the same ASCII-insensitive behavior as
  // this concrete environment's default registry name comparison policy.
  const std::string Folded = folded(Path);
  auto It = Keys.find(Folded);
  const bool Exists = It != Keys.end();
  if (!Exists) {
    if (!Create)
      return ObjectNameNotFound;
    auto Parent = Keys.find(parentPath(Folded));
    if (Parent == Keys.end())
      return ObjectPathNotFound;
    if (Parent->second->Deleted)
      return KeyDeleted;
    if (RootHandle && !(RootHandle->GrantedAccess & CreateSubKeyAccess))
      return AccessDenied;
    if (RootHandle && RootPath != Parent->first)
      return registryError(
          "relative registry creation requires an immediate parent handle");
    if (Parent->second->Volatile && !(uint32_t(A[5]) & VolatileOption))
      return ChildMustBeVolatile;
    if (Keys.size() >= MaxRegistryKeys)
      return registryError("registry key count exceeds the limit");
  }
  const auto Context = Model.executionProcessContext();
  const uint32_t ProcessID = Context ? Context->ProcessID : 0;
  if (ProcessID != windows::SystemProcessID &&
      !(Flags & windows::ObjectKernelHandle))
    return registryError(
        "registry open outside the system process requires OBJ_KERNEL_HANDLE");
  if (auto E = checkHandleCapacity())
    return E;
  if (KeyBodies.size() >= MaxRegistryHandles)
    return registryError("registry key object count exceeds the limit");
  if (Model.NextObjectHandle > profile::SystemThreadHandleLimit)
    return InsufficientResources;
  std::optional<uint64_t> CreatedAt;
  if (!Exists) {
    auto Time = mutationTime(Model);
    if (!Time)
      return Time.takeError();
    CreatedAt = *Time;
  }
  // Validate every output before writes, and finish all guest writes before
  // publishing a key or handle. A guest fault cannot leak invisible state.
  if (auto E = Model.validateObjectOutput(A[0], 8))
    return E;
  if (Create && A[6])
    if (auto E = Model.validateObjectOutput(A[6], 4))
      return E;
  auto Body = Model.allocate(profile::ProcessTokenSize);
  if (!Body)
    return Body.takeError();
  auto WriteOutputs = [&]() -> llvm::Error {
    if (Create && A[6])
      if (auto E = Memory.writeInteger(
              A[6], Exists ? OpenedExistingKey : CreatedNewKey, 4))
        return E;
    return Memory.writeInteger(A[0], Model.NextObjectHandle, 8);
  };
  if (auto E = WriteOutputs()) {
    Model.FreedRanges.emplace(*Body, profile::ProcessTokenSize);
    return E;
  }
  if (!Exists) {
    Key &Parent = *Keys.at(parentPath(Folded));
    Path = Parent.Path + Path.substr(Path.rfind(Separator));
    Keys.emplace(
        Folded, std::make_shared<Key>(Key{Path,
                                          {},
                                          {},
                                          false,
                                          bool(uint32_t(A[5]) & VolatileOption),
                                          CreatedAt}));
    Parent.LastWriteTime100ns = CreatedAt;
    Parent.MaxNameLength =
        std::max(Parent.MaxNameLength,
                 uint32_t((Path.size() - parentPath(Folded).size() - 1) * 2));
  }
  if (!Exists) {
    Keys.at(Folded)->Identity = NextKeyIdentity++;
    Keys.at(parentPath(Folded))->SubkeyOrder.reset();
  }
  KeyBodies.emplace(*Body, Keys.at(Folded));
  Model.ReferencedObjects.emplace(
      *Body, KernelModel::ObjectReferences{
                 KernelModel::ObjectKind::Key, 0, 0, {}, this});
  Model.retainObjectHandle(*Body, *Access, Flags & windows::ObjectKernelHandle,
                           ProcessID);
  return Success;
}

bool KernelRegistry::ownsHandle(uint64_t Handle) const {
  const auto H = Owner.ObjectHandles.find(Handle);
  return H != Owner.ObjectHandles.end() && KeyBodies.contains(H->second.Object);
}

std::optional<uint64_t> KernelRegistry::keyIdentity(uint64_t Handle) const {
  const auto *H = Owner.findObjectHandle(Handle);
  if (!H)
    return std::nullopt;
  auto It = KeyBodies.find(H->Object);
  return It == KeyBodies.end() ? std::nullopt
                               : std::optional<uint64_t>(It->second->Identity);
}

llvm::Expected<uint64_t> KernelRegistry::query(const KernelModel &Model,
                                               Key &Entry,
                                               llvm::ArrayRef<uint64_t> A,
                                               bool Enumerate) {
  const uint32_t Class = A[2];
  if (Class >= MaxInformationClass)
    return InvalidParameter;
  if (Class == LayerInformation)
    return registryError("registry layer information is unsupported");
  if ((Class == FullInformationAlign64 || Class == PartialInformationAlign64) &&
      (A[3] % FullDataAlignment64))
    return DatatypeMisalignment;
  std::string LookupName;
  if (Enumerate) {
    const uint32_t Index = A[1];
    if (Index >= Entry.ValueOrder.size())
      return NoMoreEntries;
    LookupName = Entry.ValueOrder[Index];
  } else {
    auto Name = readName(Model, Memory, A[1], MaxRegistryValueNameLength);
    if (!Name)
      return Name.takeError();
    LookupName = folded(*Name);
  }
  auto V = Entry.Values.find(LookupName);
  if (V == Entry.Values.end())
    return ObjectNameNotFound;
  const DriverRegistryValue &Value = V->second;
  const uint32_t NameBytes = Value.Name.size() * 2;
  const uint32_t DataSize = Value.Data.size();
  uint32_t Header;
  uint32_t Required;
  uint32_t DataStart = 0;
  switch (Class) {
  case BasicInformation:
    Header = BasicHeaderSize;
    Required = Header + NameBytes;
    break;
  case FullInformation:
  case FullInformationAlign64: {
    Header = FullHeaderSize;
    const uint32_t Alignment = Class == FullInformationAlign64
                                   ? FullDataAlignment64
                                   : FullDataAlignment;
    DataStart = DataSize
                    ? (Header + NameBytes + Alignment - 1) & ~(Alignment - 1)
                    : UINT32_MAX;
    Required = DataSize ? DataStart + DataSize : Header + NameBytes;
    break;
  }
  default:
    Header =
        Class == PartialInformation ? PartialHeaderSize : Partial64HeaderSize;
    DataStart = Header;
    Required = Header + DataSize;
    break;
  }
  const uint32_t Length = A[4];
  const uint32_t Written = Length < Header ? 0 : std::min(Length, Required);
  if (auto E = validateOutput(Model, Memory, A[5], 4))
    return E;
  if (auto E = validateOutput(Model, Memory, A[3], Written))
    return E;
  if (!Written) {
    if (auto E = Memory.writeInteger(A[5], Required, 4))
      return E;
    return BufferTooSmall;
  }
  std::vector<uint8_t> Bytes(Header, 0);
  store32(Bytes,
          Class == PartialInformationAlign64 ? Partial64TypeOffset : TypeOffset,
          Value.Type);
  switch (Class) {
  case BasicInformation:
    store32(Bytes, BasicNameLengthOffset, NameBytes);
    break;
  case FullInformation:
  case FullInformationAlign64:
    store32(Bytes, FullDataOffset, DataStart);
    store32(Bytes, FullDataLengthOffset, DataSize);
    store32(Bytes, FullNameLengthOffset, NameBytes);
    break;
  default:
    store32(Bytes,
            Class == PartialInformation ? PartialDataLengthOffset
                                        : Partial64DataLengthOffset,
            DataSize);
    break;
  }
  if (auto E = Memory.write(A[3], Bytes))
    return E;
  // The x64 kernel aligns Full data to eight bytes for both Full classes.
  // With no data it uses DataOffset=-1 and no alignment tail. ASCII names
  // occupy compressed hive names: truncation writes complete UTF-16 units.
  // Padding and any final half-unit retain the caller's bytes.
  if (Class == BasicInformation || Class == FullInformation ||
      Class == FullInformationAlign64) {
    std::vector<uint8_t> NameBytesOut(std::min(NameBytes, Written - Header) &
                                      ~1u);
    storeName(NameBytesOut, 0,
              llvm::StringRef(Value.Name).take_front(NameBytesOut.size() / 2));
    if (!NameBytesOut.empty())
      if (auto E = Memory.write(A[3] + Header, NameBytesOut))
        return E;
  }
  if (Class != BasicInformation && DataSize && Written > DataStart)
    if (auto E = Memory.write(A[3] + DataStart,
                              llvm::ArrayRef<uint8_t>(Value.Data)
                                  .take_front(Written - DataStart)))
      return E;
  // NtQueryValueKey/NtEnumerateValueKey publish ResultLength after the payload,
  // including when it aliases part of that payload.
  if (auto E = Memory.writeInteger(A[5], Required, 4))
    return E;
  return Length < Required ? BufferOverflow : Success;
}

llvm::Expected<uint64_t>
KernelRegistry::queryKey(const KernelModel &Model, const Key &Entry,
                         uint32_t Class, uint64_t Buffer, uint32_t Length,
                         uint64_t ResultLength) {
  const llvm::StringRef Path(Entry.Path);
  const auto Name = Class == KeyNameInformation
                        ? Path
                        : Path.drop_front(Path.rfind(Separator) + 1);
  uint32_t Header = 0, NameOffset = 0;
  switch (Class) {
  case KeyBasicInformation:
    Header = NameOffset = KeyBasicHeaderSize;
    break;
  case KeyNodeInformation:
    Header = NameOffset = KeyNodeHeaderSize;
    break;
  case KeyFullInformation:
    Header = KeyFullHeaderSize;
    break;
  case KeyNameInformation:
    Header = NameOffset = KeyNameHeaderSize;
    break;
  case KeyCachedInformation:
    Header = KeyCachedSize;
    break;
  default:
    return registryError("registry key information class is unsupported");
  }
  const uint32_t Required = Header + (NameOffset ? Name.size() * 2 : 0);
  const uint32_t Written = Length < Header ? 0 : std::min(Length, Required);
  if (Written && Class != KeyNameInformation && !Entry.LastWriteTime100ns)
    return registryError("registry last-write time is unspecified; supply "
                         "last_write_time_100ns or registry_system_time_100ns");
  if (auto E = validateOutput(Model, Memory, ResultLength, 4))
    return E;
  if (auto E = validateOutput(Model, Memory, Buffer, Written))
    return E;
  if (!Written) {
    if (auto E = Memory.writeInteger(ResultLength, Required, 4))
      return E;
    return BufferTooSmall;
  }
  // CachedInformation is sizeof=40 on x64; its four tail padding bytes are
  // not fields. Basic/Node store complete compressed-ASCII name units, while
  // NameInformation copies the constructed full path byte by byte.
  std::vector<uint8_t> Bytes(Class == KeyCachedInformation ? 36 : Header, 0);
  if (Class == KeyNameInformation)
    store32(Bytes, 0, Name.size() * 2);
  else
    llvm::support::endian::write64le(Bytes.data(), *Entry.LastWriteTime100ns);
  if (Class == KeyBasicInformation)
    store32(Bytes, 12, Name.size() * 2);
  if (Class == KeyNodeInformation) {
    store32(Bytes, 12, UINT32_MAX); // No key class in this registry profile.
    store32(Bytes, 20, Name.size() * 2);
  }
  if (Class == KeyFullInformation || Class == KeyCachedInformation) {
    uint32_t Subkeys = 0;
    const std::string Parent = folded(Entry.Path);
    for (const auto &[ChildPath, Child] : Keys) {
      if (Child->Deleted || parentPath(ChildPath) != Parent)
        continue;
      ++Subkeys;
    }
    if (Class == KeyFullInformation) {
      store32(Bytes, 12, UINT32_MAX);
      store32(Bytes, 20, Subkeys);
      store32(Bytes, 24, Entry.MaxNameLength);
      store32(Bytes, 32, Entry.Values.size());
      store32(Bytes, 36, Entry.MaxValueNameLength);
      store32(Bytes, 40, Entry.MaxValueDataLength);
    } else {
      store32(Bytes, 12, Subkeys);
      store32(Bytes, 16, Entry.MaxNameLength);
      store32(Bytes, 20, Entry.Values.size());
      store32(Bytes, 24, Entry.MaxValueNameLength);
      store32(Bytes, 28, Entry.MaxValueDataLength);
      store32(Bytes, 32, Name.size() * 2);
    }
  }
  if (auto E = Memory.write(Buffer, Bytes))
    return E;
  if (NameOffset) {
    std::vector<uint8_t> NameBytes(Name.size() * 2, 0);
    storeName(NameBytes, 0, Name);
    uint32_t Count = Written - NameOffset;
    if (Class != KeyNameInformation)
      Count &= ~1u;
    if (Count)
      if (auto E = Memory.write(
              Buffer + NameOffset,
              llvm::ArrayRef<uint8_t>(NameBytes).take_front(Count)))
        return E;
  }
  if (auto E = Memory.writeInteger(ResultLength, Required, 4))
    return E;
  if (Length < Required)
    return BufferOverflow;
  // NameInformation still copies the retained path for a deleted handle.
  return Entry.Deleted ? KeyDeleted : Success;
}

llvm::Expected<uint64_t> KernelRegistry::enumerate(const KernelModel &Model,
                                                   Key &Parent,
                                                   llvm::ArrayRef<uint64_t> A) {
  const uint32_t Class = uint32_t(A[2]), Index = uint32_t(A[1]),
                 Capacity = uint32_t(A[4]);
  std::vector<const Key *> Children;
  const auto ParentName = folded(Parent.Path);
  for (const auto &[Path, Entry] : Keys)
    if (!Entry->Deleted && Path != ParentName && parentPath(Path) == ParentName)
      Children.push_back(Entry.get());
  if (Index >= Children.size())
    return NoMoreEntries;
  auto It = Keys.end();
  if (!Parent.SubkeyOrder) {
    if (Children.size() != 1)
      return registryError("ZwEnumerateKey requires explicit subkey_order");
    // The only remaining child is unambiguous without any ordering policy.
    It = Keys.find(folded(Children.front()->Path));
  } else {
    if (Parent.SubkeyOrder->size() != Children.size())
      return registryError("registry child order lost its live tree identity");
    It = Keys.find(ParentName + "\\" + folded((*Parent.SubkeyOrder)[Index]));
  }
  if (It == Keys.end() || It->second->Deleted)
    return registryError("registry child order refers to an unavailable key");
  const auto &Entry = It->second;
  const std::string Name = Entry->Path.substr(Entry->Path.rfind('\\') + 1);
  const uint32_t Header = Class == 0 ? 16 : Class == 1 ? 24 : 44;
  const uint32_t Required = Header + (Class == 2 ? 0 : Name.size() * 2);
  const uint32_t Written = Capacity < Header ? 0 : std::min(Capacity, Required);
  if (Class != 0 || Written)
    if (!Entry->Information)
      return registryError("ZwEnumerateKey requires observed key information; "
                           "timestamp/class are unknown");
  std::vector<uint8_t> Bytes(Required);
  if (Written) {
    llvm::support::endian::write64le(Bytes.data(),
                                     Entry->Information->LastWriteTime);
    if (Class == 0)
      store32(Bytes, 12, Name.size() * 2);
    else {
      store32(Bytes, 12, UINT32_MAX); // Native absent class offset.
      if (Class == 1)
        store32(Bytes, 20, Name.size() * 2);
      else {
        uint32_t Count = 0, MaxName = 0;
        for (const auto &[Path, Child] : Keys) {
          if (Child->Deleted || Path == It->first ||
              parentPath(Path) != It->first)
            continue;
          if (!Child->Information)
            return registryError("ZwEnumerateKey full information requires "
                                 "child class observations");
          ++Count;
          MaxName = std::max<uint32_t>(
              MaxName, (Child->Path.size() - Entry->Path.size() - 1) * 2);
        }
        store32(Bytes, 20, Count);
        store32(Bytes, 24, MaxName);
        store32(Bytes, 32, Entry->Values.size());
        uint32_t MaxValueName = 0, MaxValueData = 0;
        for (const auto &[Key, Value] : Entry->Values) {
          MaxValueName =
              std::max<uint32_t>(MaxValueName, Value.Name.size() * 2);
          MaxValueData = std::max<uint32_t>(MaxValueData, Value.Data.size());
        }
        store32(Bytes, 36, MaxValueName);
        store32(Bytes, 40, MaxValueData);
      }
    }
    if (Class != 2) {
      // The finite ASCII hive uses native compressed names. Odd truncated
      // outputs expand whole UTF-16 characters and leave the final byte zero.
      const size_t Characters =
          std::min<size_t>(Name.size(), (Written - Header) / 2);
      storeName(Bytes, Header, llvm::StringRef(Name).take_front(Characters));
    }
  }
  auto Writable = [&](uint64_t Address, uint32_t Size) -> llvm::Error {
    if (auto E = Model.validateGuestAccess(Address, Size, true))
      return E;
    if (!Size)
      return llvm::Error::success();
    if (!Address)
      return registryError("registry output requires nonnull RAM");
    auto Pin = Memory.pinBacking(Address, Size);
    if (!Pin)
      return Pin.takeError();
    auto Access = Memory.canAccess(Address, Size, Write);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return registryError("registry output requires writable RAM");
    return llvm::Error::success();
  };
  if (auto E = Writable(A[5], 4))
    return E;
  if (Written) {
    if (auto E = Writable(A[3], Written))
      return E;
    if (A[3] < A[5] + 4 && A[5] < A[3] + Written)
      return registryError(
          "overlapping registry enumeration outputs are unsupported");
    if (auto E = Memory.write(A[3], llvm::ArrayRef(Bytes).take_front(Written)))
      return E;
  }
  if (auto E = Memory.writeInteger(A[5], Required, 4))
    return E;
  return Capacity < Header     ? BufferTooSmall
         : Capacity < Required ? BufferOverflow
                               : Success;
}

llvm::Expected<uint64_t>
KernelRegistry::enumerateKey(const KernelModel &Model, llvm::StringRef Path,
                             llvm::ArrayRef<uint64_t> A) {
  std::vector<const Key *> Children;
  for (const auto &[ChildPath, Child] : Keys)
    if (!Child->Deleted && parentPath(ChildPath) == Path)
      Children.push_back(Child.get());
  // Registry subkeys use the uppercase name collation, unlike value indices.
  llvm::sort(Children, [](const Key *L, const Key *R) {
    return std::lexicographical_compare(
        L->Path.begin(), L->Path.end(), R->Path.begin(), R->Path.end(),
        [](unsigned char A, unsigned char B) {
          return llvm::toUpper(A) < llvm::toUpper(B);
        });
  });
  if (const auto Parent = Keys.find(Path.str());
      Parent != Keys.end() && Parent->second->SubkeyOrder) {
    Children.clear();
    for (const auto &Name : *Parent->second->SubkeyOrder) {
      auto Child = Keys.find(Path.str() + "\\" + folded(Name));
      if (Child == Keys.end())
        return registryError("registry subkey_order lost its child");
      Children.push_back(Child->second.get());
    }
  }
  const uint32_t Index = A[1];
  if (Index >= Children.size())
    return NoMoreEntries;
  return queryKey(Model, *Children[Index], uint32_t(A[2]), A[3], uint32_t(A[4]),
                  A[5]);
}

llvm::Expected<uint64_t> KernelRegistry::set(const KernelModel &Model,
                                             Key &Entry,
                                             llvm::ArrayRef<uint64_t> A) {
  if (uint32_t(A[2]))
    return registryError("registry value TitleIndex must be zero");
  DriverRegistryValue Value;
  if (A[1]) {
    auto Name = readName(Model, Memory, A[1], MaxRegistryValueNameLength);
    if (!Name)
      return Name.takeError();
    Value.Name = *Name;
  }
  Value.Type = uint32_t(A[3]);
  if (auto E = validateValue(Value))
    return E;
  const uint32_t Size = A[5];
  if (Size > MaxRegistryValueBytes)
    return registryError("registry value data exceeds the byte limit");
  const std::string Name = folded(Value.Name);
  auto Old = Entry.Values.find(Name);
  const size_t OldBytes =
      Old == Entry.Values.end() ? 0 : Old->second.Data.size();
  if (Old == Entry.Values.end() && ValueCount >= MaxRegistryValues)
    return registryError("registry value count exceeds the limit");
  if (DataBytes - OldBytes + Size > MaxRegistryTotalBytes)
    return registryError("registry total value bytes exceed the limit");
  auto Bytes = readBytes(Model, Memory, A[4], Size);
  if (!Bytes)
    return Bytes.takeError();
  // The unchanged-value fast path does not update the kernel's timestamp.
  if (Old != Entry.Values.end() && Old->second.Type == Value.Type &&
      Old->second.Data == *Bytes)
    return Success;
  auto Time = mutationTime(Model);
  if (!Time)
    return Time.takeError();
  Value.Data = std::move(*Bytes);
  if (Old != Entry.Values.end())
    Value.Name = Old->second.Name;
  else {
    ++ValueCount;
    Entry.ValueOrder.push_back(Name);
  }
  DataBytes = DataBytes - OldBytes + Size;
  Entry.Values.insert_or_assign(Name, std::move(Value));
  Entry.MaxValueNameLength =
      std::max(Entry.MaxValueNameLength, uint32_t(Name.size() * 2));
  Entry.MaxValueDataLength = std::max(Entry.MaxValueDataLength, Size);
  Entry.LastWriteTime100ns = *Time;
  if (ObservedEnumeration)
    Entry.Information.reset();
  return Success;
}

llvm::Expected<uint64_t> KernelRegistry::call(llvm::StringRef Name,
                                              llvm::ArrayRef<uint64_t> A) {
  auto &Model = Owner;
  const auto *Descriptor = lookupAPI(Name);
  if (!Descriptor || A.size() != Descriptor->ArgumentCount)
    return registryError("unknown registry API or incorrect argument count");
  const API Function = Descriptor->Function;
  if (!Configured)
    return registryError("registry availability is unspecified; supply an "
                         "explicit registry scenario");
  if (Function == API::ZwEnumerateValueKey &&
      uint32_t(A[2]) > PartialInformation)
    return InvalidParameter;
  if (Function == API::ZwEnumerateKey && uint32_t(A[2]) > KeyFullInformation)
    return InvalidParameter;
  if (Function == API::ZwQueryKey && uint32_t(A[1]) >= KeyMaxInformationClass)
    return InvalidParameter;
  if (Function == API::ZwOpenKey || Function == API::ZwCreateKey)
    return open(Model, A, Function == API::ZwCreateKey);
  const auto H = Model.findObjectHandle(A[0]);
  if (!H)
    return InvalidHandle;
  const auto Body = KeyBodies.find(H->Object);
  if (Body == KeyBodies.end())
    return windows::StatusObjectTypeMismatch;
  const auto Data = Body->second;
  Key &Entry = *Data;
  const std::string Path = folded(Entry.Path);
  if (Function == API::ZwClose)
    return Model.closeHandle(A[0]);
  if (Entry.Deleted && Function != API::ZwDeleteKey &&
      !(Function == API::ZwQueryKey && uint32_t(A[1]) == KeyNameInformation))
    return KeyDeleted;
  // Zw uses KernelMode object reference with zero desired access in the
  // pinned native NtFlushKey body. It does not require KEY_SET_VALUE.
  if (Function == API::ZwFlushKey)
    return flush(Entry);
  uint32_t Required =
      (Function == API::ZwQueryValueKey ||
       Function == API::ZwEnumerateValueKey || Function == API::ZwQueryKey)
          ? QueryValueAccess
          : SetValueAccess;
  if (Function == API::ZwDeleteKey)
    Required = DeleteAccess;
  if (Function == API::ZwEnumerateKey)
    Required = EnumerateSubKeysAccess;
  if (Function == API::ZwQueryKey && uint32_t(A[1]) == KeyNameInformation)
    Required =
        windows::KeyAllAccess; // Any granted key access suffices for its name.
  if (!(H->GrantedAccess & Required))
    return AccessDenied;
  switch (Function) {
  case API::ZwQueryKey:
    return queryKey(Model, Entry, uint32_t(A[1]), A[2], uint32_t(A[3]), A[4]);
  case API::ZwEnumerateKey:
    return ObservedEnumeration ? enumerate(Model, Entry, A)
                               : enumerateKey(Model, Path, A);
  case API::ZwQueryValueKey:
  case API::ZwEnumerateValueKey:
    return query(Model, Entry, A, Function == API::ZwEnumerateValueKey);
  case API::ZwSetValueKey:
    return set(Model, Entry, A);
  case API::ZwDeleteValueKey: {
    auto ValueName = readName(Model, Memory, A[1], MaxRegistryValueNameLength);
    if (!ValueName)
      return ValueName.takeError();
    auto Value = Entry.Values.find(folded(*ValueName));
    if (Value == Entry.Values.end())
      return ObjectNameNotFound;
    auto Time = mutationTime(Model);
    if (!Time)
      return Time.takeError();
    DataBytes -= Value->second.Data.size();
    --ValueCount;
    std::erase(Entry.ValueOrder, Value->first);
    Entry.Values.erase(Value);
    if (Entry.Values.empty()) {
      Entry.MaxValueNameLength = 0;
      Entry.MaxValueDataLength = 0;
    }
    Entry.LastWriteTime100ns = *Time;
    return Success;
  }
  case API::ZwDeleteKey: {
    if (Entry.Deleted)
      return Success;
    if (rootPath(Path))
      return CannotDelete;
    if (Hives && llvm::any_of(*Hives, [&](const auto &H) {
          return folded(H.Path) == Path;
        }))
      return CannotDelete;
    for (const auto &[OtherPath, Other] : Keys)
      if (!Other->Deleted && OtherPath != Path && atOrBelow(OtherPath, Path))
        return CannotDelete;
    auto Time = mutationTime(Model);
    if (!Time)
      return Time.takeError();
    for (const auto &[ValueName, Value] : Entry.Values) {
      --ValueCount;
      DataBytes -= Value.Data.size();
    }
    Entry.Values.clear();
    Entry.ValueOrder.clear();
    Entry.Deleted = true;
    Keys.erase(Path);
    const auto ParentPath = parentPath(Path);
    Key &Parent = *Keys.at(ParentPath);
    if (ObservedEnumeration)
      Parent.SubkeyOrder.reset();
    Parent.LastWriteTime100ns = *Time;
    if (llvm::none_of(Keys, [&](const auto &Other) {
          return !Other.second->Deleted &&
                 parentPath(Other.first) == ParentPath;
        }))
      Parent.MaxNameLength = 0;
    return Success;
  }
  default:
    llvm_unreachable("handled registry API");
  }
}
} // namespace neverd::emulation
