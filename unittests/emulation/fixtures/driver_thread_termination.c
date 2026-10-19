typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char BOOLEAN;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) BOOLEAN PsIsThreadTerminating(void *);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const STRING *);
__declspec(dllimport) int PsCreateSystemThread(void **, U32, void *, void *,
                                               void *, void (*)(void *),
                                               void *);
__declspec(dllimport) int ObReferenceObjectByHandle(void *, U32, void *, U32,
                                                    void **, void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) int ZwClose(void *);
__declspec(dllimport) int KeWaitForSingleObject(void *, U32, U32, U32, void *);
__declspec(dllimport) void PsTerminateSystemThread(U32);
static BOOLEAN (*Query)(void *);
static volatile U32 ChildChecked;
static void Child(void *Context) {
  (void)Context;
  void *Self = PsGetCurrentThread();
  ChildChecked = !PsIsThreadTerminating(Self) && !Query(Self);
  PsTerminateSystemThread(ChildChecked ? 0 : 0xc000000d);
}
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"PsIsThreadTerminating");
  Query = MmGetSystemRoutineAddress(&Name);
  if (!Query || PsIsThreadTerminating(PsGetCurrentThread()) ||
      Query(PsGetCurrentThread()))
    return (int)0xc000000d;
  void *Handle = 0, *Thread = 0;
  if (PsCreateSystemThread(&Handle, 0x10000000, 0, 0, 0, Child, 0) ||
      ObReferenceObjectByHandle(Handle, 0, 0, 0, &Thread, 0))
    return (int)0xc000000d;
  // The retained pointer owns the object even after its last handle closes.
  if (KeWaitForSingleObject(Thread, 0, 0, 0, 0) || !ChildChecked ||
      PsIsThreadTerminating(Thread) != 1 || Query(Thread) != 1 ||
      ZwClose(Handle) || PsIsThreadTerminating(Thread) != 1)
    return (int)0xc000000d;
  ObfDereferenceObject(Thread);
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
