extern int record_label(const char *text);

__attribute__((noinline)) int read_label(void) {
  return record_label("OK") + 1;
}

int first(void) { return read_label() + 2; }
