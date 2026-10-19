// Exact one-byte class-202 queries, including the sample's zeroed DWORD slot.
typedef unsigned int U32;
__declspec(dllimport) int ZwQuerySystemInformation(U32, void *, U32, U32 *);
__declspec(dllimport) int NtQuerySystemInformation(U32, void *, U32, U32 *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U32 Required = 0xfeedbeef;
  if ((U32)ZwQuerySystemInformation(202, (void *)0, 0, &Required) !=
          0xc0000004 ||
      Required != 1)
    return (int)0xc000000d;
  U32 Observed = 0;
  if (ZwQuerySystemInformation(202, &Observed, 1, &Required) || Required != 1 ||
      Observed > 1)
    return (int)0xc000000d;
  U32 Preserved = 0xa5a5a5a5;
  if ((U32)ZwQuerySystemInformation(202, &Preserved, 2, &Required) !=
          0xc0000004 ||
      Required != 1 || Preserved != 0xa5a5a5a5)
    return (int)0xc000000d;
  U32 Repeated = 0xa5a5a5a5;
  if (NtQuerySystemInformation(202, &Repeated, 1, (void *)0) ||
      (Repeated & 255) != Observed || (Repeated & 0xffffff00) != 0xa5a5a500)
    return (int)0xc000000d;
  DbgPrint("DMA guard enabled=%u", Observed);
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
