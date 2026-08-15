/* AArch64 FEAT_MTE allocation-tag store/load probe. */

#define ADDRESS_MASK 0x00ffffffffffffffUL
#define STORE_TAG 0x0500000000000000UL
#define QUERY_TAG 0x0a00000000000000UL

__attribute__((noinline)) unsigned long
test_ldg_after_stg_a64(unsigned long address) {
  unsigned long tagged_store = (address & ADDRESS_MASK) | STORE_TAG;
  unsigned long tagged_query = (address & ADDRESS_MASK) | QUERY_TAG;
  unsigned long result;

  __asm__ volatile("stg %0, [%0]" : : "r"(tagged_store) : "memory");
  __asm__ volatile("ldg %0, [%1]" : "=r"(result) : "r"(tagged_query));
  return result;
}
