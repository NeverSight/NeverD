//===- DarwinSystem.cpp - Fixed Darwin system values and copy phases ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinSystem.h"

#include "DarwinUserMemory.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <type_traits>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
std::optional<ServiceResult> returned(uint64_t Value, bool Error = false) {
  return ServiceResult{Value, Error};
}
std::optional<ServiceResult> unsupported(ProcessResult &Result,
                                         const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}
enum class Observation {
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, Name, Root, Leaf) Member,
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
  Page32,
  Page64
};
struct Binding {
  Observation Kind;
  llvm::StringLiteral Name;
  uint32_t Root, Leaf;
};
constexpr Binding Bindings[] = {
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, Name, Root, Leaf)            \
  {Observation::Member, Name, Root, Leaf},
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
    {Observation::Page32, "hw.pagesize_compat", 6, 7},
    // This name has a dynamic native OID. Do not invent a numeric binding.
    {Observation::Page64, "hw.pagesize", 0, 0}};
struct Encoded {
  std::vector<uint8_t> Bytes;
  bool Quad = false;
};
template <typename T> Encoded encode(const T &Value) {
  Encoded Out;
  if constexpr (std::is_same_v<T, std::string>) {
    Out.Bytes.assign(Value.begin(), Value.end());
    Out.Bytes.push_back(0);
  } else {
    Out.Bytes.resize(sizeof(T));
    if constexpr (sizeof(T) == 8) {
      llvm::support::endian::write64le(Out.Bytes.data(), Value);
      Out.Quad = true;
    } else {
      llvm::support::endian::write32le(Out.Bytes.data(), Value);
    }
  }
  return Out;
}
std::optional<Encoded>
observation(Observation Kind, uint64_t PageSize,
            const std::optional<DarwinSystemOptions> &Options) {
  if (Kind == Observation::Page32)
    return encode(uint32_t(PageSize));
  if (Kind == Observation::Page64)
    return encode(PageSize);
  if (!Options)
    return std::nullopt;
  switch (Kind) {
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, Name, Root, Leaf)            \
  case Observation::Member:                                                    \
    return Options->Member ? std::optional(encode(*Options->Member))           \
                           : std::nullopt;
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
  default:
    return std::nullopt;
  }
}
template <typename T>
bool valid(const std::optional<T> &Value, uint64_t Limit) {
  if constexpr (std::is_same_v<T, std::string>)
    return !Value ||
           (Value->size() < Limit && Value->find('\0') == std::string::npos);
  return true;
}
template <typename T>
const char *invalidProcess(const std::optional<T> &Value) {
  if constexpr (std::is_same_v<T, uint32_t>) {
    if (Value && (!*Value || *Value > uint32_t(INT32_MAX)))
      return diagnostic::ProcessIdentityOption;
  } else if constexpr (std::is_same_v<T, int32_t>) {
    if (Value && (*Value < -int32_t(PriorityNiceBound) ||
                  *Value > int32_t(PriorityNiceBound)))
      return diagnostic::ProcessNiceOption;
  } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
    if (Value && Value->size() != LoginNameSize)
      return diagnostic::LoginNameOption;
  }
  return nullptr;
}
} // namespace

uint32_t credentialID(ServiceKind Kind,
                      const std::optional<DarwinSystemOptions> &Options) {
  const auto *C =
      Options && Options->Credentials ? &*Options->Credentials : nullptr;
  switch (Kind) {
  case ServiceKind::GetUID:
    return C ? C->RealUID : UserID;
  case ServiceKind::GetEUID:
    return C ? C->EffectiveUID : UserID;
  case ServiceKind::GetGID:
    return C ? C->RealGID : GroupID;
  case ServiceKind::GetEGID:
    return C ? C->EffectiveGID : GroupID;
  default:
    llvm_unreachable("invalid Darwin credential query");
  }
}
llvm::Error validateSystemOptions(const DarwinSystemOptions &Options) {
  if (Options.Credentials) {
    const auto &C = *Options.Credentials;
    for (auto ID : {C.RealUID, C.EffectiveUID, C.RealGID, C.EffectiveGID})
      if (ID > CredentialIDMax)
        return failure(diagnostic::CredentialOption);
    if (C.GroupAccessList) {
      const auto &G = *C.GroupAccessList;
      if (G.empty() || G.size() > GroupAccessLimit ||
          G.front() != C.EffectiveGID)
        return failure(diagnostic::CredentialOption);
      for (auto ID : G)
        if (ID > CredentialIDMax)
          return failure(diagnostic::CredentialOption);
    }
  }
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, Name, Root, Leaf)            \
  if (!valid(Options.Member, Observation::Member == Observation::HostName      \
                                 ? HostNameLimit                               \
                                 : SystemStringLimit))                         \
    return failure(Observation::Member == Observation::HostName                \
                       ? diagnostic::SystemHostName                            \
                       : diagnostic::SystemString);
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
#define NEVERD_DARWIN_PROCESS_FIELD(Member, Field)                             \
  if (const char *Reason = invalidProcess(Options.Member))                     \
    return failure(Reason);
