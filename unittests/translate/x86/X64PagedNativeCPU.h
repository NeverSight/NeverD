//===- X64PagedNativeCPU.h - Original instruction oracle -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A KVM CPU executes original instruction bytes over an independent RAM copy.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDNATIVECPU_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDNATIVECPU_H

#include "gtest/gtest.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationEvent.h"
#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
class X64PagedNativeCPU {
public:
  static constexpr unsigned Size = 0x40000;
  int KVM = -1, VM = -1, CPU = -1, RunSize = 0;
  uint8_t *RAM = nullptr;
  kvm_run *Run = nullptr;
  kvm_sregs Initial{};
  uint8_t PhysicalBits = 0;
  std::vector<uint64_t> CPUIDStorage;

  ~X64PagedNativeCPU() {
    if (Run && Run != MAP_FAILED)
      munmap(Run, RunSize);
    if (RAM && RAM != MAP_FAILED)
      munmap(RAM, Size);
    for (int FD : {CPU, VM, KVM})
      if (FD >= 0)
        close(FD);
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
          uint64_t CR4 = 0x20, bool CaptureReturn = false) {
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
    if (Pending) {
      if (Pending->Kind == neverd::analysis::X64ContinuationEventKind::NMI) {
        ASSERT_EQ(ioctl(CPU, KVM_NMI, 0), 0);
      } else {
        // KVM_INTERRUPT is an immediate injection request, not an APIC
        // pending bit. KVM itself selects the eligible instruction boundary.
        Run->request_interrupt_window = 1;
      }
    }
    ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    if (Run->exit_reason == KVM_EXIT_IRQ_WINDOW_OPEN) {
      ASSERT_NE(Pending, nullptr);
      ASSERT_TRUE(Run->ready_for_interrupt_injection);
      Run->request_interrupt_window = 0;
      kvm_interrupt Interrupt{Pending->Vector};
      ASSERT_EQ(ioctl(CPU, KVM_INTERRUPT, &Interrupt), 0);
      ASSERT_EQ(ioctl(CPU, KVM_RUN, 0), 0) << errno;
    }
    ASSERT_TRUE(Run->exit_reason == KVM_EXIT_DEBUG ||
                Run->exit_reason == KVM_EXIT_HLT)
        << "KVM exit " << Run->exit_reason;
    ASSERT_EQ(ioctl(CPU, KVM_GET_REGS, &R), 0);
    ASSERT_EQ(ioctl(CPU, KVM_GET_SREGS, &S), 0);
    if (CaptureReturn && R.rip == 0x1801) {
      // Restored RF suppresses the first execution breakpoint. The original
      // target NOP advances RIP and clears RF, with no other architectural
      // effect. The second breakpoint recovers that measured return state.
      R.rip = 0x1800;
      R.rflags |= 0x10000;
    }
    Fault = Run->exit_reason == KVM_EXIT_HLT;
    ASSERT_EQ(ioctl(CPU, KVM_GET_VCPU_EVENTS, &Events), 0);
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
} // namespace
#endif
#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X64PAGEDNATIVECPU_H
