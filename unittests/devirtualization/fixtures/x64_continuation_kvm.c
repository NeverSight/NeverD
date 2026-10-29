//===- x64_continuation_kvm.c - Isolated supervisor source oracle --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run a freestanding generated-C fixture on an independent native KVM CPU.
//===----------------------------------------------------------------------===//

#define _GNU_SOURCE
#include "x64_continuation_event_state.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define REQUIRE(C)                                                             \
  do {                                                                         \
    if (!(C)) {                                                                \
      fprintf(stderr, "KVM fixture line %d: %s (errno %d)\n", __LINE__, #C,    \
              errno);                                                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

struct segment_state {
  uint64_t base;
  uint32_t limit;
  uint16_t selector, attributes;
};
struct return_query {
  uint64_t gpr[16], flags, pc, star;
  struct segment_state cs, ss;
};

static __u64 *gpr(struct kvm_regs *r, unsigned n) {
  switch (n) {
  case 0:
    return &r->rax;
  case 1:
    return &r->rcx;
  case 2:
    return &r->rdx;
  case 3:
    return &r->rbx;
  case 4:
    return &r->rsp;
  case 5:
    return &r->rbp;
  case 6:
    return &r->rsi;
  case 7:
    return &r->rdi;
  case 8:
    return &r->r8;
  case 9:
    return &r->r9;
  case 10:
    return &r->r10;
  case 11:
    return &r->r11;
  case 12:
    return &r->r12;
  case 13:
    return &r->r13;
  case 14:
    return &r->r14;
  default:
    return &r->r15;
  }
}

static struct segment_state segment(struct kvm_segment s) {
  return (struct segment_state){s.base, s.limit, s.selector,
                                s.type | s.s << 4 | s.dpl << 5 |
                                    s.present << 7 | s.avl << 12 | s.l << 13 |
                                    s.db << 14 | s.g << 15};
}

/// Execute an independent original SYSRETQ on a separate CPU. The candidate
/// source never runs this instruction: the I/O request only supplies its test
/// inputs and receives the hardware result, including hidden segment caches.
static int sysret_query(int cpu, struct kvm_run *run, uint8_t *memory,
                        const struct kvm_sregs *initial,
                        struct return_query *query) {
  struct kvm_sregs before = *initial;
  before.efer |= 1;
  REQUIRE(ioctl(cpu, KVM_SET_SREGS, &before) == 0);
  // KVM normalizes CPU-local fields (for example the APIC BSP bit). Compare
  // the actual admitted state, not the first CPU's requested setup record.
  REQUIRE(ioctl(cpu, KVM_GET_SREGS, &before) == 0);
  struct kvm_msrs *msr = calloc(1, sizeof(*msr) + sizeof(struct kvm_msr_entry));
  REQUIRE(msr);
  msr->nmsrs = 1;
  msr->entries[0].index = 0xc0000081;
  msr->entries[0].data = query->star;
  REQUIRE(ioctl(cpu, KVM_SET_MSRS, msr) == 1);
  free(msr);
  struct kvm_regs registers = {.rip = 0x1000, .rflags = query->flags};
  for (unsigned i = 0; i < 16; ++i)
    *gpr(&registers, i) = query->gpr[i];
  REQUIRE(registers.rcx == 0x1800);
  REQUIRE(ioctl(cpu, KVM_SET_REGS, &registers) == 0);
  struct kvm_guest_debug debug = {.control = KVM_GUESTDBG_ENABLE |
                                             KVM_GUESTDBG_USE_HW_BP};
  debug.arch.debugreg[0] = registers.rcx;
  debug.arch.debugreg[7] = 0x401;
  REQUIRE(ioctl(cpu, KVM_SET_GUEST_DEBUG, &debug) == 0);
  memory[0x1000] = 0x48;
  memory[0x1001] = 0x0f;
  memory[0x1002] = 0x07;
  memory[0x1800] = 0x90;
  REQUIRE(ioctl(cpu, KVM_RUN, 0) == 0);
  REQUIRE(run->exit_reason == KVM_EXIT_DEBUG);
  REQUIRE(ioctl(cpu, KVM_GET_REGS, &registers) == 0);
  struct kvm_sregs after;
  REQUIRE(ioctl(cpu, KVM_GET_SREGS, &after) == 0);
  query->cs = segment(after.cs);
  query->ss = segment(after.ss);
  before.cs = after.cs;
  before.ss = after.ss;
  REQUIRE(memcmp(&before, &after, sizeof(before)) == 0);
  for (unsigned i = 0; i < 16; ++i)
    query->gpr[i] = *gpr(&registers, i);
  query->flags = registers.rflags;
  query->pc = registers.rip;
  return 0;
}

static struct kvm_segment event_native_segment(event_segment_t s,
                                               unsigned unusable) {
  return (struct kvm_segment){.base = s.base,
                              .limit = s.limit,
                              .selector = s.selector,
                              .type = s.attributes & 15,
                              .present = (s.attributes >> 7) & 1,
                              .dpl = (s.attributes >> 5) & 3,
                              .db = (s.attributes >> 14) & 1,
                              .s = (s.attributes >> 4) & 1,
                              .l = (s.attributes >> 13) & 1,
                              .g = (s.attributes >> 15) & 1,
                              .avl = (s.attributes >> 12) & 1,
                              .unusable = unusable};
}

static void event_record_segment(event_segment_t *s, struct kvm_segment native,
                                 uint8_t *unusable, unsigned bit) {
  s->selector = native.selector;
  *unusable = (*unusable & ~bit) | (native.unusable ? bit : 0);
  // Unusable hidden cache bytes are deliberately outside the V6 contract.
  if (!native.unusable) {
    const struct segment_state value = segment(native);
    s->base = value.base;
    s->limit = value.limit;
    s->attributes = value.attributes;
  }
}

static int event_query(int cpu, struct kvm_run *run, uint8_t *memory,
                       const struct kvm_sregs *initial, event_state_t *q,
                       unsigned kind, unsigned immediate) {
  struct kvm_sregs before = *initial;
  event_segment_t fs = {q->fs_base, q->fs.limit, q->fs.selector,
                        q->fs.attributes};
  event_segment_t gs = {q->gs_base, q->gs.limit, q->gs.selector,
                        q->gs.attributes};
  event_segment_t *segments[] = {&q->cs, &q->ss, &q->ds,  &q->es,
                                 &fs,    &gs,    &q->ldt, &q->tr};
  struct kvm_segment *native[] = {&before.cs,  &before.ss, &before.ds,
                                  &before.es,  &before.fs, &before.gs,
                                  &before.ldt, &before.tr};
  for (unsigned i = 0; i != 8; ++i)
    *native[i] = event_native_segment(*segments[i], (q->unusable >> i) & 1);
  before.gdt.base = q->gdt.base;
  before.gdt.limit = q->gdt.limit;
  before.idt.base = q->idt.base;
  before.idt.limit = q->idt.limit;
  REQUIRE(ioctl(cpu, KVM_SET_SREGS, &before) == 0);
  REQUIRE(ioctl(cpu, KVM_GET_SREGS, &before) == 0);
  struct kvm_vcpu_events events;
  REQUIRE(ioctl(cpu, KVM_GET_VCPU_EVENTS, &events) == 0);
  events.nmi.masked = q->nmi_blocked;
  events.flags |= KVM_VCPUEVENT_VALID_NMI_PENDING;
  REQUIRE(ioctl(cpu, KVM_SET_VCPU_EVENTS, &events) == 0);
  struct kvm_regs registers = {.rip = q->pc, .rflags = q->flags};
  for (unsigned i = 0; i != 16; ++i)
    *gpr(&registers, i) = q->gpr[i];
  REQUIRE(registers.rip >= 0x1000 && registers.rip < 0x1800);
  REQUIRE(ioctl(cpu, KVM_SET_REGS, &registers) == 0);
  struct kvm_guest_debug debug = {.control = KVM_GUESTDBG_ENABLE |
                                             KVM_GUESTDBG_USE_HW_BP};
  debug.arch.debugreg[0] = 0x1800;
  debug.arch.debugreg[1] = 0x1801;
  debug.arch.debugreg[7] = 0x405;
  REQUIRE(ioctl(cpu, KVM_SET_GUEST_DEBUG, &debug) == 0);
  memory[registers.rip] = kind == 2 || kind >= 5 ? 0x48
                          : kind == 4            ? 0xcd
                                                 : 0xcc;
  if (kind == 2)
    memory[registers.rip + 1] = 0xcf;
  if (kind == 4) {
    REQUIRE(immediate <= UINT8_MAX);
    memory[registers.rip + 1] = immediate;
  }
  if (kind >= 5) {
    memory[registers.rip + 1] = kind == 5 ? 0xcb : 0xca;
    if (kind == 6) {
      REQUIRE(immediate <= UINT16_MAX);
      memory[registers.rip + 2] = immediate;
      memory[registers.rip + 3] = immediate >> 8;
    }
  }
  memory[0x1800] = memory[0x1801] = 0x90;
  REQUIRE(ioctl(cpu, KVM_RUN, 0) == 0);
  REQUIRE(run->exit_reason == KVM_EXIT_DEBUG);
  REQUIRE(ioctl(cpu, KVM_GET_REGS, &registers) == 0);
  // An IRET frame with RF=1 suppresses the first execution breakpoint. The
  // intervening NOP only advances RIP and clears RF; restore that observation.
  if (kind == 2 && registers.rip == 0x1801) {
    registers.rip = 0x1800;
    registers.rflags |= 0x10000;
  }
  REQUIRE(registers.rip == 0x1800);
  struct kvm_sregs after;
  REQUIRE(ioctl(cpu, KVM_GET_SREGS, &after) == 0);
  const struct kvm_segment values[] = {after.cs, after.ss, after.ds,  after.es,
                                       after.fs, after.gs, after.ldt, after.tr};
  for (unsigned i = 0; i != 8; ++i) {
    event_record_segment(segments[i], values[i], &q->unusable, 1u << i);
    *native[i] = values[i];
  }
  REQUIRE(memcmp(&before, &after, sizeof(before)) == 0);
  q->fs = (event_metadata_t){fs.limit, fs.selector, fs.attributes};
  q->gs = (event_metadata_t){gs.limit, gs.selector, gs.attributes};
  // FS/GS bases remain architectural even when their selectors are unusable.
  q->fs_base = after.fs.base;
  q->gs_base = after.gs.base;
  for (unsigned i = 0; i != 16; ++i)
    q->gpr[i] = *gpr(&registers, i);
  q->flags = registers.rflags;
  q->pc = registers.rip;
  REQUIRE(ioctl(cpu, KVM_GET_VCPU_EVENTS, &events) == 0);
  q->nmi_blocked = events.nmi.masked;
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (kvm < 0) {
    fprintf(stderr, "KVM unavailable: errno %d\n", errno);
    return 77;
  }
  REQUIRE(ioctl(kvm, KVM_GET_API_VERSION, 0) == KVM_API_VERSION);
  int vm = ioctl(kvm, KVM_CREATE_VM, 0);
  REQUIRE(vm >= 0);
  const size_t memory_size = 4 * 1024 * 1024;
  uint8_t *memory = mmap(NULL, memory_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(memory != MAP_FAILED);
  struct kvm_userspace_memory_region region = {.slot = 0,
                                               .memory_size = memory_size,
                                               .userspace_addr =
                                                   (uintptr_t)memory};
  REQUIRE(ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) == 0);
  int cpu = ioctl(vm, KVM_CREATE_VCPU, 0);
  REQUIRE(cpu >= 0);
  const unsigned cpuid_capacity = 256;
  struct kvm_cpuid2 *cpuid = calloc(
      1, sizeof(*cpuid) + cpuid_capacity * sizeof(struct kvm_cpuid_entry2));
  REQUIRE(cpuid);
  cpuid->nent = cpuid_capacity;
  REQUIRE(ioctl(kvm, KVM_GET_SUPPORTED_CPUID, cpuid) == 0);
  REQUIRE(ioctl(cpu, KVM_SET_CPUID2, cpuid) == 0);
  int run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  REQUIRE(run_size > 0);
  struct kvm_run *run =
      mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, cpu, 0);
  REQUIRE(run != MAP_FAILED);
  *(uint64_t *)(memory + 0x2000) = 0x3007;
  *(uint64_t *)(memory + 0x3000) = 0x4007;
  *(uint64_t *)(memory + 0x4000) = 0x87;
  *(uint64_t *)(memory + 0x4008) = 0x200087;
  const uint64_t descriptors[] = {0,
                                  0x00af9b000000ffffULL,
                                  0x00cf93000000ffffULL,
                                  0x00affb000000ffffULL,
                                  0x00cff3000000ffffULL,
                                  0x0040130000001234ULL,
                                  0x0000890000000067ULL,
                                  0};
  for (unsigned i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); ++i)
    ((uint64_t *)(memory + 0x5000))[i] = descriptors[i];
  struct kvm_sregs special;
  REQUIRE(ioctl(cpu, KVM_GET_SREGS, &special) == 0);
  special.cr0 = 0x80000011;
  special.cr3 = 0x2000;
  special.cr4 = 0x20;
  special.efer = 0x500;
  special.cs = (struct kvm_segment){.limit = UINT32_MAX,
                                    .selector = 8,
                                    .type = 11,
                                    .present = 1,
                                    .s = 1,
                                    .g = 1,
                                    .l = 1};
  special.ds = special.es = special.ss =
      (struct kvm_segment){.limit = UINT32_MAX,
                           .selector = 16,
                           .type = 3,
                           .present = 1,
                           .s = 1,
                           .g = 1,
                           .db = 1};
  special.gdt.base = 0x5000;
  special.gdt.limit = sizeof(descriptors) - 1;
  special.ldt.unusable = 1;
  REQUIRE(ioctl(cpu, KVM_SET_SREGS, &special) == 0);
  FILE *file = fopen(argv[1], "rb");
  REQUIRE(file);
  const size_t capacity = 0x100000;
  size_t bytes = fread(memory + 0x10000, 1, capacity, file);
  REQUIRE(bytes && bytes < capacity && !ferror(file));
  REQUIRE(fclose(file) == 0);
  struct kvm_regs registers = {.rip = 0x10000, .rsp = 0x1f0000, .rflags = 2};
  REQUIRE(ioctl(cpu, KVM_SET_REGS, &registers) == 0);
  int oracle = -1;
  struct kvm_run *oracle_run = NULL;
  for (;;) {
    REQUIRE(ioctl(cpu, KVM_RUN, 0) == 0);
    REQUIRE(ioctl(cpu, KVM_GET_REGS, &registers) == 0);
    if (run->exit_reason != KVM_EXIT_IO)
      break;
    REQUIRE(run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0xe9 &&
            run->io.size == 1 && run->io.count == 1);
    const unsigned kind = *((uint8_t *)run + run->io.data_offset);
    REQUIRE(kind >= 1 && kind <= 6);
    REQUIRE(kind != 4 || registers.rsi <= UINT8_MAX);
    REQUIRE(kind != 6 || registers.rsi <= UINT16_MAX);
    REQUIRE(registers.rdi <= memory_size - (kind == 1
                                                ? sizeof(struct return_query)
                                                : sizeof(event_state_t)));
    if (oracle < 0) {
      oracle = ioctl(vm, KVM_CREATE_VCPU, 1);
      REQUIRE(oracle >= 0);
      REQUIRE(ioctl(oracle, KVM_SET_CPUID2, cpuid) == 0);
      oracle_run =
          mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, oracle, 0);
      REQUIRE(oracle_run != MAP_FAILED);
    }
    if (kind == 1) {
      struct return_query query;
      memcpy(&query, memory + registers.rdi, sizeof(query));
      REQUIRE(sysret_query(oracle, oracle_run, memory, &special, &query) == 0);
      memcpy(memory + registers.rdi, &query, sizeof(query));
    } else {
      event_state_t query;
      memcpy(&query, memory + registers.rdi, sizeof(query));
      REQUIRE(event_query(oracle, oracle_run, memory, &special, &query, kind,
                          registers.rsi) == 0);
      memcpy(memory + registers.rdi, &query, sizeof(query));
    }
  }
  if (run->exit_reason != KVM_EXIT_HLT || registers.rax) {
    fprintf(stderr, "guest exit=%u PC=%llx result=%llu\n", run->exit_reason,
            (unsigned long long)registers.rip,
            (unsigned long long)registers.rax);
    if (run->exit_reason == KVM_EXIT_HLT && registers.rax == 77)
      return 77;
    return 1;
  }
  puts("PASS: generated continuation C executed at CPL0 on KVM");
  return 0;
}
