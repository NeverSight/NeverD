typedef unsigned long long u64;

typedef struct {
  u64 first;
  u64 second;
} Pair;

Pair make_external_pair(void) {
  Pair value = {0x1111111111111111ULL, 0x2222222222222222ULL};
  return value;
}

u64 sum_external_varargs(u64 first, ...) { return first; }
