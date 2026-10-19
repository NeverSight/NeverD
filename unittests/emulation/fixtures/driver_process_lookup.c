// Original client of public process identity and reference-count DDIs.
typedef unsigned long long U64;
__declspec(dllimport) int PsLookupProcessByProcessId(void *, void **);
__declspec(dllimport) void *PsGetProcessId(void *);
__declspec(dllimport) void *IoGetCurrentProcess(void);
__declspec(dllimport) void ObfDereferenceObject(void *);
static void *Held;
static void Unload(void *Driver) {
  (void)Driver;
  ObfDereferenceObject(Held);
}
int DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  void *First = (void *)0;
  void *Second = (void *)0;
  void *Missing = (void *)0x12345678;
  if (PsLookupProcessByProcessId((void *)4, &First) ||
      PsLookupProcessByProcessId((void *)4, &Second))
    return (int)0xc0000001;
  if (First != Second || First != IoGetCurrentProcess() ||
      PsGetProcessId(First) != (void *)4 ||
      PsLookupProcessByProcessId((void *)0x1234, &Missing) != (int)0xc000000b ||
      Missing != (void *)0x12345678)
    return (int)0xc000000d;
  Held = First;
  ObfDereferenceObject(Second);
  *(void (**)(void *))((unsigned char *)Driver + 0x68) = Unload;
  return 0;
}
