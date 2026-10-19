//===- KernelModelProcessInformation.cpp - Guest process snapshots --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest process snapshots.
///

#include "../WindowsUnicode.h"
#include "KernelModel.h"
#include "KernelThreadPriorities.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Expected<uint64_t>
KernelModel::queryProcessInformation(llvm::ArrayRef<uint64_t> A, bool ZwCall) {
  auto Unsupported = [](const llvm::Twine &Text) -> llvm::Error {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "process information query: " + Text);
  };
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return Unsupported("class 5 requires Windows 10.0.19041");
  if (!ProcessInventory)
    return Unsupported("requires process_inventory");
  if (auto Context = executionProcessContext();
      !ZwCall && Context && Context->PreviousMode != 0)
    return Unsupported("Nt class 5 UserMode probing is not modeled");
  // Materialize the current logical thread once, through the same CID owner
  // used by PsGetCurrentThreadId and PsLookupThreadByThreadId.
  auto Current = currentThreadObject();
  if (!Current)
    return Current.takeError();

  constexpr size_t HeaderSize = 0x100, ThreadSize = 0x50;
  struct ThreadRow {
    uint64_t ID, Start;
    uint32_t Priority, State, Reason;
  };
  struct ProcessRow {
    const DriverProcessInformation *Facts;
    std::vector<uint8_t> Name;
    std::vector<ThreadRow> Threads;
    size_t Size = 0;
  };
  std::vector<ProcessRow> Rows;
  size_t Required = 0;
  for (uint32_t PID : LifecycleLiveProcesses) {
    if (!isLiveProcess(PID))
      continue;
    const auto Facts =
        std::find_if(ProcessInventory->begin(), ProcessInventory->end(),
                     [&](const auto &P) { return P.ProcessID == PID; });
    if (Facts == ProcessInventory->end())
      return Unsupported("missing metadata for live PID " + llvm::Twine(PID));
    auto Name = windows_unicode::bytes(Facts->ImageName, true);
    if (!Name)
      return Name.takeError();
    ProcessRow Row{&*Facts, std::move(*Name), {}, 0};
    for (const auto &[Object, Identity] : ThreadIdentities) {
      const auto System = ThreadObjects.find(Object);
      const bool Live =
          System != ThreadObjects.end()
              ? !System->second.Exited
              : std::any_of(
                    CurrentThreadObjects.begin(), CurrentThreadObjects.end(),
                    [&](const auto &Entry) { return Entry.second == Object; });
      if (!Live)
        continue;
      if (!Identity.CreatingProcessID)
        return Unsupported("live thread has no modeled process identity");
      if (*Identity.CreatingProcessID != PID)
        continue;
      // ExpGetProcessInformation (19041.6456) selects Win32StartAddress for
      // system threads, ETHREAD.StartAddress for ordinary user threads.
      // KernelMode callers retain kernel addresses. Never substitute one
      // entry for the other merely because only the Win32 entry is known.
      auto Start =
          PID == 4 ? Identity.StartAddress : Facts->BorrowedThreadSystemStart;
      if (!Start)
        return Unsupported("live thread has no modeled start address");
      auto Priority = queryThreadPriority(Object);
      if (!Priority)
        return Priority.takeError();
      uint64_t Key = 0;
      if (System != ThreadObjects.end())
        Key = System->second.CallbackID;
      else
        for (const auto &[Coordinate, KnownObject] : CurrentThreadObjects)
          if (KnownObject == Object)
            Key = Coordinate;
      uint32_t State = Object == *Current ? 2 : 1, Reason = 0;
      bool Waiting = false;
      for (const auto &[Registration, Wait] : WaitRegistrations)
        if (Wait.Thread == Key &&
            (Key != profile::StackBase ||
             PID == (UserRequestContext ? CurrentUserProcessID : 4))) {
          Waiting = true;
          State = 5;
          Reason = Wait.Type == KernelModel::Wait::Kind::Delay ? 4 : 0;
          break;
        }
      // A borrowed foreground coordinate between guest actions is dormant.
      // It has no scheduler-ready frame; it resumes with its next action.
      if (!Waiting && Object != *Current &&
          (System == ThreadObjects.end() || !System->second.System) &&
          Key == profile::StackBase)
        State = 5;
      Row.Threads.push_back(
          {Identity.ID, *Start, uint32_t(*Priority), State, Reason});
    }
    std::sort(Row.Threads.begin(), Row.Threads.end(),
              [](const auto &L, const auto &R) { return L.ID < R.ID; });
    Row.Size =
        (HeaderSize + ThreadSize * Row.Threads.size() + Row.Name.size() + 7) &
        ~size_t(7);
    if (Row.Size > UINT32_MAX || Required > UINT32_MAX - Row.Size)
      return Unsupported("snapshot exceeds ULONG wire length");
    Required += Row.Size;
    Rows.push_back(std::move(Row));
  }
  auto Writable = [&](uint64_t At, uint32_t Size) -> llvm::Error {
    if (auto E = objectBuffer(At, Size, true))
      return E;
    auto Storage = Memory.pinBacking(At, Size);
    if (!Storage)
      return Storage.takeError();
    return llvm::Error::success();
  };
  if (A[3])
    if (auto E = Writable(A[3], 4))
      return E;
  if (uint32_t(A[2]) < Required) {
    if (A[3])
      if (auto E = Memory.writeInteger(A[3], Required, 4))
        return E;
    return uint64_t(0xc0000004);
  }
  if (auto E = Writable(A[1], Required))
    return E;
  std::vector<uint8_t> Bytes(Required, 0);
  size_t Offset = 0;
  for (size_t I = 0; I < Rows.size(); ++I) {
    const auto &Row = Rows[I];
    auto *Out = Bytes.data() + Offset;
    auto Put32 = [&](size_t At, uint32_t N) {
      llvm::support::endian::write32le(Out + At, N);
    };
    auto Put64 = [&](size_t At, uint64_t N) {
      llvm::support::endian::write64le(Out + At, N);
    };
    Put32(0, I + 1 == Rows.size() ? 0 : Row.Size);
    Put32(4, Row.Threads.size());
    llvm::support::endian::write16le(Out + 0x38, Row.Name.size() - 2);
    llvm::support::endian::write16le(Out + 0x3a, Row.Name.size());
    const size_t NameOffset = HeaderSize + ThreadSize * Row.Threads.size();
    Put64(0x40, A[1] + Offset + NameOffset);
    Put64(0x50, Row.Facts->ProcessID);
    const auto Parent = LifecycleParents.find(Row.Facts->ProcessID);
    Put64(0x58, Parent == LifecycleParents.end() ? Row.Facts->ParentID
                                                 : Parent->second);
// All counter offsets and widths share the public observation schema.
#define NEVERD_PROCESS_COUNTER(Member, JSON, Type, Default, At, Width)         \
  if constexpr (Width == 4)                                                    \
    Put32(At, Row.Facts->Member);                                              \
  else                                                                         \
    Put64(At, Row.Facts->Member);
#include "neverd/emulation/DriverProcessInventory.def"
#undef NEVERD_PROCESS_COUNTER
    std::copy(Row.Name.begin(), Row.Name.end(), Out + NameOffset);
    for (size_t T = 0; T < Row.Threads.size(); ++T) {
      const auto &Thread = Row.Threads[T];
      const size_t At = HeaderSize + T * ThreadSize;
      Put64(At + 0x20, Thread.Start);
      Put64(At + 0x28, Row.Facts->ProcessID);
      Put64(At + 0x30, Thread.ID);
      Put32(At + 0x38, Thread.Priority);
      Put32(At + 0x3c, thread_priority::Default);
      Put32(At + 0x44, Thread.State);
      Put32(At + 0x48, Thread.Reason);
    }
    Offset += Row.Size;
  }
  if (auto E = Memory.write(A[1], Bytes))
    return E;
  if (A[3])
    if (auto E = Memory.writeInteger(A[3], Required, 4))
      return E;
  APICallDetail = "processes=" + std::to_string(Rows.size()) +
                  "; bytes=" + std::to_string(Required);
  return uint64_t(0);
}
} // namespace neverd::emulation
