typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
typedef struct {
  U32 Length;
  void *Root;
  NAME *Name;
  U32 Flags;
  void *Security, *QoS;
} ATTR;
#define API __declspec(dllimport)
API U32 ZwOpenKey(void **, U32, const ATTR *);
API U32 ZwEnumerateKey(void *, U32, U32, void *, U32, U32 *);
API U32 ZwClose(void *);
API U32 CmRegisterCallback(U32 (*)(void *, void *, void *), void *, U64 *);
API U32 CmUnRegisterCallback(U64);
API U32 DbgPrint(const char *, ...);
static U32 Pre, Post, Bad;
static void *PreInfo, *Object;
static U32 Filter(void *Context, void *Class, void *Info) {
  if (Context != (void *)0x1234)
    Bad = 1;
  if ((U64)Class == 5) {
    unsigned char *P = Info;
    void *Key = *(void **)P;
    if (!Key || (Object && Object != Key) || *(U32 *)(P + 8) > 2 ||
        *(U32 *)(P + 12) > 2 || !*(void **)(P + 32))
      Bad = 1;
    Object = Key;
    PreInfo = Info;
    *(void **)(P + 40) = (void *)0x5678;
    ++Pre;
  } else if ((U64)Class == 20) {
    unsigned char *P = Info;
    if (*(void **)(P + 16) != PreInfo || *(void **)(P + 32) != (void *)0x5678)
      Bad = 1;
    ++Post;
  } else
    Bad = 1;
  return 0;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (Bad || Pre != 9 || Post != 9)
    *(volatile U32 *)0 = 0xbad;
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  const U16 Text[] = L"\\Registry\\Machine\\Software\\NeverDEnum";
  NAME Name = {sizeof(Text) - 2, sizeof(Text), Text};
  ATTR Attr = {sizeof(ATTR), 0, &Name, 0x240, 0, 0};
  void *Key = 0;
  U64 Cookie;
  if (ZwOpenKey(&Key, 0x20019, &Attr) ||
      CmRegisterCallback(Filter, (void *)0x1234, &Cookie))
    return (int)0xc000000d;
  U64 Buffer[8];
  U32 Length;
  for (U32 I = 0; I < 2; ++I) {
    U32 Required = I ? 26 : 24;
    if (ZwEnumerateKey(Key, I, 0, 0, 0, &Length) != 0xc0000023 ||
        Length != Required)
      return (int)0xc000000d;
    Buffer[2] = ~(U64)0;
    if (ZwEnumerateKey(Key, I, 0, Buffer, 17, &Length) != 0x80000005 ||
        ((unsigned char *)Buffer)[16] || ((unsigned char *)Buffer)[17] != 0xff)
      return (int)0xc000000d;
    if (ZwEnumerateKey(Key, I, 0, Buffer, sizeof(Buffer), &Length) ||
        Length != Required ||
        Buffer[0] != (I ? 0x1122334455667788 : 0x8000000000000001) ||
        ((U16 *)((char *)Buffer + 16))[0] != (I ? 'A' : 'Z'))
      return (int)0xc000000d;
  }
  Length = 0xa5a5a5a5;
  if (ZwEnumerateKey(Key, 2, 0, 0, 0, &Length) != 0x8000001a ||
      Length != 0xa5a5a5a5)
    return (int)0xc000000d;
  if (ZwEnumerateKey(Key, 0, 1, Buffer, sizeof(Buffer), &Length) ||
      Length != 32 || ((U32 *)Buffer)[3] != 0xffffffff || ((U32 *)Buffer)[4] ||
      ((U32 *)Buffer)[5] != 8)
    return (int)0xc000000d;
  if (ZwEnumerateKey(Key, 1, 2, Buffer, sizeof(Buffer), &Length) ||
      Length != 44 || ((U32 *)Buffer)[8] != 2 || ((U32 *)Buffer)[9] != 8 ||
      ((U32 *)Buffer)[10] != 4)
    return (int)0xc000000d;
  if (CmUnRegisterCallback(Cookie) || ZwClose(Key) || Bad || Pre != 9 ||
      Post != 9)
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x68) = Unload;
  DbgPrint("registry enumeration and filters passed");
  return 0;
}
