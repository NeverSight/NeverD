typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} NAME;
__declspec(dllimport) extern unsigned char NlsMbOemCodePageTag;
__declspec(dllimport) void *MmGetSystemRoutineAddress(const NAME *);
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  const U16 Text[] = L"NlsMbOemCodePageTag";
  NAME Name = {sizeof(Text) - 2, sizeof(Text), Text};
  volatile unsigned char *Address = MmGetSystemRoutineAddress(&Name);
  if (Address != &NlsMbOemCodePageTag || *Address > 1)
    return (int)0xc000000d;
#if defined(NLS_WRITE)
  *Address = !*Address;
#elif defined(NLS_CROSS)
  volatile U16 Value = *(volatile U16 *)Address;
  (void)Value;
#elif defined(NLS_EXECUTE)
  ((void (*)(void))(U64)Address)();
#endif
  DbgPrint(*Address ? "OEM multibyte" : "OEM single byte");
  *(void **)((char *)Driver + 0x68) = Unload;
  return 0;
}
