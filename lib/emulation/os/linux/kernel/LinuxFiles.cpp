//===- LinuxFiles.cpp - Typed Linux memory-file service dispatch ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

namespace neverd::emulation::linux_model {
std::optional<uint64_t> LinuxFiles::unsupported(ProcessResult &Result,
                                                const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}

std::optional<std::variant<LinuxFiles::DescriptorStatus, uint32_t>>
LinuxFiles::descriptorStatus(uint32_t FD, ProcessResult &Result) const {
  if (!Options) {
    unsupported(Result, FileInputsMissing);
    return std::nullopt;
  }
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint32_t(BadDescriptor);
  if (const auto *File = std::get_if<OpenFile>(I->second.get()))
    return DescriptorStatus{
        File->Flags & 3,
        File->Metadata ? std::optional<uint32_t>(File->Metadata->BlockSize)
                       : std::nullopt,
        false, bool(File->Flags & OpenAppend)};
  if (std::holds_alternative<OpenProcFile>(*I->second))
    return DescriptorStatus{0, uint32_t(ProcBlockSize)};
  if (std::holds_alternative<OpenDirectory>(*I->second))
    return DescriptorStatus{0, std::nullopt, true};
  if (const auto *Device = std::get_if<OpenDevice>(I->second.get()))
    return DescriptorStatus{(*Device)->Options->AccessMode, std::nullopt};
  if (std::holds_alternative<OpenSocket>(*I->second))
    return DescriptorStatus{2, std::nullopt};
  if (const auto *S = std::get_if<Stream>(I->second.get()))
    return DescriptorStatus{*S == Stream::Input ? 0u : 1u, std::nullopt};
  unsupported(Result, "unmodeled descriptor access mode");
  return std::nullopt;
}

std::optional<uint64_t> LinuxFiles::outputError(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint64_t(0) - BadDescriptor;
  if (std::holds_alternative<ProcessDescriptor>(*I->second) ||
      std::holds_alternative<std::shared_ptr<EpollState>>(*I->second))
    return uint64_t(0) - InvalidArgument;
  const auto *S = std::get_if<Stream>(I->second.get());
  if (S && (*S == Stream::Output || *S == Stream::Error))
    return std::nullopt;
  return uint64_t(0) - BadDescriptor;
}

uint32_t LinuxFiles::outputTarget(uint32_t FD) const {
  const auto &Description = *Descriptors.at(FD);
  return std::get<Stream>(Description) == Stream::Output ? StandardOutput
                                                         : StandardError;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::handle(ServiceKind Kind, const ProcessServiceEvent &Event,
                   ProcessResult &Result, ThreadContext *Thread) {
  if (!Options)
    return unsupported(Result, FileInputsMissing);
  const auto &[A0, A1, A2, A3, A4, A5] = Event.Arguments;
  switch (Kind) {
  case ServiceKind::Getcwd:
    return getWorkingDirectory(A0, A1, Result);
  case ServiceKind::Mkdir:
    return makeDirectory(A0, A1, Result);
  case ServiceKind::MkdirAt:
    return makeDirectory(A1, A2, Result);
  case ServiceKind::Access:
    return access(A0, A1, Result);
  case ServiceKind::FaccessAt:
    return access(A1, A2, Result);
  case ServiceKind::Open:
    return open(A0, A1, A2, Result);
  case ServiceKind::OpenAt:
    return open(A1, A2, A3, Result, A0);
  case ServiceKind::Unlink:
    return unlinkPath(A0, 0, Result);
  case ServiceKind::UnlinkAt:
    // This bounded path contract admits absolute paths only. Linux ignores
    // dirfd for those paths; relative walks remain unsupported in unlinkPath.
    return unlinkPath(A1, A2, Result);
  case ServiceKind::Rename:
    return renameFile(A0, A1, Result);
  case ServiceKind::RenameAt:
    // The admitted paths are absolute, so both dirfd values are ignored.
    return renameFile(A1, A3, Result);
  case ServiceKind::Close:
    return close(A0, Result);
  case ServiceKind::Dup:
  case ServiceKind::Dup2:
  case ServiceKind::Dup3:
    return duplicate(Kind, A0, A1, A2, Result);
  case ServiceKind::Read:
    return readDescriptor(A0, A1, A2, Result, Thread);
  case ServiceKind::Getdents64:
    return directoryEntries(A0, A1, A2, Result);
  case ServiceKind::Ioctl:
    return tracedIoctl(Event, Result);
  case ServiceKind::Write:
  case ServiceKind::WriteV:
    return writeDescriptor(Kind, A0, A1, A2, Result);
  case ServiceKind::Readlink:
    return readLink(AtCurrentDirectory, A0, A1, A2, Result);
  case ServiceKind::ReadlinkAt:
    return readLink(A0, A1, A2, A3, Result);
  case ServiceKind::Lseek:
    return seekDescriptor(A0, A1, A2, Result);
  case ServiceKind::Fstat:
    return statusDescriptor(A0, A1, Result);
  case ServiceKind::FstatAt:
    return statusAt(A0, A1, A2, A3, Result);
  case ServiceKind::StatFS:
    return fileSystemStatus(A0, Result);
  case ServiceKind::FstatFS:
    return fileSystemStatusDescriptor(A0, Result);
  default:
    llvm_unreachable("not a Linux file service");
  }
}
} // namespace neverd::emulation::linux_model
