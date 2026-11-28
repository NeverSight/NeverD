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

#ifdef CHECK_LARGE_FRAME
void fill_frame(unsigned char *frame, unsigned size, unsigned char value);
#endif
