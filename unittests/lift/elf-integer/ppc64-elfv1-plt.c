// A PPC64 ELFv1 executable and its separately linked descriptor provider.
typedef unsigned long word;

#ifdef PROVIDER
static volatile word provider_value = 93;
word remote_apply(word x) { return 3 * x + provider_value; }
#ifndef STATIC_CHAIN
word remote_other(word x) { return provider_value - x; }
#endif
#else
static volatile word caller_value = 37;
extern word remote_apply(word);
extern word remote_other(word);

#ifndef ONLY_TWICE
word cross(word x) { return remote_apply(x + 3) + caller_value; }
#endif
#ifndef ONLY_CROSS
word twice(word x) {
  word first = remote_apply(x + 1);
  return remote_other(first + 2) + caller_value;
}
#endif
#endif
