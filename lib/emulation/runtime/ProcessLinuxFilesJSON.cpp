//===- ProcessLinuxFilesJSON.cpp - Bounded explicit file inputs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessLinuxFilesJSON.h"

#include "../os/linux/kernel/LinuxFiles.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

#include <utility>

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::LinuxFilesOptions + Name);
}

llvm::Expected<LinuxIoctlTraceOptions>
ioctlTrace(const llvm::json::Value &Value) {
  const auto *O = Value.getAsObject();
  const auto *Rows = O ? O->getArray(field::IoctlTraceRequests) : nullptr;
  auto Events = O ? O->getInteger(field::IoctlTraceEventLimit) : std::nullopt;
  auto Bytes = O ? O->getInteger(field::IoctlTraceByteLimit) : std::nullopt;
  const bool HasUnlisted = O && O->get(field::IoctlTraceIncludeUnlisted);
  auto Unlisted =
      O ? O->getBoolean(field::IoctlTraceIncludeUnlisted) : std::nullopt;
  if (!O || O->size() != 3 + unsigned(HasUnlisted) ||
      (HasUnlisted && !Unlisted) || !Rows ||
      Rows->size() > ::neverd::limits::emulation::
                         kProcessLinuxFilesJSONIoctlTraceRowsSizeLimit ||
      !Events || *Events <= 0 ||
      *Events > ::neverd::limits::emulation::kMaxIoctlTraceEvents || !Bytes ||
      *Bytes <= 0 ||
      *Bytes > ::neverd::limits::emulation::
                   kProcessLinuxFilesJSONIoctlTraceBytesLimit)
    return invalid(field::IoctlTrace);
  LinuxIoctlTraceOptions Out;
  Out.EventLimit = *Events;
  Out.ByteLimit = *Bytes;
  Out.IncludeUnlisted = Unlisted.value_or(Out.IncludeUnlisted);
  for (const auto &Row : *Rows) {
    const auto *R = Row.getAsObject();
    const auto *N = R ? R->get(field::DeviceRequest) : nullptr;
    auto Request = N ? process_json::integer<uint32_t>(*N) : std::nullopt;
    const auto *Ranges = R ? R->getArray(field::Memory) : nullptr;
    if (!R || R->size() != 2 || !Request || !Ranges || Ranges->size() > 16 ||
        Out.Requests.contains(*Request))
      return invalid(field::IoctlTraceRequests);
    auto &Destination = Out.Requests[*Request];
    for (const auto &Range : *Ranges) {
      const auto *M = Range.getAsObject();
      const auto *P = M ? M->get(field::DevicePointerOffset) : nullptr;
      const auto *Offset = M ? M->get(field::DeviceOffset) : nullptr;
      const auto *Size = M ? M->get(field::Size) : nullptr;
      auto A = Offset ? process_json::integer<uint64_t>(*Offset) : std::nullopt;
      auto S = Size ? process_json::integer<uint64_t>(*Size) : std::nullopt;
      auto Pointer = P ? process_json::integer<uint64_t>(*P) : std::nullopt;
      if (!M || M->size() != 2 + unsigned(P != nullptr) || !A || !S ||
          (P && !Pointer))
        return invalid(field::IoctlTrace);
      Destination.push_back({*A, *S, Pointer});
    }
  }
  // The file validator also serves the direct C++ entry point.
  LinuxFileOptions Check;
  Check.IoctlTrace = Out;
  if (auto E = linux_model::validateFileOptions(Check))
    return std::move(E);
  return Out;
}

