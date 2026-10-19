//===- KernelModelFiniteFileSystem.cpp - Finite synchronous file provider
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finite synchronous file provider.
///

#include "../WindowsUnicode.h"
#include "KernelFiniteFileSystemValues.h"
#include "KernelModel.h"
#include "KernelObjectTypes.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
using namespace windows;
using namespace finite_file_system;

llvm::Error KernelModel::prepareFiniteFileSystem() {
  if (!FiniteFileSystem || FiniteFileSystem->empty() || FiniteFileSystemDriver)
    return llvm::Error::success();
  auto Owner = allocate(DriverObjectSize);
  if (!Owner)
    return Owner.takeError();
  FiniteFileSystemDriver = *Owner;
  ReferencedObjects.emplace(*Owner, ObjectReferences{ObjectKind::Driver});
  if (auto E = Memory.writeInteger(*Owner, DriverType, 2))
    return E;
  if (auto E = Memory.writeInteger(*Owner + 2, DriverObjectSize, 2))
    return E;
  auto Publish = [&](llvm::StringRef Name,
                     llvm::StringRef Target) -> llvm::Error {
    auto Status = createSymbolicLink(Name, Target);
    if (!Status)
      return Status.takeError();
    if (*Status)
      return fileError(
          "configured filesystem alias collides with an existing link");
    auto Key = linkKey(Name);
    if (!Key)
      return Key.takeError();
    FiniteFileSystemLinks.insert(*Key);
    return llvm::Error::success();
  };
  for (size_t I = 0; I < FiniteFileSystem->size(); ++I) {
    const auto &V = (*FiniteFileSystem)[I];
    auto &Nodes = FileNodes[I];
    Nodes.emplace("\\", std::make_shared<FileNode>(
                            FileNode{"\\", true, {}, false, "\\"}));
    for (const auto &Entry : V.Entries) {
      auto Key = windows_unicode::key(Entry.Path, V.UpcaseTable);
      if (!Key)
        return Key.takeError();
      Nodes.emplace(
          *Key, std::make_shared<FileNode>(FileNode{Entry.Path, Entry.Directory,
                                                    Entry.Data, false, *Key}));
    }
    auto D =
        createDeviceObjectForOwner(V.DeviceName, 0, 7, SecureOpen, false,
                                   *Owner, DeviceOwnerKind::FiniteFileSystem);
    if (!D)
      return D.takeError();
    if (D->Status)
      return fileError("cannot publish configured volume device");
    if (auto E = Memory.writeInteger(D->Address + DeviceFlagsOffset, 0x40, 4))
      return E;
    VolumeDevices.emplace(I, D->Address);
    if (auto E = Publish("\\??\\" + V.Drive + ":", V.DeviceName))
      return E;
    for (const auto &Alias : V.Aliases) {
      auto Key = windows_unicode::key(Alias.Path, V.UpcaseTable);
      if (!Key)
        return Key.takeError();
      auto N = Nodes.at(*Key);
      if (auto E = Publish(Alias.Name, V.DeviceName + N->Path))
        return E;
    }
  }
  return llvm::Error::success();
}

bool KernelModel::canAccessFileHandle(const FileHandle &Handle) const {
  return canAccessHandle(Handle.Attributes, Handle.ProcessID);
}

