typedef unsigned short W;
typedef unsigned long long U64;
typedef struct {
  W Length, MaximumLength;
  W *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const W *);
__declspec(dllimport) int ZwLoadDriver(STRING *);
__declspec(dllimport) int ZwUnloadDriver(STRING *);
__declspec(dllimport) int DbgPrint(const char *, ...);
static void Unload(void *D) {
  (void)D;
  DbgPrint("parent unload while child retained");
}
int DriverEntry(void *D, void *Path) {
  (void)Path;
  STRING Child;
  RtlInitUnicodeString(&Child,
                       (const W *)L"\\Registry\\Machine\\System\\CurrentControl"
                                  L"Set\\Services\\ServiceChild");
  if (ZwLoadDriver(&Child))
    return (int)0xc000000d;
  if ((unsigned int)ZwUnloadDriver(&Child) != 0xc0000010)
    return (int)0xc000000d;
  DbgPrint("missing child unload rejected");
  ((U64 *)D)[0x68 / 8] = (U64)Unload;
  return 0;
}
