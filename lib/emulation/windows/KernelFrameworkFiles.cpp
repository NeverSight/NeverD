//===- KernelFrameworkFiles.cpp - KMDF file-object lifetimes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Framework file objects identify the WDM FILE_OBJECT owned by the request
/// host. CREATE completion and CLOSE own their distinct deletion paths.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error fileError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF file object: " + Message);
}

} // namespace

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callFile(llvm::StringRef Name, Binding &B,
                          llvm::ArrayRef<uint64_t> A) {
  using Result = std::optional<uint64_t>;
  if (Name == api::WdfDeviceInitSetFileObjectConfig) {
    auto Init = DeviceInits.find(A[1]);
    if (Init == DeviceInits.end() || Init->second.Binding != B.Globals)
      return fileError("configuration requires a live device initializer");
    if (auto E = ValidateAccess(A[2], FileConfigSize, false))
      return E;
    auto Size = read(A[2], sizeof(uint32_t));
    if (!Size)
      return Size.takeError();
    auto Create = read(A[2] + FileCreateCallbackOffset);
    if (!Create)
      return Create.takeError();
    auto Close = read(A[2] + FileCloseCallbackOffset);
    if (!Close)
      return Close.takeError();
    auto Cleanup = read(A[2] + FileCleanupCallbackOffset);
    if (!Cleanup)
      return Cleanup.takeError();
    auto AutoForward = read(A[2] + FileAutoForwardOffset, sizeof(uint32_t));
    if (!AutoForward)
      return AutoForward.takeError();
    auto Class = read(A[2] + FileClassOffset, sizeof(uint32_t));
    if (!Class)
      return Class.takeError();
    if (*Size != FileConfigSize)
      return fileError("unsupported file-object configuration size");
    if (*AutoForward != FileAutoForwardFalse &&
        *AutoForward != FileAutoForwardDefault)
      return fileError("forwarded file lifecycle requires a lower target");
    if (*Class != FileObjectNotRequired &&
        *Class != FileObjectCannotUseFsContexts)
      return fileError("unsupported framework file-object class");
    auto Validation = attributes(A[3], AttributesUse::Device);
    if (!Validation)
      return Validation.takeError();
    if (std::holds_alternative<uint32_t>(*Validation))
      return fileError("invalid file-object attributes");
    const auto &Attrs = std::get<Attributes>(*Validation);
    if (*Class == FileObjectNotRequired &&
        (Attrs.Type || Attrs.ContextSize || Attrs.Cleanup || Attrs.Destroy))
      return fileError("file-object attributes require a file object");
    FileConfig Config;
    Config.Enabled = true;
    Config.Create = *Create;
    Config.Close = *Close;
    Config.Cleanup = *Cleanup;
    Config.Class = uint32_t(*Class);
    Config.ObjectAttributes = Attrs;
    Init->second.Files = Config;
    return Result{0};
  }
  if (Name != api::WdfFileObjectGetFileName &&
      Name != api::WdfFileObjectGetFlags &&
      Name != api::WdfFileObjectGetDevice &&
      Name != api::WdfFileObjectWdmGetFileObject)
    return Result{};
  auto Object = Objects.find(A[1]);
  auto File = FileObjects.find(A[1]);
  if (Object == Objects.end() || Object->second.Kind != ObjectKind::File ||
      Object->second.Binding != B.Globals || Object->second.Deleting ||
      File == FileObjects.end())
    return fileError("access requires a live framework file object");
  if (Name == api::WdfFileObjectGetFileName)
    return Result{File->second.Wdm + windows::FileNameOffset};
  if (Name == api::WdfFileObjectGetFlags) {
    auto Flags =
        read(File->second.Wdm + windows::FileFlagsOffset, sizeof(uint32_t));
    if (!Flags)
      return Flags.takeError();
    return Result{*Flags};
  }
  return Result{Name == api::WdfFileObjectGetDevice ? File->second.Device
                                                    : File->second.Wdm};
}

llvm::Expected<uint64_t>
KernelFramework::requestFileObject(uint64_t Device, uint64_t WdmFile) const {
  const auto &Config = Devices.at(Device).Files;
  if (!Config.Enabled || Config.Class == FileObjectNotRequired)
    return 0;
  auto File = FileHandles.find(WdmFile);
  if (!WdmFile || File == FileHandles.end())
    return fileError("request has no matching framework file object");
  auto Object = FileObjects.find(File->second);
  if (Object == FileObjects.end() || Object->second.Device != Device)
    return fileError("request has no matching framework file object");
  return File->second;
}

llvm::Expected<std::optional<KernelFramework::RequestDispatch>>
KernelFramework::routeFileRequest(uint64_t Device, uint64_t IRP,
                                  const RequestView &View) {
  using Result = std::optional<RequestDispatch>;
  if (View.Major != RequestMajorCreate && View.Major != RequestMajorCleanup &&
      View.Major != RequestMajorClose)
    return Result{};
  const auto &Config = Devices.at(Device).Files;
  if (!Config.Enabled) {
    if (auto E = RequestsHost.Complete(IRP, windows::StatusSuccess, 0))
      return E;
    return Result{RequestDispatch{0, {}, 0}};
  }
  if (!View.File)
    return fileError("file lifecycle request has no WDM FILE_OBJECT");
  if (View.Major == RequestMajorCreate) {
    if (FileHandles.contains(View.File))
      return fileError("CREATE reused a live WDM FILE_OBJECT");
    uint64_t File = 0;
    if (Config.Class != FileObjectNotRequired) {
      Attributes Attrs = Config.ObjectAttributes;
      Attrs.Parent = Device;
      auto Created = createObject(Objects.at(Device).Binding, Attrs, false);
      if (!Created)
        return Created.takeError();
      File = *Created;
      Objects.at(File).Kind = ObjectKind::File;
      FileObjects.emplace(File, FileObject{Device, View.File});
      FileHandles.emplace(View.File, File);
    }
    if (!Config.Create) {
      if (auto E = RequestsHost.Complete(IRP, windows::StatusSuccess, 0))
        return E;
      return Result{RequestDispatch{0, {}, 0}};
    }
    Attributes Attrs;
    Attrs.Parent = Device;
    auto Created = createObject(Objects.at(Device).Binding, Attrs, false);
    if (!Created)
      return Created.takeError();
    Objects.at(*Created).Kind = ObjectKind::Request;
    Request RequestState{IRP, 0, Device};
    RequestState.File = File;
    RequestState.FileCreate = true;
    Requests.emplace(*Created, std::move(RequestState));
    if (auto E = RequestsHost.MarkPending(IRP))
      return E;
    return Result{RequestDispatch{
        Config.Create, {Device, *Created, File}, windows::StatusPending}};
  }
  auto File = requestFileObject(Device, View.File);
  if (!File)
    return File.takeError();
  std::vector<Step> Steps;
  const uint64_t Callback =
      View.Major == RequestMajorCleanup ? Config.Cleanup : Config.Close;
  if (Callback)
    Steps.push_back({StepKind::Callback, *File, Callback});
  if (View.Major == RequestMajorClose && *File)
    Steps.push_back({StepKind::DeleteFileObject, *File});
  Steps.push_back({StepKind::CompleteFileIRP, IRP});
  if (auto E = RequestsHost.MarkPending(IRP))
    return E;
  auto Started = start(std::move(Steps));
  if (!Started)
    return Started.takeError();
  return Result{RequestDispatch{0, {}, windows::StatusPending}};
}
} // namespace neverd::emulation
