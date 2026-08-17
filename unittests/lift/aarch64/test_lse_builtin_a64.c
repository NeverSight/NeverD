#define DEFINE_LSE_X_TEST(name, mnemonic)                                      \
  __attribute__((naked, noinline, used)) unsigned long name(                   \
      unsigned long *address, unsigned long value) {                           \
    __asm__ volatile("mov x8, x0\n\t" mnemonic " x1, x0, [x8]\n\tret");        \
  }

#define DEFINE_LSE_W_TEST(name, mnemonic, pointer_type)                        \
  __attribute__((naked, noinline, used)) unsigned name(pointer_type *address,  \
                                                       unsigned value) {       \
    __asm__ volatile("mov x8, x0\n\t" mnemonic " w1, w0, [x8]\n\tret");        \
  }

DEFINE_LSE_X_TEST(test_lse_ldclral, "ldclral")
DEFINE_LSE_W_TEST(test_lse_ldeorb, "ldeorb", unsigned char)
DEFINE_LSE_W_TEST(test_lse_ldsetah, "ldsetah", unsigned short)
DEFINE_LSE_W_TEST(test_lse_ldsmaxa, "ldsmaxa", unsigned)
DEFINE_LSE_X_TEST(test_lse_ldsminl, "ldsminl")
DEFINE_LSE_W_TEST(test_lse_ldumaxalb, "ldumaxalb", unsigned char)
DEFINE_LSE_W_TEST(test_lse_lduminalh, "lduminalh", unsigned short)
