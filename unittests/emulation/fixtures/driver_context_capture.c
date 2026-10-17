//===- driver_context_capture.c - Original caller-state oracle ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete guarded records and machine effects in the checked driver model.
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef void (*Capture)(void *);
__declspec(dllimport) void RtlCaptureContext(void *);
__declspec(dllimport) void RtlInitUnicodeString(void *, const unsigned short *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(void *);
__declspec(dllimport) NTSTATUS IoCreateDevice(void *, U32, void *, U32, U32, U8,
                                              void **);
__declspec(dllimport) void IoDeleteDevice(void *);
__declspec(dllimport) void IofCompleteRequest(void *, U8);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
extern void CaptureDriverContext(Capture, void *, U64 *, U32);
extern void CaptureDriverResume(void);

const U64 CaptureVectorSeeds[32] = {
    0x1122334455667700ULL, 0x8877665544332200ULL, 0x1122334455667701ULL,
    0x8877665544332201ULL, 0x1122334455667702ULL, 0x8877665544332202ULL,
    0x1122334455667703ULL, 0x8877665544332203ULL, 0x1122334455667704ULL,
    0x8877665544332204ULL, 0x1122334455667705ULL, 0x8877665544332205ULL,
    0x1122334455667706ULL, 0x8877665544332206ULL, 0x1122334455667707ULL,
    0x8877665544332207ULL, 0x1122334455667708ULL, 0x8877665544332208ULL,
    0x1122334455667709ULL, 0x8877665544332209ULL, 0x112233445566770aULL,
    0x887766554433220aULL, 0x112233445566770bULL, 0x887766554433220bULL,
    0x112233445566770cULL, 0x887766554433220cULL, 0x112233445566770dULL,
    0x887766554433220dULL, 0x112233445566770eULL, 0x887766554433220eULL,
    0x112233445566770fULL, 0x887766554433220fULL};
static void *Device;
static Capture DynamicCapture;
NTSTATUS DriverEntry(U8 *, void *);
static void *volatile EntryAddress = (void *)DriverEntry;

static void put(U8 *P, U64 V, unsigned Size) {
  // This x64 fixture is little-endian. Each call has a constant 2/4/8-byte
  // size, including unaligned fields; keep the complete oracle without guest
  // loops that reconstruct each byte on a single-stepped native transport.
  __builtin_memcpy(P, &V, Size);
}

static int verify(Capture Function, U8 Fill, U32 MXCSR) {
  // The aligned unsigned words retain all 1296 bytes, including both guards.
  // Byte stores below update their object representations before comparison.
  enum { WordCount = 1296 / sizeof(U64) };
  __declspec(align(16)) U64 AreaWords[WordCount];
  U64 ExpectedWords[WordCount];
  U8 *Area = (U8 *)AreaWords;
  U8 *Expected = (U8 *)ExpectedWords;
  U64 Observed[11] = {0};
  U8 *P = Expected + 32;
  const U64 Pattern = (U64)Fill * 0x0101010101010101ULL;
  // Retain every guard/record word while reducing single-step loop overhead.
#pragma clang loop unroll_count(8)
  for (unsigned I = 0; I < WordCount; ++I)
    AreaWords[I] = ExpectedWords[I] = Pattern;
  Observed[9] = (U64)CaptureDriverResume;
  CaptureDriverContext(Function, Area + 32, Observed, MXCSR);
  if (Observed[1] != Observed[9] || Observed[8] != Observed[0] + 128 ||
      (Observed[0] & 15) || Observed[3] != (U32)Observed[2] ||
      Observed[10] != Observed[2])
    return 0;
  // The wrapper's caller SP is aligned. Restoring the flags scratch computes
  // SP - 8, which is nonzero and cannot carry, overflow or set auxiliary carry.
  U64 SP = Observed[0] - 8;
  unsigned Ones = 0;
#pragma clang loop unroll(full)
  for (unsigned I = 0; I < 8; ++I)
    Ones += (unsigned)((SP >> I) & 1);
  U64 PostFlags =
      (Observed[2] & ~0x8d5ULL) | ((Ones & 1) ? 0 : 4) | ((SP >> 63) ? 128 : 0);
  if (Observed[4] != PostFlags)
    return 0;
  put(P + 48, 0x10000f, 4);
  put(P + 52, MXCSR, 4);
#pragma clang loop unroll(full)
  for (unsigned I = 0; I < 6; ++I)
    put(P + 56 + I * 2, I ? 16 : 8, 2);
  put(P + 68, Observed[2], 4);
#pragma clang loop unroll(full)
  for (unsigned I = 0; I < 16; ++I) {
    U64 Value = 0x12340000 + I;
    if (I == 1)
      Value = (U64)(Area + 32);
    if (I == 4)
      Value = Observed[0];
    if (I == 5)
      Value = Observed[8];
    put(P + 120 + I * 8, Value, 8);
  }
  put(P + 248, Observed[9], 8);
  const U8 *Controls = (const U8 *)Observed;
  put(P + 256, Controls[52] | ((U32)Controls[53] << 8), 2);
  put(P + 258, 0, 4);
  put(P + 280, MXCSR, 4);
#pragma clang loop unroll(full)
  for (unsigned I = 0; I < 16; ++I) {
    put(P + 416 + I * 16, 0x1122334455667700ULL + I, 8);
    put(P + 424 + I * 16, 0x8877665544332200ULL + I, 8);
  }
#pragma clang loop unroll_count(8)
  for (unsigned I = 0; I < WordCount; ++I)
    if (AreaWords[I] != ExpectedWords[I])
      return 0;
  return 1;
}

static int verifyAll(void) {
  return verify(RtlCaptureContext, 0x5a, 0x1f80) &&
         verify(RtlCaptureContext, 0xa5, 0x3f80) &&
         verify(DynamicCapture, 0x5a, 0x1f80) &&
         verify(DynamicCapture, 0xa5, 0x3f80);
}

static NTSTATUS Dispatch(void *Target, U8 *IRP) {
  U8 *Stack = *(U8 **)(IRP + 184);
  U64 Information = 0;
  NTSTATUS Status = Target == Device ? 0 : (NTSTATUS)0xc000000d;
  if (!Status && Stack[0] == 14) {
    U32 Code = *(U32 *)(Stack + 24);
    __declspec(align(16)) U8 Area[1248];
    if (Code == 0x222004)
      DynamicCapture(0);
    else if (Code == 0x222008)
      DynamicCapture(Area + 1);
    else if (!verifyAll())
      Status = (NTSTATUS)0xc000000d;
    if (!Status) {
      **(U32 **)(IRP + 24) = 0x43545831;
      Information = 4;
    }
  }
  *(NTSTATUS *)(IRP + 48) = Status;
  *(U64 *)(IRP + 56) = Information;
  IofCompleteRequest(IRP, 0);
  return Status;
}

static void Unload(void *Driver) {
  (void)Driver;
  if (!verifyAll())
    __builtin_trap();
  IoDeleteDevice(Device);
  DbgPrint("caller context unload complete");
}

NTSTATUS DriverEntry(U8 *Driver, void *RegistryPath) {
  (void)RegistryPath;
  U64 Name[2];
  RtlInitUnicodeString(Name, (const unsigned short *)L"RtlCaptureContext");
  DynamicCapture = (Capture)MmGetSystemRoutineAddress(Name);
  if (EntryAddress != (void *)DriverEntry || !DynamicCapture || !verifyAll())
    return (NTSTATUS)0xc000000d;
  NTSTATUS Status = IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device);
  if (Status < 0)
    return Status;
  *(void **)(Driver + 104) = (void *)Unload;
  for (unsigned I = 0; I < 28; ++I)
    *(void **)(Driver + 112 + I * 8) = (void *)Dispatch;
  return 0;
}
