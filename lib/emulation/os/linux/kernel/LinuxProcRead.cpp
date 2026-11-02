//===- LinuxProcRead.cpp - Live proc records and sequential reads ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Record buffering belongs to the open description. Refill observes current
/// owners; retained bytes do not change underneath a short read or dup.
#include "LinuxFiles.h"
#include "LinuxMemory.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation::linux_model {
bool LinuxFiles::growProcBuffer(OpenProcFile &File, uint64_t Size,
                                ProcessResult &Result) {
  if (!File.Capacity)
    File.Capacity = Layout.PageSize;
  while (Size >= File.Capacity && File.Capacity < FileByteLimit)
    File.Capacity *= 2;
  if (Size < File.Capacity)
    return true;
  unsupported(Result, "proc record exceeds the buffer budget");
  return false;
}

llvm::Expected<std::optional<LinuxFiles::ProcRecords>>
LinuxFiles::procRecords(const OpenProcFile &File, ProcessResult &Result,
                        uint64_t &Error) {
  ProcRecords Records;
  if (File.Index == UINT64_MAX)
    return Records;
  llvm::StringRef Tail(File.Path);
  Tail.consume_front(procProcessPath());
  uint32_t ID = ProcessID;
  if (Tail.consume_front("/task/")) {
    const auto Name = Tail.split('/').first;
    if (Name.getAsInteger(10, ID))
      return failure("invalid owned proc task identity");
    Tail = Tail.drop_front(Name.size());
  }
  const auto Tasks = procTasks();
  auto Task = llvm::find_if(Tasks, [&](const auto &T) { return T.ID == ID; });
  if (Task == Tasks.end()) {
    Error = Tail.starts_with("/fdinfo/") ? NoEntry : NoSuchProcess;
    return Records;
  }
  auto Unsupported =
      [&](const char *Reason) -> llvm::Expected<std::optional<ProcRecords>> {
    unsupported(Result, Reason);
    return std::optional<ProcRecords>();
  };
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  if (Tail == "/status") {
    uint64_t Size = 0;
    auto Maps = Memory->procMappings();
    if (!Maps)
      return Maps.takeError();
    for (const auto &M : *Maps)
      Size += M.Size;
    const auto State = Task->State;
    OS << "Name:\t" << Options->DynamicProc->Name << "\nState:\t" << State
       << (State == 'R'   ? " (running)"
           : State == 'S' ? " (sleeping)"
                          : " (zombie)")
       << "\nTgid:\t" << ProcessID << "\nPid:\t" << ID << "\nPPid:\t"
       << Options->DynamicProc->ParentPID << "\nTracerPid:\t0\nUid:\t" << UserID
       << '\t' << UserID << '\t' << UserID << '\t' << UserID << "\nGid:\t"
       << GroupID << '\t' << GroupID << '\t' << GroupID << '\t' << GroupID
       << "\nFDSize:\t"
       << ((uint64_t(Options->DescriptorLimit) + 63) & ~uint64_t(63))
       << "\nVmSize:\t"
       << llvm::format("%8llu", (unsigned long long)(Size / 1024))
       << " kB\nThreads:\t" << Tasks.size() << '\n';
    Records.emplace_back(0, std::move(Text));
    return Records;
  }
  if (Tail.consume_front("/fdinfo/")) {
    uint32_t FD;
    if (Tail.getAsInteger(10, FD))
      return failure("invalid owned proc descriptor identity");
    auto I = Descriptors.find(FD);
    if (I == Descriptors.end()) {
      Error = NoEntry;
      return Records;
    }
    uint64_t Position = 0;
    uint32_t Flags = 0;
    if (const auto *F = std::get_if<OpenFile>(I->second.get())) {
      Position = F->Offset;
      Flags = F->Flags;
    } else if (const auto *D = std::get_if<OpenDirectory>(I->second.get())) {
      Position = D->Offset;
      Flags = D->Flags;
    } else if (const auto *P = std::get_if<OpenProcFile>(I->second.get())) {
      Position = P->Offset;
      Flags = P->Flags;
    } else if (const auto *D = std::get_if<OpenDevice>(I->second.get())) {
      Flags = (*D)->OpenFlags;
    } else {
      return Unsupported(
          "fdinfo requires modeled inode and file-operation state");
    }
    // The open file stores an object key, while inode() consumes a live name.
    // Share the same namespace projection as /proc/pid/fd symlink reads.
    auto Name = descriptorPath(FD);
    if (!Name)
      return Unsupported("fdinfo descriptor has no live catalogue identity");
    const auto &Path = *Name;
    Flags &= ~(OpenCreate | OpenExclusive | OpenTruncate | OpenCloseOnExec);
    Flags |= CPU.architecture() == GuestArchitecture::X64 ? OpenLargeFileX64
                                                          : OpenLargeFileARM64;
    if (CloseOnExec.contains(FD))
      Flags |= OpenCloseOnExec;
    auto Number = inode(Path);
    if (!Number)
      return Number.takeError();
    OS << "pos:\t" << Position << "\nflags:\t0" << llvm::format("%o", Flags)
       << "\nmnt_id:\t" << (procKind(Path, false) ? 2 : 1) << "\nino:\t"
       << *Number << '\n';
    Records.emplace_back(0, std::move(Text));
    return Records;
  }
  if (Tail != "/maps")
    return Unsupported("unmodeled proc record kind");
  auto Maps = Memory->procMappings();
  if (!Maps)
    return Maps.takeError();
  for (const auto &M : *Maps) {
    // Linux 6.6 resumes the VMA iterator at the next address, even after an
    // intervening merge or split. A mapping containing that address is live.
    if (M.Address + M.Size <= File.Index)
      continue;
    std::string Line;
    llvm::raw_string_ostream L(Line);
    uint64_t Inode = M.Inode;
    if (M.Shared && Options->Devices.contains(M.Path)) {
      auto Number = inode(M.Path);
      if (!Number)
        return Number.takeError();
      Inode = *Number;
    }
    const uint32_t Major = (M.Device >> 8) & 0xfff;
    const uint32_t Minor = (M.Device & 0xff) | ((M.Device >> 12) & 0xfff00);
    L << llvm::format_hex_no_prefix(M.Address, 8) << '-'
      << llvm::format_hex_no_prefix(M.Address + M.Size, 8) << ' '
      << (M.Permissions & Read ? 'r' : '-')
      << (M.Permissions & Write ? 'w' : '-')
      << (M.Permissions & Execute ? 'x' : '-') << (M.Shared ? 's' : 'p') << ' '
      << llvm::format_hex_no_prefix(M.Offset, 8) << ' '
      << llvm::format_hex_no_prefix(Major, 2) << ':'
      << llvm::format_hex_no_prefix(Minor, 2) << ' ' << Inode << ' ';
    if (!M.Path.empty() || M.AnonymousName) {
      if (Line.size() < 72)
        Line.resize(72, ' ');
      L << ' ';
      if (!M.Path.empty()) {
        for (char C : M.Path)
          if (C == '\n')
            L << "\\012";
          else
            L << C;
      } else {
        auto Name = Memory->procAnonymousName(M.AnonymousName);
        if (!Name)
          return Name.takeError();
        L << *Name; // Legacy anonymous names do not escape newline bytes.
      }
    }
    L << '\n';
    Records.emplace_back(M.Address, std::move(Line));
  }
  return Records;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::readProc(OpenProcFile &File, uint64_t Address, uint64_t Size,
                     ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  if (Address > Layout.UserLimit || Size > Layout.UserLimit - Address)
    return Reply(uint64_t(0) - BadAddress);
  if (Size > MaxSignedIOSize - File.Offset)
    return Reply(uint64_t(0) - InvalidArgument);
  Size = std::min(Size, MaxReadWriteSize & ~(Layout.PageSize - 1));
  if (!Size)
    return Reply(0);
  if (!File.Offset) {
    File.Index = 0;
    File.Buffer.clear();
    File.From = 0;
  }
  uint64_t Done = 0;
  auto Copy = [&]() -> llvm::Expected<bool> {
    while (Done < Size && File.From < File.Buffer.size()) {
      const auto At = Address + Done;
      const auto N =
          std::min({Size - Done, uint64_t(File.Buffer.size() - File.From),
                    Layout.PageSize - At % Layout.PageSize});
      auto Accessible = CPU.canAccess(At, N, Write | UserAccessible);
      if (!Accessible)
        return Accessible.takeError();
      if (!*Accessible)
        return false;
      auto Bytes = llvm::ArrayRef<uint8_t>(
          reinterpret_cast<const uint8_t *>(File.Buffer.data()) + File.From, N);
      if (auto E = CPU.write(At, Bytes))
        return std::move(E);
      File.From += N;
      File.Offset += N;
      Done += N;
    }
    return true;
  };
  auto Copied = Copy();
  if (!Copied)
    return Copied.takeError();
  if (!*Copied || File.From != File.Buffer.size())
    return Done ? Reply(Done) : Reply(uint64_t(0) - BadAddress);
  File.Buffer.clear();
  File.From = 0;
  uint64_t Error = 0;
  auto Rows = procRecords(File, Result, Error);
  if (!Rows)
    return Rows.takeError();
  if (!*Rows)
    return Reply();
  const auto &Records = **Rows;
  for (size_t I = 0; I < Records.size(); ++I) {
    const auto &R = Records[I];
    if (File.Buffer.empty()) {
      if (!growProcBuffer(File, R.second.size(), Result))
        return Reply();
    } else if (File.Buffer.size() + R.second.size() >= File.Capacity ||
               File.Buffer.size() >= Size - Done) {
      break;
    }
    File.Buffer += R.second;
    File.Index = I + 1 < Records.size() ? Records[I + 1].first : UINT64_MAX;
  }
  if (Records.empty() && !Error)
    File.Index = UINT64_MAX;
  Copied = Copy();
  if (!Copied)
    return Copied.takeError();
  if (Done)
    return Reply(Done);
  if (!*Copied)
    return Reply(uint64_t(0) - BadAddress);
  return Reply(uint64_t(0) - Error);
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::seekProc(OpenProcFile &File, uint64_t Offset, uint32_t Whence,
                     ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  if (Whence > SeekCurrent)
    return Reply(uint64_t(0) - InvalidArgument);
  const uint64_t Position = (Whence == SeekCurrent ? File.Offset : 0) + Offset;
  if (Position > MaxSignedIOSize)
    return Reply(uint64_t(0) - InvalidArgument);
  if (Position == File.Offset)
    return Reply(Position);
  // Traversal observes the old public f_pos. This matters when an fdinfo
  // description is duplicated over the descriptor it describes.
  File.Index = File.From = 0;
  File.Buffer.clear();
  bool Committed = false;
  auto ResetOnError = llvm::make_scope_exit([&] {
    if (!Committed) {
      File.Offset = File.Index = File.From = 0;
      File.Buffer.clear();
    }
  });
  if (Position) {
    uint64_t Error = 0;
    auto Rows = procRecords(File, Result, Error);
    if (!Rows)
      return Rows.takeError();
    if (!*Rows)
      return Reply();
    if (Error)
      return Reply(uint64_t(0) - Error);
    const auto &Records = **Rows;
    uint64_t Cursor = 0;
    File.Index = UINT64_MAX;
    for (size_t I = 0; I < Records.size(); ++I) {
      const auto &R = Records[I];
      if (!growProcBuffer(File, R.second.size(), Result))
        return Reply();
      File.Index = I + 1 < Records.size() ? Records[I + 1].first : UINT64_MAX;
      if (R.second.size() > Position - Cursor) {
        File.Buffer = R.second;
        File.From = Position - Cursor;
        break;
      }
      Cursor += R.second.size();
      if (Cursor == Position)
        break;
    }
  }
  File.Offset = Position;
  Committed = true;
  return Reply(Position);
}
} // namespace neverd::emulation::linux_model
