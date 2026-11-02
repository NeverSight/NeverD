// Synthetic driver imports without debug information. The provider headers
// supply the source ABI, including NDIS's void return and output pointers.
// Regenerate with clang --target=x86_64-pc-windows-msvc -O1
// -fno-stack-protector -c driver_provider_imports.c -o
// driver_provider_imports.obj, and lld-link /entry:DriverEntry /nodefaultlib
// /subsystem:native /dll /timestamp:0 /out:driver_provider_imports.sys
// driver_provider_imports.obj ndis.lib. ndis.lib is a /machine:x64 /lib import
// library for NDIS.sys exporting the three routines declared below. No driver
// is installed or executed.
// For driver_provider_imports_arm64.sys use --target=aarch64-pc-windows-msvc
// and a /machine:arm64 import library, keeping the same source and link flags.

__declspec(dllimport) int NdisOpenConfigurationEx(void *Config, void **Handle);
__declspec(dllimport) void NdisReadConfiguration(int *Status, void **Parameter,
                                                 void *Handle, void *Keyword,
                                                 int Kind);
__declspec(dllimport) void NdisCloseConfiguration(void *Handle);

__declspec(dllexport) int ReadConfiguration(void *Config, void *Keyword) {
  void *Handle = 0;
  int Status = NdisOpenConfigurationEx(Config, &Handle);
  if (Status)
    return Status;
  void *Parameter = 0;
  NdisReadConfiguration(&Status, &Parameter, Handle, Keyword, 2);
  NdisCloseConfiguration(Handle);
  return Status;
}

int DriverEntry(void *Driver, void *Path) {
  (void)Driver;
  (void)Path;
  return 0;
}