#include "DarwinSystemFields.def"
#undef NEVERD_DARWIN_PROCESS_FIELD
  if (Options.CPUCount &&
      (!*Options.CPUCount || *Options.CPUCount > uint32_t(INT32_MAX)))
    return failure(diagnostic::SystemCPUCount);
  if (Options.MaxFilesPerProcess &&
      *Options.MaxFilesPerProcess > uint32_t(INT32_MAX))
    return failure(diagnostic::SystemMaxFilesPerProcess);
  for (const auto &[Resource, Limit] : Options.ResourceLimits)
    if (Resource >= ResourceLimitCount || Limit.Current > Limit.Maximum ||
        Limit.Maximum > ResourceLimitInfinity)
      return failure(diagnostic::ResourceLimitOption);
  for (const auto *Usage :
       {&Options.ResourceUsageSelf, &Options.ResourceUsageChildren})
    if (*Usage && ((**Usage).UserMicroseconds >= 1000000 ||
                   (**Usage).SystemMicroseconds >= 1000000))
      return failure(diagnostic::ResourceUsageOption);
  if (Options.EntropyReads) {
    if (Options.EntropyReads->size() > EntropyReplayLimit)
      return failure(diagnostic::EntropyOption);
    for (const auto &Read : *Options.EntropyReads)
      if (Read.empty() || Read.size() > EntropyReadLimit)
        return failure(diagnostic::EntropyOption);
  }
  return llvm::Error::success();
}

