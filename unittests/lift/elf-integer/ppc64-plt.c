// A PowerPC64 caller and its separately linked provider, each with its own TOC.
typedef unsigned long word;

#ifdef PROVIDER
#ifdef TEST_ELFV2_LOCALENTRY
word remote_leaf(word x) { return 3 * x + 93; }
#endif
static volatile word provider_value = 93;
word remote_apply(word x) { return 3 * x + provider_value; }
#ifndef STATIC_CHAIN
word remote_other(word x) { return provider_value - x; }
#endif
#elif defined(TEST_ELFV2_LOCALENTRY)
extern word remote_leaf(word);
static volatile word caller_value = 37;
word leaf_cross(word x) { return remote_leaf(x + 3) + caller_value; }
#else
static volatile word caller_value = 37;
extern word remote_apply(word);
extern word remote_other(word);

#ifdef TEST_ELFV2_CALLBACK
word callback_cross(word (*callback)(word), word x) {
  return callback(x + 3) + caller_value;
}
#endif
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
