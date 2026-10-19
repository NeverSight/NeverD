//===- KernelModelProcessExitStatus.cpp - Process lifetime status ---------===//
// NeverD Decompiler
///
/// \file
/// Process lifetime status.
///

#include "KernelModel.h"
#include "KernelReferenceImage.h"
#include "WindowsKernelLayout.h"
namespace neverd::emulation {
namespace {
llvm::Error statusError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PsGetProcessExitStatus: " + Text);
}
} // namespace
llvm::Expected<uint64_t> KernelModel::processExitStatus(uint64_t Object) {
  const auto Identity = ProcessObjectIDs.find(Object);
  if (Identity == ProcessObjectIDs.end())
    return statusError("requires a known process object");
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return statusError("requires Windows 10.0.19041");
  std::optional<uint32_t> Status;
  if (auto It = ProcessExitStatuses.find(Identity->second);
      It != ProcessExitStatuses.end())
    Status = It->second;
  else if (isLiveProcess(Identity->second))
    Status = windows::StatusPending;
  if (!Status)
    return statusError(
        "exit status is unknown; provide an explicit exit_status");
  constexpr auto Offset = windows::ProcessExitStatus19041Offset;
  if (ProcessObjectSize < Offset + 4)
    return statusError(
        "process layout does not contain the native status field");
  if (DebuggerReference) {
    const auto Getter =
        DebuggerReference->exports().find("PsGetProcessExitStatus");
    const uint8_t Body[] = {0x8b, 0x81, 0xd4, 0x07, 0x00, 0x00, 0xc3};
    if (Getter == DebuggerReference->exports().end() || !Getter->second ||
        !DebuggerReference->matchesCode(Getter->second, Body))
      return statusError(
          "reference getter does not match the selected native layout");
  }
  if (!PublishedProcessExitStatuses.contains(Object)) {
    if (auto E = Memory.writeInteger(Object + Offset, *Status, 4))
      return E;
    PublishedProcessExitStatuses.insert(Object);
  }
  return Memory.readInteger(Object + Offset, 4);
}
llvm::Error
KernelModel::observeProcessExitStatus(uint32_t PID,
                                      std::optional<uint32_t> Status) {
  if (PID == windows::SystemProcessID || !isLiveProcess(PID) ||
      ProcessExitStatuses.contains(PID))
    return statusError("exit observation requires a fresh live user process");
  if (auto It = ProcessObjects.find(PID);
      It != ProcessObjects.end() &&
      PublishedProcessExitStatuses.contains(It->second)) {
    if (Status) {
      if (auto E = Memory.writeInteger(
              It->second + windows::ProcessExitStatus19041Offset, *Status, 4))
        return E;
    } else
      PublishedProcessExitStatuses.erase(It->second);
  }
  ProcessExitStatuses.emplace(PID, Status);
  return llvm::Error::success();
}
} // namespace neverd::emulation
