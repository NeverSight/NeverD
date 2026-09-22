//===- driver.c - Original DriverEntry fixtures ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original DriverEntry fixtures.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;

typedef struct {
  U16 Length;
  U16 MaximumLength;
  U32 Padding;
  const U16 *Buffer;
} UNICODE_STRING;

typedef struct {
  void *DriverObject;
  void *AddDevice;
} DRIVER_EXTENSION;

typedef struct {
  U8 Prefix[0x30];
  DRIVER_EXTENSION *DriverExtension;
  U8 Middle[0x30];
  void *DriverUnload;
  void *MajorFunction[28];
} DRIVER_OBJECT;

_Static_assert(sizeof(UNICODE_STRING) == 16, "x64 UNICODE_STRING");
_Static_assert(__builtin_offsetof(DRIVER_OBJECT, DriverUnload) == 0x68,
               "x64 DriverUnload offset");
_Static_assert(__builtin_offsetof(DRIVER_OBJECT, MajorFunction) == 0x70,
               "x64 dispatch table offset");

__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
__declspec(dllimport) void *ExAllocatePoolWithTag(U32, U64, U32);
__declspec(dllimport) void ExFreePoolWithTag(void *, U32);
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER_OBJECT *, U32,
                                              UNICODE_STRING *, U32, U32, U8,
                                              void **);
__declspec(dllimport) NTSTATUS IoCreateSymbolicLink(UNICODE_STRING *,
                                                    UNICODE_STRING *);
__declspec(dllimport) NTSTATUS ZwOpenFile(void *, U32, void *, void *, U32,
                                          U32);

static void Unload(DRIVER_OBJECT *Driver) { (void)Driver; }
static NTSTATUS Dispatch(void *Device, void *Irp) {
  (void)Device;
  (void)Irp;
  return 0;
}
static volatile U64 Counter;

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  // These probes prove that the driver receives x64 WDM initialization inputs.
  if (!Driver || !RegistryPath || !RegistryPath->Buffer ||
      RegistryPath->Length == 0 || !Driver->DriverExtension)
    return (NTSTATUS)0xc000000dU;
#if DRIVER_CASE == 1
  return (NTSTATUS)0xc0000001U;
#elif DRIVER_CASE == 2
  return ZwOpenFile(0, 0, 0, 0, 0, 0);
#elif DRIVER_CASE == 3
  *(volatile U64 *)0x12345000ULL = 0x8877665544332211ULL;
  return 0;
#elif DRIVER_CASE == 4
  for (;;)
    ++Counter;
#elif DRIVER_CASE == 5
  UNICODE_STRING DeviceName, LinkName;
  void *Device = 0;
  RtlInitUnicodeString(&DeviceName, L"\\Device\\NeverDTest");
  RtlInitUnicodeString(&LinkName, L"\\DosDevices\\NeverDTest");
  void *Pool = ExAllocatePoolWithTag(0, 32, 0x44564e54);
  if (!Pool)
    return (NTSTATUS)0xc000009aU;
  *(volatile U64 *)Pool = 0x1122334455667788ULL;
  ExFreePoolWithTag(Pool, 0x44564e54);
  NTSTATUS Status =
      IoCreateDevice(Driver, 24, &DeviceName, 0x22, 0, 0, &Device);
  if (Status < 0)
    return Status;
  Status = IoCreateSymbolicLink(&LinkName, &DeviceName);
  if (Status < 0)
    return Status;
  Driver->DriverUnload = (void *)Unload;
  Driver->MajorFunction[0] = (void *)Dispatch;
  Driver->MajorFunction[14] = (void *)Dispatch;
  return 0;
#elif DRIVER_CASE == 6
  *(volatile U64 *)(void *)DriverEntry = 0;
  return 0;
#elif DRIVER_CASE == 7
  __asm__ volatile("cli");
  return 0;
#elif DRIVER_CASE == 8
  Counter = *(volatile U64 *)((U8 *)Driver + 0x28);
  return 0;
#elif DRIVER_CASE == 9
  U64 Thread;
  __asm__ volatile("movq %%gs:0x188, %0" : "=r"(Thread));
  Counter = Thread;
  return 0;
#elif DRIVER_CASE == 10
  RtlInitUnicodeString((UNICODE_STRING *)0x12345000ULL, L"test");
  return 0;
#elif DRIVER_CASE == 11
  return 0x103; // STATUS_PENDING is not valid asynchronous DriverEntry.
#elif DRIVER_CASE == 12
  // A mapped address does not establish a valid invocation stack. Stop this
  // pivot before interpreting DRIVER_OBJECT bytes as IoCreateDevice arguments.
  __asm__ volatile("lea 0x38(%%rcx), %%rsp\n\t"
                   "jmp *__imp_IoCreateDevice(%%rip)"
                   :
                   : "c"(Driver)
                   : "memory");
  __builtin_unreachable();
#else
  if (Counter != 0)
    return (NTSTATUS)0xc000000dU;
  Counter = 0x12345678;
  Driver->DriverUnload = (void *)Unload;
  Driver->MajorFunction[0] = (void *)Dispatch;
  return 0;
#endif
}
