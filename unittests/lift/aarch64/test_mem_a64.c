/* AArch64 memory operations: LDR, STR, LDP, STP, extensions */

struct mem_pair64 {
  unsigned long value;
  unsigned long pointer;
};

/*
 * Keep one load and one store observable after the lifted module's O2 pass.
 * The local round trips below are intentionally eligible for store-to-load
 * forwarding; accesses through a caller-provided address are not.
 */
int test_ldr_external_a64(const int *ptr) {
  int result;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(result) : "r"(ptr) : "memory");
  return result;
}

void test_str_external_a64(int *ptr, int val) {
  __asm__ volatile("str %w1, [%0]" : : "r"(ptr), "r"(val) : "memory");
}

__attribute__((naked, noinline, used)) unsigned long
test_ldapr_a64(const unsigned long *address) {
  __asm__ volatile("ldapr x0, [x0]\n\tret");
}

__attribute__((naked, noinline, used)) unsigned
test_ldaprb_a64(const unsigned char *address) {
  __asm__ volatile("ldaprb w0, [x0]\n\tret");
}

__attribute__((naked, noinline, used)) unsigned
test_ldaprh_a64(const unsigned short *address) {
  __asm__ volatile("ldaprh w0, [x0]\n\tret");
}

__attribute__((naked, noinline, used)) unsigned long
test_stlur_a64(unsigned long *address, unsigned long value) {
  __asm__ volatile("stlur x1, [x0]\n\tmov x0, xzr\n\tret");
}

__attribute__((naked, noinline, used)) unsigned long
test_stlurb_a64(unsigned char *address, unsigned value) {
  __asm__ volatile("stlurb w1, [x0]\n\tmov x0, xzr\n\tret");
}

__attribute__((naked, noinline, used)) unsigned long
test_stlurh_a64(unsigned short *address, unsigned value) {
  __asm__ volatile("stlurh w1, [x0]\n\tmov x0, xzr\n\tret");
}

__attribute__((naked, noinline, used)) struct mem_pair64
test_ldr_post_return_a64(unsigned long *address) {
  __asm__ volatile("ldr x1, [x0], #8\n\t"
                   "mov x2, x0\n\t"
                   "mov x0, x1\n\t"
                   "mov x1, x2\n\t"
                   "ret");
}

int test_ldr_str_a64(int val) {
  int result;
  int buf;
  __asm__ volatile("str %w1, [%2]\n\t"
                   "ldr %w0, [%2]"
                   : "=r"(result)
                   : "r"(val), "r"(&buf)
                   : "memory");
  return result;
}

long long test_ldrsw_a64(int val) {
  long long result;
  __asm__ volatile("str %w1, [%2]\n\t"
                   "ldrsw %0, [%2]"
                   : "=r"(result)
                   : "r"(val), "r"(&(int){0})
                   : "memory");
  return result;
}

int test_ldrb_a64(int val) {
  int result;
  unsigned char buf;
  __asm__ volatile("strb %w1, [%2]\n\t"
                   "ldrb %w0, [%2]"
                   : "=r"(result)
                   : "r"(val), "r"(&buf)
                   : "memory");
  return result;
}

int test_ldrh_a64(int val) {
  int result;
  unsigned short buf;
  __asm__ volatile("strh %w1, [%2]\n\t"
                   "ldrh %w0, [%2]"
                   : "=r"(result)
                   : "r"(val), "r"(&buf)
                   : "memory");
  return result;
}

int test_sxtw_a64(int val) {
  long long result;
  __asm__ volatile("sxtw %0, %w1" : "=r"(result) : "r"(val));
  return (int)result;
}

int test_uxtb_a64(int val) {
  int result;
  __asm__ volatile("uxtb %w0, %w1" : "=r"(result) : "r"(val));
  return result;
}

int test_uxth_a64(int val) {
  int result;
  __asm__ volatile("uxth %w0, %w1" : "=r"(result) : "r"(val));
  return result;
}

void _start(void) {
  __asm__ volatile("mov x8, #93\n\t"
                   "mov x0, #0\n\t"
                   "svc #0");
}
