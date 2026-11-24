//===- ud0_length_probe.c - Original x86 UD0 length oracle -------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe repeated-prefix UD0 faults at the architectural length limit.
//===----------------------------------------------------------------------===//

#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
static sigjmp_buf resume;
static volatile sig_atomic_t caught;
static unsigned long trap, error;
static uintptr_t rip, address;
static void on_fault(int number, siginfo_t *info, void *opaque) {
  ucontext_t *ctx = opaque;
  caught = number;
  trap = (unsigned long)ctx->uc_mcontext.gregs[REG_TRAPNO];
  error = (unsigned long)ctx->uc_mcontext.gregs[REG_ERR];
  rip = (uintptr_t)ctx->uc_mcontext.gregs[REG_RIP];
  address = (uintptr_t)info->si_addr;
  siglongjmp(resume, 1);
}
int main(void) {
  const long page = sysconf(_SC_PAGESIZE);
  uint8_t *memory =
      mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE | PROT_EXEC,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED || mprotect(memory + page, (size_t)page, PROT_NONE))
    return 2;
  struct sigaction action = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO};
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, NULL);
  sigaction(SIGILL, &action, NULL);
  for (unsigned prefix_count = 0; prefix_count <= 14; ++prefix_count) {
    uint8_t *start = memory + page - prefix_count - 2;
    memset(start, 0x66, prefix_count);
    start[prefix_count] = 0x0f;
    start[prefix_count + 1] = 0xff;
    caught = 0;
    if (sigsetjmp(resume, 1) == 0)
      ((void (*)(void))start)();
    printf("prefixes=%u signal=%d trap=%lu error=%#lx rip_delta=%ld",
           prefix_count, (int)caught, trap, error,
           (long)(rip - (uintptr_t)start));
    if (trap == 14)
      printf(" fault_delta=%ld\n",
             (long)(address - (uintptr_t)(memory + page)));
    else
      puts(" fault_delta=NA");
  }
  return 0;
}
