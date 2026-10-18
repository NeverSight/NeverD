//===- LinuxServices.cpp - Shared Linux kernel service semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxServices.h"

#include "LinuxKernelAvailability.h"
#include "LinuxTime.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/FormatVariadic.h"

#include <cassert>
namespace neverd::emulation::linux_model {
namespace {
std::optional<ServiceKind> serviceKind(GuestArchitecture ISA, uint64_t Number) {
  struct Binding {
    uint64_t Number;
    ServiceKind Kind;
  };
  static constexpr Binding X64[] = {
#define NEVERD_LINUX_SERVICE(Name, X64Number, ARMNumber, Count)                \
  {X64Number, ServiceKind::Name},
#define NEVERD_LINUX_X64_SERVICE(Name, Code, Count) {Code, ServiceKind::Name},
#include "../LinuxValues.def"
#undef NEVERD_LINUX_X64_SERVICE
#undef NEVERD_LINUX_SERVICE
  };
  static constexpr Binding ARM[] = {
#define NEVERD_LINUX_SERVICE(Name, X64Number, ARMNumber, Count)                \
  {ARMNumber, ServiceKind::Name},
#include "../LinuxValues.def"
#undef NEVERD_LINUX_SERVICE
  };
  llvm::ArrayRef<Binding> Bindings = ISA == GuestArchitecture::X64
                                         ? llvm::ArrayRef<Binding>(X64)
                                         : llvm::ArrayRef<Binding>(ARM);
  for (const auto &Entry : Bindings)
    if (Entry.Number == Number)
      return Entry.Kind;
  return std::nullopt;
}
} // namespace

llvm::Expected<std::optional<uint64_t>>
LinuxServices::handle(const ProcessServiceEvent &Event, ThreadContext *Thread) {
  auto Kind = serviceKind(CPU.architecture(), Event.Number);
  if (!Kind) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = llvm::formatv(Service, Event.Number).str();
    return std::optional<uint64_t>();
  }
  return handle(*Kind, Event, Thread);
}
llvm::Expected<std::optional<uint64_t>>
LinuxServices::handle(ServiceKind Kind, const ProcessServiceEvent &Event,
                      ThreadContext *Thread) {
  Files.setCurrentTask(Thread ? Thread->ID : ThreadID);
  if (auto E = synchronizeDevices())
    return std::move(E);
  if (unavailableKernelService(Kind, Options.LinuxKernel))
    return std::optional<uint64_t>(uint64_t(0) - ServiceNotImplemented);
  switch (Kind) {
  case ServiceKind::SetSocketOption:
  case ServiceKind::GetSocketOption:
  case ServiceKind::Socket:
  case ServiceKind::Connect:
  case ServiceKind::SendTo:
  case ServiceKind::ReceiveFrom:
    return Files.network(Kind, Event, Options.LinuxNetwork, Result);
  case ServiceKind::PidFDOpen:
    if (Options.LinuxKernel && Options.LinuxKernel->GKI) {
      return Files.openProcessDescriptor(Event.Arguments[0], Event.Arguments[1],
                                         *Options.LinuxKernel, Result,
                                         Thread != nullptr);
    }
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = llvm::formatv(Service, Event.Number).str();
    return std::optional<uint64_t>();
  case ServiceKind::SignalAction:
    return Signals.handle(CPU, Layout, Event, Result);
  case ServiceKind::GetXAttr:
  case ServiceKind::LGetXAttr:
  case ServiceKind::FGetXAttr:
    return Files.extendedAttribute(Kind, Event, Options.LinuxKernel, Result);
  case ServiceKind::PPoll:
    return Files.poll(Event, Options.LinuxKernel, Result);
  case ServiceKind::PSelect:
    return Files.select(Event, Result);
  case ServiceKind::Getdents64:
  case ServiceKind::Open:
  case ServiceKind::OpenAt:
  case ServiceKind::Access:
  case ServiceKind::FaccessAt:
  case ServiceKind::Mkdir:
  case ServiceKind::MkdirAt:
  case ServiceKind::Read:
  case ServiceKind::Readlink:
  case ServiceKind::ReadlinkAt:
  case ServiceKind::Close:
  case ServiceKind::Dup:
  case ServiceKind::Dup2:
  case ServiceKind::Dup3:
  case ServiceKind::Ioctl:
  case ServiceKind::Lseek:
  case ServiceKind::Fstat:
  case ServiceKind::FstatAt:
  case ServiceKind::StatFS:
  case ServiceKind::FstatFS:
    return Files.handle(Kind, Event, Result);
  case ServiceKind::Time:
  case ServiceKind::GetTimeOfDay:
  case ServiceKind::ClockGetTime:
    return timeService(CPU, Kind, Event, Layout, Options, Clock, Result,
                       Thread ? Thread->ID : ThreadID);
  case ServiceKind::Nanosleep:
    return sleepService(CPU, Event, Layout, Clock, Thread, Result);
  case ServiceKind::Exit:
    if (Thread) {
      Thread->Exit = Event.Arguments[0] & ExitMask;
      return std::optional<uint64_t>();
    }
    [[fallthrough]];
  case ServiceKind::ExitGroup:
    Result.Stop = ProcessStopReason::Exited;
    Result.ExitStatus = Event.Arguments[0] & ExitMask;
    return std::optional<uint64_t>();
  case ServiceKind::GetPriority:
  case ServiceKind::SetPriority:
    return Priority.handle(Kind, Event, Thread ? Thread->ID : ThreadID, Result);
  case ServiceKind::GetPID:
    return std::optional<uint64_t>(ProcessID);
  case ServiceKind::GetTID:
    return std::optional<uint64_t>(Thread ? Thread->ID : ThreadID);
  case ServiceKind::GetUID:
  case ServiceKind::GetEUID:
    return std::optional<uint64_t>(UserID);
  case ServiceKind::GetGID:
  case ServiceKind::GetEGID:
    return std::optional<uint64_t>(GroupID);
  case ServiceKind::Mmap:
    if (Event.Arguments[3] == MapShared ||
        (!(Event.Arguments[3] & MapAnonymous) &&
         Files.isDevice(uint32_t(Event.Arguments[4])))) {
      auto Device = Files.mappingDevice(uint32_t(Event.Arguments[4]));
      if (const auto *Error = std::get_if<uint64_t>(&Device))
        return std::optional<uint64_t>(*Error);
      auto Owner = std::get<std::shared_ptr<LinuxDeviceState>>(Device);
      return Memory.mapDevice(Event, Owner ? Owner->Options : nullptr, Result,
                              Owner);
    }
    return Memory.handle(Kind, Event, Result);
  case ServiceKind::Mprotect:
  case ServiceKind::Munmap:
  case ServiceKind::Madvise:
  case ServiceKind::Mincore:
  case ServiceKind::Brk:
    return Memory.handle(Kind, Event, Result);
  case ServiceKind::ArchPrctl:
    return archPrctl(CPU, Event, Layout, Result);
  case ServiceKind::Write:
  case ServiceKind::WriteV:
    if (Files.isDevice(Event.Arguments[0]) ||
        Files.isRegularFile(Event.Arguments[0]) ||
        Files.isSocket(Event.Arguments[0]))
      return Files.handle(Kind, Event, Result);
    if (Options.LinuxFiles) {
      if (auto E = Files.outputError(Event.Arguments[0])) {
        if (*E == uint64_t(0) - InvalidArgument)
          return writeOutput(CPU, Kind, Event, Layout, Options, Result, E);
        return E;
      }
      auto OutputEvent = Event;
      OutputEvent.Arguments[0] = Files.outputTarget(Event.Arguments[0]);
      return writeOutput(CPU, Kind, OutputEvent, Layout, Options, Result);
    }
    return writeOutput(CPU, Kind, Event, Layout, Options, Result);
  }
  llvm_unreachable("unhandled Linux service kind");
}
} // namespace neverd::emulation::linux_model