llvm::Expected<LinuxFileMetadata> metadata(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 12)
    return invalid(field::FileMetadata);
  LinuxFileMetadata Out;
  auto Number = [&](llvm::StringRef Name, auto &Destination) -> llvm::Error {
    const auto *V = Object->get(Name);
    using T = std::remove_reference_t<decltype(Destination)>;
    auto N = V ? process_json::integer<T>(*V) : std::nullopt;
    if (!N)
      return invalid(Name);
    Destination = *N;
    return llvm::Error::success();
  };
  if (auto E = Number(field::FileDevice, Out.Device))
    return std::move(E);
  if (auto E = Number(field::FileInode, Out.Inode))
    return std::move(E);
  if (auto E = Number(field::FileMode, Out.Mode))
    return std::move(E);
  if (auto E = Number(field::FileLinkCount, Out.LinkCount))
    return std::move(E);
  if (auto E = Number(field::FileUID, Out.UID))
    return std::move(E);
  if (auto E = Number(field::FileGID, Out.GID))
    return std::move(E);
  if (auto E = Number(field::Size, Out.Size))
    return std::move(E);
  if (auto E = Number(field::FileBlockSize, Out.BlockSize))
    return std::move(E);
  if (auto E = Number(field::FileBlocks, Out.Blocks))
    return std::move(E);
  for (const auto &[Name, Destination] :
       {std::pair<llvm::StringRef, LinuxTimespec *>{field::FileAccessTime,
                                                    &Out.AccessTime},
        {field::FileModificationTime, &Out.ModificationTime},
        {field::FileChangeTime, &Out.ChangeTime}}) {
    const auto *Time = Object->getObject(Name);
    if (!Time || Time->size() != 2 || !Time->get(field::Seconds) ||
        !Time->get(field::Nanoseconds))
      return invalid(Name);
    auto Seconds = process_json::integer<int64_t>(*Time->get(field::Seconds));
    auto Nanoseconds =
        process_json::integer<int64_t>(*Time->get(field::Nanoseconds));
    if (!Seconds || !Nanoseconds)
      return invalid(Name);
    *Destination = {*Seconds, *Nanoseconds};
  }
  return Out;
}
} // namespace

