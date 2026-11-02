//===- LinuxDirectories.cpp - Live guest directory and FD namespace -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Linux dirent64 records over the workload catalogue and live descriptors.
#include "LinuxFiles.h"
#include "LinuxUserMemory.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <set>

namespace neverd::emulation::linux_model {
std::optional<uint32_t> LinuxFiles::procDescriptor(llvm::StringRef Path) const {
  const std::string Canonical = procPath(Path);
  Path = Canonical;
  const auto Prefix = Options->DynamicProc ? procProcessPath() + "/fd/"
                                           : std::string("/proc/self/fd/");
  if (!Path.consume_front(Prefix) || Path.empty() ||
      (Path.size() > 1 && Path.front() == '0'))
    return std::nullopt;
  uint32_t FD;
  if (!llvm::all_of(Path, llvm::isDigit) || Path.getAsInteger(10, FD))
    return std::nullopt;
  return FD;
}

std::optional<std::string> LinuxFiles::descriptorPath(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return std::nullopt;
  if (const auto *F = std::get_if<OpenFile>(I->second.get()))
    return fileName(F->Path);
  if (const auto *P = std::get_if<OpenProcFile>(I->second.get()))
    return P->Path;
  if (const auto *D = std::get_if<OpenDirectory>(I->second.get()))
    return D->Path;
  if (const auto *D = std::get_if<OpenDevice>(I->second.get()))
    for (const auto &[Path, Device] : Options->Devices)
      if (&Device == (*D)->Options)
        return Path;
  if (std::holds_alternative<ProcessDescriptor>(*I->second))
    return "anon_inode:[pidfd]";
  // Captured stdout/stderr do not establish a tty, pipe inode or pathname.
  return std::nullopt;
}

llvm::Expected<uint64_t> LinuxFiles::inode(const std::string &Path) {
  if (auto *M = directoryMetadata(Path))
    return M->Inode;
  const auto *Object = fileIdentity(Path);
  const auto &Identity = Object ? *Object : Path;
  // Only a live file binding can borrow its original object's metadata.
  // A new directory may reuse a retired pathname without becoming that file.
  if (Object)
    if (auto M = Options->Metadata.find(*Object); M != Options->Metadata.end())
      return M->second.Inode;
  if (Object && CreatedFileLimits.contains(*Object))
    return fileMetadata(*Object)->Inode;
  if (auto I = Inodes.find(Identity); I != Inodes.end())
    return I->second;
  if (Inodes.size() >= FileDescriptorLimit * 2 + FileCountLimit)
    return failure("directory inode identity budget exhausted");
  if (UsedInodes.empty()) {
    for (const auto &[P, M] : Options->Metadata)
      UsedInodes.insert(M.Inode);
    if (Options->DirectoryCreation)
      for (const auto &[P, M] : Options->DirectoryCreation->Parents)
        UsedInodes.insert(M.Inode);
  }
  while (UsedInodes.contains(NextInode))
    ++NextInode;
  const auto Number = NextInode++;
  UsedInodes.insert(Number);
  return Inodes.emplace(Identity, Number).first->second;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::directoryEntries(uint32_t FD, uint64_t Address, uint32_t Size,
                             ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return Reply(uint64_t(0) - BadDescriptor);
  auto *D = std::get_if<OpenDirectory>(I->second.get());
  if (!D)
    return Reply(uint64_t(0) - NotDirectory);
  if (Options->DynamicProc && procKind(D->Path, true) == PathKind::Missing)
    return Reply(uint64_t(0) - NoEntry);
  const bool TaskDirectory =
      Options->DynamicProc && D->Path == procProcessPath() + "/task";
  struct Entry {
    std::string Name, Path;
    uint64_t Cookie;
    uint8_t Type;
  };
  const auto Slash = D->Path.rfind('/');
  const auto Parent = Slash ? D->Path.substr(0, Slash) : "/";
  std::vector<Entry> Entries{{".", D->Path, 0, 4}, {"..", Parent, 1, 4}};
  auto Proc = procEntries(D->Path);
  if (TaskDirectory && Proc && D->Offset >= 2 && !D->NextTask.empty()) {
    auto First = llvm::find_if(
        *Proc, [&](const auto &E) { return E.Name == D->NextTask; });
    if (First != Proc->end()) {
      Proc->erase(Proc->begin(), First);
      for (size_t I = 0; I < Proc->size(); ++I)
        (*Proc)[I].Cookie = D->Offset + I;
    }
  }
  D->NextTask.clear();
  if (Proc) {
    for (const auto &E : *Proc)
      Entries.push_back({E.Name, D->Path + '/' + E.Name, E.Cookie, E.Type});
  } else if (Options->DynamicProcFDs && D->Path == "/proc/self/fd") {
    for (const auto &[Number, Description] : Descriptors) {
      const auto Name = std::to_string(Number);
      Entries.push_back({Name, D->Path + '/' + Name, uint64_t(Number) + 2, 10});
    }
  } else {
    const std::string Prefix = D->Path == "/" ? "/" : D->Path + '/';
    std::set<std::string> Names;
    auto Add = [&](llvm::StringRef Path) {
      if (Path.consume_front(Prefix) && !Path.empty())
        Names.insert(Path.split('/').first.str());
    };
    forEachFilePath([&](const std::string &Path) { Add(Path); });
    for (const auto &[Path, Bytes] : Options->SymbolicLinks)
      Add(Path);
    for (const auto &[Path, Device] : Options->Devices)
      Add(Path);
    for (const auto &Path : Options->Directories)
      Add(Path);
    if (Options->DynamicProc)
      Add(procProcessPath());
    else if (Options->DynamicProcFDs)
      Add("/proc/self/fd");
    std::vector<std::string> Ordered(Names.begin(), Names.end());
    // Append new children without renumbering cookies already returned by an
    // open description. dup shares that cursor; rewind sees the same inodes.
    for (const auto &Created : CreatedDirectories) {
      llvm::StringRef Child(Created);
      if (Child.consume_front(Prefix) && !Child.empty() && !Child.contains('/'))
        Ordered.push_back(Child.str());
    }
    uint64_t Cookie = 2;
    for (const auto &Name : Ordered) {
      auto Path = Prefix + Name;
      const auto Kind = lookupPath(Path);
      uint8_t Type = Kind == PathKind::Directory ? 4
                     : (Kind == PathKind::File || Kind == PathKind::ProcFile)
                         ? 8
                     : Kind == PathKind::SymbolicLink ? 10
                                                      : 0;
      // A device contract does not establish char versus block device type.
      Entries.push_back({Name, std::move(Path), Cookie++, Type});
    }
  }
  const bool FDDirectory =
      (Options->DynamicProc &&
       llvm::StringRef(D->Path).starts_with(procProcessPath() + "/") &&
       (llvm::StringRef(D->Path).ends_with("/fd") ||
        llvm::StringRef(D->Path).ends_with("/fdinfo"))) ||
      (Options->DynamicProcFDs && D->Path == "/proc/self/fd");
  const uint64_t EndCookie =
      FDDirectory
          ? ((uint64_t(Options->DescriptorLimit) + 63) & ~uint64_t(63)) + 2
      : Entries.empty() ? 0
                        : Entries.back().Cookie + 1;
  uint64_t Done = 0;
  for (size_t Index = 0; Index < Entries.size(); ++Index) {
    const auto &E = Entries[Index];
    if (E.Cookie < D->Offset)
      continue;
    if (TaskDirectory && Index >= 2)
      D->NextTask = E.Name;
    D->Offset = E.Cookie; // Skipped closed FD slots still advance f_pos.
    const uint64_t Next =
        Index + 1 < Entries.size() ? Entries[Index + 1].Cookie : EndCookie;
    const uint64_t Length = (19 + E.Name.size() + 1 + 7) & ~uint64_t(7);
    if (Size > INT32_MAX || Length > Size - Done)
      return Done ? Reply(Done) : Reply(uint64_t(0) - InvalidArgument);
    if (Address > Layout.UserLimit || Done > Layout.UserLimit - Address ||
        Length > Layout.UserLimit - Address - Done)
      return Done ? Reply(Done) : Reply(uint64_t(0) - BadAddress);
    // Padding is not initialized by filldir64. Write only the defined header,
    // name and terminating NUL, retaining the caller's padding bytes.
    std::vector<uint8_t> Bytes(19 + E.Name.size() + 1, 0);
    auto Number = inode(E.Path);
    if (!Number)
      return Number.takeError();
    llvm::support::endian::write64le(Bytes.data(), *Number);
    llvm::support::endian::write64le(Bytes.data() + 8, Next);
    llvm::support::endian::write16le(Bytes.data() + 16, Length);
    Bytes[18] = E.Type;
    std::copy(E.Name.begin(), E.Name.end(), Bytes.begin() + 19);
    auto Stored = writeUserMemory(CPU, Layout, Address + Done, Bytes);
    if (!Stored)
      return Stored.takeError();
    if (*Stored == UserWriteResult::MixedAccess)
      return unsupported(Result, "partial Linux directory record output");
    if (*Stored == UserWriteResult::BadAddress)
      return Done ? Reply(Done) : Reply(uint64_t(0) - BadAddress);
    D->Offset = Next;
    Done += Length;
  }
  D->NextTask.clear();
  D->Offset = std::max(D->Offset, EndCookie);
  return Reply(Done);
}
} // namespace neverd::emulation::linux_model
