//===- driver_unicode_filesystem.c - Genuine UTF-16 filename operations ---===//
// NeverD Decompiler
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  U64 Buffer;
} US;
typedef struct {
  U32 Length;
  U64 Root, Name;
  U32 Attributes;
  U64 Security, QoS;
} OA;
typedef struct {
  U64 Status, Information;
} IOSB;
#define API __declspec(dllimport)
API U32 ZwCreateFile(U64 *, U32, OA *, IOSB *, void *, U32, U32, U32, U32,
                     void *, U32);
API U32 ZwClose(U64);
API U32 ZwReadFile(U64, U64, U64, U64, IOSB *, void *, U32, U64 *, void *);
API U32 ZwWriteFile(U64, U64, U64, U64, IOSB *, void *, U32, U64 *, void *);
API U32 ZwQueryInformationFile(U64, IOSB *, void *, U32, U32);
API U32 ZwQueryObject(U64, U32, void *, U32, U32 *);
API U32 ZwSetInformationFile(U64, IOSB *, void *, U32, U32);
static U64 Retained;
static void unload(U64 Driver) {
  (void)Driver;
  if (ZwClose(Retained))
    *(volatile U32 *)0 = 0xbad;
}
static U32 length(const U16 *S) {
  U32 N = 0;
  while (S[N])
    ++N;
  return N;
}
static U32 equal(const U16 *A, const U16 *B, U32 N) {
  U32 I;
  for (I = 0; I < N; ++I)
    if (A[I] != B[I])
      return 0;
  return 1;
}
static U32 open(U64 *H, const U16 *Name, U64 Root, U32 Access, U32 Disposition,
                U32 Options) {
  US S = {(U16)(length(Name) * 2), (U16)((length(Name) + 1) * 2), (U64)Name};
  OA O = {48, Root, (U64)&S, 0x240, 0, 0};
  IOSB B;
  return ZwCreateFile(H, Access, &O, &B, 0, 0x80, 7, Disposition, Options, 0,
                      0);
}
U32 DriverEntry(U64 Driver, U64 Registry) {
  static const U16 Missing[] = u"\\??\\Z:\\\u7f51\u7edc\u6e38\u620f";
  static const U16 Name[] = u"\\??\\C:\\\u6e38\u620f\\\u00e9COLE.TXT";
  static const U16 Relative[] = u"\\\u6e38\u620f\\\u00c9cole.txt";
  static const U16 Full[] =
      u"\\Device\\NeverDVolumeC\\\u6e38\u620f\\\u00c9cole.txt";
  static const U16 Alias[] = u"\\GameRoot";
  static const U16 Leaf[] = u"\u00e9COLE.TXT";
  static const U16 New[] = u"\\GameRoot\\\u03c3-new.bin";
  static const U16 Collision[] = u"\\GameRoot\\\u03a3-NEW.BIN";
  U64 H = 0, Dir = 0, Temp = 0, Offset = 0;
  IOSB B;
  U8 Data[3], Write[2] = {8, 9}, Delete = 1;
  U32 Need = 0;
  struct {
    U32 Length;
    U16 Name[256];
  } Info;
  struct {
    US S;
    U16 Name[256];
  } Object;
  (void)Registry;
  *(U64 *)(Driver + 0x68) = (U64)unload;
  if (open(&H, Missing, 0, 0x100001, 1, 0x21) != 0xc000003aU)
    return 0xc0ffee01U;
  if (open(&H, Name, 0, 0x120089, 1, 0x60))
    return 0xc0ffee02U;
  if (ZwReadFile(H, 0, 0, 0, &B, Data, 3, 0, 0) || B.Information != 3 ||
      Data[0] != 1 || Data[1] != 2 || Data[2] != 3)
    return 0xc0ffee03U;
  if (ZwQueryInformationFile(H, &B, &Info, sizeof(Info), 9) ||
      Info.Length != length(Relative) * 2 ||
      B.Information != 4 + length(Relative) * 2 ||
      !equal(Info.Name, Relative, length(Relative)))
    return 0xc0ffee04U;
  if (ZwQueryObject(H, 1, &Object, sizeof(Object), &Need) ||
      Need != 16 + (length(Full) + 1) * 2 ||
      Object.S.Length != length(Full) * 2 ||
      !equal((const U16 *)Object.S.Buffer, Full, length(Full)))
    return 0xc0ffee05U;
  if (ZwClose(H) || open(&Dir, Alias, 0, 0x120089, 1, 0x21) ||
      open(&Retained, Leaf, Dir, 0x120089, 1, 0x60) || ZwClose(Dir))
    return 0xc0ffee06U;
  if (open(&H, New, 0, 0x130003, 2, 0x60))
    return 0xc0ffee07U;
  if (open(&Temp, Collision, 0, 0x130003, 2, 0x60) != 0xc0000035U)
    return 0xc0ffee08U;
  if (ZwWriteFile(H, 0, 0, 0, &B, Write, 2, &Offset, 0) || B.Information != 2)
    return 0xc0ffee09U;
  if (ZwSetInformationFile(H, &B, &Delete, 1, 13) || ZwClose(H))
    return 0xc0ffee0aU;
  if (open(&Temp, Collision, 0, 0x120089, 1, 0x60) != 0xc0000034U)
    return 0xc0ffee0bU;
  return 0;
}
