typedef unsigned short W;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  W Length, MaximumLength;
  W *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const W *);
__declspec(dllimport) int ZwLoadDriver(STRING *);
__declspec(dllimport) int ZwUnloadDriver(STRING *);
__declspec(dllimport) int PsCreateSystemThread(void **, U32, void *, void *,
                                               void *, void *, void *);
__declspec(dllimport) int ZwClose(void *);
__declspec(dllimport) int DbgPrint(const char *, ...);
static STRING Child;
#ifdef USE_THREAD
__declspec(dllimport) int PsSetCreateThreadNotifyRoutine(void *);
__declspec(dllimport) int PsRemoveCreateThreadNotifyRoutine(void *);
static void Notify(void *PID, void *TID, unsigned char Created) {
  (void)PID;
  (void)TID;
  DbgPrint(Created ? "service thread create notification"
                   : "service thread exit notification");
}
#endif
static void Unload(void *D) {
  (void)D;
#ifdef KEEP_CHILD
  if (ZwUnloadDriver(&Child))
    DbgPrint("parent child unload failed");
#endif
#ifdef USE_THREAD
  PsRemoveCreateThreadNotifyRoutine((void *)Notify);
#endif
  DbgPrint("parent service unload executed");
}
int DriverEntry(void *D, void *Path) {
  (void)Path;
  RtlInitUnicodeString(&Child,
                       (const W *)L"\\Registry\\Machine\\System\\CurrentControl"
                                  L"Set\\Services\\ServiceChild");
  if (ZwLoadDriver(&Child))
    return (int)0xc000000d;
  if ((U32)ZwLoadDriver(&Child) != 0xc000010e)
    return (int)0xc000000d;
#if defined(KEEP_CHILD)
  DbgPrint("parent retained child for requests");
#elif defined(USE_THREAD)
  if (PsSetCreateThreadNotifyRoutine((void *)Notify))
    return (int)0xc000000d;
  void *Thread = 0;
  if (PsCreateSystemThread(&Thread, 0x1fffff, 0, 0, 0, (void *)ZwUnloadDriver,
                           &Child))
    return (int)0xc000000d;
  if (ZwClose(Thread))
    return (int)0xc000000d;
  DbgPrint("parent queued service unload thread");
#else
  STRING Failed;
  RtlInitUnicodeString(&Failed,
                       (const W *)L"\\Registry\\Machine\\System\\CurrentControl"
                                  L"Set\\Services\\ServiceFailure");
  if ((U32)ZwLoadDriver(&Failed) != 0xc0000022)
    return (int)0xc000000d;
  if ((U32)ZwUnloadDriver(&Failed) != 0xc0000034)
    return (int)0xc000000d;
  if (ZwUnloadDriver(&Child))
    return (int)0xc000000d;
  if (ZwLoadDriver(&Child))
    return (int)0xc000000d;
  if (ZwUnloadDriver(&Child))
    return (int)0xc000000d;
  DbgPrint("parent service lifecycle passed");
#endif
  ((U64 *)D)[0x68 / 8] = (U64)Unload;
  return 0;
}
