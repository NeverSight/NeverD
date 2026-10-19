typedef unsigned int U32;
typedef struct {
  U32 Length, Options, Canary[2];
} CI;
__declspec(dllimport) int ZwQuerySystemInformation(U32, void *, U32, U32 *);
__declspec(dllimport) int NtQuerySystemInformation(U32, void *, U32, U32 *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U32 Required = 0;
  if ((U32)ZwQuerySystemInformation(103, 0, 0, &Required) != 0xc0000004 ||
      Required != 8)
    return (int)0xc000000d;
  CI B = {7, 0xa5a5a5a5, {0x12345678, 0x9abcdef0}};
  if ((U32)ZwQuerySystemInformation(103, &B, 8, &Required) != 0xc0000004 ||
      Required != 8 || B.Options != 0xa5a5a5a5 || B.Length != 7)
    return (int)0xc000000d;
  B.Length = 8;
  if ((U32)ZwQuerySystemInformation(103, &B, sizeof(B), &Required) !=
          0xc0000004 ||
      Required != 8 || B.Options != 0xa5a5a5a5)
    return (int)0xc000000d;
  if (ZwQuerySystemInformation(103, &B, 8, &Required) || Required != 8 ||
      B.Length != 8 || B.Canary[0] != 0x12345678 || B.Canary[1] != 0x9abcdef0)
    return (int)0xc000000d;
  U32 Observed = B.Options;
  B.Options = ~Observed;
  if (NtQuerySystemInformation(103, &B, 8, 0) || B.Options != Observed)
    return (int)0xc000000d;
  if (ZwQuerySystemInformation(103, &B, 8, &B.Options) || B.Options != 8 ||
      B.Canary[0] != 0x12345678 || B.Canary[1] != 0x9abcdef0)
    return (int)0xc000000d;
  DbgPrint("CI options=%u", Observed);
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
