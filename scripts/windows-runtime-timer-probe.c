#include <unicorn/unicorn.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
int main(void) {
  uc_engine *engine = 0;
  const uint8_t code[] = {0x90, 0x90};
  LARGE_INTEGER frequency, start, end;
  QueryPerformanceFrequency(&frequency);
  if (uc_open(UC_ARCH_X86, UC_MODE_64, &engine) ||
      uc_mem_map(engine, 0x10000, 4096, UC_PROT_ALL) ||
      uc_mem_write(engine, 0x10000, code, sizeof(code))) return 1;
  for (unsigned round = 0; round != 2; ++round) {
    for (unsigned bounded = 0; bounded != 2; ++bounded) {
      QueryPerformanceCounter(&start);
      for (unsigned i = 0; i != 256; ++i) {
        if (uc_emu_start(engine, 0x10000, 0x10002, bounded ? 1000000 : 0, 0)) return 2;
      }
      QueryPerformanceCounter(&end);
      printf("{\"round\":%u,\"unicorn_timeout_us\":%u,\"calls\":256,\"seconds\":%.9f}\n",
             round, bounded ? 1000000 : 0, (double)(end.QuadPart-start.QuadPart)/frequency.QuadPart);
    }
  }
  return uc_close(engine);
}
