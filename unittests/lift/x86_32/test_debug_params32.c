// Debug signatures whose pointers are four bytes wide on i386: the parameter
// after one arrives in the next stack slot, and the decompiler names it after
// the source parameter there, not as the pointer's second half.

struct Window {
  int width, height;
};

int dp32_sum(const unsigned char *buf, int len, int bias) {
  int s = bias;
  for (int i = 0; i < len; ++i)
    s += buf[i];
  return s;
}

int dp32_area(struct Window *w, int scale) {
  return w->width * w->height * scale;
}
