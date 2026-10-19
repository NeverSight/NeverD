// Real Win64 stack arguments and balanced named-driver pointer references.
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, Maximum;
  const U16 *Buffer;
} STRING;
__declspec(dllimport) void RtlInitUnicodeString(STRING *, const U16 *);
__declspec(dllimport) int ObReferenceObjectByName(const STRING *, U32, void *,
                                                  U32, void *, unsigned char,
                                                  void *, void **);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) void *IoDriverObjectType;
__declspec(dllimport) void *IoFileObjectType;
__declspec(dllimport) U32 DbgPrint(const char *, ...);
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  void *Found = 0, *Peer = 0;
  if (ObReferenceObjectByName((const STRING *)((char *)Driver + 0x38), 0, 0, 0,
                              IoDriverObjectType, 0, 0, &Found) ||
      Found != Driver)
    return (int)0xc000000d;
  STRING Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"\\DRIVER\\NEVERDDRIVER");
  if (ObReferenceObjectByName(&Name, 0x240, 0, 0, IoDriverObjectType, 0, 0,
                              &Peer) ||
      Peer != Driver)
    return (int)0xc000000d;
  ObfDereferenceObject(Peer);
  ObfDereferenceObject(Found);
  RtlInitUnicodeString(&Name, (const U16 *)L"\\Driver\\pci");
  if (ObReferenceObjectByName(&Name, 0x240, 0, 0x10000000, IoDriverObjectType,
                              0, 0, &Found) ||
      !Found || Found == Driver || *(U16 *)Found != 4 ||
      *(U16 *)((char *)Found + 2) != 0x150 || *(void **)((char *)Found + 8))
    return (int)0xc000000d;
  const STRING *Published = (const STRING *)((char *)Found + 0x38);
  if (Published->Length != Name.Length)
    return (int)0xc000000d;
  for (U32 I = 0; I != Name.Length / 2; ++I)
    if (Published->Buffer[I] != Name.Buffer[I])
      return (int)0xc000000d;
#ifdef WRITE_NAMED_DRIVER
  *(volatile U64 *)((char *)Found + 8) = 0;
#endif
  Peer = (void *)0x11223344;
  if ((U32)ObReferenceObjectByName(&Name, 0x240, 0, 0, IoFileObjectType, 0, 0,
                                   &Peer) != 0xc0000024 ||
      Peer)
    return (int)0xc000000d;
  RtlInitUnicodeString(&Name, (const U16 *)L"\\Driver\\MissingDriver");
  Peer = (void *)0x55667788;
  if ((U32)ObReferenceObjectByName(&Name, 0x240, 0, 0, IoDriverObjectType, 0, 0,
                                   &Peer) != 0xc0000034 ||
      Peer)
    return (int)0xc000000d;
#ifndef LEAK_NAMED_DRIVER
  ObfDereferenceObject(Found);
#endif
  *(void **)((char *)Driver + 0x68) = Unload;
  DbgPrint("driver-objects-pass");
  return 0;
}
