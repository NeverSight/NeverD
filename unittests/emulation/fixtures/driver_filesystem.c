//===- driver_filesystem.c - Driver-originated finite file I/O ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U32;
typedef unsigned long long U64;
typedef unsigned short U16;
typedef struct {
  U16 Length, MaximumLength;
  U64 Buffer;
} UnicodeString;
typedef struct {
  U32 Length;
  U64 Root, Name;
  U32 Attributes;
  U64 Security, QoS;
} ObjectAttributes;
typedef struct {
  U64 Status, Information;
} StatusBlock;
#define API __declspec(dllimport)
API U32 ZwCreateFile(U64 *, U32, ObjectAttributes *, StatusBlock *, void *, U32,
                     U32, U32, U32, void *, U32);
API U32 IoCreateFile(U64 *, U32, ObjectAttributes *, StatusBlock *, void *, U32,
                     U32, U32, U32, void *, U32, U32, void *, U32);
API U32 IoCreateFileSpecifyDeviceObjectHint(U64 *, U32, ObjectAttributes *,
                                            StatusBlock *, void *, U32, U32,
                                            U32, U32, void *, U32, U32, void *,
                                            U32, U64);
API U32 ZwQueryObject(U64, U32, void *, U32, U32 *);
API U32 ObReferenceObjectByHandle(U64, U32, U64, U32, U64 *, void *);
API void ObfDereferenceObject(U64);
API U32 ZwClose(U64);
API U32 ZwReadFile(U64, U64, U64, U64, StatusBlock *, void *, U32, U64 *,
                   void *);
API U32 ZwQueryInformationFile(U64, StatusBlock *, void *, U32, U32);
API U32 ZwWriteFile(U64, U64, U64, U64, StatusBlock *, void *, U32, U64 *,
                    void *);
