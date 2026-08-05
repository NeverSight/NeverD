/* x86-64: SSE float conversions, comparisons, sqrt */

float test_cvtsi2ss(int a) {
    float result;
    __asm__ volatile (
        "cvtsi2ssl %1, %0"
        : "=x"(result)
        : "r"(a)
    );
    return result;
}

int test_cvtss2si(float a) {
    int result;
    __asm__ volatile (
        "cvtss2sil %1, %0"
        : "=r"(result)
        : "x"(a)
    );
    return result;
}

int test_cvttss2si(float a) {
    int result;
    __asm__ volatile (
        "cvttss2sil %1, %0"
        : "=r"(result)
        : "x"(a)
    );
    return result;
}

double test_cvtsi2sd(int a) {
    double result;
    __asm__ volatile (
        "cvtsi2sdl %1, %0"
        : "=x"(result)
        : "r"(a)
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

float test_sqrtss(float a) {
    float result;
    __asm__ volatile (
        "sqrtss %1, %0"
        : "=x"(result)
        : "x"(a)
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

double test_sqrtsd(double a) {
    double result;
    __asm__ volatile (
        "sqrtsd %1, %0"
        : "=x"(result)
        : "x"(a)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
