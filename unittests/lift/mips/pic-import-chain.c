typedef unsigned long long word;

extern word dispatch(word, word);
extern word transform(word, word);

word import_probe(word value) {
  dispatch(value, 37);
  return transform(value, 11) + 1;
}
