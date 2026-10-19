// Two-phase class-76 RSMB client, including the sample's 19-byte first call.
typedef unsigned int U32;
typedef unsigned char U8;
__declspec(dllimport) int ZwQuerySystemInformation(U32, void *, U32, U32 *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, unsigned long long, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  U32 First[5] = {0x52534d42, 1, 0, 0, 0xfeedbeef};
  U32 Need = 0;
  if ((U32)ZwQuerySystemInformation(76, First, 19, &Need) != 0xc0000023 ||
      Need != First[3] + 16 || Need < 30 || First[4] != 0xfeedbeef)
    return (int)0xc000000d;
  U32 *B = (U32 *)ExAllocatePoolWithTag(0, Need, 0x46725462);
  if (!B)
    return (int)0xc000009a;
  B[0] = 0x52534d42;
  B[1] = 1;
  B[2] = 0;
  B[3] = 0; // Outer length is authoritative despite this zero.
  U32 Got = 0;
  int Status = ZwQuerySystemInformation(76, B, Need, &Got);
  U8 *Raw = (U8 *)(B + 4);
  U32 At = 8, Records = 0, Identities = 0;
  if (Status || Got != Need || B[3] + 16 != Need ||
      *(U32 *)(Raw + 4) + 8 != B[3] || Raw[1] != 3)
    goto Bad;
  while (At + 6 <= B[3]) {
    U32 Type = Raw[At], Len = Raw[At + 1];
    if (Len < 4 || At + Len + 2 > B[3])
      goto Bad;
    if (Type == 1 && Len >= 27 && Raw[At + 4] && Raw[At + 5])
      ++Identities;
    At += Len;
    while (At + 1 < B[3] && (Raw[At] || Raw[At + 1]))
      ++At;
    At += 2;
    ++Records;
    if (Type == 127)
      break;
  }
  if (At != B[3] || Records < 3 || Identities != 1)
    goto Bad;
  ExFreePoolWithTag(B, 0x46725462);
  First[1] = 0;
  if (ZwQuerySystemInformation(76, First, 20, &Need) || Need != 20 ||
      First[3] != 4 || First[4] != 0)
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
Bad:
  ExFreePoolWithTag(B, 0x46725462);
  return (int)0xc000000d;
}
