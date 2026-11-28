typedef unsigned long long u64;

struct Input {
  u64 value;
  int sign;
  unsigned char tag;
};

struct Result {
  u64 first;
  u64 second;
  u64 third;
};

struct Result calc(struct Input value, unsigned seed, ...);
