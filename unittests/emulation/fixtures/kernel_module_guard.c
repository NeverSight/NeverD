// Native no-entry module using the same original documented CFG fixture.
#include "driver_guard.c"
__declspec(dllexport) volatile U64 ModuleGuardCount;
__declspec(dllexport) U64 ModuleGuardSelf(void) {
  ++ModuleGuardCount;
  return DriverEntry((void *)1, (void *)2) == 0 ? 308 : 0xbad;
}
__declspec(dllexport) U64 ModuleGuardCall(U64 Address, U64 InvalidStage) {
  ++ModuleGuardCount;
  U64 (*Selected)(U64) = (void *)Address;
  if (InvalidStage == 1)
    Selected = (void *)(Address + 1);
  __asm__ volatile("callq *__guard_check_icall_fptr(%%rip)"
                   :
                   : "c"(Selected)
                   : "memory");
  if (InvalidStage == 2)
    Selected = (void *)(Address + 1);
  return Selected(1);
}
