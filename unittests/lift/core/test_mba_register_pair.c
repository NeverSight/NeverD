typedef unsigned int u32;
typedef unsigned long long u64;

__attribute__((noinline)) u64 mba_pair_add(u64 x, u64 y) {
  u64 parity = x ^ y;
  u64 carry = (x & y) << 1;
  return parity + carry;
}

__attribute__((noinline)) u64 mba_pair_or_add(u64 x, u64 y) {
  return (x | y) + (x & y);
}

__attribute__((noinline)) u64 mba_pair_sub(u64 x, u64 y) {
  u64 parity = x ^ ~y;
  u64 carry = (x & ~y) << 1;
  return parity + carry + 1;
}

u32 mba_pair_fold(u32 xl, u32 xh, u32 yl, u32 yh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 sum = mba_pair_add(x, y);
  return (u32)sum ^ (u32)(sum >> 32);
}

u32 mba_pair_or_fold(u32 xl, u32 xh, u32 yl, u32 yh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 sum = mba_pair_or_add(x, y);
  return (u32)sum ^ (u32)(sum >> 32);
}

u32 mba_pair_sub_fold(u32 xl, u32 xh, u32 yl, u32 yh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 difference = mba_pair_sub(x, y);
  return (u32)difference ^ (u32)(difference >> 32);
}
