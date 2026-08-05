/* AArch64 system intrinsics — exercises paths that previously fell through
   to __nd_* extern stubs.  Compiled with GNU asm for cross-compile,
   decompile output should use ACLE intrinsics or GNU __asm__ volatile. */

void test_dmb_intrinsic(void) {
    __asm__ volatile ("dmb ish" ::: "memory");
}

void test_dsb_intrinsic(void) {
    __asm__ volatile ("dsb ish" ::: "memory");
}

void test_isb_intrinsic(void) {
    __asm__ volatile ("isb" ::: "memory");
}

void test_yield_intrinsic(void) {
    __asm__ volatile ("yield");
}

void test_wfe_intrinsic(void) {
    __asm__ volatile ("wfe");
}

void test_wfi_intrinsic(void) {
    __asm__ volatile ("wfi");
}

void test_sev_intrinsic(void) {
    __asm__ volatile ("sev");
}

void test_sevl_intrinsic(void) {
    __asm__ volatile ("sevl");
}

void test_brk_intrinsic(void) {
    __asm__ volatile ("brk #0x1");
}

void test_svc_intrinsic(void) {
    __asm__ volatile ("svc #0");
}

void test_clrex_intrinsic(void) {
    __asm__ volatile ("clrex" ::: "memory");
}
