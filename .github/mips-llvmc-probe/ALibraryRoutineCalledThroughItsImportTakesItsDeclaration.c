#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


uint64_t use(uint64_t stream, uint64_t lock, uint64_t name);
extern void EnterCriticalSection(void *);

uint64_t use(uint64_t stream, uint64_t lock, uint64_t name) {
    uint64_t put0;
    uint64_t value2;

    put0 = fputc(0x78, (void *)(uintptr_t)(stream));
    EnterCriticalSection((void *)(uintptr_t)(lock));
    value2 = ((uint64_t)(uintptr_t)getenv((void *)(uintptr_t)(name)));
    free((void *)(uintptr_t)(value2));
    return (put0 + value2);
}


static int entered;
void EnterCriticalSection(void *lock) { entered = lock != 0; }
int main(void) {
  int lock = 0;
  if (use((uint64_t)(uintptr_t)stderr, (uint64_t)(uintptr_t)&lock,
          (uint64_t)(uintptr_t)"NEVERD_UNSET_VARIABLE") != 'x')
    return 1;
  return entered ? 0 : 2;
}