llvm::Expected<uint64_t> KernelModel::openFile(llvm::ArrayRef<uint64_t> A,
                                               uint64_t DeviceHint) {
  if (A[4] || A[9] || A[10])
    return fileError("allocation hints and EAs are unsupported");
  const uint32_t Disposition = uint32_t(A[7]), Options = uint32_t(A[8]),
                 Access = accessMask(uint32_t(A[1]));
  if (Disposition > 5)
    return StatusInvalidParameter;
  if ((Options & ~uint32_t(0x1061)) || !(Options & 0x20) ||
      ((Options & 1) && (Options & 0x40)))
    return fileError("requires synchronous nonalert directory or file open");
  if (Access & ~uint32_t(0x1f01ff))
    return fileError("unsupported desired-access mask");
  if (!(Access & Synchronize) || (uint32_t(A[6]) & ~uint32_t(7)) ||
      ((Options & DeleteOnClose) && !(Access & Delete)))
    return StatusInvalidParameter;
  auto P = resolveFilePath(A[2]);
  if (!P)
    return P.takeError();
  if (P->Status)
    return P->Status;
  if (DeviceHint) {
    if (!Devices.contains(DeviceHint))
      return fileError("device hint requires a live modeled device object");
    if (DeviceHint != P->Device)
      return uint64_t(0xc0000369); // STATUS_INVALID_DEVICE_OBJECT_PARAMETER
  }
  auto N = findFileNode(*P);
  const bool Existing = bool(N);
  if (P->Status && P->Status != StatusObjectNameNotFound)
    return P->Status;
  if (P->Raw && Disposition != 1)
    return fileError("raw volume creation is unsupported");
  if (!P->Raw && !Existing && (Disposition == 1 || Disposition == 4))
    return StatusObjectNameNotFound;
  if (Existing && Disposition == 2)
    return StatusObjectNameCollision;
  const bool Create = !P->Raw && !Existing;
  const bool Truncate =
      Existing && (Disposition == 0 || Disposition == 4 || Disposition == 5);
  const bool Mutating =
      Create || Truncate || bool(Options & DeleteOnClose) ||
      bool(Access & (WriteData | AppendData | 0x10 | 0x100 | Delete | 0xc0000));
  if (P->Raw && Mutating)
    return fileError("raw volume mutation is unsupported");
  if (Mutating && (*FiniteFileSystem)[P->Volume].ReadOnly)
    return MediaWriteProtected;
  if (Disposition == 0 && Existing && !(Access & Delete))
    return AccessDenied;
  if ((Disposition == 4 || Disposition == 5) && !(Access & WriteData))
    return AccessDenied;
  if (Create && (A[5] & ~uint64_t(0x90)))
    return fileError("unsupported creation file attributes");
  if (Create && FileNodes.at(P->Volume).contains(P->Key))
    return StatusObjectNameCollision;
  const bool Directory = Existing ? N->Directory : bool(Options & 1);
  if (!P->Raw && (Options & 1) && !Directory)
    return NotDirectory;
  if ((Options & 0x40) && Directory)
    return IsDirectory;
  if (Truncate && Directory)
    return fileError("directory overwrite is outside this profile");
  for (const auto &[Object, F] : NativeFiles)
    if (((P->Raw && F.RawVolume && F.Volume == P->Volume) ||
         (N && F.Node == N)) &&
        NativeOpenFiles.at(F.ID).State == NativeFileState::Open &&
        ((shareRequired(Access) & ~F.Share) ||
         (shareRequired(F.Access) & ~uint32_t(A[6]))))
      return SharingViolation;
  if (N && !N->Image.expired() &&
      (Truncate || (Options & DeleteOnClose) ||
       (Access & (WriteData | AppendData | Delete))))
    return SharingViolation;
  if (auto E = objectBuffer(A[0], 8, true))
    return E;
  if (auto E = objectBuffer(A[3], 16, true))
    return E;
  if (NextFileHandle > FileHandleLimit || NativeFiles.size() >= 256)
    return StatusInsufficientResources;
  if (Create &&
      finiteFileSystemUsage().first >= MaxDriverFiniteFileSystemEntries)
    return DiskFull;
  if (Create)
    N = std::make_shared<FileNode>(
        FileNode{P->Relative, Directory, {}, false, P->Key});
  const std::string Relative = P->Raw ? "" : N->Path;
  const auto &V = (*FiniteFileSystem)[P->Volume];
  while (NativeOpenFiles.contains(NextNativeFileID))
    --NextNativeFileID;
  const uint32_t ID = NextNativeFileID--;
  auto Object = allocate(FileSize);
  if (!Object)
    return Object.takeError();
  auto FileName = makeUnicodeString(Relative);
  if (!FileName)
    return FileName.takeError();
  auto NameLength = Memory.readInteger(*FileName, 2);
  if (!NameLength)
    return NameLength.takeError();
  for (const auto &[Offset, Value, Size] :
       std::initializer_list<std::tuple<uint64_t, uint64_t, unsigned>>{
           {0, FileType, 2},
           {2, FileSize, 2},
           {FileDeviceOffset, P->Device, 8},
           {FileReadAccessOffset, bool(Access & (ReadData | 0x20)), 1},
           {FileWriteAccessOffset, bool(Access & (WriteData | AppendData)), 1},
           {FileDeleteAccessOffset, bool(Access & Delete), 1},
           {FileSharedReadOffset, bool(A[6] & 1), 1},
           {FileSharedWriteOffset, bool(A[6] & 2), 1},
           {FileSharedDeleteOffset, bool(A[6] & 4), 1},
           {FileFlagsOffset,
            (P->Raw ? FileFlags : FileSynchronousIO) |
                ((Options & DeleteOnClose) ? 0x10000 : 0),
            4},
           {FileNameOffset, *NameLength, 2},
           {FileNameOffset + 2, *NameLength + 2, 2},
           {FileNameBufferOffset, *FileName + 16, 8}})
    if (auto E = Memory.writeInteger(*Object + Offset, Value, Size))
      return E;
  const uint64_t Information = P->Raw     ? 0
                               : Create   ? 2
                               : Truncate ? (Disposition == 0 ? 0 : 3)
                                          : 1;
  if (auto E = Memory.writeInteger(A[0], NextFileHandle, 8))
    return E;
  if (auto E = Memory.writeInteger(A[3], 0, 4))
    return E;
  if (auto E = Memory.writeInteger(A[3] + 8, Information, 8))
    return E;
  if (Create)
    FileNodes[P->Volume].emplace(P->Key, N);
  if (Truncate)
    N->Data.clear();
  NativeOpenFiles.emplace(
      ID, NativeOpenFile{*Object, P->Device, 0, false, NativeFileState::Open});
  NativeFiles.emplace(*Object,
                      NativeFile{ID, P->Volume, N, P->Raw, Access,
                                 uint32_t(A[6]), V.DeviceName + Relative,
                                 *FileName, bool(Options & DeleteOnClose)});
  ReferencedObjects.emplace(*Object, ObjectReferences{ObjectKind::File});
  retainReservedObjectHandle(NextFileHandle, *Object, Access,
                             P->Attributes & uint32_t(ObjectKernelHandle),
                             handleProcessID());
  FileHandles.emplace(NextFileHandle,
                      FileHandle{*Object,
                                 P->Attributes & uint32_t(ObjectKernelHandle),
                                 handleProcessID(), Access});
  NextFileHandle += 4;
  if (auto E = updateDeviceReferences(P->Device))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::referenceFile(llvm::ArrayRef<uint64_t> A) {
  auto H = FileHandles.find(A[0]);
  if (H == FileHandles.end() || !canAccessFileHandle(H->second))
    return StatusInvalidHandle;
  if (!matchesObjectType(ObjectKind::File, A[2]))
    return StatusObjectTypeMismatch;
  if (uint8_t(A[3]) != KernelMode || A[5])
    return fileError(
        "object references require KernelMode without handle info");
  // WDK: DesiredAccess contains specific rights, not generic rights. The
  // KernelMode reference does not compare it to the handle's granted mask.
  if (uint32_t(A[1]) & ~uint32_t(0x1f01ff))
    return fileError("unsupported file object-reference access mask");
  if (auto E = objectBuffer(A[4], 8, true))
    return E;
  auto &References = ReferencedObjects.at(H->second.Object);
  if (References.Pointers == SIZE_MAX)
    return fileError("pointer reference overflow");
  if (auto E = Memory.writeInteger(A[4], H->second.Object, 8))
    return E;
  ++References.Pointers;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::openFilePointer(llvm::ArrayRef<uint64_t> A) {
  if (!matchesObjectType(ObjectKind::File, A[4]))
    return StatusObjectTypeMismatch;
  if (A[2] || uint8_t(A[5]) != KernelMode ||
      uint32_t(A[1]) & ~ObjectKernelHandle)
    return fileError("unsupported pointer-open mode or attributes");
  auto &F = NativeFiles.at(A[0]);
  if (NativeOpenFiles.at(F.ID).State != NativeFileState::Open)
    return fileError("cannot reopen a cleaned-up FILE_OBJECT in this profile");
  const uint32_t Access = accessMask(uint32_t(A[3]));
  if (Access & ~F.Access)
    return fileError("pointer-open access expansion is outside this profile");
  if (auto E = objectBuffer(A[6], 8, true))
    return E;
  if (NextFileHandle > FileHandleLimit)
    return StatusInsufficientResources;
  if (auto E = Memory.writeInteger(A[6], NextFileHandle, 8))
    return E;
  retainReservedObjectHandle(NextFileHandle, A[0], Access, uint32_t(A[1]),
                             handleProcessID());
  FileHandles.emplace(NextFileHandle, FileHandle{A[0], uint32_t(A[1]),
                                                 handleProcessID(), Access});
  NextFileHandle += 4;
  return StatusSuccess;
}

llvm::Error KernelModel::retireNativeFile(uint64_t Object) {
  auto &F = NativeFiles.at(Object);
  for (const auto &[H, Handle] : FileHandles)
    if (Handle.Object == Object)
      return llvm::Error::success();
  if (auto E = retireFilterNamesForFile(Object))
    return E;
  auto &Shared = NativeOpenFiles.at(F.ID);
  if (Shared.State == NativeFileState::Open) {
    auto Flags = Memory.readInteger(Object + FileFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    if (auto E = Memory.writeInteger(Object + FileFlagsOffset,
                                     *Flags | FileCleanupComplete, 4))
      return E;
    Shared.State = NativeFileState::Cleaned;
    if (F.DeleteOnClose && F.Node)
      if (auto E = setFileDeletePending(F.Volume, F.Node, true))
        return E;
    if (auto E = unlinkFileIfClosed(F.Volume, F.Node))
      return E;
    if (auto E = updateDeviceReferences(Shared.Device))
      return E;
  }
  if (!ReferencedObjects.at(Object).Pointers) {
    FreedRanges.emplace(Object, FileSize);
    FreedRanges.emplace(F.NameStorage, ArenaAllocations.at(F.NameStorage));
    const auto Device = Shared.Device;
    NativeOpenFiles.erase(F.ID);
    NativeFiles.erase(Object);
    ReferencedObjects.erase(Object);
    if (auto E = updateDeviceReferences(Device))
      return E;
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::closeFile(uint64_t Handle) {
  auto H = FileHandles.find(Handle);
  if (!canAccessFileHandle(H->second))
    return StatusInvalidHandle;
  uint64_t Object = H->second.Object;
  FileHandles.erase(H);
  ObjectHandles.erase(Handle);
  --ReferencedObjects.at(Object).Handles;
  if (auto E = retireNativeFile(Object))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::dereferenceFile(uint64_t Object) {
  if (!NativeFiles.contains(Object))
    return fileError("dereference requires a live finite file object");
  return dereferenceObject(Object);
}

llvm::Expected<uint64_t> KernelModel::finiteFileSystemDevice(uint64_t Object) {
  auto File = std::find_if(
      NativeOpenFiles.begin(), NativeOpenFiles.end(), [&](const auto &Entry) {
        return Entry.second.Address == Object &&
               Entry.second.State != NativeFileState::Closed;
      });
  if (File == NativeOpenFiles.end())
    return fileError(
        "filesystem device query requires a live FILE_OBJECT identity");
  const auto &F = File->second;
  if (!Devices.contains(F.Device))
    return fileError("FILE_OBJECT lost its retained device identity");
  for (auto [Offset, Width] :
       {std::pair{FileDeviceOffset, 8u}, std::pair{FileVPBOffset, 8u},
        std::pair{FileFlagsOffset, 4u}})
    if (auto E = objectBuffer(Object + Offset, Width, false))
      return E;
  auto Device = Memory.readInteger(Object + FileDeviceOffset, 8),
       VPB = Memory.readInteger(Object + FileVPBOffset, 8),
       Flags = Memory.readInteger(Object + FileFlagsOffset, 4);
  if (!Device)
    return Device.takeError();
  if (!VPB)
    return VPB.takeError();
  if (!Flags)
    return Flags.takeError();
  if (*Device != F.Device)
    return fileError("FILE_OBJECT device field changed identity");
  // The current provider owns no mounted VPBs. Never invent a mount graph
  // or confuse the base filesystem device with an attached upper filter.
  if (*VPB)
    return fileError(
        "filesystem device query requires an explicit VPB provider");
  if (!(*Flags & 0x800)) {
    auto DeviceVPB = Memory.readInteger(F.Device + DeviceVPBOffset, 8);
    if (!DeviceVPB)
      return DeviceVPB.takeError();
    if (*DeviceVPB)
      return fileError(
          "filesystem device query requires an explicit VPB provider");
  }
  return F.Device;
}

llvm::Expected<uint64_t> KernelModel::fileCall(llvm::StringRef Name,
                                               llvm::ArrayRef<uint64_t> A) {
  if (!FiniteFileSystem)
    return fileError("requires an explicit filesystem inventory");
  auto Disabled = allApcsDisabled();
  if (!Disabled)
    return Disabled.takeError();
  if (*Disabled)
    return fileError("requires special kernel APCs enabled");
  if (Name == "ZwDeleteFile")
    return deleteFile(A[0]);
  if (Name == "ZwCreateFile")
    return openFile(A);
  if (Name == "IoCreateFile" || Name == "IoCreateFileSpecifyDeviceObjectHint") {
    const bool WithHint = Name == "IoCreateFileSpecifyDeviceObjectHint";
    if (uint32_t(A[11]) || A[12])
      return fileError("IoCreateFile requires CreateFileTypeNone and NULL "
                       "InternalParameters");
    constexpr uint32_t NoParameterChecking = 0x100;
    // The matched SpecifyDeviceObjectHint wrapper unconditionally selects
    // kernel parameter handling before reaching the common native opener.
    const uint32_t Options =
        uint32_t(A[13]) | (WithHint ? NoParameterChecking : 0);
    if (Options & ~NoParameterChecking)
      return fileError("IoCreateFile access checks, share-check overrides, "
                       "target directories and reparse options are unmodeled");
    const auto Context = executionProcessContext();
    if (!(Options & NoParameterChecking) &&
        (!Context || Context->PreviousMode != KernelMode))
      return fileError("IoCreateFile user-mode parameter probing is unmodeled; "
                       "requires a known KernelMode caller or explicit "
                       "IO_NO_PARAMETER_CHECKING");
    // The extra flags select parameter policy, not a second filesystem. Buffer
    // bounds and semantic object access remain enforced by the shared opener.
    return openFile(A.take_front(11), WithHint ? A[14] : 0);
  }
  if (Name == "ZwOpenFile") {
    const uint64_t Normalized[] = {A[0], A[1], A[2], A[3], 0, 0,
                                   A[4], 1,    A[5], 0,    0};
    return openFile(Normalized);
  }
  auto H = FileHandles.find(A[0]);
  if (H == FileHandles.end()) {
    if (ObjectHandles.contains(A[0]) || Registry.ownsHandle(A[0]))
      return fileError("this object class has no file information contract");
    return StatusInvalidHandle;
  }
  if (!canAccessFileHandle(H->second))
    return StatusInvalidHandle;
  const uint64_t Object = H->second.Object;
  auto &F = NativeFiles.at(Object);
  const auto &V = (*FiniteFileSystem)[F.Volume];
  if (Name == "ZwQueryObject") {
    if (uint32_t(A[1]) != 1)
      return fileError("unsupported object information class");
    auto Encoded = windows_unicode::bytes(F.Name, true);
    if (!Encoded)
      return Encoded.takeError();
    const uint32_t Needed = 16 + uint32_t(Encoded->size());
    const uint32_t Length = uint32_t(A[3]);
    if (A[4]) {
      if (auto E = objectBuffer(A[4], 4, true))
        return E;
      if (A[4] < A[2] + uint64_t(Needed) && A[2] < A[4] + 4)
        return fileError("overlapping object-name and return-length outputs "
                         "are unsupported");
    }
    if (Length >= Needed)
      if (auto E = objectBuffer(A[2], Needed, true))
        return E;
    if (A[4])
      if (auto E = Memory.writeInteger(A[4], Needed, 4))
        return E;
    if (Length < 16)
      return StatusInfoLengthMismatch;
    if (Length < Needed)
      return BufferOverflow;
    std::vector<uint8_t> Bytes(Needed);
    const uint16_t StringLength = uint16_t(Encoded->size() - 2);
    Bytes[0] = uint8_t(StringLength);
    Bytes[1] = uint8_t(StringLength >> 8);
    Bytes[2] = uint8_t(StringLength + 2);
    Bytes[3] = uint8_t((StringLength + 2) >> 8);
    const uint64_t Buffer = A[2] + 16;
    for (unsigned I = 0; I < 8; ++I)
      Bytes[8 + I] = uint8_t(Buffer >> (I * 8));
    std::copy(Encoded->begin(), Encoded->end(), Bytes.begin() + 16);
    if (auto E = Memory.write(A[2], Bytes))
      return E;
    return StatusSuccess;
  }
  if (Name == "ZwQueryInformationFile" || Name == "ZwSetInformationFile")
    return fileInformation(A, Name == "ZwSetInformationFile");
  if (Name == "ZwFlushBuffersFile") {
    if (auto E = objectBuffer(A[1], 16, true))
      return E;
    if (auto E = Memory.writeInteger(A[1], 0, 8))
      return E;
    if (auto E = Memory.writeInteger(A[1] + 8, 0, 8))
      return E;
    return 0;
  }
  const bool Writing = Name == "ZwWriteFile";
  if (!Writing && Name != "ZwReadFile")
    return fileError("unknown file API");
  if (A[1] || A[2] || A[3] || A[8])
    return fileError("events, APCs and keyed reads are unsupported");
  if (Writing ? !(H->second.Access & (WriteData | AppendData))
              : !(H->second.Access & ReadData))
    return AccessDenied;
  if (F.RawVolume)
    return fileError("raw volume bytes are not configured");
  if (!F.Node || F.Node->Directory)
    return IsDirectory;
  if (Writing && V.ReadOnly)
    return MediaWriteProtected;
  auto &Data = F.Node->Data;
  auto Offset = Memory.readInteger(Object + FileCurrentByteOffset, 8);
  if (!Offset)
    return Offset.takeError();
  if (A[7]) {
    if (auto E = objectBuffer(A[7], 8, false))
      return E;
    auto Position = Memory.readInteger(A[7], 8);
    if (!Position)
      return Position.takeError();
    if (*Position == uint64_t(-1) && Writing)
      Offset = Data.size();
    else if (*Position != uint64_t(-2))
      Offset = *Position;
  }
  if (Writing && (H->second.Access & AppendData) &&
      !(H->second.Access & WriteData))
    Offset = Data.size();
  if (*Offset > INT64_MAX)
    return StatusInvalidParameter;
  const uint32_t Length = uint32_t(A[6]);
  if (auto E = objectBuffer(A[4], 16, true))
    return E;
  uint32_t Count = Length;
  uint32_t Status = 0;
  if (Writing) {
    if (Length) {
      const auto Usage = finiteFileSystemUsage();
      if (*Offset > MaxDriverFiniteFileSystemBytes ||
          Length > MaxDriverFiniteFileSystemBytes - *Offset ||
          (*Offset + Length > Data.size() &&
           *Offset + Length - Data.size() >
               MaxDriverFiniteFileSystemBytes - Usage.second)) {
        if (auto E = Memory.writeInteger(A[4], DiskFull, 8))
          return E;
        if (auto E = Memory.writeInteger(A[4] + 8, 0, 8))
          return E;
        return DiskFull;
      }
      if (auto E = objectBuffer(A[5], Length, false))
        return E;
      std::vector<uint8_t> Bytes(Length);
      if (auto E = Memory.read(A[5], Bytes))
        return E;
      if (*Offset + Length > Data.size())
        Data.resize(size_t(*Offset + Length), 0);
      std::copy(Bytes.begin(), Bytes.end(), Data.begin() + size_t(*Offset));
    }
  } else {
    Count = uint32_t(std::min<uint64_t>(
        Length, *Offset >= Data.size() ? 0 : Data.size() - *Offset));
    if (auto E = objectBuffer(A[5], Count, true))
      return E;
    if (Count)
      if (auto E = Memory.write(
              A[5], llvm::ArrayRef<uint8_t>(Data).slice(*Offset, Count)))
        return E;
    if (!Count && Length)
      Status = EndOfFile;
  }
  // The in-memory provider completes a full IOSB; there is no NTFS FastIo path.
  if (auto E = Memory.writeInteger(A[4], Status, 8))
    return E;
  if (auto E = Memory.writeInteger(A[4] + 8, Count, 8))
    return E;
  if (!Status && Length)
    if (auto E = Memory.writeInteger(Object + FileCurrentByteOffset,
                                     *Offset + Count, 8))
      return E;
  return Status;
}

llvm::Error KernelModel::releaseFiniteFileSystem() {
  if (!NativeFiles.empty() || !FileHandles.empty())
    return fileError("unload returned with live file objects or handles");
  for (const auto &[I, Address] : VolumeDevices) {
    const auto &D = Devices.at(Address);
    if (D.Upper || D.Lower || D.InternalReferences ||
        Scheduler.hasOutstanding(Address))
      return fileError(
          "volume still has an attached filter or outstanding work");
  }
  for (const auto &[I, Address] : VolumeDevices) {
    FreedRanges.emplace(Address, Devices.at(Address).Size);
    Devices.erase(Address);
    ReferencedObjects.erase(Address);
  }
  auto Links = std::move(FiniteFileSystemLinks);
  FiniteFileSystemLinks.clear();
  for (const auto &Key : Links) {
    auto Status = deleteSymbolicLink(Key);
    if (!Status)
      return Status.takeError();
    if (*Status != StatusSuccess)
      return fileError("finite provider lost a symbolic link");
  }
  VolumeDevices.clear();
  if (FiniteFileSystemDriver)
    FreedRanges.emplace(FiniteFileSystemDriver, DriverObjectSize);
  ReferencedObjects.erase(FiniteFileSystemDriver);
  FiniteFileSystemDriver = 0;
  return llvm::Error::success();
}
} // namespace neverd::emulation
