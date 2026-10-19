// Original client of independently scoped FLTMGR imports.
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
  U32 Size;
  void *Root;
  US *Name;
  U32 Flags;
  void *Security, *Qos;
} OA;
typedef struct {
  U32 Status, Padding;
  U64 Information;
} IOSB;
typedef struct {
  U16 Size, Parsed;
  U32 Format;
  US Name, Volume, Share, Extension, Stream, Final, Parent;
} NAMEINFO;
_Static_assert(sizeof(NAMEINFO) == 120, "x64 FLT_FILE_NAME_INFORMATION");
__declspec(dllimport) void RtlInitUnicodeString(US *, const U16 *);
__declspec(dllimport) STATUS ZwCreateFile(void **, U32, OA *, IOSB *, void *,
                                          U32, U32, U32, U32, void *, U32);
__declspec(dllimport) STATUS ObReferenceObjectByHandle(void *, U32, void *, U8,
                                                       void **, void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) STATUS ZwClose(void *);
__declspec(dllimport) STATUS FltGetFileNameInformationUnsafe(void *, void *,
                                                             U32, NAMEINFO **);
__declspec(dllimport) STATUS FltParseFileNameInformation(NAMEINFO *);
__declspec(dllimport) void FltReferenceFileNameInformation(NAMEINFO *);
__declspec(dllimport) void FltReleaseFileNameInformation(NAMEINFO *);
__declspec(dllimport) int DbgPrint(const char *, ...);
static void Unload(void *D) {
  (void)D;
  DbgPrint("filter name unload passed");
}
STATUS DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  US Path;
  RtlInitUnicodeString(&Path, L"\\??\\C:\\dir\\file.txt");
  OA Attr = {sizeof(OA), 0, &Path, 0x240, 0, 0};
  IOSB Io;
  void *Handle = 0, *Object = 0;
  NAMEINFO *First = 0, *Second = 0, *Fresh = 0;
  if (ZwCreateFile(&Handle, 0x100001, &Attr, &Io, 0, 0, 7, 1, 0x20, 0, 0) ||
      ObReferenceObjectByHandle(Handle, 0, 0, 0, &Object, 0))
    return (STATUS)0xc000000d;
  if ((U32)FltGetFileNameInformationUnsafe(Object, 0, 0x201, &First) !=
          0xc01c0018 ||
      First)
    return (STATUS)0xc000000d;
  if (FltGetFileNameInformationUnsafe(Object, 0, 0x101, &First) || !First ||
      First->Size != 120 || First->Parsed || First->Format != 1 ||
      First->Name.Length != 72 || First->Volume.Length != 46)
    return (STATUS)0xc000000d;
  if (FltParseFileNameInformation(First) || First->Parsed != 15 ||
      First->Extension.Length != 6 || First->Extension.Buffer[0] != 't' ||
      First->Final.Length != 16 || First->Parent.Length != 10)
    return (STATUS)0xc000000d;
  if (FltGetFileNameInformationUnsafe(Object, 0, 0x201, &Second) ||
      Second != First)
    return (STATUS)0xc000000d;
  FltReferenceFileNameInformation(First);
  FltReleaseFileNameInformation(First);
  FltReleaseFileNameInformation(Second);
  if (FltGetFileNameInformationUnsafe(Object, 0, 0x301, &Fresh) ||
      Fresh == First)
    return (STATUS)0xc000000d;
  FltReleaseFileNameInformation(Fresh);
  if (ZwClose(Handle) ||
      (U32)FltGetFileNameInformationUnsafe(Object, 0, 0x101, &Fresh) !=
          0xc01c0005 ||
      First->Extension.Buffer[2] != 't')
    return (STATUS)0xc000000d;
  FltReleaseFileNameInformation(First);
  ObfDereferenceObject(Object);
  *(void **)((U8 *)Driver + 0x68) = (void *)Unload;
  DbgPrint("filter name lifetime passed");
  return 0;
}
