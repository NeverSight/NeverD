//===- driver_runtime.c - Original Windows runtime integration fixture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercises compiled Win64 variadic calls, dynamically resolved pool
/// allocation, and counted Unicode strings without host or WDK dependencies.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  U16 Length, MaximumLength;
  U32 Padding;
  U16 *Buffer;
} UNICODE_STRING;

typedef void *(*ALLOCATE_POOL)(U64, U64, U32);
typedef void *(*ALLOCATE_LEGACY_POOL)(U32, U64);
typedef NTSTATUS (*QUERY_SYSTEM)(U32, void *, U32, U32 *);
__declspec(dllimport) void *ExAllocatePool(U32, U64);
__declspec(dllimport) NTSTATUS NtQuerySystemInformation(U32, void *, U32,
                                                        U32 *);
__declspec(dllimport) NTSTATUS ZwQuerySystemInformation(U32, void *, U32,
                                                        U32 *);
typedef long long (*QUERY_COUNTER)(long long *);
__declspec(dllimport) long long KeQueryPerformanceCounter(long long *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
__declspec(dllimport) U32 DbgPrintEx(U32, U32, const char *, ...);
__declspec(dllimport) void *MmGetSystemRoutineAddress(const UNICODE_STRING *);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
__declspec(dllimport) void RtlCopyUnicodeString(UNICODE_STRING *,
                                                const UNICODE_STRING *);
__declspec(dllimport) int RtlCompareUnicodeString(const UNICODE_STRING *,
                                                  const UNICODE_STRING *, U8);
__declspec(dllimport) U8 RtlEqualUnicodeString(const UNICODE_STRING *,
                                               const UNICODE_STRING *, U8);

static int same(const char *A, U32 Capacity, const char *B) {
  for (U32 I = 0; I < Capacity; ++I) {
    if (A[I] != B[I])
      return 0;
    if (!A[I])
      return 1;
  }
  return 0;
}

static void *exported(U8 *Base, U32 Size, const char *Name) {
  if (Size < 4096 || *(U16 *)Base != 0x5a4d)
    return 0;
  U32 PE = *(U32 *)(Base + 0x3c);
  if (PE > Size - 0x90 || *(U32 *)(Base + PE) != 0x4550 ||
      *(U16 *)(Base + PE + 0x18) != 0x20b || *(U32 *)(Base + PE + 0x50) != Size)
    return 0;
  U32 RVA = *(U32 *)(Base + PE + 0x88);
  if (RVA > Size - 40)
    return 0;
  U8 *D = Base + RVA;
  U32 Functions = *(U32 *)(D + 20), Count = *(U32 *)(D + 24);
  U32 Addresses = *(U32 *)(D + 28), Names = *(U32 *)(D + 32);
  U32 Ordinals = *(U32 *)(D + 36);
  if (Functions > Size / 4 || Count > Size / 4 ||
      Addresses > Size - Functions * 4 || Names > Size - Count * 4 ||
      Ordinals > Size - Count * 2)
    return 0;
  for (U32 I = 0; I < Count; ++I) {
    U32 N = *(U32 *)(Base + Names + I * 4);
    if (N >= Size || !same((const char *)(Base + N), Size - N, Name))
      continue;
    U32 Ordinal = *(U16 *)(Base + Ordinals + I * 2);
    if (Ordinal >= Functions)
      return 0;
    U32 Entry = *(U32 *)(Base + Addresses + Ordinal * 4);
    return Entry < Size ? Base + Entry : 0;
  }
  return 0;
}

NTSTATUS DriverEntry(void *Driver, const UNICODE_STRING *RegistryPath) {
  (void)Driver;
  (void)RegistryPath;
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"ExAllocatePool2");
  ALLOCATE_POOL Allocate = (ALLOCATE_POOL)MmGetSystemRoutineAddress(&Name);
  if (!Allocate || Allocate != (ALLOCATE_POOL)MmGetSystemRoutineAddress(&Name))
    return (NTSTATUS)0xc000000dU;
  U8 *Pool = (U8 *)Allocate(0x40, 48, 0x74736554);
  if (!Pool || ((U64)Pool & 15))
    return (NTSTATUS)0xc000009aU;
  for (U32 I = 0; I < 48; ++I)
    if (Pool[I])
      return (NTSTATUS)0xc000000dU;
  ExFreePoolWithTag(Pool, 0x74736554);

  RtlInitUnicodeString(&Name, (const U16 *)L"ExAllocatePool");
  ALLOCATE_LEGACY_POOL Legacy =
      (ALLOCATE_LEGACY_POOL)MmGetSystemRoutineAddress(&Name);
  if (!Legacy || Legacy != ExAllocatePool)
    return (NTSTATUS)0xc000000dU;
  Pool = (U8 *)ExAllocatePool(512, 17);
  if (!Pool || ((U64)Pool & 15))
    return (NTSTATUS)0xc000009aU;
  for (U32 I = 0; I < 17; ++I)
    ((volatile U8 *)Pool)[I] = (U8)(I ^ 0x5a);
  for (U32 I = 0; I < 17; ++I)
    if (((volatile U8 *)Pool)[I] != (U8)(I ^ 0x5a))
      return (NTSTATUS)0xc000000dU;
  ExFreePoolWithTag(Pool, 0);
  Pool = (U8 *)Legacy(1, 8);
  if (!Pool)
    return (NTSTATUS)0xc000009aU;
  ExFreePoolWithTag(Pool, 0);

  RtlInitUnicodeString(&Name, (const U16 *)L"NtQuerySystemInformation");
  QUERY_SYSTEM Query = (QUERY_SYSTEM)MmGetSystemRoutineAddress(&Name);
  U32 Need = 0, SameNeed = 0;
  if (Query != NtQuerySystemInformation ||
      (U32)NtQuerySystemInformation(11, 0, 0, &Need) != 0xc0000004U ||
      (U32)ZwQuerySystemInformation(11, 0, 0, &SameNeed) != 0xc0000004U ||
      Need != SameNeed || Need < 8 || Need > 1024 * 1024)
    return (NTSTATUS)0xc000000dU;
  Pool = (U8 *)Legacy(0, Need + 16);
  if (!Pool)
    return (NTSTATUS)0xc000009aU;
  for (U32 I = 0; I < 16; ++I)
    Pool[Need + I] = 0xa7;
  if (Query(11, Pool, Need, &SameNeed) || Need != SameNeed)
    return (NTSTATUS)0xc000000dU;
  U32 Count = *(U32 *)Pool, Seen = 0;
  if (Count != (Need - 8) / 296 || Need != 8 + Count * 296)
    return (NTSTATUS)0xc000000dU;
  for (U32 I = 0; I < Count; ++I) {
    U8 *Row = Pool + 8 + I * 296;
    U8 *Base = *(U8 **)(Row + 16);
    U32 Size = *(U32 *)(Row + 24);
    U16 Offset = *(U16 *)(Row + 38);
    if (Offset >= 256)
      return (NTSTATUS)0xc000000dU;
    const char *File = (const char *)(Row + 40 + Offset);
    if (same(File, 256 - Offset, "ntoskrnl.exe")) {
      if (exported(Base, Size, "ExAllocatePool") != (void *)ExAllocatePool ||
          exported(Base, Size, "NtQuerySystemInformation") != (void *)Query)
        return (NTSTATUS)0xc000000dU;
      Seen |= 1;
    } else if (same(File, 256 - Offset, "hal.dll")) {
      if (exported(Base, Size, "KeQueryPerformanceCounter") !=
          (void *)KeQueryPerformanceCounter)
        return (NTSTATUS)0xc000000dU;
      Seen |= 2;
    }
  }
  if (Seen != 3)
    return (NTSTATUS)0xc000000dU;
  for (U32 I = 0; I < 16; ++I)
    if (Pool[Need + I] != 0xa7)
      return (NTSTATUS)0xc000000dU;
  ExFreePoolWithTag(Pool, 0);

  RtlInitUnicodeString(&Name, (const U16 *)L"KeQueryPerformanceCounter");
  QUERY_COUNTER Counter = (QUERY_COUNTER)MmGetSystemRoutineAddress(&Name);
  if (!Counter || Counter != KeQueryPerformanceCounter)
    return (NTSTATUS)0xc000000dU;
  long long Frequency = 0, OtherFrequency = 0;
  long long First = KeQueryPerformanceCounter(&Frequency);
  long long Second = Counter(&OtherFrequency);
  if (Frequency <= 0 || Frequency != OtherFrequency || First > Second ||
      Counter(0) < Second)
    return (NTSTATUS)0xc000000dU;

  UNICODE_STRING Source;
  U16 Buffer[8] = {'!', '!', '!', '!', '!', '!', '!', '!'};
  UNICODE_STRING Copy = {0, sizeof(Buffer), 0, Buffer};
  RtlInitUnicodeString(&Source, (const U16 *)L"Copied");
  RtlCopyUnicodeString(&Copy, &Source);
  if (Copy.Length != 12 || Buffer[6] != 0 || Buffer[7] != '!' ||
      !RtlEqualUnicodeString(&Source, &Copy, 0) ||
      RtlCompareUnicodeString(&Copy, &Source, 0))
    return (NTSTATUS)0xc000000dU;
  DbgPrint("runtime %d %u %x %X %p %s %c %%", -7, 42U, 0xabU, 0xcdU,
           (void *)0x1234, "text", 'Q');
  DbgPrintEx(0, 0, "%+06d|%#08x|%I64u|%.*s|%wZ", -42, 0xabU,
             0xffffffffffffffffULL, 3, "abcdef", &Copy);
  return 0;
}
