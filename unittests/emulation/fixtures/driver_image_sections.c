typedef unsigned int U32;
typedef unsigned short U16;
typedef unsigned char U8;
typedef unsigned long long U64;
#define API __declspec(dllimport)
API U32 ZwCreateFile(U64 *, U32, void *, void *, void *, U32, U32, U32, U32,
                     void *, U32);
API U32 ZwClose(U64);
API U32 ObReferenceObjectByHandle(U64, U32, void *, U8, void **, void *);
API void *IoGetBaseFileSystemDeviceObject(void *);
API U32 ZwCreateSection(U64 *, U32, void *, void *, U32, U32, U64);
API U32 ZwQuerySection(U64, U32, void *, U32, U32 *);
API U32 ZwMapViewOfSection(U64, U64, void **, U64, U64, U64 *, U64 *, U32, U32,
                           U32);
API U32 ZwUnmapViewOfSection(U64, void *);
API U32 ZwQueryVirtualMemory(U64, void *, U32, void *, U64, U64 *);
API U32 PsLookupProcessByProcessId(U64, void **);
API void KeStackAttachProcess(void *, void *);
API void KeUnstackDetachProcess(void *);
API void ObfDereferenceObject(void *);
API void *IoAllocateMdl(void *, U32, U8, U8, void *);
API void MmProbeAndLockPages(void *, U8, U32);
API void *MmMapLockedPagesSpecifyCache(void *, U8, U32, void *, U32, U32);
API void MmUnmapLockedPages(void *, void *);
API void MmUnlockPages(void *);
API void IoFreeMdl(void *);
typedef struct {
  U16 Length, Maximum;
  U32 Padding;
  const U16 *Buffer;
} STRING;
typedef struct {
  U32 Length, Padding;
  U64 Root;
  STRING *Name;
  U32 Flags, Padding2;
  U64 Security, QoS;
} ATTRIBUTES;
static void Unload(void *Driver) { (void)Driver; }
int DriverEntry(void *Driver, const void *Path) {
  (void)Path;
  const U16 FileName[] = L"\\??\\C:\\app\\image.exe";
  STRING Name = {sizeof(FileName) - 2, sizeof(FileName), 0, FileName};
  ATTRIBUTES FileAttributes = {48, 0, 0, &Name, 0x240, 0, 0, 0};
  ATTRIBUTES SectionAttributes = {48, 0, 0, 0, 0x200, 0, 0, 0};
  U64 File = 0, Section = 0, IOStatus[2] = {0};
  if (ZwCreateFile(&File, 0x120089, &FileAttributes, IOStatus, 0, 0, 7, 1, 0x60,
                   0, 0) ||
      ZwCreateSection(&Section, 0xf001f, &SectionAttributes, 0, 2, 0x1000000,
                      File))
    return (int)0xc000000d;
  U64 Basic[3] = {~(U64)0, ~(U64)0, ~(U64)0};
  void *FileObject = 0;
  if (ObReferenceObjectByHandle(File, 0, 0, 0, &FileObject, 0) || !FileObject)
    return (int)0xc000000d;
  void *Device = IoGetBaseFileSystemDeviceObject(FileObject);
  if (!Device || *(void **)((U8 *)FileObject + 8) != Device)
    return (int)0xc000000d;
  U32 Length = 0;
  if (ZwQuerySection(Section, 0, Basic, sizeof(Basic), &Length) || Basic[0] ||
      (U32)Basic[1] != 0x1800000 || (U32)(Basic[1] >> 32) != ~(U32)0 ||
      Basic[2] != 0x3000 || Length != 24 || ZwClose(File))
    return (int)0xc000000d;
  if (IoGetBaseFileSystemDeviceObject(FileObject) != Device)
    return (int)0xc000000d;
  ObfDereferenceObject(FileObject);
  void *Process = 0;
  if (PsLookupProcessByProcessId(100, &Process))
    return (int)0xc000000d;
  U64 State[6] = {0};
  KeStackAttachProcess(Process, State);
  void *Base = 0;
  U64 Size = 0;
  if (ZwMapViewOfSection(Section, ~(U64)0, &Base, 0, 0, 0, &Size, 2, 0, 2) ||
      Base != (void *)0x400000 || Size != 0x3000 || ZwClose(Section))
    return (int)0xc000000d;
  volatile U32 *Data = (volatile U32 *)0x402000;
  void *MDL = IoAllocateMdl((void *)Data, 4096, 0, 0, 0);
  if (!MDL || *Data != 0x31415926)
    return (int)0xc000000d;
  MmProbeAndLockPages(MDL, 1, 0);
  volatile U32 *Alias = MmMapLockedPagesSpecifyCache(MDL, 0, 1, 0, 0, 16);
  if (!Alias || *Alias != 0x31415926)
    return (int)0xc000000d;
  if (__sync_fetch_and_add(Data, 1) != 0x31415926 || *Data != 0x31415927 ||
      *Alias != 0x31415926)
    return (int)0xc000000d;
  U64 Info[6] = {0};
  if (ZwQueryVirtualMemory(~(U64)0, (void *)Data, 0, Info, sizeof(Info), 0) ||
      Info[1] != 0x400000 || (U32)Info[2] != 0x80 ||
      (U32)(Info[4] >> 32) != 4 || (U32)Info[5] != 0x1000000)
    return (int)0xc000000d;
#ifndef LEAK_IMAGE_VIEW
  if (ZwUnmapViewOfSection(~(U64)0, (void *)0x402123))
    return (int)0xc000000d;
#endif
  if (*Alias != 0x31415926)
    return (int)0xc000000d;
  MmUnmapLockedPages((void *)Alias, MDL);
  MmUnlockPages(MDL);
  IoFreeMdl(MDL);
  KeUnstackDetachProcess(State);
  ObfDereferenceObject(Process);
  *(void **)((U8 *)Driver + 0x68) = Unload;
  return 0;
}
