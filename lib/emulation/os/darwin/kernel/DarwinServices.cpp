//===- DarwinServices.cpp - Darwin BSD service ABI and effects ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinEntropy.h"
#include "DarwinFiles.h"
#include "DarwinMemory.h"
#include "DarwinSystem.h"
#include "DarwinTime.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
using enum CPURegister;
constexpr CPURegister X64Arguments[] = {X64DI,  X64SI, X64DX,
                                        X64R10, X64R8, X64R9};
constexpr CPURegister ARMArguments[] = {AArch64X0, AArch64X1, AArch64X2,
                                        AArch64X3, AArch64X4, AArch64X5};
struct ServiceBinding {
  uint32_t Number;
  ServiceKind Kind;
  ServiceConvention Convention;
  bool ARM64Only;
};
std::optional<ServiceBinding> resolveService(GuestArchitecture ISA,
                                             uint64_t RawNumber) {
  // Both native entry paths decode only the low 32 bits. Keep the original
  // register value in the event; normalization belongs solely to resolution.
  uint32_t Number = RawNumber;
  auto Convention = ServiceConvention::BSD;
  if (ISA == GuestArchitecture::X64) {
    const uint32_t Class = Number & 0xff000000;
    if (Class == MachClass)
      Convention = ServiceConvention::Mach;
    else if (Class != BSDClass)
      return std::nullopt;
    Number &= 0x00ffffff;
  } else if (Number & 0x80000000) {
    Convention = ServiceConvention::Mach;
    // Unsigned negation also handles the unknown INT32_MIN trap safely.
    Number = 0u - Number;
  }
  static constexpr ServiceBinding Bindings[] = {
#define NEVERD_DARWIN_SERVICE(Name, Code, ReturnType)                          \
  {Code, ServiceKind::Name, ServiceConvention::BSD, false},
#define NEVERD_DARWIN_SERVICE_ALIAS(Name, Code)                                \
  {Code, ServiceKind::Name, ServiceConvention::BSD, false},
#define NEVERD_DARWIN_MACH_SERVICE(Name, Code, ARM64Only)                      \
  {Code, ServiceKind::Name, ServiceConvention::Mach, ARM64Only},
#include "../DarwinValues.def"
#undef NEVERD_DARWIN_MACH_SERVICE
#undef NEVERD_DARWIN_SERVICE_ALIAS
#undef NEVERD_DARWIN_SERVICE
  };
  for (const auto &Entry : Bindings)
    if (Entry.Number == Number && Entry.Convention == Convention &&
        (!Entry.ARM64Only || ISA == GuestArchitecture::AArch64))
      return Entry;
  return std::nullopt;
}
} // namespace

llvm::Expected<ProcessServiceEvent> readService(ExecutionBackend &CPU,
                                                const ServiceRequest &Request) {
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  auto Number = CPU.readRegister(X64 ? X64AX : AArch64X16);
  if (!Number)
    return Number.takeError();
  ProcessServiceEvent Event{Request.PC, (*Number)[0], {}, std::nullopt};
  for (size_t I = 0; I < Event.Arguments.size(); ++I) {
    auto Value = CPU.readRegister(X64 ? X64Arguments[I] : ARMArguments[I]);
    if (!Value)
      return Value.takeError();
    Event.Arguments[I] = (*Value)[0];
  }
  return Event;
}

llvm::Error returnService(ExecutionBackend &CPU, const ServiceRequest &Request,
                          ServiceResult Result) {
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  const auto FlagRegister = X64 ? X64FLAGS : AArch64NZCV;
  auto Flags = CPU.readRegister(FlagRegister);
  if (!Flags)
    return Flags.takeError();
  const bool BSD = Result.Convention == ServiceConvention::BSD;
  if (BSD) {
    const uint64_t Carry = X64 ? 1 : CarryARM64;
    (*Flags)[0] = ((*Flags)[0] & ~Carry) | (Result.Error ? Carry : 0);
    if (auto E = CPU.writeRegister(FlagRegister, *Flags))
      return E;
  }
  if (auto E = CPU.writeRegister(X64 ? X64AX : AArch64X0, {Result.Value, 0}))
    return E;
  // Mach preserves the secondary carrier and flags. BSD preserves RDX on
  // x64 error; ARM64 BSD clears X1 on both paths.
  if (BSD && (!X64 || !Result.Error))
    if (auto E = CPU.writeRegister(X64 ? X64DX : AArch64X1, {0, 0}))
      return E;
  if (X64) {
    if (auto E = CPU.writeRegister(X64CX, {Request.NextPC, 0}))
      return E;
    if (auto E = CPU.writeRegister(X64R11, *Flags))
      return E;
  }
  return CPU.writeRegister(X64 ? X64PC : AArch64PC, {Request.NextPC, 0});
}

