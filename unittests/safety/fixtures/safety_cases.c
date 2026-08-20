// Rebuildable memory-safety fixtures for the audit and hunt tracks.
//
// The prototypes are declared locally so the sources stay header- and
// fortification-free: every copy lifts as a plain, unguarded call on each
// target, and the same source produces an equivalent PE, ELF, or Mach-O binary
// through the recipes in the Makefile.  The program is analysed, never run, so
// the deliberately unsafe bodies are harmless.

#if defined(_MSC_VER)
#pragma function(strcpy)
#pragma function(strncpy)
#pragma function(memcpy)
#pragma function(strlen)
#pragma function(malloc)
#pragma function(free)
#endif

#if defined(_WIN32)
typedef unsigned long long fixture_size_t;
#else
typedef unsigned long fixture_size_t;
#endif

extern fixture_size_t strlen(const char *);
extern char *strcpy(char *, const char *);
extern char *strncpy(char *, const char *, fixture_size_t);
extern void *memcpy(void *, const void *, fixture_size_t);
extern char *getenv(const char *);
extern void *malloc(fixture_size_t);
extern void free(void *);
extern int puts(const char *);
#if defined(_WIN32)
extern int _read(int, void *, unsigned);
#else
extern long read(int, void *, unsigned long);
#endif

// Hunt: an unguarded copy of attacker input into a fixed stack buffer.
void tainted_stack_overflow(void) {
  char buf[16];
  char *s = getenv("PAYLOAD");
  strcpy(buf, s);
  puts(buf);
}

// Hunt: an attacker-controlled length copied into a fixed heap buffer.
void tainted_heap_overflow(void) {
  char src[256];
  char *dst = (char *)malloc(16);
#if defined(_WIN32)
  int n = _read(0, src, sizeof src);
#else
  long n = read(0, src, sizeof src);
#endif
  memcpy(dst, src, (fixture_size_t)n);
  puts(dst);
  free(dst);
}

// Hunt: a length-bounded copy that stays within the destination.
void bounded_copy_is_safe(void) {
  char buf[64];
  char *s = getenv("PAYLOAD");
  strncpy(buf, s, 63);
  puts(buf);
}

// Hunt: strcpy only runs after a length check against the destination.
void strlen_guarded_copy(void) {
  char buf[64];
  char *s = getenv("PAYLOAD");
  if (s && strlen(s) < sizeof(buf))
    strcpy(buf, s);
  puts(buf);
}

// Audit: an allocation that is neither released nor allowed to escape.
void leaks_memory(void) {
  void *p = malloc(32);
  (void)p;
}

// Audit: the handle is released on one exit and leaked on the other.
void leak_on_one_path(void) {
  void *p = malloc(32);
  if (getenv("EARLY"))
    return;
  free(p);
}

// Audit: a null-checked release is not a leak.
void guarded_free(void) {
  void *p = malloc(32);
  if (p)
    free(p);
}

// Audit: a handle released twice on one path.
void double_frees(void) {
  void *p = malloc(24);
  free(p);
  free(p);
}

// Audit: a handle used after it is released.
void uses_after_free(void) {
  char *p = (char *)malloc(24);
  free(p);
  p[0] = 65;
}

// Audit negative control: a balanced allocate/release pair.
void balanced_alloc(void) {
  void *p = malloc(24);
  free(p);
}

int main(void) {
  tainted_stack_overflow();
  tainted_heap_overflow();
  bounded_copy_is_safe();
  strlen_guarded_copy();
  leaks_memory();
  leak_on_one_path();
  guarded_free();
  double_frees();
  uses_after_free();
  balanced_alloc();
  return 0;
}
