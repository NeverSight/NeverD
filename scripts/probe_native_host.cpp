//===- probe_native_host.cpp - Standalone KVM/WHP host setup probe --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#if __has_include(<WinHvPlatform.h>)
#include <WinHvPlatform.h>
#else
#include <winhvplatform.h>
#endif
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <cpuid.h>
#include <cstddef>
#endif
#else
#error "Native host setup probe requires Linux or Windows"
#endif

namespace {
#define NEVERD_HOST_VALUE(Name, Value)                                         \
  [[maybe_unused]] constexpr uint64_t Name = Value;
#define NEVERD_HOST_TEXT(Name, Value)                                          \
  [[maybe_unused]] constexpr char Name[] = Value;
#include "NativeHostProbe.def"
#undef NEVERD_HOST_TEXT
#undef NEVERD_HOST_VALUE
namespace stage {
#define NEVERD_HOST_STAGE(Scope, Name, Phase)                                  \
  [[maybe_unused]] constexpr char Name[] = #Name;
#include "NativeHostProbe.def"
#undef NEVERD_HOST_STAGE
} // namespace stage

#if defined(__aarch64__) || defined(_M_ARM64)
constexpr uint64_t Architecture = ARM64Architecture;
#elif defined(__x86_64__) || defined(_M_X64)
constexpr uint64_t Architecture = X64Architecture;
#else
#error "Native host setup probe requires an x64 or ARM64 executable"
#endif

class Evidence {
public:
  int Result = ReadyExit;

  bool record(const char *Stage, bool Success, uint64_t Code = 0,
              uint64_t Value = 0, bool Missing = false) {
    const char *Outcome = Success ? OK : Missing ? Unavailable : Failed;
    std::printf(RecordFormat, Stage, Outcome,
                static_cast<unsigned long long>(Code),
                static_cast<unsigned long long>(Value));
    std::fflush(stdout);
    // Cleanup failure cannot turn a real failure into an availability skip.
    if (!Success && (Result == ReadyExit || !Missing))
      Result = Missing ? UnavailableExit : FailedExit;
    return Success;
  }

  bool nativeArchitecture() {
#if defined(_WIN32)
    SYSTEM_INFO Info{};
    GetNativeSystemInfo(&Info);
    const auto Required = Architecture == ARM64Architecture
                              ? PROCESSOR_ARCHITECTURE_ARM64
                              : PROCESSOR_ARCHITECTURE_AMD64;
    return record(stage::NativeArchitecture,
                  Info.wProcessorArchitecture == Required, 0, Architecture);
#else
    utsname Host{};
    if (uname(&Host))
      return record(stage::NativeArchitecture, false, errno, Architecture);
    const char *Required =
        Architecture == ARM64Architecture ? LinuxARM64 : LinuxX64;
    return record(stage::NativeArchitecture,
                  std::strcmp(Host.machine, Required) == 0, 0, Architecture);
#endif
  }
};

#if defined(__linux__)
#define NEVERD_KVM_STRING(Name, Value) constexpr char Name[] = Value;
#include "../lib/emulation/backends/kvm/KvmProtocol.def"
#undef NEVERD_KVM_STRING

#if defined(__x86_64__)
int reportAddressWidths() {
  unsigned A, B, C, D;
  if (__get_cpuid(0x80000008, &A, &B, &C, &D))
    std::printf("host_cpuid_80000008_eax=0x%08x\n", A);
  const int System = open(Device, O_RDONLY | O_CLOEXEC);
  if (System < 0) {
    std::printf("open_errno=%d\n", errno);
    return 0;
  }
  std::printf("kvm_api=%d\n", ioctl(System, KVM_GET_API_VERSION, 0));
#ifdef KVM_CAP_SMALLER_MAXPHYADDR
  std::printf("kvm_cap_smaller_maxphyaddr=%d\n",
              ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_SMALLER_MAXPHYADDR));
#endif
  constexpr unsigned Capacity = 256;
  struct Query {
    uint32_t Count, Padding;
    kvm_cpuid_entry2 Entries[Capacity];
  } Supported{};
  static_assert(offsetof(Query, Count) == offsetof(kvm_cpuid2, nent));
  static_assert(offsetof(Query, Entries) == offsetof(kvm_cpuid2, entries));
  Supported.Count = Capacity;
  if (ioctl(System, KVM_GET_SUPPORTED_CPUID, &Supported) == 0) {
    for (unsigned I = 0; I < Supported.Count && I < Capacity; ++I)
      if (Supported.Entries[I].function == 0x80000008)
        std::printf("kvm_supported_cpuid_80000008_eax=0x%08x\n",
                    Supported.Entries[I].eax);
  } else {
    std::printf("kvm_supported_cpuid_errno=%d\n", errno);
  }
  return close(System) != 0;
}
#endif

