static int local_bias = 9;
int global_value = 5;
const int readonly_value = 11;

__attribute__((noinline)) int i386_add(int x) {
  return x + local_bias + global_value;
}

int (*i386_dispatch)(int) = i386_add;
const int *i386_readonly_dispatch = &readonly_value;

int *i386_global_address(void) { return &global_value; }

__attribute__((noinline)) int i386_call_dispatch(int x) {
  return i386_dispatch(x);
}

static int zero_fill_value;

__attribute__((noinline)) int i386_bss_access(int delta) {
  zero_fill_value += delta;
  return zero_fill_value;
}
