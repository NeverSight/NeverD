// Original x64 registry client: close differs observably from whole-hive flush.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int STATUS;
typedef struct {
  U16 Length, Maximum;
  const U16 *Buffer;
} US;
typedef struct {
  U32 Size;
  void *Root;
  US *Name;
  U32 Flags;
  void *Security, *Qos;
} OA;
__declspec(dllimport) void RtlInitUnicodeString(US *, const U16 *);
__declspec(dllimport) STATUS ZwOpenKey(void **, U32, OA *);
__declspec(dllimport) STATUS ZwSetValueKey(void *, US *, U32, U32, const void *,
                                           U32);
__declspec(dllimport) STATUS ZwFlushKey(void *);
__declspec(dllimport) STATUS ZwClose(void *);
__declspec(dllimport) STATUS CmRegisterCallback(STATUS (*)(void *, U64, void *),
                                                void *, U64 *);
__declspec(dllimport) STATUS CmUnRegisterCallback(U64);
__declspec(dllimport) int DbgPrint(const char *, ...);
#define AT(P, O, T) (*(T *)((U8 *)(P) + (O)))
static U32 Failed, Pre, Post;
static void *PreInfo, *KeyObject;
static void Check(U32 Good) {
  if (!Good)
    ++Failed;
}
static STATUS Callback(void *Context, U64 Class, void *Info) {
  Check(Context == &Failed);
  if (Class == 30) {
    ++Pre;
    PreInfo = Info;
    KeyObject = AT(Info, 0, void *);
    AT(Info, 8, U64) = 0x12345678;
  }
  if (Class == 31) {
    ++Post;
    Check(AT(Info, 0, void *) == KeyObject && AT(Info, 8, U32) == 0 &&
          AT(Info, 16, void *) == PreInfo && AT(Info, 32, U64) == 0x12345678);
  }
  return 0;
}
static void *Open(const U16 *Name, U32 Access) {
  US String;
  void *H = 0;
  RtlInitUnicodeString(&String, Name);
  OA Attr = {sizeof(OA), 0, &String, 0x240, 0, 0};
  Check(ZwOpenKey(&H, Access, &Attr) == 0);
  return H;
}
static void Set(void *H, U32 Value) {
  US Name;
  RtlInitUnicodeString(&Name, L"Mode");
  Check(ZwSetValueKey(H, &Name, 0, 4, &Value, 4) == 0);
}
static void Unload(void *Driver) {
  (void)Driver;
  DbgPrint("registry hive unload passed");
}
STATUS DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  void *A = Open(L"\\Registry\\Machine\\Software\\A", 0xf003f);
  void *B = Open(L"\\Registry\\Machine\\Software\\B", 0xf003f);
  void *C = Open(L"\\Registry\\Machine\\System\\C", 0xf003f);
  void *Reader = Open(L"\\Registry\\Machine\\Software\\A", 0);
  U64 Cookie = 0;
  Set(A, 10);
  Set(B, 20);
  Set(C, 30);
  Check(CmRegisterCallback(Callback, &Failed, &Cookie) == 0);
  Check(ZwFlushKey(Reader) == 0);
  Check(Pre == 1 && Post == 1);
  Check(CmUnRegisterCallback(Cookie) == 0);
  Set(B, 99);
  Check(ZwClose(A) == 0);
  Check(ZwClose(B) == 0);
  Check(ZwClose(C) == 0);
  Check(ZwClose(Reader) == 0);
  AT(Driver, 0x68, void *) = (void *)Unload;
  if (Failed)
    return (STATUS)0xc000000d;
  DbgPrint("registry hive commit passed");
  return 0;
}