struct Host {
  Evidence &Log;
  int System = -1, VM = -1, CPU = -1;

  ~Host() {
    if (CPU >= 0)
      status(stage::CloseCPU, close(CPU));
    if (VM >= 0)
      status(stage::CloseVM, close(VM));
    if (System >= 0)
      status(stage::CloseKvm, close(System));
  }

  bool status(const char *Stage, int Value, bool Missing = false) {
    const int Error = Value < 0 ? errno : 0;
    return Log.record(Stage, Value >= 0, Error, Value < 0 ? 0 : Value, Missing);
  }

  void probe() {
    System = open(Device, O_RDWR | O_CLOEXEC);
    const bool Missing =
        errno == ENOENT || errno == ENODEV || errno == EACCES || errno == EPERM;
    if (!status(stage::OpenKvm, System, Missing))
      return;
    const int Version = ioctl(System, KVM_GET_API_VERSION, 0);
    if (!Log.record(stage::ApiVersion, Version == KVM_API_VERSION,
                    Version < 0 ? errno : 0, Version < 0 ? 0 : Version,
                    Version >= 0))
      return;
    const int Debug =
        ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_SET_GUEST_DEBUG);
    if (!Log.record(stage::GuestDebugCapability, Debug > 0,
                    Debug < 0 ? errno : 0, Debug < 0 ? 0 : Debug, Debug == 0))
      return;
    VM = ioctl(System, KVM_CREATE_VM, 0);
    if (!status(stage::CreateVM, VM))
      return;
    CPU = ioctl(VM, KVM_CREATE_VCPU, 0);
    if (!status(stage::CreateCPU, CPU))
      return;
#if defined(__aarch64__)
    kvm_vcpu_init Init{};
    if (!status(stage::PreferredTarget,
                ioctl(VM, KVM_ARM_PREFERRED_TARGET, &Init)) ||
        !status(stage::InitializeCPU, ioctl(CPU, KVM_ARM_VCPU_INIT, &Init)))
      return;
#endif
    kvm_guest_debug DebugState{};
    DebugState.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
    if (!status(stage::SetGuestDebug,
                ioctl(CPU, KVM_SET_GUEST_DEBUG, &DebugState)))
      return;
    const int Bytes = ioctl(System, KVM_GET_VCPU_MMAP_SIZE, 0);
    Log.record(stage::RunMappingSize, Bytes >= int(sizeof(kvm_run)),
               Bytes < 0 ? errno : 0, Bytes < 0 ? 0 : Bytes);
  }
};
#else
struct Host {
  Evidence &Log;
  HMODULE Module = nullptr;
  WHV_PARTITION_HANDLE Partition = nullptr;
  bool HasCPU = false;
#define NEVERD_HOST_WHP_API(Name) decltype(&::Name) Name = nullptr;
#include "NativeHostProbe.def"
#undef NEVERD_HOST_WHP_API

  ~Host() {
    if (HasCPU)
      status(stage::DeleteVirtualProcessor,
             WHvDeleteVirtualProcessor(Partition, 0));
    if (Partition)
      status(stage::DeletePartition, WHvDeletePartition(Partition));
    if (Module) {
      const bool Success = FreeLibrary(Module);
      Log.record(stage::FreeLibrary, Success, Success ? 0 : GetLastError());
    }
  }

  bool status(const char *Stage, HRESULT Status) {
    return Log.record(Stage, SUCCEEDED(Status), uint32_t(Status));
  }

  bool capability(const char *Stage, WHV_CAPABILITY_CODE Code,
                  WHV_CAPABILITY &Value) {
    Value = {};
    const HRESULT Status =
        WHvGetCapability(Code, &Value, sizeof(Value), nullptr);
    if (FAILED(Status))
      return status(Stage, Status);
    return true;
  }

