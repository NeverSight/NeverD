// Registration does not dispatch a shutdown IRP. Both explicit cancellation
// and 19041 IoDeleteDevice cancellation must leave a clean unload.
typedef unsigned int U32;
typedef unsigned long long U64;
__declspec(dllimport) int IoCreateDevice(void *, U32, const void *, U32, U32,
                                         unsigned char, void **);
__declspec(dllimport) int IoRegisterShutdownNotification(void *);
__declspec(dllimport) void IoUnregisterShutdownNotification(void *);
__declspec(dllimport) void IoDeleteDevice(void *);
static void *First, *Second;
static U32 ShutdownCalls;
static U32 flags(void *Device) {
  return *(volatile U32 *)((char *)Device + 0x30);
}
static int Shutdown(void *Device, void *Irp) {
  (void)Device;
  (void)Irp;
  ++ShutdownCalls;
  return (int)0xc000000d;
}
static void Unload(void *Driver) {
  (void)Driver;
  if (ShutdownCalls)
    return; // Leaving devices live makes the harness reject the unload.
  IoUnregisterShutdownNotification(Second);
  IoDeleteDevice(Second);
  IoDeleteDevice(First); // Cancels its remaining registration automatically.
}
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  void *Temporary;
  if (IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &First) ||
      IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Second) ||
      IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Temporary))
    return (int)0xc000000d;
  *(void **)((char *)Driver + 0x70 + 0x10 * 8) = Shutdown;
  if (IoRegisterShutdownNotification(First) ||
      IoRegisterShutdownNotification(First) ||
      IoRegisterShutdownNotification(Second) ||
      IoRegisterShutdownNotification(Temporary))
    return (int)0xc000000d;
  if (!(flags(First) & 0x800) || !(flags(Second) & 0x800))
    return (int)0xc000000d;
  IoUnregisterShutdownNotification(First);
  if ((flags(First) & 0x800) || !(flags(Second) & 0x800) || ShutdownCalls)
    return (int)0xc000000d;
  if (IoRegisterShutdownNotification(First))
    return (int)0xc000000d;
  IoDeleteDevice(Temporary);
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
