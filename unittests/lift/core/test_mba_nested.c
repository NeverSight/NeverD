typedef unsigned int u32;
typedef unsigned long long u64;

u32 mba_nested_add(u32 x, u32 y) {
  u32 parity = (x | y) - (x & y);
  u32 carry = (x + y) - (x ^ y);
  return parity + carry;
}

u32 mba_nested_sub(u32 x, u32 y) {
  u32 parity = x ^ y;
  u32 borrow = (~x & y) << 1;
  return parity - borrow;
}

u32 mba_nested_xor(u32 x, u32 y) {
  u32 sum = (x ^ y) + ((x & y) << 1);
  return sum - ((x & y) << 1);
}

u32 mba_nested_or(u32 x, u32 y) {
  u32 disjunction = (x ^ y) + (x & y);
  u32 zero = ((x | y) - (x ^ y)) - (x & y);
  return disjunction + zero;
}

u32 mba_three_input(u32 x, u32 y, u32 z) {
  u32 pair = (x ^ y) + ((x & y) << 1);
  return (pair ^ z) + ((pair & z) << 1);
}

#if __SIZEOF_POINTER__ == 8
u64 mba_wide_add(u64 x, u64 y) {
  u64 sum = (x ^ y) + ((x & y) << 1);
  u64 other = (x | y) + (x & y);
  return sum + (other - x - y);
}
#endif
