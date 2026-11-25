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
                                       0x67, 0xf2, 0xf3, 0x40, 0x41, 0x42, 0x43,
                                       0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
                                       0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
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
  };
  const unsigned repeated_count = sizeof(repeated_prefixes);
  const unsigned mixed_count =
      sizeof(mixed_patterns) / sizeof(mixed_patterns[0]);
  unsigned total = 0, mismatch = 0, pf = 0, gp = 0, ud = 0;
  for (unsigned kind = 0; kind < repeated_count + mixed_count; ++kind)
    for (unsigned prefix_count = 0; prefix_count <= 13; ++prefix_count)
      for (unsigned form = 0; form < 3; ++form)
        for (unsigned available = 0; available <= tail_lengths[form];
             ++available) {
          const unsigned front_length = prefix_count + 2;
          uint8_t *start = memory + page_size - front_length - available;
          if (kind < repeated_count)
            memset(start, repeated_prefixes[kind], prefix_count);
          else
            memcpy(start, mixed_patterns[kind - repeated_count], prefix_count);
          start[prefix_count] = 0x0f;
          start[prefix_count + 1] = 0xff;
          memcpy(start + front_length, tails[form], available);
          __builtin___clear_cache((char *)start,
                                  (char *)(start + front_length + available));
          signal_number = 0;
          trap = error = 0;
          if (!sigsetjmp(resume, 1))
            ((void (*)(void))start)();
          const unsigned next_byte = front_length + available;
          const unsigned expected_trap =
              expected_profile == 6 ? 6
              : available == tail_lengths[form] &&
                      front_length + tail_lengths[form] <= 15
                  ? 6
              : next_byte >= 15 ? 13
                                : 14;
          const unsigned long expected_error = expected_trap == 14 ? 0x14 : 0;
          const int expected_signal = expected_trap == 6 ? SIGILL : SIGSEGV;
          const int good = trap == expected_trap && error == expected_error &&
                           signal_number == expected_signal &&
                           rip == (uintptr_t)start &&
                           (expected_trap != 14 ||
                            fault_address == (uintptr_t)(memory + page_size));
          ++total;
          pf += expected_trap == 14;
          gp += expected_trap == 13;
          ud += expected_trap == 6;
          if (!good) {
            ++mismatch;
            printf("mismatch kind=%u prefixes=%u form=%u available=%u "
                   "signal=%d trap=%lu error=%#lx rip_delta=%ld "
                   "fault_delta=%ld expected_trap=%u\n",
                   kind, prefix_count, form, available, (int)signal_number,
                   trap, error, (long)(rip - (uintptr_t)start),
                   (long)(fault_address - (uintptr_t)(memory + page_size)),
                   expected_trap);
          }
        }
  printf("profile=%u total=%u mismatches=%u pf=%u gp=%u ud=%u\n",
         expected_profile, total, mismatch, pf, gp, ud);
  munmap(memory, (size_t)page_size * 2);
  return mismatch || total != (repeated_count + mixed_count) * 14 * 12;
}
