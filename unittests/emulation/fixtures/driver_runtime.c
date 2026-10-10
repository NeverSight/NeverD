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
__declspec(dllimport) void *ExAllocatePool(U32, U64);
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