  void probe() {
#define NEVERD_WHP_STRING(Name, Value) constexpr auto Name = Value;
#include "../lib/emulation/backends/whp/WhpProtocol.def"
#undef NEVERD_WHP_STRING
    Module = LoadLibraryExW(Library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DWORD Error = Module ? ERROR_SUCCESS : GetLastError();
    if (!Log.record(stage::LoadLibrary, Module != nullptr, Error, 0,
                    Error == ERROR_MOD_NOT_FOUND))
      return;
    unsigned Export = 0;
#define NEVERD_HOST_WHP_API(Name)                                              \
  Name = reinterpret_cast<decltype(Name)>(GetProcAddress(Module, #Name));      \
  if (!Name) {                                                                 \
    Log.record(stage::LibraryExports, false, GetLastError(), Export, true);    \
    return;                                                                    \
  }                                                                            \
  ++Export;
#include "NativeHostProbe.def"
#undef NEVERD_HOST_WHP_API
    Log.record(stage::LibraryExports, true, 0, Export);
    WHV_CAPABILITY Capability{};
    if (!capability(stage::HypervisorPresent,
                    WHvCapabilityCodeHypervisorPresent, Capability) ||
        !Log.record(stage::HypervisorPresent, Capability.HypervisorPresent, 0,
                    Capability.HypervisorPresent, true))
      return;
#if defined(_M_ARM64) || defined(__aarch64__)
    if (!capability(stage::Arm64Support, WHvCapabilityCodeFeatures,
                    Capability) ||
        !Log.record(stage::Arm64Support, Capability.Features.Arm64Support, 0,
                    Capability.Features.AsUINT64, true))
      return;
#endif
    if (!capability(stage::ExtendedExits, WHvCapabilityCodeExtendedVmExits,
                    Capability))
      return;
#if defined(_M_ARM64) || defined(__aarch64__)
    const bool Supported = Capability.ExtendedVmExits.HypercallExit;
#else
    const bool Supported = Capability.ExtendedVmExits.ExceptionExit;
#endif
    if (!Log.record(stage::ExtendedExits, Supported, 0,
                    Capability.ExtendedVmExits.AsUINT64, true) ||
        !status(stage::CreatePartition, WHvCreatePartition(&Partition)))
      return;
    WHV_PARTITION_PROPERTY Property{};
    Property.ProcessorCount = 1;
    if (!status(stage::ProcessorCount,
                WHvSetPartitionProperty(Partition,
                                        WHvPartitionPropertyCodeProcessorCount,
                                        &Property, sizeof(Property))))
      return;
    Property = {};
#if defined(_M_ARM64) || defined(__aarch64__)
    Property.ExtendedVmExits.HypercallExit = 1;
#else
    Property.ExtendedVmExits.ExceptionExit = 1;
#endif
    if (!status(stage::ConfigureExits,
                WHvSetPartitionProperty(Partition,
                                        WHvPartitionPropertyCodeExtendedVmExits,
                                        &Property, sizeof(Property))))
      return;
#if defined(_M_ARM64) || defined(__aarch64__)
    if (!capability(stage::GicLpiBits, WHvCapabilityCodeGicLpiIntIdBits,
                    Capability))
      return;
    Log.record(stage::GicLpiBits, true, 0, Capability.GicLpiIntIdBits);
    Property = {};
    Property.Arm64IcParameters.EmulationMode = WHvArm64IcEmulationModeGicV3;
    auto &Gic = Property.Arm64IcParameters.GicV3Parameters;
    Gic.GicdBaseAddress = GicDistributor;
    Gic.GitsTranslaterBaseAddress = GicITS;
    Gic.GicLpiIntIdBits = Capability.GicLpiIntIdBits;
    Gic.GicPpiOverflowInterruptFromCntv = GicVirtualTimerPPI;
    Gic.GicPpiPerformanceMonitorsInterrupt = GicPerformancePPI;
    if (!status(stage::ConfigureGic,
                WHvSetPartitionProperty(
                    Partition, WHvPartitionPropertyCodeArm64IcParameters,
                    &Property, sizeof(Property))))
      return;
#endif
    if (!status(stage::SetupPartition, WHvSetupPartition(Partition)))
      return;
    const HRESULT Status = WHvCreateVirtualProcessor(Partition, 0, 0);
    HasCPU = SUCCEEDED(Status);
    if (!status(stage::CreateVirtualProcessor, Status))
      return;
#if defined(_M_ARM64) || defined(__aarch64__)
    const WHV_REGISTER_NAME Name = WHvArm64RegisterGicrBaseGpa;
    WHV_REGISTER_VALUE Value{};
    Value.Reg64 = GicRedistributor;
    status(stage::SetRedistributor,
           WHvSetVirtualProcessorRegisters(Partition, 0, &Name, 1, &Value));
#endif
  }
};
#endif
} // namespace

int main([[maybe_unused]] int Argc, [[maybe_unused]] char **Argv) {
#if defined(__linux__) && defined(__x86_64__)
  if (Argc == 2 && std::strcmp(Argv[1], "--address-widths") == 0)
    return reportAddressWidths();
#endif
  Evidence Log;
  if (Log.nativeArchitecture()) {
    Host Native{Log};
    Native.probe();
  }
  return Log.Result;
}
