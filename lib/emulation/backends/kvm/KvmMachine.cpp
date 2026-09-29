//===- KvmMachine.cpp - Linux x64 single-step execution ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"

#include "llvm/Support/FormatVariadic.h"
#if defined(__linux__) && defined(__x86_64__) && defined(NEVERD_EMULATION_KVM)
#include <cerrno>
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace neverd::emulation {
namespace {
#define NEVERD_KVM_STRING(Name, Value) constexpr char Name[] = Value;
#include "KvmProtocol.def"
#undef NEVERD_KVM_STRING
class KvmMachine final : public X64Machine {
public:
  int System = -1, VM = -1, CPU = -1;
  kvm_run *Run = nullptr;
  size_t RunSize = 0;
  ~KvmMachine() override {
    if (Run)
      munmap(Run, RunSize);
    if (CPU >= 0)
      close(CPU);
    if (VM >= 0)
      close(VM);
    if (System >= 0)
      close(System);
  }
  llvm::Error step(X64MachineState &State, uint64_t Root) override {
    kvm_regs R{};
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  R.Field = State.reg(X64Register::Name);
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    kvm_sregs S{};
    if (ioctl(CPU, KVM_GET_SREGS, &S) < 0)
      return diagnostic::error(diagnostic::KvmState);
    S.cr0 = x64::CR0;
    S.cr3 = Root;
    S.cr4 = x64::CR4;
    S.efer = x64::EFER;
    S.cr8 = State.reg(X64Register::CR8);
    kvm_segment Code{}, Data{};
    Code.selector = x64::CodeSelector;
    Code.type = x64::CodeType;
    Code.present = Code.s = Code.l = Code.g = 1;
    Code.limit = x64::SegmentLimit;
    Data.selector = x64::DataSelector;
    Data.type = x64::DataType;
    Data.present = Data.s = Data.db = Data.g = 1;
    Data.limit = x64::SegmentLimit;
    S.cs = Code;
    S.ds = S.es = S.ss = S.fs = S.gs = Data;
    S.gs.base = State.GSBase;
    if (ioctl(CPU, KVM_SET_SREGS, &S) < 0 || ioctl(CPU, KVM_SET_REGS, &R) < 0)
      return diagnostic::error(diagnostic::KvmState);
    // KVM associates software single stepping with the current linear RIP.
    // Arm it after installing this invocation's registers, including on resume.
    kvm_guest_debug Debug{};
    Debug.control =
        KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_BLOCKIRQ;
    if (ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities);
    int Status;
    do {
      Status = ioctl(CPU, KVM_RUN, 0);
    } while (Status < 0 && errno == EINTR);
    if (Status < 0)
      return diagnostic::error(diagnostic::KvmRun);
    // There is no generic KVM userspace exception bitmap. An unexpected exit
    // is terminal; this integer profile admits no instruction that should
    // require exception delivery or an unfinished IO/MMIO completion.
    if (Run->exit_reason != KVM_EXIT_DEBUG ||
        Run->debug.arch.exception != x64::DebugVector ||
        !(Run->debug.arch.dr6 & x64::DebugSingleStep)) {
      (void)ioctl(CPU, KVM_GET_REGS, &R);
      (void)ioctl(CPU, KVM_GET_SREGS, &S);
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          llvm::formatv(diagnostic::KvmExit, Run->exit_reason,
                        Run->exit_reason == KVM_EXIT_DEBUG
                            ? Run->debug.arch.exception
                        : Run->exit_reason == KVM_EXIT_FAIL_ENTRY
                            ? Run->fail_entry.hardware_entry_failure_reason
                            : 0,
                        R.rip, S.cr2)
              .str());
    }
    if (ioctl(CPU, KVM_GET_REGS, &R) < 0)
      return diagnostic::error(diagnostic::KvmState);
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  State.reg(X64Register::Name) = R.Field;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    return llvm::Error::success();
  }
};
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>> createKvmMachine(uint8_t *Backing,
                                                             uint64_t Size) {
  auto M = std::make_unique<KvmMachine>();
  M->System = open(Device, O_RDWR | O_CLOEXEC);
  if (M->System < 0)
    return diagnostic::unavailable(diagnostic::KvmOpen);
  if (ioctl(M->System, KVM_GET_API_VERSION, 0) != KVM_API_VERSION ||
      ioctl(M->System, KVM_CHECK_EXTENSION, KVM_CAP_SET_GUEST_DEBUG) <= 0)
    return diagnostic::unavailable(diagnostic::KvmCapabilities);
  M->VM = ioctl(M->System, KVM_CREATE_VM, 0);
  if (M->VM < 0)
    return diagnostic::error(diagnostic::KvmCreate);
  kvm_userspace_memory_region Region{};
  Region.memory_size = Size;
  Region.userspace_addr = reinterpret_cast<uintptr_t>(Backing);
  if (ioctl(M->VM, KVM_SET_USER_MEMORY_REGION, &Region) < 0)
    return diagnostic::error(diagnostic::KvmMap);
  M->CPU = ioctl(M->VM, KVM_CREATE_VCPU, 0);
  if (M->CPU < 0)
    return diagnostic::error(diagnostic::KvmCreate);
  int RunSize = ioctl(M->System, KVM_GET_VCPU_MMAP_SIZE, 0);
  if (RunSize < int(sizeof(kvm_run)))
    return diagnostic::error(diagnostic::KvmMap);
  M->RunSize = RunSize;
  void *Mapping =
      mmap(nullptr, M->RunSize, PROT_READ | PROT_WRITE, MAP_SHARED, M->CPU, 0);
  if (Mapping == MAP_FAILED)
    return diagnostic::error(diagnostic::KvmMap);
  M->Run = static_cast<kvm_run *>(Mapping);
  kvm_guest_debug Debug{};
  Debug.control =
      KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_BLOCKIRQ;
  if (ioctl(M->CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
    return diagnostic::unavailable(diagnostic::KvmCapabilities);
  return std::unique_ptr<X64Machine>(std::move(M));
}
} // namespace neverd::emulation
#else

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<X64Machine>> createKvmMachine(uint8_t *,
                                                             uint64_t) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
