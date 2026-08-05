volatile int g_result;

__declspec(noinline) int pe_leaf(int x) { return x * 3 + 1; }

__declspec(noinline) int pe_stacky(int x) {
  volatile int slots[96];
  slots[0] = x;
  slots[95] = pe_leaf(x + 2);
  return slots[0] + slots[95];
}

void mainCRTStartup(void) { g_result = pe_stacky(7); }
