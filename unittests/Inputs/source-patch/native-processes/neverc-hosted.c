
typedef int (*Callback)(int);
__attribute__((noinline)) static int inner(Callback cb, int x) {
  volatile int value = cb(x); return value + 1;
}
int replacement(Callback cb, int x) {
  volatile int value = inner(cb, x); return value + 39;
}