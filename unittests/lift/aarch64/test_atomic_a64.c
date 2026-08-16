/* AArch64 atomic / exclusive ops: LDXR, STXR, CAS, SWP (ARMv8.1) */

int test_ldxr_stxr(int* addr, int newval) {
    int old;
    int status;
    __asm__ volatile (
        "ldxr %w0, [%2]\n\t"
        "stxr %w1, %w3, [%2]"
        : "=&r"(old), "=&r"(status)
        : "r"(addr), "r"(newval)
        : "memory"
    );
    return old;
}

long long test_ldxr_stxr_64(long long* addr, long long newval) {
    long long old;
    int status;
    __asm__ volatile (
        "ldxr %0, [%2]\n\t"
        "stxr %w1, %3, [%2]"
        : "=&r"(old), "=&r"(status)
        : "r"(addr), "r"(newval)
        : "memory"
    );
    return old;
}

unsigned long long test_ldar(const unsigned long long *addr) {
    unsigned long long value;
    __asm__ volatile ("ldar %0, [%1]"
                      : "=r"(value)
                      : "r"(addr)
                      : "memory");
    return value;
}

void test_ldar_discard(const unsigned long long *addr) {
    unsigned long long value;
    __asm__ volatile ("ldar %0, [%1]"
                      : "=r"(value)
                      : "r"(addr)
                      : "memory");
}

void test_stlr(unsigned long long *addr, unsigned long long value) {
    __asm__ volatile ("stlr %1, [%0]"
                      :
                      : "r"(addr), "r"(value)
                      : "memory");
}

struct pair64 {
    unsigned long low;
    unsigned long high;
};

unsigned test_stxr_after_clrex(unsigned long *addr) {
  unsigned long value;
  unsigned status;
  __asm__ volatile("ldxr %1, [%2]\n\t"
                   "clrex\n\t"
                   "stxr %w0, %1, [%2]"
                   : "=&r"(status), "=&r"(value)
                   : "r"(addr)
                   : "memory");
  return status;
}

unsigned test_stxp_after_clrex(struct pair64 *addr) {
  unsigned long low;
  unsigned long high;
  unsigned status;
  __asm__ volatile("ldxp %1, %2, [%3]\n\t"
                   "clrex\n\t"
                   "stxp %w0, %1, %2, [%3]"
                   : "=&r"(status), "=&r"(low), "=&r"(high)
                   : "r"(addr)
                   : "memory");
  return status;
}

#define DEFINE_SWPP_TEST(name, mnemonic)                                      \
    struct pair64 name(struct pair64 *addr, unsigned long low,                \
                       unsigned long high) {                                  \
        __asm__ volatile(mnemonic " %0, %1, [%2]"                            \
                         : "+r"(low), "+r"(high)                             \
                         : "r"(addr)                                          \
                         : "memory");                                         \
        return (struct pair64){low, high};                                    \
    }

DEFINE_SWPP_TEST(test_swpp, "swpp")
DEFINE_SWPP_TEST(test_swppa, "swppa")
DEFINE_SWPP_TEST(test_swppal, "swppal")
DEFINE_SWPP_TEST(test_swppl, "swppl")

#define DEFINE_LDCLRP_TEST(name, mnemonic)                                     \
  struct pair64 name(struct pair64 *addr, unsigned long low,                   \
                     unsigned long high) {                                     \
    __asm__ volatile(mnemonic " %0, %1, [%2]"                                  \
                     : "+r"(low), "+r"(high)                                   \
                     : "r"(addr)                                               \
                     : "memory");                                              \
    return (struct pair64){low, high};                                         \
  }

DEFINE_LDCLRP_TEST(test_ldclrp, "ldclrp")
DEFINE_LDCLRP_TEST(test_ldclrpa, "ldclrpa")
DEFINE_LDCLRP_TEST(test_ldclrpal, "ldclrpal")
DEFINE_LDCLRP_TEST(test_ldclrpl, "ldclrpl")

