typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned char U8;

#if defined(PROBE_DLL)
#pragma section(".probe", read, write)
__declspec(allocate(".probe")) volatile U8 ProbeData[8192] = {
    [0x100] = 0x4a, [0xfff] = 0x5c, [0x1000] = 0x7b, [0x1fff] = 0x6d};
int dllEntry(void *Base, U32 Reason, void *Reserved) { return 1; }
#else
#ifndef PROBE_LOAD_FLAGS
#define PROBE_LOAD_FLAGS 0
#endif
__declspec(dllimport) void *LoadLibraryExA(const char *, void *, U32);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) U64 VirtualQuery(const void *, void *, U64);
__declspec(dllimport) int ReadProcessMemory(void *, const void *, void *, U64,
                                            U64 *);
__declspec(dllimport) void *GetCurrentProcess(void);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) __declspec(noreturn) void ExitProcess(U32);

struct MemoryInfo {
  void *Base, *Allocation;
  U32 AllocationProtection, Padding;
  U64 Size;
  U32 State, Protection, Type, Padding2;
};
static char Line[1024];
static U32 Length;
static void text(const char *Value) {
  while (*Value)
    Line[Length++] = *Value++;
}
static void hex(const char *Key, U64 Value) {
  text(Key);
  text("=0x");
  for (int Shift = 60; Shift >= 0; Shift -= 4) {
    U32 Digit = (U32)(Value >> Shift) & 15;
    Line[Length++] = (char)(Digit < 10 ? '0' + Digit : 'a' + Digit - 10);
  }
  text(" ");
}
static void emit(void) {
  U32 Written;
  text("\n");
  WriteFile(GetStdHandle((U32)-11), Line, Length, &Written, 0);
  Length = 0;
}
static U32 word32(const U8 *Bytes) {
  return (U32)Bytes[0] | (U32)Bytes[1] << 8 | (U32)Bytes[2] << 16 |
         (U32)Bytes[3] << 24;
}
static U32 word16(const U8 *Bytes) {
  return (U32)Bytes[0] | (U32)Bytes[1] << 8;
}
void hostEntry(void) {
  static const char *Cases[] = {
      "page-full.dll",         "page-raw-tail.dll", "page-one-page.dll",
      "page-zero-virtual.dll", "large-full.dll",    "large-raw-tail.dll",
      "large-one-page.dll",    "large-bss.dll",     "large-raw-padding.dll"};
  static const U32 Offsets[] = {0,      0x100,  0xfff, 0x1000,
                                0x1fff, 0x2000, 0xffff};
  for (U32 Case = 0; Case != sizeof(Cases) / sizeof(Cases[0]); ++Case) {
    U8 *Base = LoadLibraryExA(Cases[Case], 0, PROBE_LOAD_FLAGS);
    text("case=");
    text(Cases[Case]);
    text(" ");
    hex("loaded", Base != 0);
    if (!Base) {
      hex("error", GetLastError());
      emit();
      continue;
    }
    const U8 *NT = Base + word32(Base + 0x3c);
    const U8 *Section = NT + 24 + word16(NT + 20);
    const U32 Count = word16(NT + 6);
    U32 Found = 0;
    for (U32 I = 0; I != Count; ++I, Section += 40)
      if (Section[0] == '.' && Section[1] == 'p' && Section[2] == 'r' &&
          Section[3] == 'o' && Section[4] == 'b' && Section[5] == 'e' &&
          !Section[6]) {
        Found = 1;
        break;
      }
    hex("found", Found);
    if (!Found) {
      emit();
      FreeLibrary(Base);
      continue;
    }
    const U32 RVA = word32(Section + 12);
    hex("base", (U64)Base);
    hex("rva", RVA);
    hex("virtual_size", word32(Section + 8));
    hex("raw_size", word32(Section + 16));
    hex("section_alignment", word32(NT + 24 + 32));
    hex("file_alignment", word32(NT + 24 + 36));
    emit();
    for (U32 I = 0; I != sizeof(Offsets) / sizeof(Offsets[0]); ++I) {
      struct MemoryInfo Info;
      U8 *Address = Base + RVA + Offsets[I];
      U64 Query = VirtualQuery(Address, &Info, sizeof(Info));
      U8 Byte = 0xcc;
      U64 Read = 0;
      U32 Success =
          ReadProcessMemory(GetCurrentProcess(), Address, &Byte, 1, &Read);
      U32 Error = Success ? 0 : GetLastError();
      text("case=");
      text(Cases[Case]);
      text(" ");
      hex("offset", Offsets[I]);
      hex("query", Query);
      if (Query) {
        hex("region_rva", (U64)Info.Base - (U64)Base);
        hex("region_size", Info.Size);
        hex("state", Info.State);
        hex("protection", Info.Protection);
      }
      hex("read", Success);
      hex("count", Read);
      hex("byte", Byte);
      hex("error", Error);
      emit();
    }
    FreeLibrary(Base);
  }
  ExitProcess(0);
}
#endif
