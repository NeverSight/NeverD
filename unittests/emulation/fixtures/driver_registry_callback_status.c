// Verify actual Win64 caller results across blocking, post override and bypass.
typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned long long U64;
typedef struct {
  U16 Length, Maximum;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) int ZwOpenKey(void **, U32, const void *);
__declspec(dllimport) int ZwSetValueKey(void *, const STRING *, U32, U32,
                                        const void *, U32);
__declspec(dllimport) int ZwQueryValueKey(void *, const STRING *, U32, void *,
                                          U32, U32 *);
__declspec(dllimport) int ZwClose(void *);
__declspec(dllimport) int CmRegisterCallback(int (*)(void *, void *, void *),
                                             void *, U64 *);
__declspec(dllimport) int CmUnRegisterCallback(U64);
static U32 Policy, Calls, Errors;
static int Filter(void *Context, void *Kind, void *Information) {
  U64 *Info = Information;
  ++Calls;
  if (Context != &Policy)
    ++Errors;
  if ((U64)Kind == 1) {
    if (Policy == 1)
      return (int)0xc0000022;
    Info[5] = 0x12345678;
    return 0;
  }
  if ((U64)Kind == 16) {
    if (Policy != 2 || Info[4] != 0x12345678 || *(U32 *)(Info + 1))
      ++Errors;
    *(U32 *)(Info + 3) = 0xc0000022;
    return (int)0xc0000503;
  }
  if ((U64)Kind == 8 && Policy == 3) {
    U32 *Output = (U32 *)Info[3];
    Output[0] = 0;
    Output[1] = 4;
    Output[2] = 4;
    Output[3] = 0x11223344;
    *(U32 *)Info[5] = 16;
    return (int)0xc0000503;
  }
  ++Errors;
  return (int)0xc000000d;
}
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const STRING *Path) {
  U64 OA[6] = {48, 0, (U64)Path, 0x240, 0, 0};
  void *Key;
  if (ZwOpenKey(&Key, 0xf003f, OA))
    return (int)0xc000000d;
  U64 Cookie;
  if (CmRegisterCallback(Filter, &Policy, &Cookie))
    return (int)0xc000000d;
  STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"Mode");
  U32 Value = 5;
  Policy = 1;
  if ((U32)ZwSetValueKey(Key, &Name, 0, 4, &Value, 4) != 0xc0000022 ||
      Calls != 1)
    return (int)0xc000000d;
  Policy = 2;
  if ((U32)ZwSetValueKey(Key, &Name, 0, 4, &Value, 4) != 0xc0000022 ||
      Calls != 3)
    return (int)0xc000000d;
  Policy = 3;
  U32 Output[4], Required = 0;
  if (ZwQueryValueKey(Key, &Name, 2, Output, sizeof(Output), &Required) ||
      Calls != 4 || Required != 16 || Output[3] != 0x11223344 || Errors)
    return (int)0xc000000d;
  if (CmUnRegisterCallback(Cookie) || ZwClose(Key))
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
