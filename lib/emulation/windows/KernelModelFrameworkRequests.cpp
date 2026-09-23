//===- KernelModelFrameworkRequests.cpp - WDF request ownership bridge ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Connect framework requests to the authoritative WDM packet, transfer
/// storage, pending state and completion path without a second IRP lifetime.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error frameworkRequestError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

llvm::Expected<uint32_t> requestMajor(DriverRequestKind Kind) {
  switch (Kind) {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major)                      \
  case DriverRequestKind::Name:                                                \
    return Major;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  }
  return frameworkRequestError("framework request has an unknown WDM kind");
}
} // namespace

llvm::Error KernelModel::markRequestPending(uint64_t IRP) {
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return frameworkRequestError(
        "IoMarkIrpPending requires the live active IRP");
  auto Stack = currentRequestStack(IRP);
  if (!Stack)
    return Stack.takeError();
  auto Control = Memory.readInteger(*Stack + StackControlOffset, 1);
  if (!Control)
    return Control.takeError();
  return Memory.writeInteger(*Stack + StackControlOffset,
                             *Control | StackPendingReturned, 1);
}

llvm::Error KernelModel::validateRequestCompletion(uint64_t IRP,
                                                   uint32_t Status,
                                                   uint64_t Information) const {
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return frameworkRequestError(
        "completion requires the active IRP and cannot occur twice");
  // An accepted AdapterControl may still need this exact captured packet as
  // input. Check before a terminal unwind consumes stack slots; beginning the
  // callback releases that input hold and permits completion from its body.
  if (auto E =
          DMA.canReleaseRange(IRP, IRPSize + Request->StackCount * StackSize))
    return E;
  if (Status == StatusPending)
    return frameworkRequestError(
        "IoCompleteRequest cannot complete with STATUS_PENDING");
  if (Request->Kind == DriverRequestKind::Pnp && Information)
    return frameworkRequestError(
        "modeled PnP operations require zero IoStatus.Information");
  // An IOCTL with no output allocation can use Information as a driver-defined
  // value. Otherwise READ/WRITE and IOCTL output retain the WDM byte limit.
  // https://learn.microsoft.com/windows-hardware/drivers/kernel/failure-to-initialize-output-buffers
  const bool HasIOCTLOutput =
      Request->Kind == DriverRequestKind::DeviceControl && Request->OutputSize;
  const bool HasTransferCount = HasIOCTLOutput ||
                                Request->Kind == DriverRequestKind::Read ||
                                Request->Kind == DriverRequestKind::Write;
  if (HasTransferCount && Information > Request->TransferSize)
    return frameworkRequestError(
        "IoStatus.Information exceeds the requested transfer buffer");
  if ((Request->Kind == DriverRequestKind::Cleanup ||
       Request->Kind == DriverRequestKind::Close) &&
      Information)
    return frameworkRequestError(
        "CLEANUP and CLOSE require zero IoStatus.Information");
  if (Request->Kind == DriverRequestKind::Create &&
      Information > MaxCreateInformation)
    return frameworkRequestError(
        "CREATE IoStatus.Information is not a defined create result");
  return llvm::Error::success();
}

