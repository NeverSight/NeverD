typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API U32 IoGetDeviceProperty(void *, U32, U32, void *, U32 *);
API void *ExAllocatePoolWithTag(U32, U64, U32);
API void ExFreePool(void *);
API U32 DbgPrint(const char *, ...);
static U32 Added;
static U32 AddDevice(void *Driver, void *PDO) {
  (void)Driver;
  U32 Length = 0xa5a5a5a5, Value = 0;
  U16 Short = 0xa5a5;
  if (IoGetDeviceProperty(PDO, 1, 1, &Short, &Length) != 0xc0000023 ||
      Length != 8 || Short != 0xa5a5)
    return 0xc000000d;
  U16 *IDs = ExAllocatePoolWithTag(1, Length, 0x5052444e);
  if (!IDs)
    return 0xc000009a;
  U32 Status = IoGetDeviceProperty(PDO, 0xabcd0001, Length, IDs, &Length);
  U32 Bad = Status || Length != 8 || IDs[0] != 'N' || IDs[1] != 'D' || IDs[2] ||
            IDs[3];
  ExFreePool(IDs);
  if (Bad || IoGetDeviceProperty(PDO, 14, sizeof(Value), &Value, &Length) ||
      Value != 7 || Length != 4 ||
      IoGetDeviceProperty(PDO, 17, sizeof(Value), &Value, &Length) ||
      Value != 42 || Length != 4)
    return 0xc000000d;
  if (IoGetDeviceProperty(PDO, 2, 5, 0, &Length) != 0xc0000034 || Length != 5)
    return 0xc000000d;
  // A filter may inspect a PDO and decline attachment. The configured bus
  // subsequently receives its real START/REMOVE packets and retires the PDO.
  Added = 1;
  DbgPrint("PDO property queries passed");
  return 0;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (Added != 1)
    *(volatile U32 *)0 = 0xbad;
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  void *Extension = *(void **)((char *)Driver + 0x30);
  *(void **)((char *)Extension + 8) = AddDevice;
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
