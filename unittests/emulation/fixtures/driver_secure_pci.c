// Observed 19041 zero-argument BOOLEAN ABI, through imports and dynamic lookup.
typedef unsigned short U16;
typedef unsigned char BOOLEAN;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} UNICODE_STRING;
__declspec(dllimport) BOOLEAN VslGetSecurePciEnabled(void);
__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const UNICODE_STRING *);
__declspec(dllimport) unsigned long DbgPrintEx(unsigned long, unsigned long,
                                               const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  BOOLEAN First = VslGetSecurePciEnabled();
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"VslGetSecurePciEnabled");
  BOOLEAN (*Query)(void) = MmGetSystemRoutineAddress(&Name);
  if (!Query || First > 1 || Query() != First ||
      VslGetSecurePciEnabled() != First)
    return (int)0xc000000d;
  DbgPrintEx(0, 0, First ? "secure-pci:enabled" : "secure-pci:disabled");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
