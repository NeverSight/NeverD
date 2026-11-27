#define _GNU_SOURCE
#include <asm/ldt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef void Copy(unsigned, unsigned, unsigned, unsigned, unsigned);
extern Copy byte16, word16, dword16, byte32, word32, dword32;
static unsigned char memory[65536] __attribute__((aligned(65536)));

static void copy_elements(unsigned char *bytes, int src, int dst, int count,
                          int width, int direction) {
  while (count-- > 0) {
    unsigned char element[4];
    memcpy(element, bytes + src, width);
    memcpy(bytes + dst, element, width);
    src += direction * width;
    dst += direction * width;
  }
}

int main(void) {
  struct user_desc segment = {.entry_number = (unsigned)-1,
                              .base_addr = (unsigned)(uintptr_t)memory,
                              .limit = sizeof(memory) - 1,
                              .seg_32bit = 1, .useable = 1};
  if (syscall(SYS_set_thread_area, &segment) < 0) {
    perror("set_thread_area");
    return 2;
  }
  unsigned selector = segment.entry_number * 8 + 3;
  Copy *copies[2][3] = {{byte16, word16, dword16}, {byte32, word32, dword32}};
  const int distances[] = {-4, -1, 0, 1, 4};
  puts("address,width,distance,backward,element_differences");
  for (int a = 0; a != 2; ++a)
    for (int w = 0; w != 3; ++w)
      for (unsigned d = 0; d != sizeof(distances)/sizeof(distances[0]); ++d)
        for (int backward = 0; backward != 2; ++backward) {
          unsigned char expectation[256];
          for (unsigned i = 0; i != sizeof(expectation); ++i)
            expectation[i] = memory[0xc000 + i] = i * 37 + 11;
          copy_elements(expectation, 128, 128 + distances[d], 8, 1 << w,
                        backward ? -1 : 1);
          unsigned upper = a ? 0 : 0xabcd0000u;
          copies[a][w](selector, upper | 0xc080,
                       upper | (unsigned)(0xc080 + distances[d]), 8, backward);
          unsigned differences = 0;
          for (unsigned i = 0; i != sizeof(expectation); ++i)
            differences += expectation[i] != memory[0xc000 + i];
          printf("%d,%d,%d,%d,%u\n", a ? 32 : 16, 1 << w, distances[d],
                 backward, differences);
        }
  return 0;
}
