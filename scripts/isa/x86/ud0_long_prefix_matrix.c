//===- ud0_long_prefix_matrix.c - Original code64 UD0 oracle ----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check prefixed UD0 length, fetch priority and address-tail faults on P/E.
//===----------------------------------------------------------------------===//

#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static sigjmp_buf resume;
static volatile sig_atomic_t signal_number;
static uintptr_t rip, fault_address;
static unsigned long trap, error;

static void on_fault(int number, siginfo_t *information, void *opaque) {
  ucontext_t *context = opaque;
  signal_number = number;
  rip = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];
  fault_address = (uintptr_t)information->si_addr;
  trap = (unsigned long)context->uc_mcontext.gregs[REG_TRAPNO];
  error = (unsigned long)context->uc_mcontext.gregs[REG_ERR];
  siglongjmp(resume, 1);
}

static int observe(uint8_t *start, uint8_t *guard, unsigned expected_trap) {
  signal_number = 0;
  trap = error = 0;
  if (!sigsetjmp(resume, 1))
    ((void (*)(void))start)();
  return trap == expected_trap && error == (expected_trap == 14 ? 0x14u : 0u) &&
         signal_number == (expected_trap == 6 ? SIGILL : SIGSEGV) &&
         rip == (uintptr_t)start &&
         (expected_trap != 14 || fault_address == (uintptr_t)guard);
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const unsigned expected_profile = (unsigned)strtoul(argv[1], NULL, 10);
  if (expected_profile != 6 && expected_profile != 14)
    return 2;
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return 2;
  uint8_t *memory =
      mmap(NULL, (size_t)page_size * 2, PROT_READ | PROT_WRITE | PROT_EXEC,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED ||
      mprotect(memory + page_size, (size_t)page_size, PROT_NONE))
    return 2;
  struct sigaction action = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGILL, &action, NULL) || sigaction(SIGSEGV, &action, NULL))
    return 2;
  const uint8_t tails[][6] = {
      {0x00}, {0x04, 0x20}, {0x84, 0x20, 0x00, 0x50, 0x34, 0x12}};
  const unsigned tail_lengths[] = {1, 2, 6};
  const uint8_t repeated_prefixes[] = {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66,
                                       0x67, 0xf0, 0xf2, 0xf3, 0x40, 0x41, 0x42,
                                       0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
                                       0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
  const uint8_t mixed_patterns[][13] = {
      {0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66, 0x67, 0x66, 0x67,
       0x66},
      {0x66, 0x67, 0xf2, 0x64, 0x4f, 0x2e, 0xf3, 0x40, 0x65, 0x3e, 0x48, 0x36,
       0x26},
      {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
       0x4c},
      {0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3, 0xf2, 0xf3,
       0xf2},
      {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65,
       0x26},
      {0xf0, 0x67, 0xf3, 0x64, 0x4f, 0x2e, 0x66, 0x40, 0x65, 0x3e, 0x48, 0x36,
       0x26},
      {0x66, 0x67, 0xf3, 0x64, 0x4f, 0x2e, 0xf0, 0x40, 0x65, 0x3e, 0x48, 0x36,
       0x26},
      {0x66, 0x67, 0xf3, 0x64, 0x4f, 0x2e, 0x26, 0x40, 0x65, 0x3e, 0x48, 0x36,
       0xf0},
  };
  const unsigned repeated_count = sizeof(repeated_prefixes);
  const unsigned mixed_count =
      sizeof(mixed_patterns) / sizeof(mixed_patterns[0]);
  unsigned total = 0, overlong = 0, mismatch = 0, pf = 0, gp = 0, ud = 0;
  uint8_t *guard = memory + page_size;
  for (unsigned kind = 0; kind < repeated_count + mixed_count; ++kind) {
    for (unsigned prefix_count = 0; prefix_count <= 15; ++prefix_count) {
      const unsigned front = prefix_count + 2;
      const int prefix_overflow = front > 15;
      uint8_t code[15 + 2 + sizeof(tails[0])];
      for (unsigned i = 0; i < prefix_count; ++i)
        code[i] = kind < repeated_count
                      ? repeated_prefixes[kind]
                      : mixed_patterns[kind - repeated_count]
                                      [i % sizeof(mixed_patterns[0])];
      code[prefix_count] = 0x0f;
      code[prefix_count + 1] = 0xff;
      for (unsigned form = 0; form < (prefix_overflow ? 1u : 3u); ++form) {
        const unsigned length = front + tail_lengths[form];
        memcpy(code + front, tails[form], tail_lengths[form]);
        // For an overlong front, put the guard at every offset, including RIP.
        for (unsigned available = prefix_overflow ? 0 : front;
             available <= length; ++available) {
          uint8_t *start = guard - available;
          if (available)
            memcpy(start, code, available);
          __builtin___clear_cache((char *)start, (char *)guard);
          unsigned expected = 14;
          if (!prefix_overflow &&
              (expected_profile == 6 || (available == length && length <= 15)))
            expected = 6;
          else if (available >= 15)
            expected = 13;
          const int good = observe(start, guard, expected);
          ++total;
          overlong += prefix_overflow;
          pf += trap == 14;
          gp += trap == 13;
          ud += trap == 6;
          if (!good) {
            ++mismatch;
            printf("mismatch kind=%u prefixes=%u form=%u available=%u "
                   "signal=%d trap=%lu error=%#lx rip_delta=%ld "
                   "fault_delta=%ld expected_trap=%u\n",
                   kind, prefix_count, form, available, (int)signal_number,
                   trap, error, (long)(rip - (uintptr_t)start),
                   (long)(fault_address - (uintptr_t)guard), expected);
          }
        }
      }
    }
  }
  printf("profile=%u total=%u overlong=%u mismatches=%u pf=%u gp=%u ud=%u\n",
         expected_profile, total, overlong, mismatch, pf, gp, ud);
  munmap(memory, (size_t)page_size * 2);
  const unsigned patterns = repeated_count + mixed_count;
  return mismatch || total - overlong != patterns * 14 * 12 ||
         overlong != patterns * (18 + 19);
}
