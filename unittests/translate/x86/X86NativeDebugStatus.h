//===- X86NativeDebugStatus.h - Host DR6 status bits ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// DR6 bit 11 (BLD) reports a bus-lock debug exception and bit 16 (RTM) one
/// inside a transactional region. A processor without the feature keeps the
/// bit reserved, reading 1; a processor with it lets MOV DR6 clear the bit.
/// The x86 model implements neither feature, so a native comparison leaves
/// out exactly the bits this host lets a guest clear.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86NATIVEDEBUGSTATUS_H
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86NATIVEDEBUGSTATUS_H

#include <cstdint>

#if defined(__linux__) && defined(__x86_64__)
#include <algorithm>
#include <fcntl.h>
#include <iterator>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace neverd::test {
namespace detail {

/// Runs `mov dr6, eax; mov eax, dr6; hlt` with EAX zero at CPL 0 in a
/// throwaway VM, so no fixture's vCPU changes how KVM treats its debug
/// registers. Returns the status bits that read back as 0, or 0 when KVM
/// cannot run the probe.
inline uint32_t probeClearableDR6StatusBits() {
  constexpr uint32_t StatusBits = (1u << 11) | (1u << 16);
  constexpr size_t Page = 0x1000;
  constexpr uint8_t Probe[] = {0x0f, 0x23, 0xf0, 0x0f, 0x21, 0xf0, 0xf4};
  int KVM = -1, VM = -1, CPU = -1, RunSize = 0;
  void *RAM = MAP_FAILED;
  void *Run = MAP_FAILED;
  uint32_t Clearable = 0;
  [&] {
    KVM = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (KVM < 0 || (VM = ioctl(KVM, KVM_CREATE_VM, 0)) < 0)
      return;
    RAM = mmap(nullptr, Page, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (RAM == MAP_FAILED)
      return;
    std::copy(std::begin(Probe), std::end(Probe), static_cast<uint8_t *>(RAM));
    kvm_userspace_memory_region Region{};
    Region.memory_size = Page;
    Region.userspace_addr = reinterpret_cast<uintptr_t>(RAM);
    if (ioctl(VM, KVM_SET_USER_MEMORY_REGION, &Region) ||
        (CPU = ioctl(VM, KVM_CREATE_VCPU, 0)) < 0 ||
        (RunSize = ioctl(KVM, KVM_GET_VCPU_MMAP_SIZE, 0)) <= 0)
      return;
    Run = mmap(nullptr, RunSize, PROT_READ | PROT_WRITE, MAP_SHARED, CPU, 0);
    if (Run == MAP_FAILED)
      return;
    kvm_sregs Special{};
    if (ioctl(CPU, KVM_GET_SREGS, &Special))
      return;
    // Flat 32-bit protected mode without paging.
    const auto Flat = [](kvm_segment &Segment, uint16_t Selector,
                         uint8_t Type) {
      Segment = {};
      Segment.limit = 0xffffffff;
      Segment.selector = Selector;
      Segment.type = Type;
      Segment.present = 1;
      Segment.db = 1;
      Segment.s = 1;
      Segment.g = 1;
    };
    Flat(Special.cs, 8, 0xb);
    for (kvm_segment *Data :
         {&Special.ds, &Special.es, &Special.fs, &Special.gs, &Special.ss})
      Flat(*Data, 0x10, 3);
    Special.cr0 |= 1;
    kvm_regs Registers{};
    Registers.rflags = 2;
    kvm_debugregs Debug{};
    Debug.dr6 = 0xffff0ff0;
    Debug.dr7 = 0x400;
    if (ioctl(CPU, KVM_SET_SREGS, &Special) ||
        ioctl(CPU, KVM_SET_REGS, &Registers) ||
        ioctl(CPU, KVM_SET_DEBUGREGS, &Debug) || ioctl(CPU, KVM_RUN, 0) ||
        static_cast<kvm_run *>(Run)->exit_reason != KVM_EXIT_HLT ||
        ioctl(CPU, KVM_GET_REGS, &Registers))
      return;
    Clearable = ~static_cast<uint32_t>(Registers.rax) & StatusBits;
  }();
  if (Run != MAP_FAILED)
    munmap(Run, RunSize);
  if (RAM != MAP_FAILED)
    munmap(RAM, Page);
  for (int FD : {CPU, VM, KVM})
    if (FD >= 0)
      close(FD);
  return Clearable;
}

} // namespace detail

/// DR6 status bits this host lets a guest clear and the model keeps reserved.
inline uint32_t clearableDR6StatusBits() {
  static const uint32_t Bits = detail::probeClearableDR6StatusBits();
  return Bits;
}

} // namespace neverd::test
#else
namespace neverd::test {
inline uint32_t clearableDR6StatusBits() { return 0; }
} // namespace neverd::test
#endif

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86NATIVEDEBUGSTATUS_H
