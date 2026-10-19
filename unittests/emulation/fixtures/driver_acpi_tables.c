typedef unsigned int U32;
typedef unsigned char U8;
__declspec(dllimport) int ZwQuerySystemInformation(U32, void *, U32, U32 *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, unsigned long long, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U32 H[32] = {0x41435049, 0, 0, 0xffffffff}, Need = 0;
  if ((U32)ZwQuerySystemInformation(76, H, 16, &Need) != 0xc0000023 ||
      H[3] == 0 || H[3] % 4 || Need != H[3] + 16)
    return (int)0xc000000d;
  U32 *B = ExAllocatePoolWithTag(0, Need, 0x69706341);
  if (!B)
    return (int)0xc000009a;
  B[0] = H[0];
  B[1] = 0;
  B[2] = 0;
  B[3] = 0;
  U32 Status = ZwQuerySystemInformation(76, B, Need, &Need);
  U32 APICs = 0, SSDTs = 0;
  if (!Status && Need == B[3] + 16)
    for (U32 I = 0; I < B[3] / 4; ++I) {
      APICs += B[I + 4] == 0x43495041;
      SSDTs += B[I + 4] == 0x54445353;
    }
  ExFreePoolWithTag(B, 0x69706341);
  if (Status || APICs != 1 || SSDTs != 2)
    return (int)0xc000000d;
  H[1] = 1;
  H[2] = 0x43495041;
  H[3] = 0xffffffff;
  if (ZwQuerySystemInformation(76, H, sizeof(H), &Need) || H[3] != 52 ||
      Need != 68)
    return (int)0xc000000d;
  U8 *T = (U8 *)(H + 4);
  U8 Sum = 0;
  for (U32 I = 0; I < 52; ++I)
    Sum += T[I];
  if (Sum || H[4] != 0x43495041 || H[5] != 52 || T[44] != 0 || T[45] != 8 ||
      *(U32 *)(T + 48) != 1)
    return (int)0xc000000d;
  H[2] = 0x54445353;
  if (ZwQuerySystemInformation(76, H, sizeof(H), &Need) || H[3] != 37 ||
      H[10] != 1 || T[36] != 0xa3)
    return (int)0xc000000d;
  H[2] = 0xffffffff;
  T[0] = 0xa5;
  if ((U32)ZwQuerySystemInformation(76, H, sizeof(H), &Need) != 0xc0000225 ||
      H[3] != sizeof(H) - 16 || Need != sizeof(H) || T[0] != 0xa5)
    return (int)0xc000000d;
  H[1] = 2;
  if ((U32)ZwQuerySystemInformation(76, H, sizeof(H), &Need) != 0xc0000002 ||
      Need != sizeof(H) || T[0] != 0xa5)
    return (int)0xc000000d;
  DbgPrint("ACPI inventory validated");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
