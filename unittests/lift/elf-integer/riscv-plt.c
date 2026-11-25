// Rebuild the caller while keeping both independently linked providers.
typedef unsigned long word;

#if defined(FIRST_PROVIDER)
word remote_first(word x) { return 3 * x + 17; }
__attribute__((naked)) void fixture_resolver(void) { __asm__("jr t3"); }
#elif defined(SECOND_PROVIDER)
word remote_second(word x) { return 93 - x; }
#else
extern word remote_first(word);
extern word remote_second(word);
word call_remote(word x) {
#if defined(TAIL_CALL)
  return remote_second(x + 3);
#else
  return remote_second(remote_first(x + 3) + 2) + 5;
#endif
}
#endif
