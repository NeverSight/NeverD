/* x86-64 SSE/AVX miscellaneous: packed ops, movaps/movups/movdqa/movdqu,
   pxor self-clear, packed integer, extract/insert, and SSE conversion gaps. */

void test_movaps_copy(float* dst, const float* src) {
    __asm__ volatile (
        "movaps (%1), %%xmm0\n\t"
        "movaps %%xmm0, (%0)"
        :
        : "r"(dst), "r"(src)
        : "xmm0", "memory"
    );
}

void test_movups_copy(float* dst, const float* src) {
    __asm__ volatile (
        "movups (%1), %%xmm0\n\t"
        "movups %%xmm0, (%0)"
        :
        : "r"(dst), "r"(src)
        : "xmm0", "memory"
    );
}

void test_movdqa_copy(void* dst, const void* src) {
    __asm__ volatile (
        "movdqa (%1), %%xmm0\n\t"
        "movdqa %%xmm0, (%0)"
        :
        : "r"(dst), "r"(src)
        : "xmm0", "memory"
    );
}

void test_pxor_clear(void) {
    __asm__ volatile (
        "pxor %%xmm0, %%xmm0"
        ::: "xmm0"
    );
}

void test_xorps_clear(void) {
    __asm__ volatile (
        "xorps %%xmm1, %%xmm1"
        ::: "xmm1"
    );
}

void test_paddd(int* dst, const int* a, const int* b) {
    __asm__ volatile (
        "movdqa (%1), %%xmm0\n\t"
        "paddd (%2), %%xmm0\n\t"
        "movdqa %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_psubd(int* dst, const int* a, const int* b) {
    __asm__ volatile (
        "movdqa (%1), %%xmm0\n\t"
        "psubd (%2), %%xmm0\n\t"
        "movdqa %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_pcmpeqd(int* dst, const int* a, const int* b) {
    __asm__ volatile (
        "movdqa (%1), %%xmm0\n\t"
        "pcmpeqd (%2), %%xmm0\n\t"
        "movdqa %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_addps(float* dst, const float* a, const float* b) {
    __asm__ volatile (
        "movaps (%1), %%xmm0\n\t"
        "addps (%2), %%xmm0\n\t"
        "movaps %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_subps(float* dst, const float* a, const float* b) {
    __asm__ volatile (
        "movaps (%1), %%xmm0\n\t"
        "subps (%2), %%xmm0\n\t"
        "movaps %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_mulps(float* dst, const float* a, const float* b) {
    __asm__ volatile (
        "movaps (%1), %%xmm0\n\t"
        "mulps (%2), %%xmm0\n\t"
        "movaps %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_divps(float* dst, const float* a, const float* b) {
    __asm__ volatile (
        "movaps (%1), %%xmm0\n\t"
        "divps (%2), %%xmm0\n\t"
        "movaps %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_pshufd(int* dst, const int* src) {
    __asm__ volatile (
        "movdqa (%1), %%xmm0\n\t"
        "pshufd $0x1B, %%xmm0, %%xmm1\n\t"
        "movdqa %%xmm1, (%0)"
        :
        : "r"(dst), "r"(src)
        : "xmm0", "xmm1", "memory"
    );
}

void test_punpcklbw(void* dst, const void* a, const void* b) {
    __asm__ volatile (
        "movdqa (%1), %%xmm0\n\t"
        "punpcklbw (%2), %%xmm0\n\t"
        "movdqa %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_movd_extract(const int* src, int* dst) {
    __asm__ volatile (
        "movd (%1), %%xmm0\n\t"
        "movd %%xmm0, (%0)"
        :
        : "r"(dst), "r"(src)
        : "xmm0", "memory"
    );
}

void test_movq_copy(const long long* src, long long* dst) {
    __asm__ volatile (
        "movq (%1), %%xmm0\n\t"
        "movq %%xmm0, (%0)"
        :
        : "r"(dst), "r"(src)
        : "xmm0", "memory"
    );
}

void test_pand_por(void* dst, const void* a, const void* b) {
    __asm__ volatile (
        "movdqa (%1), %%xmm0\n\t"
        "pand (%2), %%xmm0\n\t"
        "por (%2), %%xmm0\n\t"
        "movdqa %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

void test_stmxcsr_ldmxcsr(void) {
    unsigned int mxcsr;
    __asm__ volatile (
        "stmxcsr %0\n\t"
        "ldmxcsr %0"
        : "=m"(mxcsr)
        :: "memory"
    );
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
