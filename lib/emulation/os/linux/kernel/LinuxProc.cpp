//===- LinuxProc.cpp - Process-owned live proc namespace ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The namespace borrows live task and descriptor owners, never host procfs.
#include "LinuxFiles.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation::linux_model {
namespace {
std::optional<uint32_t> decimal(llvm::StringRef Name) {
  uint32_t N;
  if (Name.empty() || (Name.size() > 1 && Name.front() == '0') ||
      !llvm::all_of(Name, llvm::isDigit) || Name.getAsInteger(10, N))
    return std::nullopt;
  return N;
}
} // namespace

void LinuxFiles::opened(uint32_t FD, uint32_t Flags) {
  CloseOnExec.erase(FD);
  if (Flags & OpenCloseOnExec)
    CloseOnExec.insert(FD);
}
std::vector<ProcTask> LinuxFiles::procTasks() const {
  return TaskProvider ? TaskProvider() : std::vector<ProcTask>{{ThreadID, 'R'}};
}

std::string LinuxFiles::procPath(llvm::StringRef Path) const {
  if (!Options->DynamicProc)
    return Path.str();
  for (const auto Alias :
       {llvm::StringRef("/proc/self"), llvm::StringRef("/proc/thread-self")}) {
    auto Tail = Path;
    if (Tail.consume_front(Alias) && (Tail.empty() || Tail.starts_with('/')))
      return procProcessPath() +
             (Alias == "/proc/self" ? std::string()
                                    : "/task/" + std::to_string(CurrentTask)) +
             Tail.str();
  }
  return Path.str();
}

std::optional<LinuxFiles::PathKind>
LinuxFiles::procKind(const std::string &Path, bool Directory) const {
  if (!Options->DynamicProc)
    return std::nullopt;
  if (Path == "/proc")
    return PathKind::Directory;
  if (Path == "/proc/self" || Path == "/proc/thread-self")
    return Directory ? PathKind::Directory : PathKind::SymbolicLink;
  const std::string Canonical = procPath(Path);
  llvm::StringRef Tail = Canonical;
  if (!Tail.consume_front(procProcessPath()) ||
      (!Tail.empty() && !Tail.starts_with('/')))
    return std::nullopt;
  if (Tail.empty() || Tail == "/task")
    return PathKind::Directory;
  if (Tail.consume_front("/task/")) {
    const auto [Name, Rest] = Tail.split('/');
    auto ID = decimal(Name);
    const auto Tasks = procTasks();
    if (!ID || !llvm::any_of(Tasks, [&](const auto &T) { return T.ID == *ID; }))
      return PathKind::Missing;
    if (Rest.empty())
      return PathKind::Directory;
    Tail = Tail.drop_front(Name.size());
  }
  if (Tail == "/fd" || Tail == "/fdinfo")
    return PathKind::Directory;
  if (Tail == "/maps" || Tail == "/status")
    return Directory ? PathKind::NotDirectory : PathKind::ProcFile;
  for (const auto Prefix :
       {llvm::StringRef("/fd/"), llvm::StringRef("/fdinfo/")}) {
    auto Name = Tail;
    if (!Name.consume_front(Prefix))
      continue;
    const auto [Number, Rest] = Name.split('/');
    auto FD = decimal(Number);
    if (!FD || !Descriptors.contains(*FD))
      return PathKind::Missing;
    if (Prefix == "/fd/")
      return Directory || !Rest.empty() ? PathKind::LinkTraversal
                                        : PathKind::SymbolicLink;
    return Directory || !Rest.empty() ? PathKind::NotDirectory
                                      : PathKind::ProcFile;
  }
  if (Tail.starts_with("/maps/") || Tail.starts_with("/status/"))
    return PathKind::NotDirectory;
  return PathKind::UnmodeledProc;
}

std::optional<std::string> LinuxFiles::procLink(const std::string &Path) const {
  if (Path == "/proc/self")
    return std::to_string(ProcessID);
  if (Path == "/proc/thread-self")
    return std::to_string(ProcessID) + "/task/" + std::to_string(CurrentTask);
  const auto Canonical = procPath(Path);
  const auto Pos = Canonical.find("/fd/");
  if (Pos == std::string::npos)
    return std::nullopt;
  auto FD = decimal(llvm::StringRef(Canonical).drop_front(Pos + 4));
  return FD ? descriptorPath(*FD) : std::nullopt;
}

std::optional<std::vector<LinuxFiles::ProcEntry>>
LinuxFiles::procEntries(const std::string &Path) const {
  if (!Options->DynamicProc)
    return std::nullopt;
  const auto P = procPath(Path);
  const auto Root = procProcessPath();
  std::vector<ProcEntry> Entries;
  auto Add = [&](llvm::StringRef Name, uint8_t Type) {
    Entries.push_back({Name.str(), Entries.size() + 2, Type});
  };
  if (P == "/proc") {
    Add("self", 10);
    Add("thread-self", 10);
    Add(std::to_string(ProcessID), 4);
    std::set<std::string> Others;
    auto Child = [&](llvm::StringRef Path) {
      if (Path.consume_front("/proc/"))
        Others.insert(Path.split('/').first.str());
    };
    forEachFilePath([&](const std::string &Path) { Child(Path); });
    for (const auto &[Path, Bytes] : Options->SymbolicLinks)
      Child(Path);
    for (const auto &[Path, Device] : Options->Devices)
      Child(Path);
    for (const auto &Path : Options->Directories)
      Child(Path);
    for (const auto &Name : Others) {
      const auto Kind = lookupPath("/proc/" + Name);
      Add(Name, Kind == PathKind::Directory      ? 4
                : Kind == PathKind::SymbolicLink ? 10
                : Kind == PathKind::File         ? 8
                                                 : 0);
    }
  } else if (P == Root + "/task") {
    for (const auto &Task : procTasks())
      Add(std::to_string(Task.ID), 4);
  } else if (P == Root || (llvm::StringRef(P).starts_with(Root + "/task/") &&
                           P.find('/', Root.size() + 6) == std::string::npos)) {
    if (P == Root)
      Add("task", 4);
    Add("fd", 4);
    Add("fdinfo", 4);
    Add("status", 8);
    Add("maps", 8);
  } else if (llvm::StringRef(P).ends_with("/fd") ||
             llvm::StringRef(P).ends_with("/fdinfo")) {
    if (!llvm::StringRef(P).starts_with(Root + "/"))
      return std::nullopt;
    const uint8_t Type = llvm::StringRef(P).ends_with("/fd") ? 10 : 8;
    for (const auto &[FD, Description] : Descriptors)
      Entries.push_back({std::to_string(FD), uint64_t(FD) + 2, Type});
  } else {
    return std::nullopt;
  }
  return Entries;
}
} // namespace neverd::emulation::linux_model
