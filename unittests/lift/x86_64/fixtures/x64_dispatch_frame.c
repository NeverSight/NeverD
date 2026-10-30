//===- x64_dispatch_frame.c - Independent native bridge oracle ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Executes the original instruction bytes and rebuilt bridge in CPL0. The
/// dispatcher captures the complete GPR/flags/stack interface before services.
//===----------------------------------------------------------------------===//

#include <stddef.h>
#include <stdint.h>

#if BRIDGE_TYPED
#define bridge_entry ZwQueryVirtualMemory
#endif

extern int memcmp(const void *, const void *, size_t);
extern void *memcpy(void *, const void *, size_t);

uint64_t bridge_input[16], bridge_output[16], bridge_input_flags;
uint64_t bridge_output_flags, bridge_host_sp;
unsigned char bridge_call_mode;
uint64_t bridge_stack_arguments[2];
uint8_t bridge_stack[4096] __attribute__((aligned(16)));
void (*bridge_address)(void);

__asm__(".text\n"
        ".globl native_linkage\nnative_linkage: ret\n"
        ".globl original_bridge\noriginal_bridge:\n"
        // Original MOV encoding is deliberately independent of the renderer.
        ".byte 0x48,0x8b,0xc4,0xfa,0x48,0x83,0xec,0x10,0x50,0x9c,0x6a,0x10\n"
        ".byte 0x48,0x8d,0x05\n.long native_linkage-.-4\n"
        ".byte 0x50,0xb8\n.long 0xf1234567\n"
        ".byte 0xe9\n.long native_dispatch-.-4\n"
        ".globl run_bridge\nrun_bridge:\n"
        "pushq %rbp\npushq %rbx\npushq %r12\npushq %r13\npushq %r14\npushq "
        "%r15\npushfq\n"
        "movq %rsp,bridge_host_sp(%rip)\n"
        "pushq bridge_input_flags(%rip)\npopfq\n"
        "movq bridge_input+0(%rip),%rax\n"
        "movq bridge_input+8(%rip),%rcx\n"
        "movq bridge_input+16(%rip),%rdx\n"
        "movq bridge_input+24(%rip),%rbx\n"
        "movq bridge_input+32(%rip),%rsp\n"
        "movq bridge_input+40(%rip),%rbp\n"
        "movq bridge_input+48(%rip),%rsi\n"
        "movq bridge_input+56(%rip),%rdi\n"
        "movq bridge_input+64(%rip),%r8\n"
        "movq bridge_input+72(%rip),%r9\n"
        "movq bridge_input+80(%rip),%r10\n"
        "movq bridge_input+88(%rip),%r11\n"
        "movq bridge_input+96(%rip),%r12\n"
        "movq bridge_input+104(%rip),%r13\n"
        "movq bridge_input+112(%rip),%r14\n"
        "movq bridge_input+120(%rip),%r15\n"
        "jmp *bridge_address(%rip)\n"
        ".globl native_dispatch\nnative_dispatch:\n"
        "movq %rax,bridge_output+0(%rip)\n"
        "movq %rcx,bridge_output+8(%rip)\n"
        "movq %rdx,bridge_output+16(%rip)\n"
        "movq %rbx,bridge_output+24(%rip)\n"
        "movq %rsp,bridge_output+32(%rip)\n"
        "movq %rbp,bridge_output+40(%rip)\n"
        "movq %rsi,bridge_output+48(%rip)\n"
        "movq %rdi,bridge_output+56(%rip)\n"
        "movq %r8,bridge_output+64(%rip)\n"
        "movq %r9,bridge_output+72(%rip)\n"
        "movq %r10,bridge_output+80(%rip)\n"
        "movq %r11,bridge_output+88(%rip)\n"
        "movq %r12,bridge_output+96(%rip)\n"
        "movq %r13,bridge_output+104(%rip)\n"
        "movq %r14,bridge_output+112(%rip)\n"
        "movq %r15,bridge_output+120(%rip)\n"
        "pushfq\npopq bridge_output_flags(%rip)\n"
        "cmpb $0,bridge_call_mode(%rip)\njne 1f\n"
        "movq bridge_host_sp(%rip),%rsp\n"
        "popfq\npopq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rbx\npopq "
        "%rbp\nret\n"
        "1: movq 24(%rsp),%rax\n"
        "movq 40(%rax),%rdx\nmovq %rdx,bridge_stack_arguments(%rip)\n"
        "movq 48(%rax),%rdx\nmovq %rdx,bridge_stack_arguments+8(%rip)\n"
        "movq %rax,%rsp\nmovq bridge_output(%rip),%rax\nret\n");
extern void original_bridge(void), native_linkage(void), run_bridge(void);

int main(void) {
  const unsigned FlagBits[] = {0, 2, 4, 6, 7, 11};
  uint8_t Before[4096], After[4096];
  uint64_t Expected[16], Flags;
  for (unsigned Pattern = 0; Pattern < 64; ++Pattern) {
    bridge_input_flags = 0x202;
    for (unsigned Bit = 0; Bit < 6; ++Bit)
      bridge_input_flags |= (uint64_t)((Pattern >> Bit) & 1) << FlagBits[Bit];
    for (unsigned I = 0; I < 16; ++I)
      bridge_input[I] = 0x1122334400000000ULL + Pattern * 119 + I * 73;
    bridge_input[4] = (uintptr_t)bridge_stack + 2048 + (Pattern % 16) * 8;
    for (unsigned I = 0; I < sizeof(Before); ++I)
      Before[I] = I * 17 + Pattern;
    memcpy(bridge_stack, Before, sizeof(Before));
    bridge_address = original_bridge;
    run_bridge();
    memcpy(Expected, bridge_output, sizeof(Expected));
    Flags = bridge_output_flags;
    memcpy(After, bridge_stack, sizeof(After));
    if (Expected[0] != 0xf1234567 || Expected[4] != bridge_input[4] - 48 ||
        (Flags & 0x200))
      return 1;
    const uint64_t *Frame = (const uint64_t *)(uintptr_t)Expected[4];
    if (Frame[0] != (uintptr_t)native_linkage || Frame[1] != 16 ||
        Frame[2] != Flags || Frame[3] != bridge_input[4])
      return 2;
    for (unsigned I = 1; I < 16; ++I)
      if (I != 4 && Expected[I] != bridge_input[I])
        return 3;
    memcpy(bridge_stack, Before, sizeof(Before));
    bridge_address = (void (*)(void))(uintptr_t)bridge_entry;
    run_bridge();
    if (memcmp(Expected, bridge_output, sizeof(Expected)) ||
        Flags != bridge_output_flags ||
        memcmp(After, bridge_stack, sizeof(After)))
      return 4;
  }
#if BRIDGE_TYPED
  bridge_call_mode = 1;
  const int32_t Result = bridge_caller();
  if (Result != (int32_t)0xf1234567 ||
      bridge_output[1] != 0x1234567812345678ULL ||
      bridge_output[2] != 0xfedcba98 || bridge_output[8] != 0x12340 ||
      bridge_output[9] != 0xbc)
    return 5;
  if (bridge_stack_arguments[0] != 0x9876543212345678ULL ||
      bridge_stack_arguments[1] != 0xf1234567)
    return 6;
#endif
  return 0;
}