llvm::Expected<std::optional<ServiceResult>>
systemService(GuestMemory &Memory, uint64_t PageSize, ServiceKind Kind,
              const ProcessServiceEvent &Event,
              const std::optional<DarwinSystemOptions> &Options,
              ProcessResult &Result) {
  const auto &A = Event.Arguments;
  if (Kind == ServiceKind::GetPriority) {
    const uint32_t Which = uint32_t(A[0]), Who = uint32_t(A[1]);
    // XNU checks unsigned id_t before its selector switch. GPU selector5
    // belongs to setpriority only; no flags are masked from a getter selector.
    if (Who > CredentialIDMax)
      return returned(InvalidArgument, true);
    switch (Which) {
    case PriorityProcess:
      if (Who && Who != ProcessID)
        return unsupported(Result, diagnostic::ProcessPeerObservation);
      break;
    case PriorityThread:
      if (Who)
        return returned(InvalidArgument, true);
      [[fallthrough]];
    case PriorityGroup:
    case PriorityUser:
    case PriorityBackground:
    case PriorityRole:
    case PriorityGame:
    case PriorityCarPlay:
      return unsupported(Result, diagnostic::PrioritySelectionObservation);
    default:
      return returned(InvalidArgument, true);
    }
    if (!Options || !Options->ProcessNice)
      return unsupported(Result, diagnostic::ProcessNiceObservation);
    // INT returns use signed uu_rval on both native entry paths. In
    // particular -1 is a successful full-width result with carry clear.
    return returned(uint64_t(int64_t(*Options->ProcessNice)));
  }
  if (Kind == ServiceKind::GetLogin) {
    const uint32_t Size = std::min(uint32_t(A[1]), uint32_t(LoginNameSize));
    // XNU copies the raw session buffer, without string decoding or a NUL.
    // Native zero-length probes confirm no observation or pointer is needed.
    if (!Size)
      return returned(0);
    if (!Options || !Options->LoginNameBytes)
      return unsupported(Result, diagnostic::LoginNameObservation);
    return copyUserMemory(
        Memory, A[0], llvm::ArrayRef(*Options->LoginNameBytes).take_front(Size),
        diagnostic::LoginNamePartialOutput, Result);
  }
  switch (Kind) {
  case ServiceKind::GetUID:
  case ServiceKind::GetEUID:
  case ServiceKind::GetGID:
  case ServiceKind::GetEGID:
    return returned(credentialID(Kind, Options));
  default:
    break;
  }
  if (Kind == ServiceKind::GetPGID || Kind == ServiceKind::GetSID) {
    const uint32_t PID = uint32_t(A[0]);
    // XNU narrows the carrier to signed pid_t, then looks up that PID.
    // Negative values cannot name allocated processes: the read-only native
    // oracle confirms ESRCH before any self observation is selected.
    if (PID & 0x80000000u)
      return returned(NoProcess, true);
    if (PID && PID != ProcessID)
      return unsupported(Result, diagnostic::ProcessPeerObservation);
  }
  switch (Kind) {
  case ServiceKind::GetPgrp:
  case ServiceKind::GetPGID:
    return Options && Options->ProcessGroupID
               ? returned(*Options->ProcessGroupID)
               : unsupported(Result, diagnostic::ProcessGroupObservation);
  case ServiceKind::GetSID:
    return Options && Options->SessionID
               ? returned(*Options->SessionID)
               : unsupported(Result, diagnostic::ProcessSessionObservation);
  case ServiceKind::ThreadSelfID:
    return Options && Options->ThreadID
               ? returned(*Options->ThreadID)
               : unsupported(Result, diagnostic::ThreadIDObservation);
  case ServiceKind::IsSetUGID:
    return Options && Options->ProcessTainted
               ? returned(*Options->ProcessTainted)
               : unsupported(Result, diagnostic::ProcessTaintObservation);
  default:
    break;
  }
  if (Kind == ServiceKind::GetDTableSize) {
    if (!Options || !Options->MaxFilesPerProcess ||
        !Options->ResourceLimits.contains(ResourceLimitNoFile))
      return unsupported(Result, diagnostic::DescriptorTableObservation);
    // XNU proc_limitgetcur_nofile clips the full rlim_t before returning int.
    // Neither the hard limit nor a guest execution/descriptor budget applies.
    return returned(
        std::min(Options->ResourceLimits.at(ResourceLimitNoFile).Current,
                 uint64_t(*Options->MaxFilesPerProcess)));
  }
  if (Kind == ServiceKind::GetGroups) {
    const uint32_t Capacity = uint32_t(A[0]);
    if (Capacity & 0x80000000u)
      return returned(InvalidArgument, true);
    if (!Options || !Options->Credentials ||
        !Options->Credentials->GroupAccessList)
      return unsupported(Result, diagnostic::GroupObservation);
    const auto &Groups = *Options->Credentials->GroupAccessList;
    if (!Capacity)
      return returned(Groups.size());
    if (Capacity < Groups.size())
      return returned(InvalidArgument, true);
    std::vector<uint8_t> Bytes(Groups.size() * 4);
    for (size_t I = 0; I != Groups.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + I * 4, Groups[I]);
    auto Out = copyUserMemory(Memory, A[1], Bytes,
                              diagnostic::GroupPartialOutput, Result);
    if (!Out)
      return Out.takeError();
    if (*Out && !(**Out).Error)
      (**Out).Value = Groups.size();
    return Out;
  }
  if (Kind == ServiceKind::GetRusage) {
    const uint32_t Who = uint32_t(A[0]);
    if (Who != 0 && Who != UINT32_MAX)
      return returned(InvalidArgument, true);
    if (!Options)
      return unsupported(Result, diagnostic::ResourceUsageObservation);
    const auto &Selected =
        Who == 0 ? Options->ResourceUsageSelf : Options->ResourceUsageChildren;
    if (!Selected)
      return unsupported(Result, diagnostic::ResourceUsageObservation);
    const auto &Usage = *Selected;
    std::array<uint8_t, 32 + 8 * DarwinResourceUsage::CounterCount> Bytes{};
    llvm::support::endian::write64le(Bytes.data(), uint64_t(Usage.UserSeconds));
    llvm::support::endian::write32le(Bytes.data() + 8, Usage.UserMicroseconds);
    llvm::support::endian::write64le(Bytes.data() + 16,
                                     uint64_t(Usage.SystemSeconds));
    llvm::support::endian::write32le(Bytes.data() + 24,
                                     Usage.SystemMicroseconds);
    for (size_t I = 0; I != Usage.Counters.size(); ++I)
      llvm::support::endian::write64le(Bytes.data() + 32 + 8 * I,
                                       uint64_t(Usage.Counters[I]));
    return copyUserMemory(Memory, A[1], Bytes,
                          diagnostic::ResourceUsagePartialOutput, Result);
  }
  if (Kind == ServiceKind::GetRlimit) {
    const uint32_t Resource =
        uint32_t(A[0]) & ~uint32_t(ResourceLimitPosixFlag);
    if (Resource >= ResourceLimitCount)
      return returned(InvalidArgument, true);
    if (!Options || !Options->ResourceLimits.contains(Resource))
      return unsupported(Result, diagnostic::ResourceLimitObservation);
    const auto &Limit = Options->ResourceLimits.at(Resource);
    std::array<uint8_t, 16> Bytes;
    llvm::support::endian::write64le(Bytes.data(), Limit.Current);
    llvm::support::endian::write64le(Bytes.data() + 8, Limit.Maximum);
    return copyUserMemory(Memory, A[1], Bytes,
                          diagnostic::ResourceLimitPartialOutput, Result);
  }
  const bool Named = Kind == ServiceKind::SysctlByName;
  const uint64_t Count = Named ? A[1] : uint32_t(A[1]);
  if (Named ? Count >= SystemStringLimit : Count < 2 || Count > SystemMIBLimit)
    return returned(Named ? NameTooLong : InvalidArgument, true);
  std::vector<uint8_t> Input(Named ? Count : Count * 4);
  if (!Input.empty()) {
    auto Prefix = userMemoryPrefix(Memory, A[0], Input.size(), Read);
    if (!Prefix)
      return Prefix.takeError();
    if (!*Prefix)
      return returned(BadAddress, true);
    if (*Prefix != Input.size())
      return unsupported(Result, diagnostic::SystemPartialInput);
    if (auto E = Memory.read(A[0], Input))
      return std::move(E);
  }
  uint64_t Capacity = 0;
  if (A[3]) {
    // Native probes with faulting length pointers did not return within their
    // deadline. Admit only a complete readable/writable size_t before effects.
    auto Prefix = userMemoryPrefix(Memory, A[3], 8, Read | Write);
    if (!Prefix)
      return Prefix.takeError();
    if (*Prefix != 8)
      return unsupported(Result, diagnostic::SystemLengthMemory);
    std::array<uint8_t, 8> Bytes;
    if (auto E = Memory.read(A[3], Bytes))
      return std::move(E);
    Capacity = llvm::support::endian::read64le(Bytes.data());
  }
  const Binding *Selected = nullptr;
  if (Named) {
    // XNU copies every supplied byte before interpreting the first NUL and
    // removing one final dot. Remaining noncanonical names stay unmodeled.
    Input.push_back(0);
    llvm::StringRef Name(reinterpret_cast<const char *>(Input.data()));
    if (Name.empty())
      return returned(NoEntry, true);
    if (Name.ends_with("."))
      Name = Name.drop_back();
    for (const auto &B : Bindings)
      if (Name == B.Name) {
        Selected = &B;
        break;
      }
  } else if (Count == 2) {
    const auto Root = llvm::support::endian::read32le(Input.data());
    const auto Leaf = llvm::support::endian::read32le(Input.data() + 4);
    for (const auto &B : Bindings)
      if (B.Root && B.Root == Root && B.Leaf == Leaf) {
        Selected = &B;
        break;
      }
  }
  if (!Selected)
    return unsupported(Result, diagnostic::SystemKey);
  // A pointer alone is not a write request. Keep name/MIB and oldlenp
  // preflight before this decision. Root cannot infer a modeled privileged
  // write to kern.osversion, kern.maxfilesperproc or kern.hostname. A visible
  // hostname also supplies no mobile entitlement. Read-only nodes reject
  // writes.
  if (A[4] && A[5]) {
    if ((Selected->Kind == Observation::OSVersion ||
         Selected->Kind == Observation::MaxFilesPerProcess ||
         Selected->Kind == Observation::HostName) &&
        credentialID(ServiceKind::GetEUID, Options) == 0)
      return unsupported(Result, diagnostic::SystemPrivilegedWrite);
    return returned(OperationNotPermitted, true);
  }
  auto Value = observation(Selected->Kind, PageSize, Options);
  if (!Value)
    return unsupported(Result, diagnostic::SystemObservation);
  // XNU sysctl_hostname uses sysctl_io_string with truncation enabled.
  // A positive short capacity succeeds with its final byte replaced by NUL.
  // Check/copy only this actual span, once; zero capacity keeps ENOMEM below.
  if (Selected->Kind == Observation::HostName && A[2] && Capacity &&
      Capacity < Value->Bytes.size()) {
    Value->Bytes.resize(Capacity);
    Value->Bytes.back() = 0;
  }
  if (A[2] && Capacity == 4 && Value->Quad) {
    const auto Bits = llvm::support::endian::read64le(Value->Bytes.data());
    const uint64_t Low = uint32_t(Bits);
    const uint64_t Extended =
        Low & 0x80000000 ? Low | 0xffffffff00000000ULL : Low;
    if (Bits != Extended)
      return returned(ResultTooLarge, true);
    Value->Bytes.resize(4);
  }
  const bool Short = A[2] && Capacity < Value->Bytes.size();
  if (A[2] && !Short) {
    auto Copy = copyUserMemory(Memory, A[2], Value->Bytes,
                               diagnostic::SystemPartialOutput, Result);
    if (!Copy || !*Copy || (**Copy).Error)
      return Copy;
  }
  if (A[3]) {
    std::array<uint8_t, 8> Bytes;
    llvm::support::endian::write64le(Bytes.data(),
                                     Short ? 0 : Value->Bytes.size());
    auto Copy = copyUserMemory(Memory, A[3], Bytes,
                               diagnostic::SystemPartialOutput, Result);
    if (!Copy || !*Copy || (**Copy).Error)
      return Copy;
  }
  return returned(Short ? NoMemory : 0, Short);
}
} // namespace neverd::emulation::darwin_model
