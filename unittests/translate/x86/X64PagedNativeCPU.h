//===- X64PagedNativeCPU.h - Original instruction oracle -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A KVM CPU executes original instruction bytes over an independent RAM copy.
/// Port exits invoke an explicitly supplied synthetic device. KVM emulates
/// intercepted port instructions; their results are not bare CPU evidence.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDNATIVECPU_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDNATIVECPU_H

#include "gtest/gtest.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationEvent.h"
#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/translate/X86/X86PagedMemory.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <cpuid.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
class X64PagedNativeCPU {
public:
  static constexpr unsigned Size = 0x40000;
  void (*PortTransfer)(void *, bool, uint16_t, unsigned, unsigned,
                       uint8_t *) = nullptr;
  void *PortContext = nullptr;

  void run() {
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    while (Run->exit_reason == KVM_EXIT_IO) {
      ASSERT_NE(PortTransfer, nullptr) << "unexpected device access";
      ASSERT_TRUE(Run->io.size == 1 || Run->io.size == 2 || Run->io.size == 4);
      ASSERT_LE(Run->io.data_offset + uint64_t(Run->io.size) * Run->io.count,
                uint64_t(RunSize));
      PortTransfer(PortContext, Run->io.direction == KVM_EXIT_IO_IN,
                   Run->io.port, Run->io.size, Run->io.count,
                   reinterpret_cast<uint8_t *>(Run) + Run->io.data_offset);
      ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    }
  }

  int KVM = -1, VM = -1, CPU = -1, RunSize = 0;
  uint8_t *RAM = nullptr;
  kvm_run *Run = nullptr;
  kvm_sregs Initial{};
  uint8_t PhysicalBits = 0;
  bool SMAP = false;
  uint8_t InterruptShadow = 0;
  std::optional<kvm_debugregs> DebugRegisters;
  unsigned ReturnCoreType = 0;
  int ReturnProcessor = -1;
  std::optional<cpu_set_t> OriginalAffinity;
  std::vector<uint64_t> CPUIDStorage;

  ~X64PagedNativeCPU() {
    if (Run && Run != MAP_FAILED)
      munmap(Run, RunSize);
    if (RAM && RAM != MAP_FAILED)
      munmap(RAM, Size);
    for (int FD : {CPU, VM, KVM})
      if (FD >= 0)
        close(FD);
    if (OriginalAffinity)
      EXPECT_EQ(sched_setaffinity(0, sizeof(cpu_set_t), &*OriginalAffinity), 0);
  }

  // Competing faults and compatibility-IP validation are processor bindings.
  // A hybrid host must not migrate a comparison onto another core type after
  // the test chooses its explicit profile. This never chooses runtime policy.
  void pinForReturnComparison() {
    OriginalAffinity.emplace();
    ASSERT_EQ(sched_getaffinity(0, sizeof(cpu_set_t), &*OriginalAffinity), 0);
    ReturnProcessor = sched_getcpu();
    ASSERT_GE(ReturnProcessor, 0);
    cpu_set_t Selected;
    CPU_ZERO(&Selected);
    CPU_SET(ReturnProcessor, &Selected);
    ASSERT_EQ(sched_setaffinity(0, sizeof(Selected), &Selected), 0);
    unsigned A, B, C, D;
    if (__get_cpuid_count(0x1a, 0, &A, &B, &C, &D))
      ReturnCoreType = A >> 24;
  }

