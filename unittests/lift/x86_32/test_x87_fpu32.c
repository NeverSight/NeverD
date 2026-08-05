/* i386 x87 FPU instruction semantics: fld, fst, fadd, fsub, fmul, fdiv,
   fild, fistp, fabs, fchs, fxch, fld1/fldz, fnstcw/fldcw */

double test_fadd32(double a, double b) {
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

double test_fsub32(double a, double b) {
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

double test_fmul32(double a, double b) {
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

double test_fdiv32(double a, double b) {
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

double test_fabs32(double a) {
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

double test_fchs32(double a) {
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

int test_fild_fistp32(int a) {
    int result;
    __asm__ volatile (
        "fildl %1\n\t"
        "fistpl %0"
        : "=m"(result)
        : "m"(a)
    );
    return result;
}

void test_fnstcw_fldcw32(void) {
    unsigned short cw;
    __asm__ volatile (
        "fnstcw %0\n\t"
        "fldcw %0"
        : "=m"(cw)
    );
}

void _start(void) {
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(0));
}
