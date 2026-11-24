//===- ud0_prefix_matrix.c - Original x86 UD0 prefix oracle -----*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare original UD0 fetch faults and Capstone opcode lengths on P/E cores.
//===----------------------------------------------------------------------===//

#define _GNU_SOURCE
#include <capstone/capstone.h>
#include <capstone/x86.h>
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
static volatile sig_atomic_t caught;
static uintptr_t fault_pc;
static uintptr_t fault_address;
static unsigned long fault_error;
static unsigned long fault_trap;

static void on_fault(int number, siginfo_t *information, void *context) {
  ucontext_t *state = (ucontext_t *)context;
  caught = number;
  fault_pc = (uintptr_t)state->uc_mcontext.gregs[REG_RIP];
  fault_address = (uintptr_t)information->si_addr;
  fault_error = (unsigned long)state->uc_mcontext.gregs[REG_ERR];
  fault_trap = (unsigned long)state->uc_mcontext.gregs[REG_TRAPNO];
  siglongjmp(resume, 1);
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const unsigned expected_trap = (unsigned)strtoul(argv[1], NULL, 10);
  if (expected_trap != 6 && expected_trap != 14)
    return 2;
  const long page_size = sysconf(_SC_PAGESIZE);
  uint8_t *memory =
      mmap(NULL, (size_t)page_size * 2, PROT_READ | PROT_WRITE | PROT_EXEC,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED ||
      mprotect(memory + page_size, (size_t)page_size, PROT_NONE) != 0)
    return 2;
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = on_fault;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  sigaction(SIGILL, &action, NULL);
  sigaction(SIGSEGV, &action, NULL);
  csh decoder = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &decoder) != CS_ERR_OK)
    return 2;
  const uint8_t prefixes[] = {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66,
                              0x67, 0xf0, 0xf2, 0xf3, 0x40, 0x41, 0x42,
                              0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
                              0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
  unsigned total = 0, decoded = 0, matched = 0, mismatch = 0, rejected = 0;
  unsigned unexpected_decode = 0;
  for (unsigned prefix_count = 0; prefix_count <= 2; ++prefix_count) {
    const unsigned first_count = prefix_count ? sizeof(prefixes) : 1;
    const unsigned second_count = prefix_count == 2 ? sizeof(prefixes) : 1;
    for (unsigned i = 0; i < first_count; ++i)
      for (unsigned j = 0; j < second_count; ++j) {
        uint8_t code[5] = {0};
        if (prefix_count >= 1)
          code[0] = prefixes[i];
        if (prefix_count == 2)
          code[1] = prefixes[j];
        code[prefix_count] = 0x0f;
        code[prefix_count + 1] = 0xff;
        code[prefix_count + 2] = 0xc0;
        const size_t opcode_length = prefix_count + 2;
        cs_insn *instruction = NULL;
        const size_t count = cs_disasm(decoder, code, opcode_length + 1, 0x1000,
                                       1, &instruction);
        volatile int is_ud0 = count && instruction[0].id == X86_INS_UD0 &&
                              instruction[0].size == opcode_length;
        if (count)
          cs_free(instruction, count);
        const int expected_ud0 =
            !prefix_count ||
            (prefixes[i] != 0xf0 && (prefix_count != 2 || prefixes[j] != 0xf0));
        if (is_ud0)
          ++decoded;
        else
          ++rejected;
        if (!!is_ud0 != expected_ud0) {
          ++unexpected_decode;
          printf("unexpected decoder result p=%u i=%02x j=%02x decoded=%d\n",
                 prefix_count, prefixes[i], prefixes[j], (int)is_ud0);
        }
        uint8_t *start = memory + page_size - opcode_length;
        memcpy(start, code, opcode_length);
        __builtin___clear_cache((char *)start, (char *)(start + opcode_length));
        caught = 0;
        if (sigsetjmp(resume, 1) == 0)
          ((void (*)(void))start)();
        ++total;
        const int good =
            fault_trap == expected_trap && fault_pc == (uintptr_t)start &&
            ((expected_trap == 14 && caught == SIGSEGV && fault_error == 0x14 &&
              fault_address == (uintptr_t)(memory + page_size)) ||
             (expected_trap == 6 && caught == SIGILL && fault_error == 0));
        if (good)
          ++matched;
        else {
          ++mismatch;
          printf("mismatch p=%u i=%02x j=%02x decoded=%d signal=%d trap=%lu "
                 "err=%#lx\n",
                 prefix_count, i < sizeof(prefixes) ? prefixes[i] : 0,
                 j < sizeof(prefixes) ? prefixes[j] : 0, is_ud0, (int)caught,
                 fault_trap, fault_error);
        }
      }
  }
  printf("expected_trap=%u total=%u decoded_ud0=%u rejected_by_decoder=%u "
         "native_match=%u native_mismatch=%u\n",
         expected_trap, total, decoded, rejected, matched, mismatch);
  cs_close(&decoder);
  munmap(memory, (size_t)page_size * 2);
  const unsigned prefix_alphabet = sizeof(prefixes);
  const unsigned legal_prefixes = prefix_alphabet - 1;
  const unsigned expected_total =
      1 + prefix_alphabet + prefix_alphabet * prefix_alphabet;
  const unsigned expected_decoded =
      1 + legal_prefixes + legal_prefixes * legal_prefixes;
  return mismatch || unexpected_decode || total != expected_total ||
         decoded != expected_decoded || rejected != total - decoded;
}
