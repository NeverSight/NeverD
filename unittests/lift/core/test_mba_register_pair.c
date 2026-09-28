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

__attribute__((noinline)) u64 mba_pair_three(u64 x, u64 y, u64 z) {
  u64 parity = x ^ y ^ z;
  u64 majority = (x & y) | (x & z) | (y & z);
  return parity + (majority << 1);
}

__attribute__((noinline)) u64 mba_pair_four(u64 x, u64 y, u64 z, u64 w) {
  u64 parity = x ^ y ^ z;
  u64 majority = (x & y) | (x & z) | (y & z);
  return (parity ^ w) + ((parity & w) << 1) + (majority << 1);
}

__attribute__((noinline)) u64 mba_pair_five(u64 x, u64 y, u64 z, u64 w, u64 v) {
  u64 first_parity = x ^ y ^ z;
  u64 first_majority = (x & y) | (x & z) | (y & z);
  u64 second_parity = first_parity ^ w ^ v;
  u64 second_majority = (first_parity & w) | (first_parity & v) | (w & v);
  return second_parity + (second_majority << 1) + (first_majority << 1);
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

u32 mba_pair_three_fold(u32 xl, u32 xh, u32 yl, u32 yh, u32 zl, u32 zh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 z = ((u64)zh << 32) | zl;
  u64 result = mba_pair_three(x, y, z);
  return (u32)result ^ (u32)(result >> 32);
}

u32 mba_pair_four_fold(u32 xl, u32 xh, u32 yl, u32 yh, u32 zl, u32 zh, u32 wl,
                       u32 wh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 z = ((u64)zh << 32) | zl;
  u64 w = ((u64)wh << 32) | wl;
  u64 result = mba_pair_four(x, y, z, w);
  return (u32)result ^ (u32)(result >> 32);
}

u32 mba_pair_five_fold(u32 xl, u32 xh, u32 yl, u32 yh, u32 zl, u32 zh, u32 wl,
                       u32 wh, u32 vl, u32 vh) {
  u64 x = ((u64)xh << 32) | xl;
  u64 y = ((u64)yh << 32) | yl;
  u64 z = ((u64)zh << 32) | zl;
  u64 w = ((u64)wh << 32) | wl;
  u64 v = ((u64)vh << 32) | vl;
  u64 result = mba_pair_five(x, y, z, w, v);
  return (u32)result ^ (u32)(result >> 32);
}