llvm::Expected<LinuxFileOptions>
linuxFileOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || !Object->get(field::Files))
    return invalid(field::LinuxFiles);
  LinuxFileOptions Out;
  uint64_t PayloadBytes = 0;
  uint64_t Transactions = 0;
  size_t Entries = 0;
  auto Reserve = [&](llvm::StringRef Path, llvm::StringRef Hex) {
    const uint64_t Cost = Path.size() + 1;
    if (++Entries > linux_model::FileCountLimit ||
        Cost > linux_model::FileByteLimit - PayloadBytes ||
        Hex.size() / 2 > linux_model::FileByteLimit - PayloadBytes - Cost)
      return false;
    PayloadBytes += Cost + Hex.size() / 2;
    return true;
  };
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::IoctlTrace) {
      auto Trace = ioctlTrace(V);
      if (!Trace)
        return Trace.takeError();
      Out.IoctlTrace = std::move(*Trace);
      continue;
    }
    if (Name == field::DynamicProc) {
      const auto *P = V.getAsObject();
      auto Comm = P ? P->getString(field::Name) : std::nullopt;
      const auto *Parent = P ? P->get(field::ProcParentPID) : nullptr;
      auto PID =
          Parent ? process_json::integer<uint32_t>(*Parent) : std::nullopt;
      if (!P ||
          P->size() != 2 + unsigned(P->get(field::ProcMappings) != nullptr) ||
          !Comm || !PID)
        return invalid(Name);
      Out.DynamicProc = LinuxProcOptions{Comm->str(), *PID};
      if (const auto *Rows = P->get(field::ProcMappings)) {
        const auto *Array = Rows->getAsArray();
        if (!Array || Array->size() > linux_model::FileCountLimit)
          return invalid(field::ProcMappings);
        for (const auto &Row : *Array) {
          const auto *R = Row.getAsObject();
          if (!R || R->size() != 7)
            return invalid(field::ProcMappings);
          LinuxProcMapping M;
          auto N = [&](llvm::StringRef Key, auto &Value) {
            using T = std::remove_reference_t<decltype(Value)>;
            const auto *V = R->get(Key);
            auto Number = V ? process_json::integer<T>(*V) : std::nullopt;
            if (!Number)
              return false;
            Value = *Number;
            return true;
          };
          auto Path = R->getString(field::Path);
          auto Shared = R->getBoolean(field::ProcShared);
          if (!Path || !Shared || !N(field::Address, M.Address) ||
              !N(field::Size, M.Size) || !N(field::ProcOffset, M.Offset) ||
              !N(field::FileDevice, M.Device) || !N(field::FileInode, M.Inode))
            return invalid(field::ProcMappings);
          M.Path = Path->str();
          M.Shared = *Shared;
          Out.DynamicProc->Mappings.push_back(std::move(M));
        }
      }
      continue;
    }
    if (Name == field::DynamicProcFDs) {
      auto B = V.getAsBoolean();
      if (!B)
        return invalid(Name);
      Out.DynamicProcFDs = *B;
      continue;
    }
    if (Name == field::LinuxWorkingDirectory) {
      auto Path = V.getAsString();
      if (!Path || Path->size() >= linux_model::FilePathLimit)
        return invalid(Name);
      Out.WorkingDirectory = Path->str();
      continue;
    }
    if (Name == field::DirectoryCreation) {
      const auto *C = V.getAsObject();
      const auto *Parents =
          C ? C->getArray(field::DirectoryCreationParents) : nullptr;
      auto Number = [&](llvm::StringRef Key) {
        const auto *N = C ? C->get(Key) : nullptr;
        return N ? process_json::integer<uint32_t>(*N) : std::nullopt;
      };
      auto Mask = Number(field::DirectoryCreationUmask);
      auto Limit = Number(field::DirectoryCreationLimit);
      if (!C || C->size() != 3 || !Parents || !Mask || !Limit ||
          Parents->size() > linux_model::FileCountLimit)
        return invalid(Name);
      auto &D = Out.DirectoryCreation.emplace();
      D.Umask = *Mask;
      D.Limit = *Limit;
      for (const auto &Row : *Parents) {
        const auto *P = Row.getAsObject();
        auto Path = P ? P->getString(field::Path) : std::nullopt;
        const auto *M = P ? P->get(field::FileMetadata) : nullptr;
        if (!P || P->size() != 2 || !Path || !M ||
            Path->size() >= linux_model::FilePathLimit ||
            D.Parents.contains(Path->str()))
          return invalid(Name);
        auto Parsed = metadata(*M);
        if (!Parsed)
          return Parsed.takeError();
        D.Parents.emplace(Path->str(), *Parsed);
      }
      continue;
    }
    if (Name == field::FileCreations) {
      const auto *Rows = V.getAsArray();
      if (!Rows || Rows->size() > LinuxFileCreationOptions::MaxFiles)
        return invalid(Name);
      auto &Creations = Out.FileCreations.emplace();
      for (const auto &Row : *Rows) {
        const auto *R = Row.getAsObject();
        auto Path = R ? R->getString(field::Path) : std::nullopt;
        const auto *N = R ? R->get(field::FileWriteLimit) : nullptr;
        auto Limit = N ? process_json::integer<uint64_t>(*N) : std::nullopt;
        LinuxFileCreationReservation Reservation;
        const auto *C = R ? R->get(field::FileCreationCount) : nullptr;
        auto Count = C ? process_json::integer<uint32_t>(*C)
                       : std::optional<uint32_t>(Reservation.Count);
        if (!R || R->size() != 2 + bool(C) || !Path || !Limit || !Count ||
            Path->size() >= linux_model::FilePathLimit ||
            !Creations.Files
                 .emplace(Path->str(),
                          LinuxFileCreationReservation{*Limit, *Count})
                 .second)
          return invalid(Name);
      }
      continue;
    }
    if (Name == field::FileRenames) {
      const auto *R = V.getAsObject();
      const auto *Parents =
          R ? R->getArray(field::DirectoryCreationParents) : nullptr;
      const auto *N = R ? R->get(field::DirectoryCreationLimit) : nullptr;
      auto Limit = N ? process_json::integer<uint32_t>(*N) : std::nullopt;
      if (!R || R->size() != 2 || !Parents ||
          Parents->size() > LinuxFileRenameOptions::MaxParents || !Limit)
        return invalid(Name);
      auto &Renames = Out.FileRenames.emplace();
      Renames.Limit = *Limit;
      for (const auto &P : *Parents) {
        auto Path = P.getAsString();
        if (!Path || Path->size() >= linux_model::FilePathLimit ||
            !Renames.Parents.insert(Path->str()).second)
          return invalid(Name);
      }
      continue;
    }
    if (Name == field::FileUnlinks) {
      const auto *U = V.getAsObject();
      const auto *Parents =
          U ? U->getArray(field::DirectoryCreationParents) : nullptr;
      const auto *N = U ? U->get(field::DirectoryCreationLimit) : nullptr;
      auto Limit = N ? process_json::integer<uint32_t>(*N) : std::nullopt;
      if (!U || U->size() != 2 || !Parents ||
          Parents->size() > LinuxFileUnlinkOptions::MaxParents || !Limit)
        return invalid(Name);
      auto &Unlinks = Out.FileUnlinks.emplace();
      Unlinks.Limit = *Limit;
      for (const auto &P : *Parents) {
        auto Path = P.getAsString();
        if (!Path || Path->size() >= linux_model::FilePathLimit ||
            !Unlinks.Parents.insert(Path->str()).second)
          return invalid(Name);
      }
      continue;
    }
    if (Name == field::FileDirectories) {
      const auto *Dirs = V.getAsArray();
      if (!Dirs || Dirs->size() > linux_model::FileCountLimit)
        return invalid(Name);
      for (const auto &D : *Dirs) {
        auto Path = D.getAsString();
        if (!Path || !Reserve(*Path, {}))
          return invalid(Name);
        Out.Directories.push_back(Path->str());
      }
      continue;
    }
    if (Name == field::FileDevices) {
      const auto *Devices = V.getAsArray();
      if (!Devices || Devices->size() > linux_model::FileCountLimit)
        return invalid(Name);
      for (const auto &Value : *Devices) {
        const auto *Device = Value.getAsObject();
        auto Path = Device ? Device->getString(field::Path) : std::nullopt;
        if (!Path || !Reserve(*Path, {}))
          return invalid(Name);
        auto Input =
            linuxDeviceOptionsFromJSON(*Device, PayloadBytes, Transactions);
        if (!Input)
          return Input.takeError();
        if (!Out.Devices.emplace(Path->str(), std::move(*Input)).second)
          return invalid(field::Path);
      }
      continue;
    }
    if (Name == field::DescriptorLimit) {
      auto N = V.getAsUINT64();
      if (!N || *N > UINT32_MAX)
        return invalid(Name);
      Out.DescriptorLimit = *N;
      continue;
    }
    if (Name == field::SymbolicLinks) {
      const auto *Links = V.getAsArray();
      if (!Links || Links->size() > linux_model::FileCountLimit)
        return invalid(Name);
      for (const auto &Link : *Links) {
        const auto *L = Link.getAsObject();
        if (!L || L->size() != 2)
          return invalid(Name);
        auto Path = L->getString(field::Path);
        auto Target = L->getString(field::SymbolicLinkTarget);
        if (!Path || !Target || Target->size() % 2 ||
            !Reserve(*Path, *Target) ||
            !llvm::all_of(*Target, llvm::isHexDigit))
          return invalid(Name);
        std::string Bytes = llvm::fromHex(*Target);
        if (!Out.SymbolicLinks
                 .emplace(Path->str(),
                          std::vector<uint8_t>(Bytes.begin(), Bytes.end()))
                 .second)
          return invalid(field::Path);
      }
      continue;
    }
    if (Name != field::Files)
      return invalid(Name);
    const auto *Files = V.getAsArray();
    if (!Files || Files->size() > linux_model::FileCountLimit)
      return invalid(Name);
    for (const auto &File : *Files) {
      const auto *F = File.getAsObject();
      if (!F || F->size() !=
                    2 + unsigned(F->get(field::FileMetadata) != nullptr) +
                        unsigned(F->get(field::FileWriteLimit) != nullptr) +
                        unsigned(F->get(field::TrackWriteMetadata) != nullptr))
        return invalid(Name);
      auto Path = F->getString(field::Path);
      auto Bytes = F->getString(field::Bytes);
      if (!Path || !Bytes || Bytes->size() % 2 || !Reserve(*Path, *Bytes) ||
          !llvm::all_of(*Bytes, llvm::isHexDigit))
        return invalid(Name);
      std::string Data = llvm::fromHex(*Bytes);
      if (!Out.Files
               .emplace(Path->str(),
                        std::vector<uint8_t>(Data.begin(), Data.end()))
               .second)
        return invalid(field::Path);
      if (const auto *Track = F->get(field::TrackWriteMetadata)) {
        auto Enabled = Track->getAsBoolean();
        if (!Enabled)
          return invalid(field::TrackWriteMetadata);
        if (*Enabled)
          Out.TrackWriteMetadata.insert(Path->str());
      }
      if (const auto *Limit = F->get(field::FileWriteLimit)) {
        auto N = process_json::integer<uint64_t>(*Limit);
        if (!N)
          return invalid(field::FileWriteLimit);
        Out.WriteLimits.emplace(Path->str(), *N);
      }
      if (const auto *M = F->get(field::FileMetadata)) {
        auto Parsed = metadata(*M);
        if (!Parsed)
          return Parsed.takeError();
        Out.Metadata.emplace(Path->str(), std::move(*Parsed));
      }
    }
  }
  if (auto E = linux_model::validateFileOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
