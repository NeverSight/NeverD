//===- test_coff_indexed_tables_native.c - Native i386 table oracle ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Exercises the original assembly and lifted IR without a CRT.
//===----------------------------------------------------------------------===//

typedef unsigned u32;
#ifdef ORIGINAL
extern u32 fixture_main(void) __asm__("_main");
extern u32 dispatch(u32) __asm__("_dispatch");
extern u32 classify(u32) __asm__("_classify");
#else
extern u32 fixture_main(void) __asm__("sub_401000");
extern u32 dispatch(u32) __asm__("sub_401020");
extern u32 classify(u32) __asm__("sub_401040");
#endif
int check(void) {
  static const u32 cases[] = {11, 23, 37, 53};
  static const u32 inputs[] = {
      0, 1, 2, 3, 7, 255, 0x7fffffffU, 0x80000000U, 0xfffffffeU, 0xffffffffU};
  if (fixture_main() != 55)
    return 1;
  for (u32 i = 0; i != sizeof(inputs) / sizeof(inputs[0]); ++i) {
    u32 x = inputs[i];
    if (dispatch(x) != x + (x & 1) + 1)
      return 2;
    if (classify(x) != cases[x & 3])
      return 3;
  }
  return 0;
}
__attribute__((naked, noreturn)) void _start(void) {
  __asm__("calll check\n\tmovl %eax, %ebx\n\tmovl $1, %eax\n\tint $0x80");
}
