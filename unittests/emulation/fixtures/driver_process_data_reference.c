typedef unsigned long long U64;
// Deliberately not a guest identity: the model must initialize its own value.
__declspec(dllexport) void *PsInitialSystemProcess =
    (void *)0x1122334455667788ULL;
__declspec(dllexport) U64 UnknownProcessData = 0x8877665544332211ULL;
int DriverEntry(void *Driver, const void *Path) {
  (void)Driver;
  (void)Path;
  return 0;
}
