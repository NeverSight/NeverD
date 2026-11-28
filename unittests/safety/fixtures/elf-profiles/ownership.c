extern void *malloc(__SIZE_TYPE__ size);
extern void free(void *memory);

#if RECORD_COMPLETION
volatile int completed;
#endif

__attribute__((noinline)) int sample(void) {
  void *memory = malloc(32);
  if (!memory)
    return 0;
#if RELEASE_ALLOCATION == 2
  free((void *)0);
#elif RELEASE_ALLOCATION
  free(memory);
#endif
#if RECORD_COMPLETION
  completed = 1;
#endif
  return 1;
}

int main(void) {
  return sample();
}
