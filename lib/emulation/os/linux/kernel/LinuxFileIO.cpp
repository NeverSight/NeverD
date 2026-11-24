//===- LinuxFileIO.cpp - Descriptor ownership and byte transfers ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
std::optional<uint64_t> LinuxFiles::duplicate(ServiceKind Kind, uint32_t Old,
                                              uint32_t New, uint32_t Flags,
                                              ProcessResult &Result) {
  if (Kind == ServiceKind::Dup3 && (Old == New || (Flags & ~OpenCloseOnExec)))
    return uint64_t(0) - InvalidArgument;
  auto I = Descriptors.find(Old);
  if (I == Descriptors.end())
    return uint64_t(0) - BadDescriptor;
  if (Kind == ServiceKind::Dup) {
    New = nextDescriptor();
    if (New == Options->DescriptorLimit)
      return uint64_t(0) - TooManyFiles;
  } else if (New >= Options->DescriptorLimit)
    return uint64_t(0) - BadDescriptor;
  auto Target = Descriptors.find(New);
  if (Target != Descriptors.end() && Target->second != I->second &&
      !canRelease(Target->second))
    return unsupported(
        Result, "Device descriptor replacement cannot retire pending work");
  // Save before replacement, including Old == New. No observer can see an
  // intermediate close/reallocation or a different open-description identity.
  if (Old == New)
    return New;
  auto Description = I->second;
  Descriptors.insert_or_assign(New, std::move(Description));
  opened(New, Kind == ServiceKind::Dup3 ? Flags : 0);
  return New;
}

uint32_t LinuxFiles::nextDescriptor() const {
  uint32_t FD = 0;
  while (FD < Options->DescriptorLimit && Descriptors.contains(FD))
    ++FD;
  return FD;
}
llvm::Expected<std::optional<uint64_t>>
LinuxFiles::close(uint32_t FD, ProcessResult &Result) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  if (!canRelease(I->second))
    return unsupported(Result, "Device last close cannot retire pending work");
  Descriptors.erase(I);
  CloseOnExec.erase(FD);
  return std::optional<uint64_t>(0);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::readDescriptor(uint32_t FD, uint64_t Address, uint64_t Size,
                           ProcessResult &Result, ThreadContext *Thread) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  if (std::holds_alternative<OpenSocket>(*I->second)) {
    if (!Layout.containsUserRange(Address, Size))
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    // Linux sock_read_iter returns before dequeuing for a zero-size read.
    // recvfrom with a zero-size buffer still consumes a UDP datagram.
    if (!Size)
      return std::optional<uint64_t>(0);
    return receiveSocket(I->second, Address,
                         std::min(Size, Layout.maxReadWriteCount()), Result,
                         Thread);
  }
  if (std::holds_alternative<ProcessDescriptor>(*I->second) ||
      std::holds_alternative<std::shared_ptr<EpollState>>(*I->second))
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  if (std::holds_alternative<OpenDevice>(*I->second))
    return unsupported(Result, DeviceOperation);
  if (std::holds_alternative<OpenDirectory>(*I->second))
    return std::optional<uint64_t>(uint64_t(0) - IsDirectory);
  if (auto *P = std::get_if<OpenProcFile>(I->second.get()))
    return readProc(*P, Address, Size, Result);
  auto *File = std::get_if<OpenFile>(I->second.get());
  if (!File) {
    if (std::get<Stream>(*I->second) == Stream::Input)
      return unsupported(Result, FileInputStream);
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  }
  if ((File->Flags & 3) == 1)
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  auto Value = read(*File, Address, Size);
  if (!Value)
    return Value.takeError();
  return std::optional<uint64_t>(*Value);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::seekDescriptor(uint32_t FD, uint64_t Offset, uint32_t Whence,
                           ProcessResult &Result) {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  if (Whence > SeekHole)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  if (std::holds_alternative<ProcessDescriptor>(*I->second) ||
      std::holds_alternative<std::shared_ptr<EpollState>>(*I->second) ||
      std::holds_alternative<OpenSocket>(*I->second))
    return std::optional<uint64_t>(uint64_t(0) - IllegalSeek);
  if (std::holds_alternative<OpenDevice>(*I->second))
    return unsupported(Result, DeviceOperation);
  if (auto *D = std::get_if<OpenDirectory>(I->second.get())) {
    if (Whence > SeekCurrent)
      return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
    const uint64_t Position = (Whence == SeekCurrent ? D->Offset : 0) + Offset;
    if (Position > MaxSignedIOSize)
      return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
    if (liveProcFDs() && llvm::StringRef(D->Path).starts_with("/proc/") &&
        (llvm::StringRef(D->Path).ends_with("/fd") ||
         llvm::StringRef(D->Path).ends_with("/fdinfo")) &&
        Position > UINT32_MAX)
      return unsupported(Result,
                         "proc FD cookie exceeds the supported index extent");
    if (D->Offset != Position)
      D->NextTask.clear();
    D->Offset = Position;
    return std::optional<uint64_t>(Position);
  }
  if (auto *P = std::get_if<OpenProcFile>(I->second.get()))
    return seekProc(*P, Offset, Whence, Result);
  auto *File = std::get_if<OpenFile>(I->second.get());
  if (!File) {
    if (std::get<Stream>(*I->second) == Stream::Input)
      return unsupported(Result, FileInputStream);
    return std::optional<uint64_t>(uint64_t(0) - IllegalSeek);
  }
  if (Whence > SeekEnd)
    return unsupported(Result, FileSeekMode);
  return std::optional<uint64_t>(seek(*File, Offset, Whence));
}

