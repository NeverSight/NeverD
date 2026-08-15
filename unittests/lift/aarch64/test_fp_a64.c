/* AArch64: floating-point arithmetic and conversions */

float test_fadd_a64(float a, float b) {
    float result;
    __asm__ volatile (
        "fadd %s0, %s1, %s2"
        : "=w"(result)
        : "w"(a), "w"(b)
    );
    return result;
}

float test_fsub_a64(float a, float b) {
    float result;
    __asm__ volatile (
        "fsub %s0, %s1, %s2"
        : "=w"(result)
        : "w"(a), "w"(b)
    );
    return result;
}

float test_fmul_a64(float a, float b) {
    float result;
    __asm__ volatile (
        "fmul %s0, %s1, %s2"
        : "=w"(result)
        : "w"(a), "w"(b)
    );
    return result;
}

float test_fdiv_a64(float a, float b) {
    float result;
    __asm__ volatile (
        "fdiv %s0, %s1, %s2"
        : "=w"(result)
        : "w"(a), "w"(b)
    );
    return result;
}

float test_fsqrt_a64(float a) {
    float result;
    __asm__ volatile (
        "fsqrt %s0, %s1"
        : "=w"(result)
        : "w"(a)
    );
    return result;
}

float test_fneg_a64(float a) {
    float result;
    __asm__ volatile (
        "fneg %s0, %s1"
        : "=w"(result)
        : "w"(a)
    );
    return result;
}

float test_fabs_a64(float a) {
    float result;
    __asm__ volatile (
        "fabs %s0, %s1"
        : "=w"(result)
        : "w"(a)
    );
    return result;
}

double test_fcvt_s2d_a64(float a) {
    double result;
    __asm__ volatile (
        "fcvt %d0, %s1"
        : "=w"(result)
        : "w"(a)
    );
    return result;
}

float test_scvtf_a64(int a) {
    float result;
    __asm__ volatile (
        "scvtf %s0, %w1"
        : "=w"(result)
        : "r"(a)
    );
    return result;
}

int test_fcvtzs_a64(float a) {
    int result;
    __asm__ volatile (
        "fcvtzs %w0, %s1"
        : "=r"(result)
        : "w"(a)
    );
    return result;
}

unsigned test_fjcvtzs_z_a64(double input) {
  unsigned converted;
  unsigned exact;
  __asm__ volatile("cmp wzr, wzr\n\t"
                   "fjcvtzs %w0, %d2\n\t"
                   "cset %w1, eq"
                   : "=&r"(converted), "=&r"(exact)
                   : "w"(input)
                   : "cc");
  return exact;
}

float test_frecpx_a64(float input) {
  float result;
  __asm__ volatile("frecpx %s0, %s1" : "=w"(result) : "w"(input));
  return result;
}

double test_fmadd_a64(double a, double b, double c) {
    double result;
    __asm__ volatile (
        "fmadd %d0, %d1, %d2, %d3"
        : "=w"(result)
        : "w"(a), "w"(b), "w"(c)
    );
    return result;
}

void _start(void) {
    __asm__ volatile (
        "mov x8, #93\n\t"
        "mov x0, #0\n\t"
        "svc #0"
    );
}
