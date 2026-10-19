// A retained metadata pointer does not keep the DriverEntry stack alive.
// It must remain the original pointer and must not prevent deregistration.
typedef unsigned long long U64;
__declspec(dllimport) unsigned char KeRegisterBugCheckReasonCallback(
    void *, void (*)(unsigned, void *, void *, unsigned), unsigned, char *);
__declspec(dllimport) unsigned char KeDeregisterBugCheckReasonCallback(void *);
static U64 Record[6];
static unsigned Calls;
static void Reason(unsigned Reason, void *R, void *Data, unsigned Size) {
  (void)Reason;
  (void)R;
  (void)Data;
  (void)Size;
  ++Calls;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (Calls)
    return; // Retained registration makes a spurious callback fail unload.
  KeDeregisterBugCheckReasonCallback(Record);
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  char Name[] = "BorrowedName";
  if (!KeRegisterBugCheckReasonCallback(Record, Reason, 2, Name))
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
