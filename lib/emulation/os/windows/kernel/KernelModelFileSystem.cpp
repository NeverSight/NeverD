//===- KernelModelFileSystem.cpp - Canonical filesystem volume devices ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Install declared terminal providers without fabricating file I/O results.

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint32_t DiskFileSystemDeviceType = 8;
llvm::Error filesystemError(const llvm::Twine &Text) {
  return llvm::createStringError("filesystem provider: " + Text);
}
} // namespace

uint64_t KernelModel::deviceOwnerDriver(DeviceOwnerKind Kind) const {
  switch (Kind) {
  case DeviceOwnerKind::Guest:
    return DriverObject;
  case DeviceOwnerKind::PnpProvider:
    return PnpProviderDriver;
  case DeviceOwnerKind::FiniteFileSystem:
    return FiniteFileSystemDriver;
  case DeviceOwnerKind::FileSystemProvider:
    return FileSystemProviderDriver;
  }
  return 0;
}

llvm::Error KernelModel::initializeFileSystem(const DriverOptions &Options) {
  if (auto E = FileSystem.initialize(Options.FileSystem))
    return E;
  if (!Options.FileSystem)
    return llvm::Error::success();
  // Pre-entry devices enter the recovery baseline. Unchanged bytes or an unused
  // declaration cannot make this unreconstructed environment a native fact.
  UnpackOpaqueEffects = true;
  if (Options.FileSystem->empty())
    return llvm::Error::success();
  auto Driver = allocate(DriverObjectSize);
  if (!Driver)
    return Driver.takeError();
  if (auto E = Memory.writeInteger(*Driver, DriverType, 2))
    return E;
  if (auto E = Memory.writeInteger(*Driver + 2, DriverObjectSize, 2))
    return E;
  FileSystemProviderDriver = *Driver;
  ReferencedObjects.emplace(*Driver, ObjectReferences{ObjectKind::Driver});
  for (size_t I = 0; I < Options.FileSystem->size(); ++I) {
    const auto &Volume = (*Options.FileSystem)[I];
    auto Device = createDeviceObjectForOwner(
        Volume.DeviceName, 0, DiskFileSystemDeviceType, SecureOpen, false,
        FileSystemProviderDriver, DeviceOwnerKind::FileSystemProvider);
    if (!Device)
      return Device.takeError();
    if (Device->Status != StatusSuccess)
      return filesystemError("declared volume device could not be installed");
    if (auto E = Memory.writeInteger(Device->Address + DeviceAlignmentOffset,
                                     *Volume.AlignmentMask, 4))
      return E;
    if (auto E = Memory.writeInteger(Device->Address + DeviceSectorSizeOffset,
                                     *Volume.SectorSize, 2))
      return E;
    if (auto E = FileSystem.bindDevice(I, Device->Address))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateFileSystemDevice(uint64_t Address,
                                                  ObjectRead Mode) const {
  const auto *Volume = FileSystem.volumeForDevice(Address);
  const auto Found = Devices.find(Address);
  if (!Volume || Found == Devices.end() ||
      Found->second.OwnerKind != DeviceOwnerKind::FileSystemProvider ||
      Found->second.OwnerDriver != FileSystemProviderDriver ||
      Found->second.Name != Volume->DeviceName)
    return filesystemError("volume lost its canonical device identity");
  for (const auto [Offset, Expected] :
       {std::pair{uint64_t(0), uint64_t(DriverType)},
        std::pair{uint64_t(2), uint64_t(DriverObjectSize)}}) {
    auto Value = readObjectField(FileSystemProviderDriver + Offset, 2, Mode);
    if (!Value)
      return Value.takeError();
    if (*Value != Expected)
      return filesystemError("volume driver identity differs from its owner");
  }
  struct Field {
    uint64_t Offset, Value;
    unsigned Width;
  };
  for (const auto &F : std::array<Field, 6>{
           {{DeviceTypeOffset, DiskFileSystemDeviceType, 4},
            {DeviceFlagsOffset, 0, 4},
            {DeviceCharacteristicsOffset, SecureOpen, 4},
            {DeviceVPBOffset, 0, 8},
            {DeviceAlignmentOffset, *Volume->AlignmentMask, 4},
            {DeviceSectorSizeOffset, *Volume->SectorSize, 2}}}) {
    auto Value = readObjectField(Address + F.Offset, F.Width, Mode);
    if (!Value)
      return Value.takeError();
    if (*Value != F.Value)
      return filesystemError("volume device fields differ from their owner");
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateFileSystemRoute(llvm::ArrayRef<uint64_t> Route,
                                                 uint64_t FileObject,
                                                 DriverRequestKind Kind) const {
  for (uint64_t Address : Route) {
    const auto Found = Devices.find(Address);
    if (Found == Devices.end())
      return filesystemError("captured route lost its device identity");
    if (Found->second.OwnerKind == DeviceOwnerKind::FileSystemProvider) {
      const auto File = FileObjects.find(FileObject);
      if (File == FileObjects.end() ||
          File->second.Source != OpenFile::Origin::DriverCreate ||
          File->second.Device != Address ||
          (Kind != DriverRequestKind::Create &&
           Kind != DriverRequestKind::Cleanup &&
           Kind != DriverRequestKind::Close &&
           Kind != DriverRequestKind::QueryInformation &&
           Kind != DriverRequestKind::UnlockAll))
        return filesystemError(
            "file I/O for this declared volume route is not implemented");
    }
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::dispatchFileSystem(uint64_t Device,
                                                         uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed || Request->FileSystemReceived)
    return filesystemError("terminal dispatch requires a fresh live packet");
  if (auto E = validateFileSystemRoute(Request->DeviceRoute,
                                       Request->FileAddress, Request->Kind))
    return E;
  auto &File = FileObjects.at(Request->FileAddress);
  if (!FileSystem.ownsDevice(Device) || File.Device != Device)
    return filesystemError("terminal dispatch lost its bound volume");
  auto Cursor = requestStackCursor(IRP);
  if (!Cursor)
    return Cursor.takeError();
  const uint64_t Stack = IRP + IRPSize + *Cursor * StackSize;
  auto StackFile = Memory.readInteger(Stack + StackFileOffset, 8);
  auto Original = Memory.readInteger(IRP + IRPOriginalFileOffset, 8);
  if (!StackFile || !Original)
    return llvm::joinErrors(StackFile.takeError(), Original.takeError());
  if (*StackFile != File.Address || *Original != File.Address)
    return filesystemError("terminal packet substituted its FILE_OBJECT");
  uint32_t Status = StatusSuccess;
  uint64_t Information = 0;
  if (Request->Kind == DriverRequestKind::QueryInformation) {
    if (!File.ProviderOpen)
      return filesystemError("query lost its retained provider open");
    auto Class = Memory.readInteger(Stack + StackQueryClassOffset, 4);
    auto Length = Memory.readInteger(Stack + StackQueryLengthOffset, 4);
    auto Buffer = Memory.readInteger(IRP + IRPSystemBufferOffset, 8);
    if (!Class || !Length || !Buffer)
      return llvm::joinErrors(
          Class.takeError(),
          llvm::joinErrors(Length.takeError(), Buffer.takeError()));
    if (*Buffer != Request->SystemBuffer || *Length > Request->BufferSize)
      return filesystemError("query substituted or exceeded its buffer");
    auto Query = FileSystem.queryInformation(
        *File.ProviderOpen, uint32_t(*Class), uint32_t(*Length));
    if (!Query)
      return Query.takeError();
    Status = Query->Status;
    Information = Query->Bytes.size();
    if (!Query->Bytes.empty()) {
      if (auto E = validateObjectOutput(*Buffer, Query->Bytes.size()))
        return E;
      if (auto E = Memory.write(*Buffer, Query->Bytes))
        return E;
    }
  } else if (Request->Kind == DriverRequestKind::Create) {
    if (File.ProviderOpen || File.OpenCancelled ||
        !FileOperations.contains(File.CreateOperation))
      return filesystemError("CREATE lost its unpublished open operation");
    auto Security = Memory.readInteger(Stack + StackParametersOffset, 8);
    auto Options = Memory.readInteger(Stack + StackInputLengthOffset, 4);
    auto Share = Memory.readInteger(Stack + StackCreateShareOffset, 2);
    auto Flags = Memory.readInteger(Stack + StackFlagsOffset, 1);
    auto Related = Memory.readInteger(File.Address + FileRelatedOffset, 8);
    auto EALength = Memory.readInteger(Stack + 0x20, 4);
    auto EABuffer = Memory.readInteger(IRP + IRPSystemBufferOffset, 8);
    if (!Security || !Options || !Share || !Flags || !Related || !EALength ||
        !EABuffer)
      return llvm::joinErrors(
          Security.takeError(),
          llvm::joinErrors(
              Options.takeError(),
              llvm::joinErrors(
                  Share.takeError(),
                  llvm::joinErrors(
                      Flags.takeError(),
                      llvm::joinErrors(
                          Related.takeError(),
                          llvm::joinErrors(EALength.takeError(),
                                           EABuffer.takeError()))))));
    if (*EALength || *EABuffer)
      return filesystemError("CREATE extended attributes are not implemented");
    if (*Security != Request->SecurityContext || *Related != File.RelatedFile ||
        *Flags & ~0x80u)
      return filesystemError("CREATE changed unsupported structural fields");
    auto Access =
        Memory.readInteger(*Security + SecurityDesiredAccessOffset, 4);
    if (!Access)
      return Access.takeError();
    if (*Access != FileOperations.at(File.CreateOperation).Access ||
        (*Options >> 24) != 1 || (*Options & 0xffffff & ~0x61u) || *Share & ~7u)
      return filesystemError(
          "CREATE requires admitted FILE_OPEN access and options");
    auto NameBuffer =
        Memory.readInteger(File.Address + FileNameBufferOffset, 8);
    auto NameCapacity =
        Memory.readInteger(File.Address + FileNameOffset + 2, 2);
    if (!NameBuffer || !NameCapacity)
      return llvm::joinErrors(NameBuffer.takeError(), NameCapacity.takeError());
    if (*NameBuffer != File.NameStorage || *NameCapacity > File.NameStorageSize)
      return filesystemError("replacement filename storage is not implemented");
    auto Name =
        readObjectName(File.Address + FileNameOffset, MaxFileSystemPathBytes);
    if (!Name)
      return Name.takeError();
    std::string Path = *Name;
    if (File.RelatedFile) {
      const auto Root = FileObjects.find(File.RelatedFile);
      const auto *Node =
          Root != FileObjects.end() && Root->second.ProviderOpen
              ? FileSystem.nodeForOpen(*Root->second.ProviderOpen)
              : nullptr;
      if (!Node || Node->Kind != DriverFileNodeKind::Directory ||
          Name->empty() || Name->front() == '\\')
        return filesystemError("relative CREATE lost its retained directory");
      Path = Node->Path + (Node->Path == "\\" ? "" : "\\") + *Name;
    }
    auto Node = FileSystem.lookupNode(Device, Path, !(*Flags & 0x80));
    if (!Node)
      return Node.takeError();
    if ((*Options & 0x41) == 0x41) {
      Status = StatusInvalidParameter;
    } else if (!*Node) {
      const auto Last = Path.rfind('\\');
      auto Parent = FileSystem.lookupNode(
          Device, Last ? Path.substr(0, Last) : "\\", !(*Flags & 0x80));
      if (!Parent)
        return Parent.takeError();
      Status = *Parent && (*Parent)->Kind == DriverFileNodeKind::Directory
                   ? StatusObjectNameNotFound
                   : StatusObjectPathNotFound;
    } else if ((*Options & 1) &&
               (*Node)->Kind != DriverFileNodeKind::Directory) {
      Status = StatusNotADirectory;
    } else if ((*Options & 0x40) &&
               (*Node)->Kind == DriverFileNodeKind::Directory) {
      Status = StatusFileIsADirectory;
    } else if ((*Node)->Kind == DriverFileNodeKind::File &&
               (*(*Node)->Attributes & 1) && (*Access & 0x6)) {
      Status = StatusAccessDenied;
    } else {
      auto Open = FileSystem.reserveOpen(Device, *(*Node)->FileID, File.Address,
                                         uint32_t(*Access), uint32_t(*Share));
      if (!Open)
        return Open.takeError();
      Status = Open->Status;
      if (!Status) {
        File.ProviderOpen = Open->Token;
        const auto *State = FileSystem.findOpen(Open->Token);
        const uint32_t Demand = State->SharingDemand;
        for (unsigned I = 0; I != 6; ++I) {
          const bool Value = I < 3 ? (Demand & (1u << I)) != 0
                                   : Demand && (*Share & (1u << (I - 3)));
          if (auto E = Memory.writeInteger(
                  File.Address + FileReadAccessOffset + I, Value, 1))
            return E;
        }
        Information = 1; // FILE_OPENED; the declared node is unchanged.
      }
    }
  } else {
    if (!File.ProviderOpen || !FileSystem.findOpen(*File.ProviderOpen))
      return filesystemError("lifecycle packet lost its provider token");
    if (Request->Kind == DriverRequestKind::Cleanup)
      if (auto E = FileSystem.cleanupOpen(*File.ProviderOpen))
        return E;
  }
  Request->FileSystemReceived = true;
  associateFileRequest(Result.Requests[Request->ResultIndex], File);
  if (auto E = Memory.writeInteger(IRP + IRPStatusOffset, Status, 4))
    return E;
  if (auto E = Memory.writeInteger(IRP + IRPInformationOffset, Information, 8))
    return E;
  Request->IOStatusWritten.fill(true);
  if (auto E = completeRequest(IRP, 0))
    return E;
  if (PendingWdmCall)
    IRPCalls.at(PendingWdmCall->Token.ID).ReturnValue = Status;
  return Status;
}
} // namespace neverd::emulation
