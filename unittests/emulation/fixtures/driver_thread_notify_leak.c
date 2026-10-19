typedef unsigned long long U64;
typedef unsigned int U32;
typedef unsigned char U8;
__declspec(dllimport) U32 PsSetCreateThreadNotifyRoutine(void (*)(U64, U64,
                                                                  U8));
static void Notify(U64 P, U64 T, U8 C) {
  (void)P;
  (void)T;
  (void)C;
}
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  *(void **)((char *)Driver + 0x68) = Unload;
  return (int)PsSetCreateThreadNotifyRoutine(Notify);
}