llvm::Expected<uint64_t> LinuxFiles::read(OpenFile &File, uint64_t Address,
                                          uint64_t Size) {
  // access_ok checks the original extent, before count clamping or EOF.
  if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
    return uint64_t(0) - BadAddress;
  // Every cursor is nonnegative. Check the original signed file extent before
  // either transfer clamping or EOF can reduce the request.
  if (Size > MaxSignedIOSize - File.Offset)
    return uint64_t(0) - InvalidArgument;
  const auto Bytes = fileBytes(File);
  const uint64_t Available =
      Bytes.size() - std::min<uint64_t>(File.Offset, Bytes.size());
  Size = std::min({Size, Available, Layout.maxReadWriteCount()});
  uint64_t Copied = 0;
  while (Copied < Size) {
    const uint64_t Start = Address + Copied;
    const uint64_t Count =
        std::min(Size - Copied, Layout.PageSize - Start % Layout.PageSize);
    auto Access = CPU.canAccess(Start, Count, Write | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return Copied ? Copied : uint64_t(0) - BadAddress;
    if (auto E = CPU.write(Start, Bytes.slice(File.Offset, Count)))
      return std::move(E);
    Copied += Count;
    File.Offset += Count;
  }
  return Copied;
}

uint64_t LinuxFiles::seek(OpenFile &File, uint64_t Offset, uint32_t Whence) {
  uint64_t Base;
  switch (Whence) {
  case SeekSet:
    Base = 0;
    break;
  case SeekCurrent:
    Base = File.Offset;
    break;
  case SeekEnd:
    Base = fileBytes(File).size();
    break;
  default:
    llvm_unreachable("seek mode must be admitted by the service owner");
  }
  // Unsigned arithmetic preserves the syscall's signed offset bits without
  // invoking host signed overflow. Every admitted cursor is <= INT64_MAX.
  const uint64_t Position = Base + Offset;
  if (Position > MaxSignedIOSize ||
      (Offset <= MaxSignedIOSize && Position < Base))
    return uint64_t(0) - InvalidArgument;
  File.Offset = Position;
  return Position;
}

} // namespace neverd::emulation::linux_model
