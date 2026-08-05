/* x86-64 SSE/FP instruction semantics */

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

int test_comiss_flag(float a, float b) {
    int result;
    __asm__ volatile (
        "comiss %2, %1\n\t"
        "setb %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "x"(a), "x"(b)
        : "eax"
    );
    return result;
}

int test_ucomisd_flag(double a, double b) {
    int result;
    __asm__ volatile (
        "ucomisd %2, %1\n\t"
        "sete %%al\n\t"
        "movzbl %%al, %0"
        : "=r"(result)
        : "x"(a), "x"(b)
        : "eax"
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
