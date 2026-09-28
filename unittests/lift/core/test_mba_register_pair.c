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

__attribute__((noinline)) u64 mba_pair_affine_add(u64 x, u64 y) {
  u64 parity = x ^ y;
  u64 carry = (x & y) << 1;
  return parity + carry + 0x123456789abcdef0ULL;
}

__attribute__((noinline)) u64 mba_pair_affine_sub(u64 x, u64 y) {
  u64 parity = x ^ ~y;
  u64 carry = (x & ~y) << 1;
  return parity + carry + 1 + 0x123456789abcdef0ULL;
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

u32 mba_pair_affine_add_fold(u32 xl, u32 xh, u32 yl, u32 yh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 result = mba_pair_affine_add(x, y);
  return (u32)result ^ (u32)(result >> 32);
}

u32 mba_pair_affine_sub_fold(u32 xl, u32 xh, u32 yl, u32 yh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 result = mba_pair_affine_sub(x, y);
  return (u32)result ^ (u32)(result >> 32);
}