void KernelModel::configureFrameworkRequestHost() {
  KernelFramework::RequestHost Host;
  Host.View =
      [this](uint64_t IRP) -> llvm::Expected<KernelFramework::RequestView> {
    const auto *Request = requestForIRP(IRP);
    if (!Request || Request->Completed)
      return frameworkRequestError(
          "framework request inspection requires the live active IRP");
    auto Major = requestMajor(Request->Kind);
    if (!Major)
      return Major.takeError();
    const auto &Observation = Result.Requests[Request->ResultIndex];
    return KernelFramework::RequestView{
        IRP,
        Request->ByteOffset,
        *Major,
        Observation.ControlCode,
        Request->InputSize,
        Request->OutputSize,
        Request->Neither,
        Request->Kind == DriverRequestKind::DeviceControl ? Request->UserInput
        : Request->Kind == DriverRequestKind::Write       ? Request->UserBuffer
                                                          : 0,
        Request->Kind == DriverRequestKind::Read ||
                Request->Kind == DriverRequestKind::DeviceControl
            ? Request->UserBuffer
            : 0};
  };
  Host.Buffer = [this](uint64_t IRP, bool Output) -> llvm::Expected<uint64_t> {
    const auto *Request = requestForIRP(IRP);
    if (!Request || Request->Completed)
      return frameworkRequestError(
          "framework buffer retrieval requires the live active IRP");
    const bool IsIOCTL = Request->Kind == DriverRequestKind::DeviceControl;
    const bool IsRead = Request->Kind == DriverRequestKind::Read;
    const bool IsWrite = Request->Kind == DriverRequestKind::Write;
    if ((!IsIOCTL && !IsRead && !IsWrite) || (IsRead && !Output) ||
        (IsWrite && Output))
      return frameworkRequestError(
          "framework buffer direction does not match the WDM request");
    const uint32_t Length = Output ? Request->OutputSize : Request->InputSize;
    if (!Length)
      return 0;
    // Direct IOCTL input is still the first, buffered allocation. Only its
    // second buffer and direct READ/WRITE use the request-owned locked MDL.
    // Public WDF implementation: FxRequest::GetMemoryObject, ioctl/input case.
    if (Request->Direct && (!IsIOCTL || Output)) {
      if (!Request->Mdl)
        return frameworkRequestError(
            "framework direct buffer lost the active request's MDL");
      // Reuse the same mapping and preserve any existing MdlMappingNoWrite or
      // MdlMappingNoExecute restrictions, exactly as the WDM safe helper does.
      return mapLockedPages(Request->Mdl, NormalPagePriority, true);
    }
    if (!Request->SystemBuffer || Length > Request->BufferSize)
      return frameworkRequestError(
          "framework buffered request lost its system allocation");
    return Request->SystemBuffer;
  };
  Host.MarkPending = [this](uint64_t IRP) { return markRequestPending(IRP); };
  Host.IsCanceled = [this](uint64_t IRP) -> llvm::Expected<bool> {
    const auto *Request = requestForIRP(IRP);
    if (!Request || Request->Completed)
      return frameworkRequestError(
          "framework cancellation inspection requires a live IRP");
    return Request->CancelRequested;
  };
  Host.Information = [this](uint64_t IRP) -> llvm::Expected<uint64_t> {
    const auto *Request = requestForIRP(IRP);
    if (!Request || Request->Completed)
      return frameworkRequestError(
          "framework information retrieval requires the live active IRP");
    return Memory.readInteger(IRP + IRPInformationOffset, 8);
  };
  Host.SetInformation = [this](uint64_t IRP, uint64_t Value) -> llvm::Error {
    auto *Request = requestForIRP(IRP);
    if (!Request || Request->Completed)
      return frameworkRequestError(
          "framework information update requires the live active IRP");
    if (auto E = Memory.writeInteger(IRP + IRPInformationOffset, Value, 8))
      return E;
    // SetInformation is not completion: leave Status initialization and the
    // eventual transfer-count validation to their existing authoritative paths.
    constexpr uint64_t Offset = IRPInformationOffset - IRPStatusOffset;
    for (unsigned I = Offset; I < Offset + sizeof(uint64_t); ++I)
      Request->IOStatusWritten[I] = true;
    return llvm::Error::success();
  };
  Host.Mdl = [this](uint64_t IRP, bool Output) {
    return frameworkRequestMDL(IRP, Output);
  };
  Host.ProbeAndLock = [this](uint64_t IRP, uint64_t Buffer, uint64_t Length,
                             bool Write) {
    return frameworkProbeAndLockUserBuffer(IRP, Buffer, Length, Write);
  };
  Host.ReleaseUserBuffer = [this](uint64_t MDL) {
    return releaseFrameworkUserBuffer(MDL);
  };
  Host.ValidateCompletion = [this](uint64_t IRP, uint32_t Status,
                                   uint64_t Information) -> llvm::Error {
    return validateRequestCompletion(IRP, Status, Information);
  };
  Host.Complete = [this](uint64_t IRP, uint32_t Status,
                         uint64_t Information) -> llvm::Error {
    if (auto E = validateRequestCompletion(IRP, Status, Information))
      return E;
    auto *Request = requestForIRP(IRP);
    if (auto E = Memory.writeInteger(IRP + IRPStatusOffset, Status, 4))
      return E;
    if (auto E =
            Memory.writeInteger(IRP + IRPInformationOffset, Information, 8))
      return E;
    Request->IOStatusWritten.fill(true);
    return completeRequest(IRP, 0);
  };
  Framework->setRequestHost(std::move(Host));
}
} // namespace neverd::emulation
