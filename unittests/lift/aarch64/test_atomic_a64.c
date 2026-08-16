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
