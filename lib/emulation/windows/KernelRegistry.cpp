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
/// a concrete capability model, not an ACL, privilege, hive, or host OS model.
///
//===----------------------------------------------------------------------===//

#include "KernelRegistry.h"

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
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
    const std::optional<std::vector<DriverRegistryKey>> &Registry) {
  if (!Registry)
    return llvm::Error::success();
  if (Registry->size() > MaxRegistryKeys)
    return registryError("registry key count exceeds the limit");
  std::set<std::string> ExplicitPaths;
  std::set<std::string> AllPaths;
  size_t Values = 0;
  size_t Bytes = 0;
  for (const DriverRegistryKey &Key : *Registry) {
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
  return llvm::Error::success();
}

llvm::Error KernelRegistry::initialize(
    const std::optional<std::vector<DriverRegistryKey>> &Registry) {
  if (auto E = validateDriverRegistry(Registry))
    return E;
  Configured = Registry.has_value();
  Keys.clear();
  Handles.clear();
  NextHandle = HandleBase;
  ValueCount = 0;
  DataBytes = 0;
  if (!Registry)
    return llvm::Error::success();
  for (const DriverRegistryKey &Input : *Registry) {
    std::string Path = Input.Path;
    while (true) {
      const std::string Folded = folded(Path);
      Keys.try_emplace(Folded, Key{Path, {}, false, false});
      if (rootPath(Folded))
        break;
      Path = parentPath(Path);
    }
    Key &Entry = Keys.at(folded(Input.Path));
    // Prefer the explicit key spelling over an implicit ancestor spelling.
    Entry.Path = Input.Path;
    for (const DriverRegistryValue &Value : Input.Values) {
      Entry.Values.emplace(folded(Value.Name), Value);
      ++ValueCount;
      DataBytes += Value.Data.size();
    }
  }
  return llvm::Error::success();
}

std::optional<std::vector<DriverRegistryKey>> KernelRegistry::snapshot() const {
  if (!Configured)
    return std::nullopt;
  std::vector<DriverRegistryKey> Result;
  for (const auto &[Path, Entry] : Keys) {
    if (Entry.Deleted)
      continue;
    DriverRegistryKey Output{Entry.Path, {}};
    for (const auto &[Name, Value] : Entry.Values)
      Output.Values.push_back(Value);
    Result.push_back(std::move(Output));
  }
  return Result;
}

llvm::Expected<uint64_t> KernelRegistry::open(const KernelModel &Model,
                                              llvm::ArrayRef<uint64_t> A,
                                              bool Create) {
  const uint32_t Access = A[1];
  if (Access & ~AllAccess)
    return registryError("unsupported registry access mask (generic, "
                         "maximum-allowed, or alternate-view access)");
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
  const Handle *RootHandle = nullptr;
  if (Root) {
    auto H = Handles.find(Root);
    if (H == Handles.end())
      return InvalidHandle;
    RootHandle = &H->second;
    const Key &Parent = Keys.at(H->second.Path);
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
  if (Exists && It->second.Deleted)
    return KeyDeleted;
  if (!Exists) {
    if (!Create)
      return ObjectNameNotFound;
    auto Parent = Keys.find(parentPath(Folded));
    if (Parent == Keys.end())
      return ObjectPathNotFound;
    if (Parent->second.Deleted)
      return KeyDeleted;
    if (RootHandle && !(RootHandle->Access & CreateSubKeyAccess))
      return AccessDenied;
    if (RootHandle && RootHandle->Path != Parent->first)
      return registryError(
          "relative registry creation requires an immediate parent handle");
    if (Parent->second.Volatile && !(uint32_t(A[5]) & VolatileOption))
      return ChildMustBeVolatile;
    if (Keys.size() >= MaxRegistryKeys)
      return registryError("registry key count exceeds the limit");
  }
  if (Handles.size() >= MaxRegistryHandles ||
      NextHandle > UINT64_MAX - HandleStride)
    return registryError("registry handle count exceeds the limit");
  // Validate every output before writes, and finish all guest writes before
  // publishing a key or handle. A guest fault cannot leak invisible state.
  if (auto E = Model.validateGuestAccess(A[0], 8, true))
    return E;
  if (Create && A[6])
    if (auto E = Model.validateGuestAccess(A[6], 4, true))
      return E;
  if (Create && A[6])
    if (auto E = Memory.writeInteger(
            A[6], Exists ? OpenedExistingKey : CreatedNewKey, 4))
      return E;
  if (auto E = Memory.writeInteger(A[0], NextHandle, 8))
    return E;
  if (!Exists)
    Keys.emplace(Folded,
                 Key{Path, {}, false, bool(uint32_t(A[5]) & VolatileOption)});
  Handles.emplace(NextHandle, Handle{Folded, Access});
  NextHandle += HandleStride;
  return Success;
}

llvm::Expected<uint64_t> KernelRegistry::query(const KernelModel &Model,
                                               Key &Entry,
                                               llvm::ArrayRef<uint64_t> A) {
  const uint32_t Class = A[2];
  if (Class >= MaxInformationClass)
    return InvalidParameter;
  if (Class == LayerInformation)
    return registryError("registry layer information is unsupported");
  if ((Class == FullInformationAlign64 || Class == PartialInformationAlign64) &&
      (A[3] % FullDataAlignment64))
    return DatatypeMisalignment;
  auto Name = readName(Model, Memory, A[1], MaxRegistryValueNameLength);
  if (!Name)
    return Name.takeError();
  auto V = Entry.Values.find(folded(*Name));
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
    DataStart = (Header + NameBytes + Alignment - 1) & ~(Alignment - 1);
    Required = DataStart + DataSize;
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
  if (auto E = Model.validateGuestAccess(A[5], 4, true))
    return E;
  if (auto E = Model.validateGuestAccess(A[3], Written, true))
    return E;
  if (auto E = Memory.writeInteger(A[5], Required, 4))
    return E;
  if (!Written)
    return BufferTooSmall;
  std::vector<uint8_t> Bytes(Required, 0);
  store32(Bytes,
          Class == PartialInformationAlign64 ? Partial64TypeOffset : TypeOffset,
          Value.Type);
  switch (Class) {
  case BasicInformation:
    store32(Bytes, BasicNameLengthOffset, NameBytes);
    storeName(Bytes, Header, Value.Name);
    break;
  case FullInformation:
  case FullInformationAlign64:
    store32(Bytes, FullDataOffset, DataStart);
    store32(Bytes, FullDataLengthOffset, DataSize);
    store32(Bytes, FullNameLengthOffset, NameBytes);
    storeName(Bytes, Header, Value.Name);
    break;
  default:
    store32(Bytes,
            Class == PartialInformation ? PartialDataLengthOffset
                                        : Partial64DataLengthOffset,
            DataSize);
    break;
  }
  if (Class != BasicInformation)
    std::copy(Value.Data.begin(), Value.Data.end(), Bytes.begin() + DataStart);
  if (auto E = Memory.write(A[3],
                            llvm::ArrayRef<uint8_t>(Bytes).take_front(Written)))
    return E;
  return Length < Required ? BufferOverflow : Success;
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
  Value.Data = std::move(*Bytes);
  if (Old != Entry.Values.end())
    Value.Name = Old->second.Name;
  else
    ++ValueCount;
  DataBytes = DataBytes - OldBytes + Size;
  Entry.Values.insert_or_assign(Name, std::move(Value));
  return Success;
}

llvm::Expected<uint64_t> KernelRegistry::call(const KernelModel &Model,
                                              llvm::StringRef Name,
                                              llvm::ArrayRef<uint64_t> A) {
  API Function = API::Unknown;
  size_t Count = 0;
#define NEVERD_KERNEL_REGISTRY_API(Symbol, Arguments)                          \
  if (Name == #Symbol) {                                                       \
    Function = API::Symbol;                                                    \
    Count = Arguments;                                                         \
  }
#include "KernelRegistryAPIs.def"
#undef NEVERD_KERNEL_REGISTRY_API
  if (Function == API::Unknown || A.size() != Count)
    return registryError("unknown registry API or incorrect argument count");
  if (!Configured)
    return registryError("registry availability is unspecified; supply an "
                         "explicit registry scenario");
  if (Function == API::ZwOpenKey || Function == API::ZwCreateKey)
    return open(Model, A, Function == API::ZwCreateKey);
  auto H = Handles.find(A[0]);
  if (H == Handles.end())
    return InvalidHandle;
  const std::string Path = H->second.Path;
  Key &Entry = Keys.at(Path);
  if (Function == API::ZwClose) {
    Handles.erase(H);
    if (Entry.Deleted && llvm::none_of(Handles, [&](const auto &Other) {
          return Other.second.Path == Path;
        }))
      Keys.erase(Path);
    return Success;
  }
  if (Entry.Deleted)
    return KeyDeleted;
  uint32_t Required =
      Function == API::ZwQueryValueKey ? QueryValueAccess : SetValueAccess;
  if (Function == API::ZwDeleteKey)
    Required = DeleteAccess;
  if (!(H->second.Access & Required))
    return AccessDenied;
  switch (Function) {
  case API::ZwQueryValueKey:
    return query(Model, Entry, A);
  case API::ZwSetValueKey:
    return set(Model, Entry, A);
  case API::ZwDeleteValueKey: {
    auto ValueName = readName(Model, Memory, A[1], MaxRegistryValueNameLength);
    if (!ValueName)
      return ValueName.takeError();
    auto Value = Entry.Values.find(folded(*ValueName));
    if (Value == Entry.Values.end())
      return ObjectNameNotFound;
    DataBytes -= Value->second.Data.size();
    --ValueCount;
    Entry.Values.erase(Value);
    return Success;
  }
  case API::ZwDeleteKey:
    if (rootPath(Path))
      return CannotDelete;
    for (const auto &[OtherPath, Other] : Keys)
      if (!Other.Deleted && OtherPath != Path && atOrBelow(OtherPath, Path))
        return CannotDelete;
    for (const auto &[ValueName, Value] : Entry.Values) {
      --ValueCount;
      DataBytes -= Value.Data.size();
    }
    Entry.Values.clear();
    Entry.Deleted = true;
    return Success;
  default:
    llvm_unreachable("handled registry API");
  }
}
} // namespace neverd::emulation
