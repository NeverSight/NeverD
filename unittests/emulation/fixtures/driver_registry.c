//===- driver_registry.c - Original registry lifecycle fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Executes registry queries and mutations through the public x64 WDM ABI.
///
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;

typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} UNICODE_STRING;

typedef struct {
  U32 Length;
  void *RootDirectory;
  const UNICODE_STRING *ObjectName;
  U32 Attributes;
  void *SecurityDescriptor;
  void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

typedef struct {
  U8 Prefix[0x68];
  void (*DriverUnload)(void *);
} DRIVER_OBJECT;

typedef struct {
  U32 TitleIndex, Type, DataLength, Value;
} VALUE_INFORMATION;

_Static_assert(sizeof(OBJECT_ATTRIBUTES) == 48, "x64 object attributes");
_Static_assert(__builtin_offsetof(OBJECT_ATTRIBUTES, ObjectName) == 16,
               "x64 name pointer");
_Static_assert(__builtin_offsetof(VALUE_INFORMATION, Value) == 12,
               "partial value data");

__declspec(dllimport) void RtlInitUnicodeString(UNICODE_STRING *, const U16 *);
__declspec(dllimport) NTSTATUS ZwOpenKey(void **, U32, OBJECT_ATTRIBUTES *);
__declspec(dllimport) NTSTATUS ZwCreateKey(void **, U32, OBJECT_ATTRIBUTES *,
                                           U32, UNICODE_STRING *, U32, U32 *);
__declspec(dllimport) NTSTATUS ZwQueryValueKey(void *, UNICODE_STRING *, U32,
                                               void *, U32, U32 *);
__declspec(dllimport) NTSTATUS ZwSetValueKey(void *, UNICODE_STRING *, U32, U32,
                                             const void *, U32);
__declspec(dllimport) NTSTATUS ZwDeleteValueKey(void *, UNICODE_STRING *);
__declspec(dllimport) NTSTATUS ZwDeleteKey(void *);
__declspec(dllimport) NTSTATUS ZwClose(void *);

static void *ServiceKey;
static U32 Mode;

static void Unload(void *Driver) {
  (void)Driver;
  if (Mode != 2)
    ZwClose(ServiceKey);
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver,
                     const UNICODE_STRING *RegistryPath) {
  OBJECT_ATTRIBUTES Attributes = {
      sizeof(Attributes), 0, RegistryPath, 0x240, 0, 0};
  NTSTATUS Status = ZwOpenKey(&ServiceKey, 0x2001f, &Attributes);
  if (Status < 0)
    return Status;
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"Mode");
  U32 Required = 0;
  Status = ZwQueryValueKey(ServiceKey, &Name, 2, 0, 0, &Required);
  if ((U32)Status != 0xc0000023 || Required != sizeof(VALUE_INFORMATION))
    return (NTSTATUS)0xc000000d;
  VALUE_INFORMATION Information;
  Status = ZwQueryValueKey(ServiceKey, &Name, 2, &Information,
                           sizeof(Information), &Required);
  if (Status < 0 || Information.Type != 4 || Information.DataLength != 4)
    return (NTSTATUS)0xc000000d;
  Mode = Information.Value;

  RtlInitUnicodeString(&Name, (const U16 *)L"Observed");
  Status = ZwSetValueKey(ServiceKey, &Name, 0, 4, &Information.Value, 4);
  if (Status < 0)
    return Status;
  if (Mode == 1)
    *(volatile U32 *)0xdead0000 = Mode;

  UNICODE_STRING ChildName;
  RtlInitUnicodeString(&ChildName, (const U16 *)L"Temporary");
  Attributes.RootDirectory = ServiceKey;
  Attributes.ObjectName = &ChildName;
  void *Child = 0;
  U32 Disposition = 0;
  Status = ZwCreateKey(&Child, 0xf003f, &Attributes, 0, 0, 0, &Disposition);
  if (Status < 0 || Disposition != 1)
    return (NTSTATUS)0xc000000d;
  Status = ZwSetValueKey(Child, &Name, 0, 4, &Information.Value, 4);
  if (Status < 0)
    return Status;
  Status = ZwDeleteValueKey(Child, &Name);
  if (Status < 0)
    return Status;
  Status = ZwDeleteKey(Child);
  if (Status < 0)
    return Status;
  Status = ZwClose(Child);
  if (Status < 0)
    return Status;
  Driver->DriverUnload = Unload;
  return 0;
}