namespace {
llvm::Expected<std::optional<ServiceResult>>
dispatchService(ServiceKind Kind, ExecutionBackend &CPU, DarwinMemory &Memory,
                DarwinFiles &Files, DarwinEntropy &Entropy,
                const ProcessServiceEvent &Event, const ProcessOptions &Options,
                ProcessResult &Result) {
  switch (Kind) {
  case ServiceKind::GetEntropy:
    return Entropy.handle(CPU, Event, Result);
  case ServiceKind::Sysctl:
  case ServiceKind::SysctlByName:
  case ServiceKind::GetRlimit:
  case ServiceKind::GetDTableSize:
  case ServiceKind::GetRusage:
  case ServiceKind::GetGroups:
  case ServiceKind::GetPgrp:
  case ServiceKind::GetPGID:
  case ServiceKind::GetSID:
  case ServiceKind::ThreadSelfID:
  case ServiceKind::ThreadSelfPort:
  case ServiceKind::TaskSelfPort:
  case ServiceKind::HostSelfPort:
  case ServiceKind::IsSetUGID:
  case ServiceKind::GetLogin:
  case ServiceKind::GetPriority:
  case ServiceKind::GetUID:
  case ServiceKind::GetEUID:
  case ServiceKind::GetGID:
  case ServiceKind::GetEGID:
    return systemService(CPU, Memory.pageSize(), Kind, Event,
                         Options.DarwinSystem, Result);
  case ServiceKind::TimebaseInfo:
  case ServiceKind::AbsoluteTime:
  case ServiceKind::ContinuousTime:
    return machTimeService(CPU, Kind, Event, Options.DarwinTime, Result);
  case ServiceKind::GetTimeOfDay:
    return timeService(CPU, Event, Options.DarwinTime, Result);
  case ServiceKind::Exit:
    Result.Stop = ProcessStopReason::Exited;
    Result.ExitStatus = Event.Arguments[0] & 0xff;
    return std::optional<ServiceResult>();
  case ServiceKind::Poll:
  case ServiceKind::Write:
  case ServiceKind::Readv:
  case ServiceKind::Writev:
  case ServiceKind::Preadv:
  case ServiceKind::Pwritev:
  case ServiceKind::Pwrite:
  case ServiceKind::Truncate:
  case ServiceKind::Ftruncate:
  case ServiceKind::Read:
  case ServiceKind::Pread:
  case ServiceKind::Symlink:
  case ServiceKind::Link:
  case ServiceKind::LinkAt:
  case ServiceKind::SymlinkAt:
  case ServiceKind::ReadLink:
  case ServiceKind::ReadLinkAt:
  case ServiceKind::Open:
  case ServiceKind::OpenAt:
  case ServiceKind::Access:
  case ServiceKind::FaccessAt:
  case ServiceKind::Mkdir:
  case ServiceKind::MkdirAt:
  case ServiceKind::Rmdir:
  case ServiceKind::Unlink:
  case ServiceKind::UnlinkAt:
  case ServiceKind::Rename:
  case ServiceKind::RenameAt:
  case ServiceKind::RenameAtX:
  case ServiceKind::Umask:
  case ServiceKind::Chdir:
  case ServiceKind::Fchdir:
  case ServiceKind::FstatAt64:
  case ServiceKind::GetDirEntries64:
  case ServiceKind::Close:
  case ServiceKind::Lseek:
  case ServiceKind::Dup:
  case ServiceKind::Dup2:
  case ServiceKind::Fcntl:
  case ServiceKind::GetAttrList:
  case ServiceKind::FgetAttrList:
  case ServiceKind::GetAttrListAt:
  case ServiceKind::GetAttrListBulk:
  case ServiceKind::GetXattr:
  case ServiceKind::FgetXattr:
  case ServiceKind::SetXattr:
  case ServiceKind::FsetXattr:
  case ServiceKind::RemoveXattr:
  case ServiceKind::FremoveXattr:
  case ServiceKind::ListXattr:
  case ServiceKind::FlistXattr:
  case ServiceKind::PathConf:
  case ServiceKind::FpathConf:
  case ServiceKind::Stat64:
  case ServiceKind::Fstat64:
  case ServiceKind::Lstat64:
    return Files.handle(Kind, Event, Result, Options.DarwinSystem);
  case ServiceKind::GetPID:
    return std::optional<ServiceResult>({ProcessID, false});
  case ServiceKind::GetPPID:
    return std::optional<ServiceResult>({ParentID, false});
  case ServiceKind::Mmap:
  case ServiceKind::Mprotect:
  case ServiceKind::Munmap:
    return Memory.handle(Kind, Event, Files, Result);
  }
  llvm_unreachable("unknown Darwin service");
}
} // namespace

llvm::Expected<std::optional<ServiceResult>>
handleService(ExecutionBackend &CPU, DarwinMemory &Memory, DarwinFiles &Files,
              DarwinEntropy &Entropy, const ProcessServiceEvent &Event,
              const ProcessOptions &Options, ProcessResult &Result) {
  const auto Binding = resolveService(CPU.architecture(), Event.Number);
  if (!Binding) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        llvm::formatv(diagnostic::UnsupportedService, Event.Number).str();
    return std::optional<ServiceResult>();
  }
  auto Returned = dispatchService(Binding->Kind, CPU, Memory, Files, Entropy,
                                  Event, Options, Result);
  if (Returned && *Returned)
    (**Returned).Convention = Binding->Convention;
  return Returned;
}
} // namespace neverd::emulation::darwin_model
