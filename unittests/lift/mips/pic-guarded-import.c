/* An optimized conditional call can split the GP setup across a delay slot. */
typedef unsigned long long word;
extern word dispatch(word, word);

word import_probe(word value) { return value ? dispatch(value, 37) + 1 : 11; }
