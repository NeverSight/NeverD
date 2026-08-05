/* SSE/SSE2/SSE3/SSSE3/SSE4/AVX scalar and packed operation tests */

float test_addss(float a, float b) {
    float result;
    __asm__ volatile (
        "addss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

float test_subss(float a, float b) {
    float result;
    __asm__ volatile (
        "subss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

float test_mulss(float a, float b) {
    float result;
    __asm__ volatile (
        "mulss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

float test_divss(float a, float b) {
    float result;
    __asm__ volatile (
        "divss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

double test_addsd(double a, double b) {
    double result;
    __asm__ volatile (
        "addsd %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

double test_subsd(double a, double b) {
    double result;
    __asm__ volatile (
        "subsd %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

double test_mulsd(double a, double b) {
    double result;
    __asm__ volatile (
        "mulsd %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

double test_divsd(double a, double b) {
    double result;
    __asm__ volatile (
        "divsd %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

float test_sqrtss(float a) {
    float result;
    __asm__ volatile (
        "sqrtss %1, %0"
        : "=x"(result)
        : "x"(a)
    );
    return result;
}

double test_sqrtsd(double a) {
    double result;
    __asm__ volatile (
        "sqrtsd %1, %0"
        : "=x"(result)
        : "x"(a)
    );
    return result;
}

float test_maxss(float a, float b) {
    float result;
    __asm__ volatile (
        "maxss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

float test_minss(float a, float b) {
    float result;
    __asm__ volatile (
        "minss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

int test_cvtss2si(float a) {
    int result;
    __asm__ volatile (
        "cvtss2si %1, %0"
        : "=r"(result)
        : "x"(a)
    );
    return result;
}

float test_cvtsi2ss(int a) {
    float result;
    __asm__ volatile (
        "cvtsi2ss %1, %0"
        : "=x"(result)
        : "r"(a)
    );
    return result;
}

int test_cvtsd2si(double a) {
    int result;
    __asm__ volatile (
        "cvtsd2si %1, %0"
        : "=r"(result)
        : "x"(a)
    );
    return result;
}

double test_cvtsi2sd(int a) {
    double result;
    __asm__ volatile (
        "cvtsi2sd %1, %0"
        : "=x"(result)
        : "r"(a)
    );
    return result;
}

float test_cvtsd2ss(double a) {
    float result;
    __asm__ volatile (
        "cvtsd2ss %1, %0"
        : "=x"(result)
        : "x"(a)
    );
    return result;
}

double test_cvtss2sd(float a) {
    double result;
    __asm__ volatile (
        "cvtss2sd %1, %0"
        : "=x"(result)
        : "x"(a)
    );
    return result;
}

int test_ucomiss_eq(float a, float b) {
    int result;
    __asm__ volatile (
        "ucomiss %2, %1\n\t"
        "sete %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "x"(a), "x"(b)
        : "al"
    );
    return result;
}

int test_ucomisd_lt(double a, double b) {
    int result;
    __asm__ volatile (
        "ucomisd %2, %1\n\t"
        "setb %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "x"(a), "x"(b)
        : "al"
    );
    return result;
}

float test_rcpss(float a) {
    float result;
    __asm__ volatile (
        "rcpss %1, %0"
        : "=x"(result)
        : "x"(a)
    );
    return result;
}

float test_rsqrtss(float a) {
    float result;
    __asm__ volatile (
        "rsqrtss %1, %0"
        : "=x"(result)
        : "x"(a)
    );
    return result;
}

void test_movntdq(long long *dst, long long a, long long b) {
    __asm__ volatile (
        "movq %1, %%xmm0\n\t"
        "pinsrq $1, %2, %%xmm0\n\t"
        "movntdq %%xmm0, (%0)"
        :
        : "r"(dst), "r"(a), "r"(b)
        : "xmm0", "memory"
    );
}

int test_ptest_zero(long long a, long long b) {
    int result;
    __asm__ volatile (
        "movq %1, %%xmm0\n\t"
        "movq %2, %%xmm1\n\t"
        "ptest %%xmm1, %%xmm0\n\t"
        "setz %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "r"(a), "r"(b)
        : "xmm0", "xmm1", "al"
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