  void initialize() {
    KVM = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (KVM < 0)
      GTEST_SKIP() << "KVM unavailable: " << errno;
    VM = ioctl(KVM, KVM_CREATE_VM, 0);
    ASSERT_GE(VM, 0);
    RAM = static_cast<uint8_t *>(mmap(nullptr, Size, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(RAM, MAP_FAILED);
    kvm_userspace_memory_region Region{};
    Region.memory_size = Size;
    Region.userspace_addr = reinterpret_cast<uintptr_t>(RAM);
    ASSERT_EQ(ioctl(VM, KVM_SET_USER_MEMORY_REGION, &Region), 0);
    CPU = ioctl(VM, KVM_CREATE_VCPU, 0);
    ASSERT_GE(CPU, 0);
    constexpr unsigned Capacity = 256;
    CPUIDStorage.resize(
        (sizeof(kvm_cpuid2) + Capacity * sizeof(kvm_cpuid_entry2) + 7) / 8);
    auto *CPUID = reinterpret_cast<kvm_cpuid2 *>(CPUIDStorage.data());
    CPUID->nent = Capacity;
    ASSERT_EQ(ioctl(KVM, KVM_GET_SUPPORTED_CPUID, CPUID), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_CPUID2, CPUID), 0);
    for (unsigned I = 0; I != CPUID->nent; ++I)
      if (CPUID->entries[I].function == 0x80000008)
        PhysicalBits = CPUID->entries[I].eax & 0xff;
      else if (CPUID->entries[I].function == 7 && !CPUID->entries[I].index)
        SMAP = CPUID->entries[I].ebx & (1u << 20);
    RunSize = ioctl(KVM, KVM_GET_VCPU_MMAP_SIZE, 0);
    ASSERT_GT(RunSize, 0);
    Run = static_cast<kvm_run *>(
        mmap(nullptr, RunSize, PROT_READ | PROT_WRITE, MAP_SHARED, CPU, 0));
    ASSERT_NE(Run, MAP_FAILED);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &Initial), 0);
  }

  static kvm_segment
  segment(const neverd::analysis::X64ContinuationSegment &S) {
    kvm_segment R{};
    R.base = S.Base;
    R.limit = S.Limit;
    R.selector = S.Selector;
    R.type = S.Attributes & 15;
    R.s = (S.Attributes >> 4) & 1;
    R.dpl = (S.Attributes >> 5) & 3;
    R.present = (S.Attributes >> 7) & 1;
    R.avl = (S.Attributes >> 12) & 1;
    R.l = (S.Attributes >> 13) & 1;
    R.db = (S.Attributes >> 14) & 1;
    R.g = (S.Attributes >> 15) & 1;
    return R;
  }

  void
  execute(const std::vector<uint8_t> &Input,
          const neverd::analysis::X64ContinuationStateV6 &State, uint64_t CR0,
          unsigned InstructionBytes,
          neverd::analysis::X64ContinuationStateV6 &Output, bool &Fault,
          bool DisableDebugStop = false,
          const neverd::analysis::X64ContinuationEventV1 *Pending = nullptr,
          uint64_t CR4 = 0x20, bool CaptureReturn = false,
          bool NormalizeReturnBreakpoint = true, bool InjectPending = true) {
    ASSERT_EQ(Input.size(), Size);
    std::copy(Input.begin(), Input.end(), RAM);
    const auto &Core = State.Core.Core.Core.Core;
    auto S = Initial;
    S.cr0 = CR0;
    S.cr3 = 0x10000;
    S.cr4 = CR4;
    S.efer = 0xd00;
    S.cr2 = 0;
    S.cs = segment(State.Core.CS);
    S.ss = segment(State.Core.SS);
    S.ds = S.es = S.ss;
    S.fs = S.gs = S.ss;
    S.fs.base = Core.FSBase;
    S.gs.base = Core.GSBase;
    if (CaptureReturn) {
      S.ds = segment(State.DS);
      S.es = segment(State.ES);
      S.fs = segment({Core.FSBase, State.FS.Limit, State.FS.Selector,
                      State.FS.Attributes});
      S.gs = segment({Core.GSBase, State.GS.Limit, State.GS.Selector,
                      State.GS.Attributes});
      S.ds.unusable = bool(State.Unusable & 4);
      S.es.unusable = bool(State.Unusable & 8);
      S.fs.unusable = bool(State.Unusable & 16);
      S.gs.unusable = bool(State.Unusable & 32);
    }
    S.gdt.base = State.GDTR.Base;
    S.gdt.limit = State.GDTR.Limit;
    S.idt.base = State.IDTR.Base;
    S.idt.limit = State.IDTR.Limit;
    S.tr = segment(State.TR);
    S.ldt.unusable = 1;
    if (CaptureReturn) {
      S.ldt = segment(State.LDTR);
      S.ldt.unusable = bool(State.Unusable & 64);
    }
    auto Flush = S;
    Flush.cr3 = 0x3f000;
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &Flush), 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_SREGS, &S), 0);
    kvm_vcpu_events Events{};
    Events.flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
    Events.nmi.masked = State.NMIBlocked;
    if (Pending)
      Events.interrupt.shadow =
          (Pending->STIBlocked ? KVM_X86_SHADOW_INT_STI : 0) |
          (Pending->SSBlocked ? KVM_X86_SHADOW_INT_MOV_SS : 0);
    ASSERT_EQ(ioctl(CPU, KVM_SET_VCPU_EVENTS, &Events), 0);
    kvm_regs R{};
    std::array<decltype(R.rax) *, 16> Registers{
        &R.rax, &R.rcx, &R.rdx, &R.rbx, &R.rsp, &R.rbp, &R.rsi, &R.rdi,
        &R.r8,  &R.r9,  &R.r10, &R.r11, &R.r12, &R.r13, &R.r14, &R.r15};
    for (unsigned I = 0; I != 16; ++I)
      *Registers[I] = Core.GPR[I];
    R.rip = Core.PC;
    R.rflags = Core.RFlags;
    ASSERT_EQ(ioctl(CPU, KVM_SET_REGS, &R), 0);
    if (DebugRegisters)
      ASSERT_EQ(ioctl(CPU, KVM_SET_DEBUGREGS, &*DebugRegisters), 0);
    kvm_guest_debug Debug{};
    Debug.control =
        DisableDebugStop ? 0 : KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
    Debug.arch.debugreg[0] = Core.PC + InstructionBytes;
    Debug.arch.debugreg[1] = 0x1800;
    Debug.arch.debugreg[2] = 0;
    Debug.arch.debugreg[7] = 0x415;
    if (CaptureReturn) {
      Debug.arch.debugreg[3] = 0x1801;
      Debug.arch.debugreg[7] |= 0x40;
    }
    ASSERT_EQ(ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug), 0);
    Run->request_interrupt_window = 0;
    if (Pending && InjectPending) {
      if (Pending->Kind == neverd::analysis::X64ContinuationEventKind::NMI) {
        ASSERT_EQ(ioctl(CPU, KVM_NMI, 0), 0);
      } else {
        // KVM_INTERRUPT is an immediate injection request, not an APIC
        // pending bit. KVM itself selects the eligible instruction boundary.
        Run->request_interrupt_window = 1;
      }
    }
    ASSERT_NO_FATAL_FAILURE(run());
    if (Run->exit_reason == KVM_EXIT_IRQ_WINDOW_OPEN) {
      ASSERT_NE(Pending, nullptr);
      ASSERT_TRUE(Run->ready_for_interrupt_injection);
      Run->request_interrupt_window = 0;
      kvm_interrupt Interrupt{Pending->Vector};
      ASSERT_EQ(ioctl(CPU, KVM_INTERRUPT, &Interrupt), 0);
      ASSERT_NO_FATAL_FAILURE(run());
    }
    ASSERT_TRUE(Run->exit_reason == KVM_EXIT_DEBUG ||
                Run->exit_reason == KVM_EXIT_HLT)
        << "KVM exit " << Run->exit_reason;
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &R), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &S), 0);
    if (DebugRegisters)
      ASSERT_EQ(ioctl(CPU, KVM_GET_DEBUGREGS, &*DebugRegisters), 0);
    if (CaptureReturn && NormalizeReturnBreakpoint && R.rip == 0x1801) {
      // Restored RF suppresses the first execution breakpoint. The original
      // target NOP advances RIP and clears RF, with no other architectural
      // effect. The second breakpoint recovers that measured return state.
      R.rip = 0x1800;
      R.rflags |= 0x10000;
    }
    Fault = Run->exit_reason == KVM_EXIT_HLT;
    ASSERT_EQ(ioctl(CPU, KVM_GET_VCPU_EVENTS, &Events), 0);
    InterruptShadow = Events.interrupt.shadow;
    Output = State;
    Output.NMIBlocked = Events.nmi.masked;
    auto &Out = Output.Core.Core.Core.Core;
    for (unsigned I = 0; I != 16; ++I)
      Out.GPR[I] = *Registers[I];
    Out.PC = R.rip - (Fault ? 1 : 0);
    Out.RFlags = R.rflags;
    Output.Core.Core.CR2 = S.cr2;
    Output.Core.CS.Selector = S.cs.selector;
    Output.Core.SS.Selector = S.ss.selector;
    if (CaptureReturn) {
      const auto Segment = [](const kvm_segment &S) {
        return neverd::analysis::X64ContinuationSegment{
            S.base, S.limit, S.selector,
            uint16_t(S.type | (S.s << 4) | (S.dpl << 5) | (S.present << 7) |
                     (S.avl << 12) | (S.l << 13) | (S.db << 14) | (S.g << 15))};
      };
      Output.Core.CS = Segment(S.cs);
      Output.Core.SS = Segment(S.ss);
      Output.DS = Segment(S.ds);
      Output.ES = Segment(S.es);
      Output.FS = {S.fs.limit, S.fs.selector, Segment(S.fs).Attributes};
      Output.GS = {S.gs.limit, S.gs.selector, Segment(S.gs).Attributes};
      Output.Unusable = (State.Unusable & 0xc0) | S.cs.unusable |
                        (S.ss.unusable << 1) | (S.ds.unusable << 2) |
                        (S.es.unusable << 3) | (S.fs.unusable << 4) |
                        (S.gs.unusable << 5);
      Out.FSBase = S.fs.base;
      Out.GSBase = S.gs.base;
    }
  }
};
void expectPagedSegmentCPU(
    const neverd::analysis::X64ContinuationStateV6 &State,
    const neverd::analysis::X64ContinuationStateV6 &Native,
    neverd::translate::X86PagedMemory &Memory, const X64PagedNativeCPU &CPU,
    unsigned PageTableEnd = 0x14000) {
  const auto &Core = State.Core.Core.Core.Core;
  EXPECT_EQ(State.NMIBlocked, Native.NMIBlocked);
  const auto &Want = Native.Core.Core.Core.Core;
  EXPECT_EQ(Core.PC, Want.PC);
  EXPECT_EQ(Core.RFlags, Want.RFlags);
  for (unsigned I = 0; I != 16; ++I)
    EXPECT_EQ(Core.GPR[I], Want.GPR[I]) << I;
  EXPECT_EQ(State.Core.Core.CR2, Native.Core.Core.CR2);
  EXPECT_EQ(State.Unusable, Native.Unusable);
  const auto CheckSegment =
      [&](const neverd::analysis::X64ContinuationSegment &A,
          const neverd::analysis::X64ContinuationSegment &B,
          unsigned Unusable) {
        EXPECT_EQ(A.Selector, B.Selector);
        if (!(Native.Unusable & Unusable)) {
          EXPECT_EQ(A.Base, B.Base);
          EXPECT_EQ(A.Limit, B.Limit);
          EXPECT_EQ(A.Attributes, B.Attributes);
        }
      };
  CheckSegment(State.Core.CS, Native.Core.CS, 1);
  CheckSegment(State.Core.SS, Native.Core.SS, 2);
  CheckSegment(State.DS, Native.DS, 4);
  CheckSegment(State.ES, Native.ES, 8);
  CheckSegment(
      {Core.FSBase, State.FS.Limit, State.FS.Selector, State.FS.Attributes},
      {Want.FSBase, Native.FS.Limit, Native.FS.Selector, Native.FS.Attributes},
      16);
  CheckSegment(
      {Core.GSBase, State.GS.Limit, State.GS.Selector, State.GS.Attributes},
      {Want.GSBase, Native.GS.Limit, Native.GS.Selector, Native.GS.Attributes},
      32);
  EXPECT_EQ(Core.FSBase, Want.FSBase);
  EXPECT_EQ(Core.GSBase, Want.GSBase);
  auto Actual = Memory.physicalMemory().snapshotMemoryRegions().front().Bytes;
  auto WantRAM = std::vector<uint8_t>(CPU.RAM, CPU.RAM + CPU.Size);
  std::fill(Actual.begin() + 0x10000, Actual.begin() + PageTableEnd, 0);
  std::fill(WantRAM.begin() + 0x10000, WantRAM.begin() + PageTableEnd, 0);
  if (Actual != WantRAM) {
    const auto Difference =
        std::mismatch(Actual.begin(), Actual.end(), WantRAM.begin());
    ADD_FAILURE() << "RAM differs at " << (Difference.first - Actual.begin())
                  << " actual=" << unsigned(*Difference.first)
                  << " native=" << unsigned(*Difference.second);
  }
}

} // namespace
#endif
#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDNATIVECPU_H
