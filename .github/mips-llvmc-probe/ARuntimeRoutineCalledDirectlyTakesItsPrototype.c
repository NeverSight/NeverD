#include <stdint.h>


extern int32_t (*_onexit(int32_t (*)(void)))(void);
uint64_t register_handler(uint64_t function);

uint64_t register_handler(uint64_t function) {
    return ((uint64_t)(uintptr_t)_onexit((int32_t (*)(void))(uintptr_t)(function)));
}


int (*_onexit(int (*function)(void)))(void) { return function; }
static int seven(void) { return 7; }
int main(void) {
  return register_handler((uint64_t)(uintptr_t)seven) ==
                 (uint64_t)(uintptr_t)seven
             ? 0
             : 1;
}
