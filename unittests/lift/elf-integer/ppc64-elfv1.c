// Native PPC64 ELFv1 descriptor, TOC, callback and stack parameter fixtures.
typedef unsigned long word;

volatile word counter = 37;

__attribute__((noinline)) word load_counter(word x) { return counter + x; }

typedef word (*callback_type)(word);

callback_type counter_address(void) { return load_counter; }

callback_type volatile selected = load_counter;

word through_global(callback_type replacement, word x) {
  if (replacement)
    selected = replacement;
  return selected(x) + 1;
}

word add_counter(word x) {
  counter += x;
  return counter;
}

word ninth(word a, word b, word c, word d, word e, word f, word g, word h,
           word i) {
  return a + b + c + d + e + f + g + h + i;
}

word after_call(word x) { return load_counter(x) + counter; }

word callback(word (*fn)(word), word x) { return fn(x) + counter; }
