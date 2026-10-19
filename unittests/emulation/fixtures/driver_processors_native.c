typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;
typedef unsigned long long U64;
typedef struct {
  U16 Group;
  U8 Number, Reserved;
} NUMBER;
#define API __declspec(dllimport)
API volatile U8 KeNumberProcessors;
API U64 KeQueryActiveProcessors(void);
API U32 KeQueryActiveProcessorCountEx(U16);
API U32 KeGetCurrentProcessorNumberEx(NUMBER *);
API U32 KeGetProcessorNumberFromIndex(U32, NUMBER *);
API void KeSetSystemGroupAffinityThread(void *, void *);
API void KeRevertToUserGroupAffinityThread(void *);
API U8 KfRaiseIrql(U8);
API void KeLowerIrql(U8);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
#ifdef WRITE_PROCESSOR_DATA
  KeNumberProcessors = 2;
  return 0;
#endif
  if (KeNumberProcessors != 1 || KeQueryActiveProcessors() != 1 ||
      KeQueryActiveProcessorCountEx(0) != 1 ||
      KeQueryActiveProcessorCountEx(0xffff) != 1 ||
      KeQueryActiveProcessorCountEx(1) || KeGetCurrentProcessorNumberEx(0))
    return (int)0xc000000d;
  U8 Old = KfRaiseIrql(2);
  NUMBER N = {0xa5a5, 0xa5, 0xa5};
  if (KeGetCurrentProcessorNumberEx(&N) || N.Group || N.Number || N.Reserved)
    return (int)0xc000000d;
  N.Group = 0xa5a5;
  N.Number = 0xa5;
  N.Reserved = 0xa5;
  if (KeGetProcessorNumberFromIndex(1, &N) != 0xc000000d || N.Group != 0xa5a5 ||
      N.Number != 0xa5 || N.Reserved != 0xa5)
    return (int)0xc000000d;
  if (KeGetProcessorNumberFromIndex(0, &N) || N.Group || N.Number || N.Reserved)
    return (int)0xc000000d;
  KeLowerIrql(Old);
  U64 Affinity[2] = {3, 0}, Previous[2] = {~(U64)0, ~(U64)0}, Nested[2] = {0};
  KeSetSystemGroupAffinityThread(Affinity, Previous);
  if (Affinity[0] != 1 || Affinity[1] || Previous[0] || Previous[1])
    return (int)0xc000000d;
  KeSetSystemGroupAffinityThread(Affinity, Nested);
  if (Nested[0] != 1 || Nested[1])
    return (int)0xc000000d;
  KeRevertToUserGroupAffinityThread(Nested);
  KeRevertToUserGroupAffinityThread(Previous);
  *(void **)((U8 *)Driver + 0x68) = Unload;
  return 0;
}
