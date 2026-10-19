// Original fixture: Ex registration in CREATE, actual exit callback and unload.
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  unsigned short Length, MaximumLength;
  U64 Buffer;
} US;
#define API __declspec(dllimport)
API U32 IoCreateDevice(U64, U32, US *, U32, U32, U8, U64 *);
API void IoDeleteDevice(U64);
API void IofCompleteRequest(U64, U8);
API U32 PsSetCreateProcessNotifyRoutineEx(void (*)(U64, U64, U64), U8);
API U64 PsGetProcessId(U64);
API U64 PsGetCurrentProcessId(void);
API U8 KeAreApcsDisabled(void);
API U8 KeAreAllApcsDisabled(void);
static U64 Device;
static U32 Registered, Count, Bad;
static void callback(U64 Process, U64 PID, U64 Info) {
  if (PID != 100 || Info || PsGetProcessId(Process) != PID ||
      PsGetCurrentProcessId() != PID || !KeAreApcsDisabled() ||
      KeAreAllApcsDisabled())
    Bad = 1;
  ++Count;
}
static U32 dispatch(U64 D, U64 IRP) {
  U64 Stack = *(U64 *)(IRP + 0xb8);
  (void)D;
  U32 Status = 0;
  if (*(U8 *)Stack == 0) {
    Status = PsSetCreateProcessNotifyRoutineEx(callback, 0);
    Registered = Status == 0;
  }
  *(U32 *)(IRP + 0x30) = Status;
  *(U64 *)(IRP + 0x38) = 0;
  IofCompleteRequest(IRP, 0);
  return Status;
}
static void unload(U64 Driver) {
  (void)Driver;
  if (!Registered || Bad || Count != 1 ||
      PsSetCreateProcessNotifyRoutineEx(callback, 1))
    *(volatile U32 *)0 = 0xbad;
  IoDeleteDevice(Device);
}
U32 DriverEntry(U64 Driver, U64 Registry) {
  static const unsigned short Text[] = u"\\Device\\NeverDProcessEx";
  US Name = {sizeof(Text) - 2, sizeof(Text), (U64)Text};
  (void)Registry;
  if (PsSetCreateProcessNotifyRoutineEx(callback, 0) ||
      PsSetCreateProcessNotifyRoutineEx(callback, 1))
    return 0xc0ffee01;
  *(U64 *)(Driver + 0x68) = (U64)unload;
  *(U64 *)(Driver + 0x70) = (U64)dispatch;
  *(U64 *)(Driver + 0x70 + 2 * 8) = (U64)dispatch;
  *(U64 *)(Driver + 0x70 + 0x12 * 8) = (U64)dispatch;
  U32 Status = IoCreateDevice(Driver, 0, &Name, 7, 0, 0, &Device);
  if (!Status)
    *(U32 *)(Device + 0x30) &= ~0x80u;
  return Status;
}
