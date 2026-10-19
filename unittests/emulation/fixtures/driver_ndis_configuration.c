typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int STATUS;
typedef struct {
  U16 Length, Maximum;
  U16 *Buffer;
} US;
typedef struct {
  U8 Type, Revision;
  U16 Size;
  U32 Padding;
  void *Handle;
  U32 Flags;
} CONFIG;
typedef struct {
  U32 Type, Padding;
  union {
    U32 Integer;
    US String;
  } Data;
} PARAM;
_Static_assert(sizeof(CONFIG) == 24, "x64 configuration object");
_Static_assert(sizeof(PARAM) == 24, "x64 parameter record");
__declspec(dllimport) STATUS NdisOpenConfigurationEx(CONFIG *, void **);
__declspec(dllimport) void NdisReadConfiguration(STATUS *, PARAM **, void *,
                                                 US *, U32);
__declspec(dllimport) void NdisCloseConfiguration(void *);
__declspec(dllimport) void RtlInitUnicodeString(US *, const U16 *);
__declspec(dllimport) int DbgPrint(const char *, ...);
static void Unload(void *D) {
  (void)D;
  DbgPrint("ndis configuration unload passed");
}
STATUS DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  CONFIG C = {0xa9, 1, 20, 0, (void *)0xffffa00000001000ULL, 0};
  void *H = 0, *Other = 0;
  STATUS S;
  PARAM *P = 0, *First = 0, *Predefined = 0;
  US Name;
  if (NdisOpenConfigurationEx(&C, &H) || !H ||
      NdisOpenConfigurationEx(&C, &Other) || H == Other)
    return (STATUS)0xc000000d;
  RtlInitUnicodeString(&Name, L"Answer");
  NdisReadConfiguration(&S, &First, H, &Name, 1);
  if (S || !First || First->Type || First->Data.Integer != 42)
    return (STATUS)0xc000000d;
  for (U32 Type = 0; Type < 5; ++Type) {
    RtlInitUnicodeString(&Name, L"Text");
    NdisReadConfiguration(&S, &P, H, &Name, Type);
    if (S || !P)
      return (STATUS)0xc000000d;
    if (Type < 2) {
      if (P->Type != Type || P->Data.Integer != (Type == 0 ? 42u : 66u))
        return (STATUS)0xc000000d;
    } else if (P->Type != 2 || P->Data.String.Length != 4 ||
               P->Data.String.Maximum != 6 || P->Data.String.Buffer[0] != '4' ||
               P->Data.String.Buffer[1] != '2')
      return (STATUS)0xc000000d;
  }
  RtlInitUnicodeString(&Name, L"Binary");
  NdisReadConfiguration(&S, &P, H, &Name, 0);
  if (S || P->Type != 4 || P->Data.String.Length != 3 ||
      ((U8 *)P->Data.String.Buffer)[2] != 3)
    return (STATUS)0xc000000d;
  RtlInitUnicodeString(&Name, L"Multi");
  NdisReadConfiguration(&S, &P, H, &Name, 0);
  if (S || P->Type != 3 || P->Data.String.Length != 10 ||
      P->Data.String.Buffer[2] != 'b')
    return (STATUS)0xc000000d;
  RtlInitUnicodeString(&Name, L"NdisVersion");
  NdisReadConfiguration(&S, &Predefined, H, &Name, 4);
  if (S || Predefined->Type || Predefined->Data.Integer != 0x60054)
    return (STATUS)0xc000000d;
  RtlInitUnicodeString(&Name, L"Missing");
  P = (PARAM *)0x1234;
  NdisReadConfiguration(&S, &P, H, &Name, 0);
  if ((U32)S != 0xc0000001 || P != (PARAM *)0x1234)
    return (STATUS)0xc000000d;
  NdisCloseConfiguration(H);
  if (Predefined->Data.Integer != 0x60054)
    return (STATUS)0xc000000d;
  RtlInitUnicodeString(&Name, L"Answer");
  NdisReadConfiguration(&S, &P, Other, &Name, 0);
  if (S || P->Data.Integer != 42)
    return (STATUS)0xc000000d;
  NdisCloseConfiguration(Other);
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  DbgPrint("ndis configuration lifetime passed");
  return 0;
}
