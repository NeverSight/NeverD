//===- KernelModel.cpp - Windows objects and APIs -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows initialization and API model.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

#include "DriverImage.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

namespace neverd::emulation {
namespace {

using namespace windows;

#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name(Value);
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

enum class KernelAPIKind {
#define NEVERD_KERNEL_API(Symbol, Arity) Symbol,
#include "KernelAPIs.def"
#undef NEVERD_KERNEL_API
};

struct KernelAPIContract {
  llvm::StringLiteral Name;
  KernelAPIKind Kind;
  unsigned Arity;
};

const KernelAPIContract *lookupKernelAPI(llvm::StringRef Name) {
  static constexpr KernelAPIContract APIs[] = {
#define NEVERD_KERNEL_API(Symbol, Arity)                                       \
  {#Symbol, KernelAPIKind::Symbol, Arity},
#include "KernelAPIs.def"
#undef NEVERD_KERNEL_API
  };
  for (const auto &API : APIs)
    if (Name == API.Name)
      return &API;
  return nullptr;
}

llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

llvm::Error checkRange(uint64_t Address, uint64_t Size) {
  if (Size && (!Address || Size - 1 > UINT64_MAX - Address))
    return modelError("invalid or overflowing guest buffer");
  if (Size > profile::KernelArenaSize)
    return modelError("API buffer exceeds the 1 MiB model operation limit");
  return llvm::Error::success();
}

llvm::Error writeBytes(GuestMemory &Memory, uint64_t Address, uint64_t Size,
                       uint8_t Value = 0) {
  if (auto E = checkRange(Address, Size))
    return E;
  if (!Size)
    return llvm::Error::success();
  return Memory.write(Address, std::vector<uint8_t>(Size, Value));
}

std::string foldedASCII(std::string Text) {
  for (char &C : Text)
    if (C >= 'a' && C <= 'z')
      C -= 'a' - 'A';
  return Text;
}

bool singleComponent(llvm::StringRef Name, llvm::StringRef Prefix) {
  return Name.starts_with(Prefix) && Name.size() > Prefix.size() &&
         !Name.drop_front(Prefix.size()).contains('\\');
}

llvm::Expected<std::string> linkKey(const std::string &Name) {
  std::string Key = foldedASCII(Name);
  if (singleComponent(Key, DosDevicesPrefix))
    return DosDeviceAliasPrefix.str() + Key.substr(DosDevicesPrefix.size());
  if (singleComponent(Key, DosDeviceAliasPrefix))
    return Key;
  return modelError("symbolic-link model supports only \\DosDevices\\Name or "
                    "\\??\\Name in one session namespace");
}

// No printf implementation is delegated to the host. This initial profile
// accepts only a format that consumes no variadic arguments.
llvm::Expected<std::string> readDebugLiteral(const KernelModel &Model,
                                             GuestMemory &Memory,
                                             uint64_t Address) {
  if (auto E = checkRange(Address, MaxDebugFormatBytes + 1))
    return E;
  std::string Text;
  bool Percent = false;
  for (unsigned I = 0; I <= MaxDebugFormatBytes; ++I) {
    if (auto E = Model.validateGuestAccess(Address + I, 1, false))
      return E;
    auto Byte = Memory.readInteger(Address + I, 1);
    if (!Byte)
      return Byte.takeError();
    if (!*Byte) {
      if (Percent)
        return modelError("DbgPrint ends with an incomplete format specifier");
      return Text;
    }
    if (*Byte > 0x7f)
      return modelError("DbgPrint model supports ASCII literals only");
    if (Percent) {
      if (*Byte != '%')
        return modelError("DbgPrint model does not support variadic formats");
      Percent = false;
    } else if (*Byte == '%') {
      Percent = true;
      continue;
    }
    if (Text.size() == MaxDebugBytes)
      return modelError("DbgPrint literal exceeds the 512-byte model limit");
    Text.push_back(static_cast<char>(*Byte));
  }
  return modelError("unterminated DbgPrint format within the model limit");
}

} // namespace

llvm::Expected<uint64_t> KernelModel::allocate(uint64_t Size,
                                               uint64_t Alignment) {
  if (!Size || Size > profile::KernelArenaSize)
    return modelError("invalid model allocation size");
  const uint64_t Start = (NextAllocation + Alignment - 1) & ~(Alignment - 1);
  if (Start > AllocationEnd || Size > AllocationEnd - Start)
    return modelError("Windows initialization model arena exhausted");
  if (auto E = writeBytes(Memory, Start, Size))
    return E;
  ArenaAllocations.emplace(Start, Size);
  NextAllocation = Start + Size;
  return Start;
}

llvm::Expected<uint64_t>
KernelModel::makeUnicodeString(const std::string &Text) {
  if (Text.size() > MaxUnicodeBytes / 2)
    return modelError("initialization string exceeds UNICODE_STRING capacity");
  auto Record = allocate(16 + (Text.size() + 1) * 2);
  if (!Record)
    return Record.takeError();
  std::vector<uint8_t> Bytes((Text.size() + 1) * 2);
  for (size_t I = 0; I < Text.size(); ++I) {
    if (static_cast<unsigned char>(Text[I]) > 0x7f || !Text[I])
      return modelError("initialization strings must be non-null ASCII");
    Bytes[I * 2] = static_cast<uint8_t>(Text[I]);
  }
  if (auto E = Memory.write(*Record + 16, Bytes))
    return E;
  if (auto E = Memory.writeInteger(*Record, Text.size() * 2, 2))
    return E;
  if (auto E = Memory.writeInteger(*Record + 2, (Text.size() + 1) * 2, 2))
    return E;
  if (auto E = Memory.writeInteger(*Record + 8, *Record + 16, 8))
    return E;
  return *Record;
}

llvm::Error KernelModel::initialize(const DriverImage &Image,
                                    const DriverOptions &Options) {
  if (DriverObject)
    return modelError("kernel model cannot be initialized twice");
  if (auto E = Memory.map(profile::KernelArenaBase, profile::KernelArenaSize,
                          Read | Write))
    return E;
  NextAllocation = profile::KernelArenaBase;
  AllocationEnd = profile::KernelArenaBase + profile::KernelArenaSize;
  auto Object = allocate(DriverObjectSize);
  if (!Object)
    return Object.takeError();
  DriverObject = *Object;
  auto Extension = allocate(DriverExtensionSize);
  if (!Extension)
    return Extension.takeError();
  DriverExtension = *Extension;
  auto Path =
      makeUnicodeString(RegistryServicesPrefix.str() + Options.ServiceName);
  if (!Path)
    return Path.takeError();
  RegistryPath = *Path;
  auto Name = makeUnicodeString(DriverPrefix.str() + Options.ServiceName);
  if (!Name)
    return Name.takeError();
  auto Hardware = makeUnicodeString(RegistryHardwarePath.str());
  if (!Hardware)
    return Hardware.takeError();
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  for (const Field &F :
       std::array<Field, 7>{{{0, DriverType, 2},
                             {2, DriverObjectSize, 2},
                             {DriverStartOffset, Image.Base, 8},
                             {DriverImageSizeOffset, Image.Size, 4},
                             {DriverExtensionOffset, DriverExtension, 8},
                             {DriverHardwareOffset, *Hardware, 8},
                             {DriverInitOffset, Image.Entry, 8}}})
    if (auto E = Memory.writeInteger(DriverObject + F.Offset, F.Value, F.Size))
      return E;
  std::array<uint8_t, 16> StringRecord{};
  if (auto E = Memory.read(*Name, StringRecord))
    return E;
  if (auto E = Memory.write(DriverObject + DriverNameOffset, StringRecord))
    return E;
  if (auto E = Memory.writeInteger(DriverExtension, DriverObject, 8))
    return E;
  Result.DriverObject = DriverObject;
  return llvm::Error::success();
}

llvm::Error KernelModel::finishEntry() {
  if (!DriverObject || !RegistryPath || EntryFinished || Request || Unloading)
    return modelError(
        "DriverEntry lifetime can end only once after initialization");
  const auto Allocation = ArenaAllocations.find(RegistryPath);
  if (Allocation == ArenaAllocations.end())
    return modelError("RegistryPath has no owned guest allocation");
  // The WDK requires the driver to copy RegistryPath's contents if it needs
  // them later: the I/O manager releases this input after DriverEntry returns.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nc-wdm-driver_initialize
  FreedRanges.emplace(RegistryPath, Allocation->second);
  EntryFinished = true;
  return llvm::Error::success();
}

std::optional<unsigned> KernelModel::argumentCount(const std::string &Name) {
  if (const auto *API = lookupKernelAPI(Name))
    return API->Arity;
  return std::nullopt;
}

llvm::Expected<std::string> KernelModel::readObjectName(uint64_t Address) {
  if (auto E = checkRange(Address, 16))
    return E;
  if (auto E = validateGuestAccess(Address, 16, false))
    return E;
  auto Length = Memory.readInteger(Address, 2);
  if (!Length)
    return Length.takeError();
  auto Maximum = Memory.readInteger(Address + 2, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(Address + 8, 8);
  if (!Buffer)
    return Buffer.takeError();
  if ((*Length & 1) || *Length > *Maximum ||
      *Length > profile::MaxDeviceNameSize * 2)
    return modelError("invalid or oversized object-name UNICODE_STRING");
  if (auto E = checkRange(*Buffer, *Length))
    return E;
  if (auto E = validateGuestAccess(*Buffer, *Length, false))
    return E;
  std::vector<uint8_t> Bytes(*Length);
  if (!Bytes.empty())
    if (auto E = Memory.read(*Buffer, Bytes))
      return E;
  std::string Name;
  for (size_t I = 0; I < Bytes.size(); I += 2) {
    if (Bytes[I + 1] || Bytes[I] < 0x20 || Bytes[I] > 0x7e)
      return modelError("object-name model supports printable ASCII only; "
                        "Unicode namespace case folding is unsupported");
    Name.push_back(static_cast<char>(Bytes[I]));
  }
  return Name;
}

llvm::Expected<uint64_t> KernelModel::createDevice(llvm::ArrayRef<uint64_t> A) {
  if (A[0] != DriverObject)
    return modelError("IoCreateDevice received an unknown DRIVER_OBJECT");
  const uint64_t ExtensionSize = static_cast<uint32_t>(A[1]);
  const uint32_t Type = static_cast<uint32_t>(A[3]);
  const uint32_t Characteristics = static_cast<uint32_t>(A[4]);
  if (Type != UnknownDeviceType || (Characteristics & ~uint32_t(SecureOpen)))
    return modelError(
        "IoCreateDevice model supports FILE_DEVICE_UNKNOWN and "
        "FILE_DEVICE_SECURE_OPEN only; hardware/PnP is unsupported");
  if (ExtensionSize > UINT16_MAX - DeviceObjectSize)
    return modelError(
        "device extension exceeds the bounded DEVICE_OBJECT size");
  if (auto E = checkRange(A[6], 8))
    return E;
  std::string Name;
  if (A[2]) {
    auto Parsed = readObjectName(A[2]);
    if (!Parsed)
      return Parsed.takeError();
    Name = *Parsed;
    const std::string Key = foldedASCII(Name);
    if (!singleComponent(Key, DevicePrefix))
      return modelError("device-name model supports \\Device\\Name only");
    for (const auto &[Address, Existing] : Devices)
      if (foldedASCII(Existing.Name) == Key)
        return StatusObjectNameCollision;
  }
  auto Previous = Memory.readInteger(DriverObject + DriverDeviceHead, 8);
  if (!Previous)
    return Previous.takeError();
  if (*Previous && !Devices.count(*Previous))
    return modelError("DRIVER_OBJECT device list was corrupted");
  const uint64_t Size = DeviceObjectSize + ExtensionSize;
  const uint64_t Start = (NextAllocation + 15) & ~uint64_t(15);
  if (Start > AllocationEnd || Size > AllocationEnd - Start)
    return StatusInsufficientResources;
  auto Object = allocate(Size);
  if (!Object)
    return Object.takeError();
  const uint64_t Extension = *Object + DeviceObjectSize;
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  for (const Field &F : std::array<Field, 10>{
           {{0, DeviceType, 2},
            {2, Size, 2},
            {DeviceDriverOffset, DriverObject, 8},
            {DeviceNext, *Previous, 8},
            {DeviceFlagsOffset,
             DeviceInitializing |
                 (static_cast<uint8_t>(A[5]) ? DeviceExclusive : 0),
             4},
            {DeviceCharacteristicsOffset, Characteristics, 4},
            {DeviceExtensionOffset, Extension, 8},
            {DeviceTypeOffset, Type, 4},
            {DeviceStackCountOffset, 1, 1},
            {DeviceAlignmentOffset, DeviceAlignmentMask, 4}}})
    if (auto E = Memory.writeInteger(*Object + F.Offset, F.Value, F.Size))
      return E;
  if (auto E = validateGuestAccess(A[6], 8, true))
    return E;
  if (auto E = Memory.writeInteger(A[6], *Object, 8))
    return E;
  if (auto E = Memory.writeInteger(DriverObject + DriverDeviceHead, *Object, 8))
    return E;
  Devices.emplace(*Object, DriverDevice{*Object, Extension, Type, Name});
  DeviceSizes.emplace(*Object, Size);
  return StatusSuccess;
}

llvm::Error KernelModel::deleteDevice(uint64_t Address) {
  if (FileObject && FileDevice == Address)
    return modelError("cannot delete a device while its file object is live");
  auto It = Devices.find(Address);
  if (It == Devices.end())
    return modelError("IoDeleteDevice received an unknown or deleted device");
  uint64_t Link = DriverObject + DriverDeviceHead;
  std::set<uint64_t> Seen;
  while (true) {
    auto Current = Memory.readInteger(Link, 8);
    if (!Current)
      return Current.takeError();
    if (!*Current || !Devices.count(*Current) || !Seen.insert(*Current).second)
      return modelError("device missing from, or cycle in, DRIVER_OBJECT list");
    auto Next = Memory.readInteger(*Current + DeviceNext, 8);
    if (!Next)
      return Next.takeError();
    if (*Current == Address) {
      if (auto E = Memory.writeInteger(Link, *Next, 8))
        return E;
      FreedRanges.emplace(Address, DeviceSizes.at(Address));
      DeviceSizes.erase(Address);
      Devices.erase(It);
      return llvm::Error::success();
    }
    Link = *Current + DeviceNext;
  }
}

llvm::Expected<uint64_t> KernelModel::call(const std::string &Name,
                                           llvm::ArrayRef<uint64_t> A) {
  const auto *API = lookupKernelAPI(Name);
  if (!API || A.size() != API->Arity)
    return modelError("unknown API or incorrect argument count: " + Name);
  const auto Kind = API->Kind;
  if (!DriverObject)
    return modelError("kernel model has not been initialized");
  if (Kind == KernelAPIKind::IofCompleteRequest ||
      Kind == KernelAPIKind::IoCompleteRequest) {
    if (auto E = completeRequest(A[0], static_cast<uint8_t>(A[1])))
      return E;
    return 0;
  }
  if (Kind == KernelAPIKind::IoGetCurrentIrpStackLocation) {
    if (!Request || Request->IRP != A[0] || Request->Completed)
      return modelError(
          "IoGetCurrentIrpStackLocation requires the live request IRP");
    return Request->Stack;
  }

  if (Kind == KernelAPIKind::RtlInitUnicodeString) {
    if (auto E = checkRange(A[0], 16))
      return E;
    uint64_t Length = 0;
    if (A[1]) {
      if (auto E = checkRange(A[1], MaxUnicodeBytes + 2))
        return E;
      while (Length <= MaxUnicodeBytes) {
        if (auto E = validateGuestAccess(A[1] + Length, 2, false))
          return E;
        auto Unit = Memory.readInteger(A[1] + Length, 2);
        if (!Unit)
          return Unit.takeError();
        if (!*Unit)
          break;
        Length += 2;
      }
      if (Length > MaxUnicodeBytes)
        return modelError("RtlInitUnicodeString source exceeds model capacity");
    }
    if (auto E = validateGuestAccess(A[0], 4, true))
      return E;
    if (auto E = validateGuestAccess(A[0] + 8, 8, true))
      return E;
    if (auto E = Memory.writeInteger(A[0], Length, 2))
      return E;
    if (auto E = Memory.writeInteger(A[0] + 2, A[1] ? Length + 2 : 0, 2))
      return E;
    if (auto E = Memory.writeInteger(A[0] + 8, A[1], 8))
      return E;
    return 0;
  }
  if (Kind == KernelAPIKind::ExAllocatePoolWithTag) {
    const uint32_t Type = static_cast<uint32_t>(A[0]);
    const uint32_t Tag = static_cast<uint32_t>(A[2]);
    if (Type != 0 && Type != 1 && Type != PoolNX)
      return modelError("pool model supports NonPagedPool, PagedPool and "
                        "NonPagedPoolNx without additional flags");
    if (!A[1] || !Tag)
      return modelError("pool allocation requires non-zero size and tag");
    // Small allocations must not cross a page; large allocations are page
    // aligned. The arena intentionally does not recycle addresses in one run.
    uint64_t Alignment = 16;
    uint64_t Start = (NextAllocation + 15) & ~uint64_t(15);
    if (A[1] >= profile::PageSize ||
        (Start & (profile::PageSize - 1)) + A[1] > profile::PageSize) {
      Alignment = profile::PageSize;
      Start = (NextAllocation + (profile::PageSize - 1)) &
              ~uint64_t((profile::PageSize - 1));
    }
    if (Start > AllocationEnd || A[1] > AllocationEnd - Start)
      return 0;
    auto Pointer = allocate(A[1], Alignment);
    if (!Pointer)
      return Pointer.takeError();
    // ExAllocatePoolWithTag is uninitialized, not zero-initialized. This is one
    // deterministic concrete memory scenario, never host heap contents.
    if (auto E = writeBytes(Memory, *Pointer, A[1], UninitializedPoolByte))
      return E;
    Allocations.emplace(*Pointer, PoolAllocation{A[1], Tag});
    return *Pointer;
  }
  if (Kind == KernelAPIKind::ExFreePoolWithTag ||
      Kind == KernelAPIKind::ExFreePool) {
    auto It = Allocations.find(A[0]);
    if (It == Allocations.end())
      return modelError(
          "pool free received an unknown or already freed pointer");
    if (Kind == KernelAPIKind::ExFreePoolWithTag &&
        static_cast<uint32_t>(A[1]) != It->second.Tag)
      return modelError("ExFreePoolWithTag tag does not match allocation");
    if (auto E = writeBytes(Memory, A[0], It->second.Size, FreedPoolByte))
      return E;
    FreedRanges.emplace(A[0], It->second.Size);
    Allocations.erase(It);
    return 0;
  }
  if (Kind == KernelAPIKind::IoCreateDevice)
    return createDevice(A);
  if (Kind == KernelAPIKind::IoDeleteDevice) {
    if (auto E = deleteDevice(A[0]))
      return E;
    return 0;
  }
  if (Kind == KernelAPIKind::IoCreateSymbolicLink ||
      Kind == KernelAPIKind::IoDeleteSymbolicLink) {
    auto ObjectName = readObjectName(A[0]);
    if (!ObjectName)
      return ObjectName.takeError();
    auto Key = linkKey(*ObjectName);
    if (!Key)
      return Key.takeError();
    if (Kind == KernelAPIKind::IoDeleteSymbolicLink)
      return SymbolicLinks.erase(*Key) ? StatusSuccess
                                       : StatusObjectNameNotFound;
    auto Target = readObjectName(A[1]);
    if (!Target)
      return Target.takeError();
    if (!singleComponent(foldedASCII(*Target), DevicePrefix))
      return modelError("symbolic-link targets must be \\Device\\Name");
    if (SymbolicLinks.count(*Key))
      return StatusObjectNameCollision;
    SymbolicLinks.emplace(*Key, *Target);
    return StatusSuccess;
  }
  if (Kind == KernelAPIKind::DbgPrint || Kind == KernelAPIKind::DbgPrintEx) {
    auto Text = readDebugLiteral(*this, Memory,
                                 A[Kind == KernelAPIKind::DbgPrint ? 0 : 2]);
    if (!Text)
      return Text.takeError();
    // The observation profile enables all debugger component/level filters.
    Result.Messages.push_back(std::move(*Text));
    return StatusSuccess;
  }
  if (Kind == KernelAPIKind::KeGetCurrentIrql)
    return 0; // DriverEntry runs at PASSIVE_LEVEL; no IRQL transitions exist.

  const bool Zero = Kind == KernelAPIKind::RtlZeroMemory;
  const bool Fill = Kind == KernelAPIKind::RtlFillMemory;
  const uint64_t Size = Zero || Fill ? A[1] : A[2];
  if (auto E = checkRange(A[0], Size))
    return E;
  if (Zero || Fill || Kind == KernelAPIKind::memset) {
    const uint8_t Value = Zero ? 0 : static_cast<uint8_t>(Fill ? A[2] : A[1]);
    if (auto E = validateGuestAccess(A[0], Size, true))
      return E;
    if (auto E = writeBytes(Memory, A[0], Size, Value))
      return E;
    return Kind == KernelAPIKind::memset ? A[0] : 0;
  }
  if (auto E = checkRange(A[1], Size))
    return E;
  if ((Kind == KernelAPIKind::memcpy || Kind == KernelAPIKind::RtlCopyMemory) &&
      Size && (A[0] < A[1] ? A[1] - A[0] < Size : A[0] - A[1] < Size))
    return modelError(
        "overlapping buffers passed to a non-overlapping copy API");
  std::vector<uint8_t> Source(Size);
  if (auto E = validateGuestAccess(A[1], Size, false))
    return E;
  if (Size)
    if (auto E = Memory.read(A[1], Source))
      return E;
  if (Kind == KernelAPIKind::memcmp ||
      Kind == KernelAPIKind::RtlCompareMemory) {
    std::vector<uint8_t> Other(Size);
    if (auto E = validateGuestAccess(A[0], Size, false))
      return E;
    if (Size)
      if (auto E = Memory.read(A[0], Other))
        return E;
    for (size_t I = 0; I < Size; ++I)
      if (Other[I] != Source[I])
        return Kind == KernelAPIKind::RtlCompareMemory
                   ? uint64_t(I)
                   : uint64_t(static_cast<uint32_t>(int(Other[I]) - Source[I]));
    return Kind == KernelAPIKind::RtlCompareMemory ? Size : 0;
  }
  if (auto E = validateGuestAccess(A[0], Size, true))
    return E;
  if (Size)
    if (auto E = Memory.write(A[0], Source))
      return E;
  return Kind == KernelAPIKind::memcpy || Kind == KernelAPIKind::memmove ? A[0]
                                                                         : 0;
}

llvm::Error KernelModel::snapshot() {
  if (!DriverObject)
    return modelError("cannot snapshot an uninitialized kernel model");
  auto Unload = Memory.readInteger(DriverObject + DriverUnloadOffset, 8);
  if (!Unload)
    return Unload.takeError();
  Result.DriverUnload = *Unload;
  auto Extension = Memory.readInteger(DriverObject + DriverExtensionOffset, 8);
  if (!Extension)
    return Extension.takeError();
  if (*Extension != DriverExtension)
    return modelError("DRIVER_OBJECT.DriverExtension was corrupted");
  auto AddDevice =
      Memory.readInteger(DriverExtension + DriverAddDeviceOffset, 8);
  if (!AddDevice)
    return AddDevice.takeError();
  Result.AddDevice = *AddDevice;
  for (size_t I = 0; I < Result.MajorFunctions.size(); ++I) {
    const auto First = DispatchBytesWritten.begin() + I * 8;
    const auto Last = First + 8;
    if (!std::any_of(First, Last, [](bool Written) { return Written; })) {
      Result.MajorFunctions[I] = 0;
      continue;
    }
    if (!std::all_of(First, Last, [](bool Written) { return Written; }))
      return modelError("driver left a partially initialized dispatch pointer");
    auto Function =
        Memory.readInteger(DriverObject + DriverDispatchOffset + I * 8, 8);
    if (!Function)
      return Function.takeError();
    Result.MajorFunctions[I] = *Function;
  }
  Result.Devices.clear();
  auto Head = Memory.readInteger(DriverObject + DriverDeviceHead, 8);
  if (!Head)
    return Head.takeError();
  std::set<uint64_t> Seen;
  uint64_t Current = *Head;
  while (Current) {
    auto It = Devices.find(Current);
    if (It == Devices.end() || !Seen.insert(Current).second)
      return modelError("unknown device or cycle in DRIVER_OBJECT device list");
    Result.Devices.push_back(It->second);
    auto Next = Memory.readInteger(Current + DeviceNext, 8);
    if (!Next)
      return Next.takeError();
    Current = *Next;
  }
  if (Seen.size() != Devices.size())
    return modelError("live device detached from DRIVER_OBJECT device list");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateGuestAccess(uint64_t Address, uint32_t Size,
                                             bool IsWrite) const {
  if (!Size)
    return llvm::Error::success();
  if (Size > UINT64_MAX - Address)
    return modelError("overflowing guest access in Windows model");
  const uint64_t End = Address + Size;
  if (Address < profile::ThunkBase + profile::ThunkSize &&
      profile::ThunkBase < End)
    return modelError(
        "kernel import thunks have no modeled readable or writable data");
  if (Address < AllocationEnd && NextAllocation < End)
    return modelError("guest access to unallocated Windows model arena");
  const uint64_t ArenaFirst = std::max(Address, profile::KernelArenaBase);
  const uint64_t ArenaLast = std::min(End, AllocationEnd);
  if (ArenaFirst < ArenaLast) {
    auto Allocation = ArenaAllocations.upper_bound(ArenaFirst);
    if (Allocation != ArenaAllocations.begin())
      --Allocation;
    uint64_t Current = ArenaFirst;
    while (Current < ArenaLast) {
      if (Allocation == ArenaAllocations.end() || Allocation->first > Current ||
          Current >= Allocation->first + Allocation->second)
        return modelError(
            "guest access to unallocated arena alignment padding");
      Current = std::min(ArenaLast, Allocation->first + Allocation->second);
      ++Allocation;
    }
  }
  for (const auto &[Start, Length] : FreedRanges)
    if (Address < Start + Length && Start < End)
      return modelError("guest access to a freed model object or allocation");
  if (auto E = validateIOAccess(Address, Size, IsWrite))
    return E;

  auto CheckObject = [&](uint64_t Object, uint64_t Length,
                         auto &&Allowed) -> llvm::Error {
    if (!Object || Address >= Object + Length || Object >= End)
      return llvm::Error::success();
    const uint64_t First = std::max(Address, Object) - Object;
    const uint64_t Last = std::min(End, Object + Length) - Object;
    for (uint64_t Offset = First; Offset < Last; ++Offset)
      if (!Allowed(Offset))
        return modelError(IsWrite ? "guest write to an opaque or read-only "
                                    "Windows object field"
                                  : "guest read of an unmodeled Windows "
                                    "object field");
    return llvm::Error::success();
  };

  if (auto E =
          CheckObject(DriverObject, DriverObjectSize, [&](uint64_t Offset) {
            if (IsWrite)
              return (Offset >= DriverFastIOOffset &&
                      Offset < DriverInitOffset) ||
                     Offset >= DriverStartIOOffset;
            if ((Offset >= DriverFlagsOffset &&
                 Offset < DriverFlagsOffset + 4) ||
                (Offset >= DriverSectionOffset &&
                 Offset < DriverExtensionOffset))
              return false;
            if (Offset >= DriverDispatchOffset)
              return DispatchBytesWritten[Offset - DriverDispatchOffset];
            return true;
          }))
    return E;
  if (auto E = CheckObject(
          DriverExtension, DriverExtensionSize, [&](uint64_t Offset) {
            return Offset >= DriverAddDeviceOffset &&
                   Offset < DriverAddDeviceOffset + profile::PointerSize;
          }))
    return E;
  for (const auto &[Object, Device] : Devices) {
    (void)Device;
    if (auto E = CheckObject(Object, DeviceObjectSize, [&](uint64_t Offset) {
          if (IsWrite)
            return (Offset >= DeviceNext && Offset < DeviceAttachedOffset) ||
                   (Offset >= DeviceFlagsOffset &&
                    Offset < DeviceCharacteristicsOffset) ||
                   Offset == DeviceStackCountOffset ||
                   (Offset >= DeviceAlignmentOffset &&
                    Offset < DeviceAlignmentOffset + 4);
          return Offset < DeviceAttachedOffset ||
                 (Offset >= DeviceCurrentIRP && Offset < DeviceVPBOffset) ||
                 (Offset >= DeviceExtensionOffset &&
                  Offset < DeviceQueueOffset) ||
                 (Offset >= DeviceAlignmentOffset &&
                  Offset < DeviceQueueStateOffset) ||
                 (Offset >= DeviceSectorSizeOffset &&
                  Offset < DeviceSectorSizeOffset + 2) ||
                 Offset >= DeviceReservedOffset;
        }))
      return E;
  }
  if (IsWrite && DriverObject) {
    const uint64_t Start = DriverObject + DriverDispatchOffset;
    const uint64_t Last = DriverObject + DriverObjectSize;
    for (uint64_t Byte = std::max(Address, Start); Byte < std::min(End, Last);
         ++Byte)
      DispatchBytesWritten[Byte - Start] = true;
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation
