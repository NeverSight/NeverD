/* x86-64 x87 FPU instruction semantics: fld, fst, fadd, fsub, fmul, fdiv,
   fild, fistp, fabs, fchs, frndint, fsqrt, fxch, fcom, fcomp, fnstcw, fldcw */

double test_fadd(double a, double b) {
    double result;
    __asm__ volatile (
        "fldl %1\n\t"
        "faddl %2\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(a), "m"(b)
    );
    return result;
}

double test_fsub(double a, double b) {
    double result;
    __asm__ volatile (
        "fldl %1\n\t"
        "fsubl %2\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(a), "m"(b)
    );
    return result;
}

double test_fmul(double a, double b) {
    double result;
    __asm__ volatile (
        "fldl %1\n\t"
        "fmull %2\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(a), "m"(b)
    );
    return result;
}

double test_fdiv(double a, double b) {
    double result;
    __asm__ volatile (
        "fldl %1\n\t"
        "fdivl %2\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(a), "m"(b)
    );
    return result;
}

double test_fabs(double a) {
    double result;
    __asm__ volatile (
        "fldl %1\n\t"
        "fabs\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(a)
    );
    return result;
}

double test_fchs(double a) {
    double result;
    __asm__ volatile (
        "fldl %1\n\t"
        "fchs\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(a)
    );
    return result;
}

double test_fsqrt(double a) {
    double result;
    __asm__ volatile (
        "fldl %1\n\t"
        "fsqrt\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(a)
    );
    return result;
}

int test_fild_fistp(int a) {
    int result;
    __asm__ volatile (
        "fildl %1\n\t"
        "fistpl %0"
        : "=m"(result)
        : "m"(a)
    );
    return result;
}

void test_fxch(double a, double b) {
    double r1, r2;
    __asm__ volatile (
        "fldl %2\n\t"
        "fldl %3\n\t"
        "fxch %%st(1)\n\t"
        "fstpl %0\n\t"
        "fstpl %1"
        : "=m"(r1), "=m"(r2)
        : "m"(a), "m"(b)
    );
}

void test_fld1_fldz(void) {
    double one, zero;
    __asm__ volatile (
        "fld1\n\t"
        "fstpl %0\n\t"
        "fldz\n\t"
        "fstpl %1"
        : "=m"(one), "=m"(zero)
    );
}

double test_frndint(double a) {
    double result;
    __asm__ volatile (
        "fldl %1\n\t"
        "frndint\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(a)
    );
    return result;
}

void test_fnstcw_fldcw(void) {
    unsigned short cw;
    __asm__ volatile (
        "fnstcw %0\n\t"
        "fldcw %0"
        : "=m"(cw)
    );
}

void test_fcom(double a, double b) {
    __asm__ volatile (
        "fldl %0\n\t"
        "fldl %1\n\t"
        "fcom %%st(1)\n\t"
        "fstp %%st(0)\n\t"
        "fstp %%st(0)"
        :
        : "m"(a), "m"(b)
    );
}

void test_fcompp(double a, double b) {
    __asm__ volatile (
        "fldl %0\n\t"
        "fldl %1\n\t"
        "fcompp"
        :
        : "m"(a), "m"(b)
    );
}

void test_fnstsw(double a, double b) {
    unsigned short sw;
    __asm__ volatile (
        "fldl %1\n\t"
        "fldl %2\n\t"
        "fcompp\n\t"
        "fnstsw %0"
        : "=m"(sw)
        : "m"(a), "m"(b)
    );
}

void test_finit(void) {
    __asm__ volatile ("fninit");
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