API U32 ZwSetInformationFile(U64, StatusBlock *, void *, U32, U32);
API U32 ZwDeleteFile(ObjectAttributes *);
API U32 ZwFlushBuffersFile(U64, StatusBlock *);
API U32 ZwOpenSymbolicLinkObject(U64 *, U32, ObjectAttributes *);
API U32 ZwQuerySymbolicLinkObject(U64, UnicodeString *, U32 *);
API U32 wcscat_s(U16 *, U64, const U16 *);
API U16 *wcsrchr(const U16 *, U16);
API int tolower(int);
static U64 Retained;
static void unload(void *Driver) {
  (void)Driver;
  ZwClose(Retained);
  Retained = 0;
}
static void string(UnicodeString *S, U16 *Buffer, const char *Text) {
  U32 I;
  for (I = 0; Text[I]; ++I)
    Buffer[I] = (unsigned char)Text[I];
  Buffer[I] = 0;
  S->Length = (U16)(I * 2);
  S->MaximumLength = (U16)((I + 1) * 2);
  S->Buffer = (U64)Buffer;
}
U32 DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  U16 Text[80];
  UnicodeString Name;
  string(&Name, Text, "\\DosDevices\\A:");
  ObjectAttributes Attr = {sizeof(Attr), 0, (U64)&Name, 0x240, 0, 0};
  StatusBlock Io;
  unsigned Success = 0;
  for (unsigned Letter = 'A'; Letter <= 'Z'; ++Letter) {
    Text[12] = (U16)Letter;
    U64 Handle = 0;
    U32 Status =
        ZwCreateFile(&Handle, 0x100000, &Attr, &Io, 0, 0x80, 1, 1, 0x21, 0, 0);
    if (Letter != 'C') {
      if (Status != 0xc0000034 || Handle)
        return 0xc0000001;
      continue;
    }
    if (Status || !Handle || Io.Information)
      return 0xc0000001;
    unsigned char Buffer[1024];
    if (ZwQueryObject(Handle, 1, Buffer, sizeof(Buffer), 0))
      return 0xc0000001;
    UnicodeString *Resolved = (UnicodeString *)Buffer;
    const char *Expected = "\\Device\\NeverDVolumeC";
    for (unsigned I = 0; Expected[I]; ++I)
      if (((U16 *)Resolved->Buffer)[I] != (unsigned char)Expected[I])
        return 0xc0000001;
    const U16 ExpectedLength = Resolved->Length;
    U64 Link = 0;
    if (ZwOpenSymbolicLinkObject(&Link, 0x80000000, &Attr))
      return 0xc0000001;
    UnicodeString Target = {0, 0, 0};
    U32 Needed = 0;
    if (ZwQuerySymbolicLinkObject(Link, &Target, &Needed) != 0xc0000023 ||
        Needed != (U32)ExpectedLength + 2)
      return 0xc0000001;
    Target.MaximumLength = sizeof(Buffer);
    Target.Buffer = (U64)Buffer;
    if (ZwQuerySymbolicLinkObject(Link, &Target, &Needed) ||
        Target.Length != ExpectedLength || ((U16 *)Buffer)[Target.Length / 2])
      return 0xc0000001;
    for (unsigned I = 0; Expected[I]; ++I)
      if (((U16 *)Buffer)[I] != (unsigned char)Expected[I])
        return 0xc0000001;
    if (ZwClose(Link))
      return 0xc0000001;
    U64 File = 0;
    if (ObReferenceObjectByHandle(Handle, 0, 0, 0, &File, 0))
      return 0xc0000001;
    U64 Device = *(U64 *)(File + 8);
    if (!Device || *(U16 *)Device != 3)
      return 0xc0000001;
    ObfDereferenceObject(File);
    if (ZwClose(Handle))
      return 0xc0000001;
    ++Success;
  }
  if (Success != 1)
    return 0xc0000001;
  string(&Name, Text, "\\??\\C:\\data\\");
  const U16 Leaf[] = {'h', 'e', 'l', 'l', 'o', '.', 'b', 'i', 'n', 0};
  volatile int Upper = 'H';
  if (wcscat_s(Text, 80, Leaf) || wcsrchr(Text, '\\') != Text + 11 ||
      tolower(Upper) != 'h')
    return 0xc0000001;
  Name.Length = 0;
  for (unsigned I = 0; Text[I]; ++I)
    Name.Length += 2;
  Name.MaximumLength = Name.Length + 2;
  if (IoCreateFile(&Retained, 0x80000000, &Attr, &Io, 0, 0x80, 1, 1, 0x60, 0, 0,
                   0, 0, 0x100))
    return 0xc0000001;
  U64 FileObject = 0;
  if (ObReferenceObjectByHandle(Retained, 0, 0, 0, &FileObject, 0))
    return 0xc0000001;
  U64 DeviceHint = *(U64 *)(FileObject + 8);
  ObfDereferenceObject(FileObject);
  for (unsigned Hint = 0; Hint < 2; ++Hint) {
    U64 Other = 0;
    if (IoCreateFileSpecifyDeviceObjectHint(&Other, 0x80000000, &Attr, &Io, 0,
                                            0x80, 1, 1, 0x60, 0, 0, 0, 0, 0,
                                            Hint ? DeviceHint : 0) ||
        Io.Information != 1 || ZwClose(Other))
      return 0xc0000001;
  }
  unsigned char Data[8];
  if (ZwReadFile(Retained, 0, 0, 0, &Io, Data, 3, 0, 0) ||
      Io.Information != 3 || Data[0] != 1 || Data[1] != 2 || Data[2] != 3)
    return 0xc0000001;
  if (ZwReadFile(Retained, 0, 0, 0, &Io, Data, 3, 0, 0) ||
      Io.Information != 2 || Data[0] != 4 || Data[1] != 5)
    return 0xc0000001;
  U64 Position = 0;
  if (ZwQueryInformationFile(Retained, &Io, &Position, 8, 14) || Position != 5)
    return 0xc0000001;

  string(&Name, Text, "\\SystemRoot\\created.bin");
  U64 Mutable = 0;
  if (IoCreateFile(&Mutable, 0x130003, &Attr, &Io, 0, 0x80, 7, 2, 0x60, 0, 0, 0,
                   0, 0) ||
      Io.Information != 2)
    return 0xc0000001;
  U64 Offset = 2;
  Data[0] = 0x41;
  Data[1] = 0x42;
  if (ZwWriteFile(Mutable, 0, 0, 0, &Io, Data, 2, &Offset, 0) ||
      Io.Information != 2)
    return 0xc0000001;
  Offset = 6;
  if (ZwSetInformationFile(Mutable, &Io, &Offset, 8, 20))
    return 0xc0000001;
  Offset = 0;
  if (ZwSetInformationFile(Mutable, &Io, &Offset, 8, 14) ||
      ZwReadFile(Mutable, 0, 0, 0, &Io, Data, 8, 0, 0) || Io.Information != 6 ||
      Data[0] || Data[1] || Data[2] != 0x41 || Data[3] != 0x42 || Data[4] ||
      Data[5])
    return 0xc0000001;
  if (ZwFlushBuffersFile(Mutable, &Io) || ZwDeleteFile(&Attr))
    return 0xc0000001;
  U64 Pending = 0;
  if (ZwCreateFile(&Pending, 0x120089, &Attr, &Io, 0, 0x80, 7, 1, 0x60, 0, 0) !=
          0xc0000056 ||
      ZwClose(Mutable) || ZwDeleteFile(&Attr) != 0xc0000034)
    return 0xc0000001;
  *(U64 *)((unsigned char *)Driver + 0x68) = (U64)unload;
  return 0;
}
