/* x86-32 floating-point instruction semantics via inline asm. */

float test_addss32(float a, float b) {
    float result;
    __asm__ volatile (
        "addss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

float test_subss32(float a, float b) {
    float result;
    __asm__ volatile (
        "subss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

float test_mulss32(float a, float b) {
    float result;
    __asm__ volatile (
        "mulss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

float test_divss32(float a, float b) {
    float result;
    __asm__ volatile (
        "divss %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

int test_cvtss2si32(float a) {
    int result;
    __asm__ volatile (
        "cvtss2si %1, %0"
        : "=r"(result)
        : "x"(a)
    );
    return result;
}

float test_cvtsi2ss32(int a) {
    float result;
    __asm__ volatile (
        "cvtsi2ss %1, %0"
        : "=x"(result)
        : "r"(a)
    );
    return result;
}

double test_addsd32(double a, double b) {
    double result;
    __asm__ volatile (
        "addsd %2, %0"
        : "=x"(result)
        : "0"(a), "x"(b)
    );
    return result;
}

int test_cvttss2si32(float a) {
    int result;
    __asm__ volatile (
        "cvttss2si %1, %0"
        : "=r"(result)
        : "x"(a)
    );
    return result;
}

void _start(void) {
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(0));
}