#define DEFINE_LDADD_TEST(name, mnemonic)                                      \
  __attribute__((naked, noinline, used)) unsigned long name(                   \
      unsigned long *address, unsigned long value) {                           \
    __asm__ volatile("mov x8, x0\n\t" mnemonic " x1, x0, [x8]\n\tret");        \
  }

DEFINE_LDADD_TEST(test_ldadd, "ldadd")
DEFINE_LDADD_TEST(test_ldadda, "ldadda")
DEFINE_LDADD_TEST(test_ldaddal, "ldaddal")
DEFINE_LDADD_TEST(test_ldaddl, "ldaddl")

#define DEFINE_CAS_TEST(name, mnemonic)                                        \
  __attribute__((naked, noinline, used)) unsigned long name(                   \
      unsigned long *address, unsigned long expected, unsigned long desired) { \
    __asm__ volatile("mov x8, x0\n\t" mnemonic                                 \
                     " x1, x2, [x8]\n\tmov x0, x1\n\tret");                    \
  }

DEFINE_CAS_TEST(test_cas, "cas")
DEFINE_CAS_TEST(test_casa, "casa")
DEFINE_CAS_TEST(test_casal, "casal")
DEFINE_CAS_TEST(test_casl, "casl")

__attribute__((naked, noinline, used)) unsigned
test_casb(unsigned char *address, unsigned expected, unsigned desired) {
  __asm__ volatile("mov x8, x0\n\tcasb w1, w2, [x8]\n\tmov w0, w1\n\tret");
}

__attribute__((naked, noinline, used)) unsigned
test_cash(unsigned short *address, unsigned expected, unsigned desired) {
  __asm__ volatile("mov x8, x0\n\tcash w1, w2, [x8]\n\tmov w0, w1\n\tret");
}

#define DEFINE_CASP_TEST(name, mnemonic)                                       \
  __attribute__((naked, noinline, used)) struct pair64 name(                   \
      struct pair64 *address, unsigned long expected_low,                      \
      unsigned long expected_high, unsigned long desired_low,                  \
      unsigned long desired_high) {                                            \
    __asm__ volatile("mov x10, x0\n\tmov x6, x1\n\tmov x7, x2\n\t"             \
                     "mov x8, x3\n\tmov x9, x4\n\t" mnemonic                   \
                     " x6, x7, x8, x9, [x10]\n\tmov x0, x6\n\t"                \
                     "mov x1, x7\n\tret");                                     \
  }

DEFINE_CASP_TEST(test_casp, "casp")
DEFINE_CASP_TEST(test_caspa, "caspa")
DEFINE_CASP_TEST(test_caspal, "caspal")
DEFINE_CASP_TEST(test_caspl, "caspl")

__attribute__((naked, noinline, used)) unsigned
test_ldaddb(unsigned char *address, unsigned value) {
  __asm__ volatile("mov x8, x0\n\tldaddb w1, w0, [x8]\n\tret");
}

__attribute__((naked, noinline, used)) unsigned
test_ldaddh(unsigned short *address, unsigned value) {
  __asm__ volatile("mov x8, x0\n\tldaddh w1, w0, [x8]\n\tret");
}

__attribute__((naked, noinline, used)) unsigned long
test_stadd(unsigned long *address, unsigned long value) {
  __asm__ volatile("stadd x1, [x0]\n\tmov x0, xzr\n\tret");
}

__attribute__((naked, noinline)) void test_rcwcasp_pair(void) {
  __asm__ volatile("rcwcasp x0, x1, x2, x3, [x4]\n\t"
                   "ret"
                   :
                   :
                   : "x0", "x1", "memory");
}

void test_dmb(void) {
    __asm__ volatile ("dmb ish" ::: "memory");
}

void test_dsb(void) {
    __asm__ volatile ("dsb ish" ::: "memory");
}

void test_isb(void) {
    __asm__ volatile ("isb" ::: "memory");
}

void _start(void) {
    __asm__ volatile (
        "mov x8, #93\n\t"
        "mov x0, #0\n\t"
        "svc #0"
    );
}
