//===- KernelFrameworkRequestAccessors.cpp - KMDF request access ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Request accessors use one authoritative WDM packet and distinguish the
/// lifetime of that packet from a referenced framework request handle.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error accessorError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF request: " + Message);
}
} // namespace

bool KernelFramework::ownsRequestIRP(uint64_t IRP) const {
  return std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
    return Entry.second.IRP == IRP;
  });
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callRequestAccessors(llvm::StringRef Name, Binding &B,
                                      llvm::ArrayRef<uint64_t> A) {
  using Result = std::optional<uint64_t>;
  const bool InputMdl = Name == api::WdfRequestRetrieveInputWdmMdl;
  const bool OutputMdl = Name == api::WdfRequestRetrieveOutputWdmMdl;
  if (!InputMdl && !OutputMdl && Name != api::WdfRequestGetInformation &&
      Name != api::WdfRequestSetInformation &&
      Name != api::WdfRequestGetIoQueue &&
      Name != api::WdfRequestGetFileObject && Name != api::WdfRequestWdmGetIrp)
    return Result{};
  auto O = Objects.find(A[1]);
  auto R = Requests.find(A[1]);
  if (O == Objects.end() || O->second.Kind != ObjectKind::Request ||
      O->second.Binding != B.Globals || R == Requests.end())
    return accessorError("invalid or foreign framework request");
  if (R->second.Queued)
    return accessorError("framework owns the request in a manual queue");
  const bool Completed = R->second.Completed || R->second.Completing;
  // These documented neutral results require a surviving object handle, not
  // a surviving IRP. Completion detaches the queue before request cleanup.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestgetinformation
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestgetioqueue
  if (Name == api::WdfRequestGetIoQueue)
    return Result{Completed ? 0 : R->second.Queue};
  if (Name == api::WdfRequestGetInformation) {
    if (Completed)
      return Result{0};
    if (!RequestsHost.Information)
      return accessorError("information host is unavailable");
    auto Information = RequestsHost.Information(R->second.IRP);
    if (!Information)
      return Information.takeError();
    return Result{*Information};
  }
  if (InputMdl || OutputMdl) {
    if (auto E = writable(A[2], sizeof(uint64_t)))
      return std::move(E);
    if (auto E = Memory.writeInteger(A[2], 0, sizeof(uint64_t)))
      return std::move(E);
    // The public API supplies a status for a retained, completed request.
    // No host lookup may touch its already retired packet or descriptor.
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestretrieveinputwdmmdl
    if (Completed)
      return Result{RequestInternalError};
    if (!RequestsHost.View || !RequestsHost.Mdl)
      return accessorError("MDL host is unavailable");
    auto View = RequestsHost.View(R->second.IRP);
    if (!View)
      return View.takeError();
    const bool IsRead = View->Major == RequestMajorRead;
    const bool IsWrite = View->Major == RequestMajorWrite;
    const bool IsIOCTL = View->Major == RequestMajorDeviceControl;
    if ((!IsRead && !IsWrite && !IsIOCTL) || (IsRead && InputMdl) ||
        (IsWrite && OutputMdl) || View->Neither ||
        (IsIOCTL && (View->ControlCode & windows::IoControlMethodMask) ==
                        windows::MethodNeither))
      return Result{ControlInvalidDeviceRequest};
    const uint32_t Length = OutputMdl ? View->OutputLength : View->InputLength;
    if (!Length)
      return Result{RequestBufferTooSmall};
    auto MDL = RequestsHost.Mdl(R->second.IRP, OutputMdl);
    if (!MDL)
      return MDL.takeError();
    if (!*MDL)
      return Result{windows::StatusInsufficientResources};
    if (auto E = Memory.writeInteger(A[2], *MDL, sizeof(uint64_t)))
      return std::move(E);
    return Result{0};
  }
  if (Completed)
    return accessorError("request is completed or completion is in progress");
  if (Name == api::WdfRequestSetInformation) {
    if (!RequestsHost.SetInformation)
      return accessorError("information host is unavailable");
    if (auto E = RequestsHost.SetInformation(R->second.IRP, A[2]))
      return std::move(E);
    return Result{0};
  }
  if (Name == api::WdfRequestGetFileObject) {
    // Current devices use the no-file-callback configuration, whose file
    // class is WdfFileObjectNotRequired. A WDM FILE_OBJECT is not this handle.
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestgetfileobject
    return Result{0};
  }
  if (!RequestsHost.View)
    return accessorError("request inspection host is unavailable");
  auto View = RequestsHost.View(R->second.IRP);
  if (!View)
    return View.takeError();
  return Result{View->IRP};
}
} // namespace neverd::emulation
