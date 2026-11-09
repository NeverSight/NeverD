//===- LinuxFileWrite.cpp - Bounded workload-owned mutable files --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Mutable contents are inode state; cursors and append/access flags belong to
/// shared open descriptions. Captured stat observations are invalidated by IO.
#include "LinuxFiles.h"
#include "LinuxTime.h"
#include "LinuxWriteBuffers.h"

#include <algorithm>
#include <cassert>

namespace neverd::emulation::linux_model {
std::optional<uint64_t>
LinuxFiles::fileWriteLimit(const std::string &Identity) const {
  if (auto I = CreatedFileLimits.find(Identity); I != CreatedFileLimits.end())
    return I->second;
  if (auto I = Options->WriteLimits.find(Identity);
      I != Options->WriteLimits.end())
    return I->second;
  return std::nullopt;
}

bool LinuxFiles::tracksFileMetadata(const std::string &Identity) const {
  return CreatedFileLimits.contains(Identity) ||
         Options->TrackWriteMetadata.contains(Identity);
}

LinuxFiles::MutableFile &LinuxFiles::mutableFile(const std::string &Path) {
  auto [I, Inserted] = MutableFiles.try_emplace(Path);
  auto &F = I->second;
  if (Inserted) {
    F.Bytes = Options->Files.at(Path);
    if (Options->TrackWriteMetadata.contains(Path)) {
      F.Metadata = Options->Metadata.at(Path);
      for (uint64_t Page = 0; Page * 4096 < F.Bytes.size(); ++Page)
        F.AllocatedPages.insert(Page);
    }
  }
  return F;
}
const LinuxFileMetadata *
LinuxFiles::fileMetadata(const std::string &Path) const {
  if (auto I = MutableFiles.find(Path); I != MutableFiles.end()) {
    if (I->second.Metadata)
      return &*I->second.Metadata;
    if (I->second.Changed)
      return nullptr;
  }
  auto I = Options->Metadata.find(Path);
  return I == Options->Metadata.end() ? nullptr : &I->second;
}
bool LinuxFiles::touchWrite(MutableFile &File, ProcessResult &Result) {
  if (!File.Metadata)
    return true;
  if (!Clock) {
    unsupported(Result,
                "file metadata update requires a virtual realtime clock");
    return false;
  }
  auto Now = Clock->read(ClockRealtime, Result);
  if (!Now)
    return false;
  File.Metadata->ModificationTime = *Now;
  File.Metadata->ChangeTime = *Now;
  return true;
}
void LinuxFiles::allocateWrittenPages(MutableFile &File, uint64_t Begin,
                                      uint64_t Size) {
  if (!File.Metadata || !Size)
    return;
  for (uint64_t Page = Begin / 4096; Page <= (Begin + Size - 1) / 4096; ++Page)
    File.AllocatedPages.insert(Page);
  File.Metadata->Size = File.Bytes.size();
  File.Metadata->Blocks = File.AllocatedPages.size() * 8;
}
bool LinuxFiles::isRegularFile(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  return I != Descriptors.end() && std::holds_alternative<OpenFile>(*I->second);
}
llvm::ArrayRef<uint8_t> LinuxFiles::fileBytes(const OpenFile &File) const {
  auto I = MutableFiles.find(File.Path);
  return I == MutableFiles.end() ? File.Bytes
                                 : llvm::ArrayRef<uint8_t>(I->second.Bytes);
}
llvm::Expected<std::optional<uint64_t>> LinuxFiles::writeDescriptor(
    ServiceKind Kind, const ProcessServiceEvent &Event,
    const std::optional<LinuxKernelOptions> &Kernel, ProcessResult &Result,
    std::optional<llvm::ArrayRef<llvm::StringRef>> Materialized) {
  using Reply = std::optional<uint64_t>;
  if (!Options)
    return unsupported(Result, FileInputsMissing);
  const uint32_t FD = Event.Arguments[0];
  const uint64_t Address = Event.Arguments[1], Size = Event.Arguments[2];
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return Reply(uint64_t(0) - BadDescriptor);
  if (auto *Socket = std::get_if<OpenSocket>(I->second.get())) {
    if (Kind != ServiceKind::Write)
      return unsupported(Result, "vectored TCP writes are unmodeled");
    return transferSocket(*Socket, true, Address, Size, Result);
  }
  auto *F = std::get_if<OpenFile>(I->second.get());
  if (!F)
    return unsupported(Result, DeviceOperation);
  if (Kind == ServiceKind::Write) {
    if (!(F->Flags & 3))
      return Reply(uint64_t(0) - BadDescriptor);
    if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
      return Reply(uint64_t(0) - BadAddress);
    const WriteBuffer Buffer{Address, Size};
    return writeFile(*F, Buffer, Result);
  }
  assert(Kind == ServiceKind::WriteV);
  llvm::SmallVector<WriteBuffer> Buffers;
  if (Materialized) {
    materializeWriteBuffers(*Materialized, Layout, Buffers);
  } else {
    auto Total =
        importWriteBuffers(CPU, Address, Size, Layout, Kernel, Buffers);
    if (!Total)
      return Total.takeError();
    if (*Total < 0)
      return Reply(*Total);
  }
  // Unlike scalar write, writev imports the complete vector before testing
  // FMODE_WRITE. Invalid descriptors were rejected before the import above.
  if (!(F->Flags & 3))
    return Reply(uint64_t(0) - BadDescriptor);
  return writeFile(*F, Buffers, Result);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::writeFile(OpenFile &F, llvm::ArrayRef<WriteBuffer> Buffers,
                      ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  uint64_t Size = 0;
  for (const auto &Buffer : Buffers)
    Size += Buffer.Size;
  // The VFS validates the original cursor and count before the file operation
  // chooses EOF for an append. A small EOF must not hide signed overflow.
  if (Size > MaxSignedIOSize - F.Offset)
    return Reply(uint64_t(0) - InvalidArgument);
  auto &Object = MutableFiles.at(F.Path);
  uint64_t Position = (F.Flags & OpenAppend) ? Object.Bytes.size() : F.Offset;
  Size = std::min(Size, MaxReadWriteSize & ~(Layout.PageSize - 1));
  const auto Limit = *fileWriteLimit(F.Path);
  if (Size && (Position > Limit || Size > Limit - Position))
    return unsupported(Result,
                       "memory-file write exceeds explicit reservation");
  // The selected noatime memory-file provider performs its timestamp update
  // before copying data, including a nonempty write that later faults.
  if (Size && !touchWrite(Object, Result))
    return Reply();
  uint64_t Done = 0;
  for (const auto &Buffer : Buffers) {
    uint64_t Copied = 0;
    const uint64_t Length = std::min(Buffer.Size, Size - Done);
    while (Copied < Length) {
      uint64_t Count = Length - Copied;
      std::vector<uint8_t> Storage;
      llvm::ArrayRef<uint8_t> Bytes;
      if (auto *Text = std::get_if<llvm::StringRef>(&Buffer.Source)) {
        Bytes = llvm::ArrayRef<uint8_t>(Text->bytes_begin() + Copied, Count);
      } else {
        const auto At = std::get<uint64_t>(Buffer.Source) + Copied;
        Count = std::min(Count, Layout.PageSize - At % Layout.PageSize);
        auto Accessible = CPU.canAccess(At, Count, Read | UserAccessible);
        if (!Accessible)
          return Accessible.takeError();
        if (!*Accessible)
          return Done ? Reply(Done) : Reply(uint64_t(0) - BadAddress);
        Storage.resize(Count);
        if (auto E = CPU.read(At, Storage))
          return std::move(E);
        Bytes = Storage;
      }
      if (Position + Count > Object.Bytes.size())
        Object.Bytes.resize(Position + Count, 0);
      std::copy(Bytes.begin(), Bytes.end(), Object.Bytes.begin() + Position);
      Object.Changed = true;
      allocateWrittenPages(Object, Position, Count);
      Copied += Count;
      Done += Count;
      Position += Count;
      F.Offset = Position;
    }
  }
  return Reply(Done);
}
} // namespace neverd::emulation::linux_model
