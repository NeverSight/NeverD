/* ARM32 system intrinsics — exercises paths that previously fell through
   to __nd_* extern stubs.  Compiled with GNU asm for cross-compile,
   decompile output should use ACLE intrinsics or GNU __asm__ volatile. */

void test_svc_arm_intrinsic(void) {
    __asm__ volatile ("svc #0");
}

void test_bkpt_arm_intrinsic(void) {
    __asm__ volatile ("bkpt #0");
}

void test_nop_arm_intrinsic(void) {
    __asm__ volatile ("nop");
}

void test_clrex_arm_intrinsic(void) {
    __asm__ volatile ("clrex" ::: "memory");
}

void test_dmb_arm_intrinsic(void) {
    __asm__ volatile ("dmb ish" ::: "memory");
}

void test_dsb_arm_intrinsic(void) {
    __asm__ volatile ("dsb ish" ::: "memory");
}

void test_isb_arm_intrinsic(void) {
    __asm__ volatile ("isb" ::: "memory");
}

void test_sel_arm_intrinsic(unsigned int a, unsigned int b) {
    unsigned int result;
    __asm__ volatile ("sel %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
}
