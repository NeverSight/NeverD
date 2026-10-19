// Verify original reference-entry control flow, thread identity and arguments.
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, Maximum;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const STRING *);
__declspec(dllimport) int PsCreateSystemThread(void **, U32, void *, void *,
                                               void *, void (*)(void *),
                                               void *);
__declspec(dllimport) int ZwClose(void *);
__declspec(dllimport) void PsTerminateSystemThread(U32);
__declspec(dllimport) void *PsGetCurrentProcessId(void);
__declspec(dllimport) int ZwQueryInformationThread(void *, U32, void *, U32,
                                                   U32 *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) int ZwOpenKey(void **, U32, const void *);
__declspec(dllimport) int ZwQueryValueKey(void *, const STRING *, U32, void *,
                                          U32, U32 *);
#ifndef REFERENCE_TEST_MODE
#define REFERENCE_TEST_MODE 0
#endif
static void *OriginalEntry;
static volatile U32 Observed;
static void *ConfiguredEntry(const void *Path) {
  U64 Attributes[6] = {48, 0, (U64)Path, 0x240, 0, 0};
  void *Key = 0;
  if (ZwOpenKey(&Key, 1, Attributes))
    return 0;
  STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"NativeStart");
  U32 Value[5], Required = 0;
  int Status = ZwQueryValueKey(Key, &Name, 2, Value, sizeof(Value), &Required);
  ZwClose(Key);
  if (Status || Required != 20 || Value[1] != 11 || Value[2] != 8)
    return 0;
  return (void *)((U64)Value[3] | ((U64)Value[4] << 32));
}
static void Child(void *Context) {
  void *Start = 0;
  U32 Size = 0;
  if (Context == (void *)Child && PsGetCurrentProcessId() == (void *)4 &&
      !ZwQueryInformationThread((void *)~1ULL, 9, &Start, 8, &Size) &&
      Size == 8 && Start == OriginalEntry)
    Observed = 1;
  PsTerminateSystemThread(Observed ? 0 : 0xc000000d);
}
static void Unload(void *Driver) {
  (void)Driver;
  DbgPrint(Observed ? "reference-entry-pass" : "reference-entry-fail");
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  STRING Name;
#if REFERENCE_TEST_MODE == 1
  RtlInitUnicodeString(&Name, (const U16 *)L"ReferenceThreadNonTransfer");
#elif REFERENCE_TEST_MODE == 2
  RtlInitUnicodeString(&Name, (const U16 *)L"ReferenceThreadMemoryTransfer");
#else
  RtlInitUnicodeString(&Name, (const U16 *)L"ReferenceThreadTransfer");
#endif
  char *Export = MmGetSystemRoutineAddress(&Name);
  if (Export)
    OriginalEntry = Export + 1;
  else {
#if REFERENCE_TEST_MODE == 0
    OriginalEntry = ConfiguredEntry(Path);
    if (!OriginalEntry)
      return (int)0xc000000d;
#else
    return (int)0xc000000d;
#endif
  }
#if REFERENCE_TEST_MODE == 5
  OriginalEntry =
      Export; // Export binding must retain priority over this bridge.
#endif
  *(void **)((char *)Driver + 0x68) = Unload;
#if REFERENCE_TEST_MODE == 3
  ((void (*)(void *))OriginalEntry)((void *)Child);
#else
  void *Handle = 0;
  void *Context = (void *)Child;
#if REFERENCE_TEST_MODE == 4
  Context = OriginalEntry; // A real loop must consume the instruction budget.
#elif REFERENCE_TEST_MODE == 6
  Context = Export + 4; // Same JMP shape, but not the registered entry address.
#endif
  if (PsCreateSystemThread(&Handle, 0, 0, 0, 0, (void (*)(void *))OriginalEntry,
                           Context) ||
      ZwClose(Handle))
    return (int)0xc000000d;
#endif
  return 0;
}
