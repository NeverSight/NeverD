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
