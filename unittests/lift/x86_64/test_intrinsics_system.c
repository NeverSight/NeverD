/* x86-64 system intrinsics — exercises paths that previously fell through
   to __nd_* extern stubs.  Compiled with GNU asm for cross-compile,
   decompile output should use MSVC __asm{} or C intrinsics. */

void test_rdtsc_intrinsic(void) {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
}

void test_pause_intrinsic(void) {
    __asm__ volatile ("pause" ::: "memory");
}

void test_mfence_intrinsic(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

void test_lfence_intrinsic(void) {
    __asm__ volatile ("lfence" ::: "memory");
}

void test_sfence_intrinsic(void) {
    __asm__ volatile ("sfence" ::: "memory");
}

void test_cpuid_intrinsic(void) {
    unsigned int a, b, c, d;
    __asm__ volatile ("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(0), "c"(0));
}

void test_nop_intrinsic(void) {
    __asm__ volatile ("nop");
}

void test_int3_intrinsic(void) {
    __asm__ volatile ("int3");
}

void test_clflush_intrinsic(void* ptr) {
    __asm__ volatile ("clflush (%0)" :: "r"(ptr) : "memory");
}

void test_xgetbv_intrinsic(void) {
    unsigned int lo, hi;
    __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
}

int test_cpuid_return_eax(void) {
    unsigned int a, b, c, d;
    __asm__ volatile ("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(0), "c"(0));
    return (int)a;
}

unsigned long long test_rdtsc_return(void) {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

void test_rdtscp_intrinsic(void) {
    unsigned int lo, hi, aux;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
}

void test_syscall_intrinsic(void) {
    __asm__ volatile ("syscall" ::: "rcx", "r11", "memory");
}

void test_wrmsr_intrinsic(void) {
    __asm__ volatile ("wrmsr" :: "c"(0), "a"(0), "d"(0) : "memory");
}
