//===- x86_division_oracle.h - Original hardware DIV/IDIV -----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent native result and #DE oracle for generated source fixtures.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_X86_DIVISION_ORACLE_H
#define NEVERD_TEST_X86_DIVISION_ORACLE_H

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

static sigjmp_buf division_resume;
static volatile sig_atomic_t division_active;
static void division_fault(int signal_number) {
  (void)signal_number;
  if (!division_active)
    _Exit(125);
  siglongjmp(division_resume, 1);
}
static void division_install(void) {
  struct sigaction action = {0};
  action.sa_handler = division_fault;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGFPE, &action, 0))
    abort();
}

// Carrier RCX receives the original divisor before AX/DX can change, including
// when the source instruction used an alias of either destination register.
static int division_native(unsigned bytes, unsigned is_signed, uint64_t low,
                           uint64_t high, uint64_t divisor, uint64_t *rax,
                           uint64_t *rdx) {
  uint64_t a = low, d = high;
  if (sigsetjmp(division_resume, 1)) {
    division_active = 0;
    return 1;
  }
  division_active = 1;
  switch (bytes) {
  case 1:
    if (is_signed)
      __asm__ volatile("idivb %b1" : "+a"(a) : "c"(divisor) : "cc");
    else
      __asm__ volatile("divb %b1" : "+a"(a) : "c"(divisor) : "cc");
    break;
  case 2:
    if (is_signed)
      __asm__ volatile("idivw %w2" : "+a"(a), "+d"(d) : "c"(divisor) : "cc");
    else
      __asm__ volatile("divw %w2" : "+a"(a), "+d"(d) : "c"(divisor) : "cc");
    break;
  case 4:
    if (is_signed)
      __asm__ volatile("idivl %k2" : "+a"(a), "+d"(d) : "c"(divisor) : "cc");
    else
      __asm__ volatile("divl %k2" : "+a"(a), "+d"(d) : "c"(divisor) : "cc");
    break;
  case 8:
    if (is_signed)
      __asm__ volatile("idivq %2" : "+a"(a), "+d"(d) : "c"(divisor) : "cc");
    else
      __asm__ volatile("divq %2" : "+a"(a), "+d"(d) : "c"(divisor) : "cc");
    break;
  default:
    abort();
  }
  division_active = 0;
  *rax = a;
  *rdx = d;
  return 0;
}

#endif // NEVERD_TEST_X86_DIVISION_ORACLE_H
