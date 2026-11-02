//===- linux_files.c - Original Linux and Android file callers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
typedef long I64;
typedef unsigned int U32;
#if defined(__aarch64__)
static const U64 UserLimit = 0x0001000000000000UL;
enum {
  FaccessAt = 48,
  MkdirAt = 34,
  UnlinkAt = 35,
  RenameAt = 38,
  OpenAt = 56,
  ReadlinkAt = 78,
  Read = 63,
  Close = 57,
  Seek = 62,
  Fstat = 80,
  FstatAt = 79,
  StatFS = 43,
  FstatFS = 44,
  Write = 64,
  Protect = 226,
  Exit = 93,
  GetXAttr = 8,
  LGetXAttr = 9,
  FGetXAttr = 10,
  LargeFile = 0400000,
  DirectoryOnly = 040000,
  DirectIO = 0200000
};
#else
static const U64 UserLimit = 0x0000800000000000UL;
enum {
  FaccessAt = 269,
  MkdirAt = 258,
  UnlinkAt = 263,
  RenameAt = 264,
  OpenAt = 257,
  ReadlinkAt = 267,
  Read = 0,
  Close = 3,
  Seek = 8,
  Fstat = 5,
  FstatAt = 262,
  StatFS = 137,
  FstatFS = 138,
  Write = 1,
  Protect = 10,
  Exit = 60,
  GetXAttr = 191,
  LGetXAttr = 192,
  FGetXAttr = 193,
  LargeFile = 00100000,
  DirectoryOnly = 00200000,
  DirectIO = 00040000
};
#endif

static U64 raw(U64 Number, U64 A, U64 B, U64 C, U64 D) {
#if defined(__aarch64__)
  register U64 N __asm__("x8") = Number;
  register U64 X0 __asm__("x0") = A;
  register U64 X1 __asm__("x1") = B;
  register U64 X2 __asm__("x2") = C;
  register U64 X3 __asm__("x3") = D;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(N), "r"(X1), "r"(X2), "r"(X3)
                   : "memory", "cc");
  return X0;
#else
  register U64 R10 __asm__("r10") = D;
  U64 Result;
  __asm__ volatile("syscall"
                   : "=a"(Result)
                   : "a"(Number), "D"(A), "S"(B), "d"(C), "r"(R10)
                   : "memory", "cc", "rcx", "r11");
  return Result;
#endif
}

static unsigned char Pages[8192] __attribute__((aligned(4096)));
static const char Path[] = "/fixture/data";
static const unsigned char Expected[] = {0, 0xff, 0x41, 0x0a, 0x80, 0x5a};
#define CHECK(A, B)                                                            \
  do {                                                                         \
    if ((U64)(A) != (U64)(B))                                                  \
      return __LINE__;                                                         \
  } while (0)

U64 files_unlink_errors(void) {
  CHECK(raw(UnlinkAt, (U64)-1, (U64) "/absent", 0, 0), (U64)-2);
  CHECK(raw(UnlinkAt, (U64)-1, (U64) "//absent///", 0, 0), (U64)-2);
  CHECK(raw(UnlinkAt, (U64)-1, (U64) "", 0, 0), (U64)-2);
  CHECK(raw(UnlinkAt, (U64)-1, (U64) "/fixture/data/child", 0, 0), (U64)-20);
  CHECK(raw(UnlinkAt, (U64)-1, (U64) "/fixture/data/", 0, 0), (U64)-20);
  CHECK(raw(UnlinkAt, (U64)-1, 0, 0, 0), (U64)-14);
  CHECK(raw(UnlinkAt, (U64)-1, UserLimit, 0, 0), (U64)-14);
  CHECK(raw(UnlinkAt, (U64)-1, (U64) "/absent", 0xabcdef0000000000UL, 0),
        (U64)-2);
#if defined(__x86_64__)
  CHECK(raw(87, (U64) "/absent", 0, 0, 0), (U64)-2);
  CHECK(raw(87, (U64) "/fixture/data/child", 0, 0, 0), (U64)-20);
#endif
  Pages[4095] = 0;
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 0, 0), 0);
  CHECK(raw(UnlinkAt, (U64)-1, (U64)Pages + 4095, 0, 0), (U64)-2);
  CHECK(raw(UnlinkAt, (U64)-1, (U64)Pages + 4096, 0, 0), (U64)-14);
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 3, 0), 0);
  for (unsigned i = 0; i != 4096; ++i)
    Pages[i] = 'x';
  CHECK(raw(UnlinkAt, (U64)-1, (U64)Pages, 0, 0), (U64)-36);
  U64 fd = raw(OpenAt, (U64)-100, (U64)Path, 0, 0);
  CHECK(fd, 3);
  CHECK(raw(Read, fd, (U64)Pages, sizeof(Expected), 0), sizeof(Expected));
  for (unsigned i = 0; i != sizeof(Expected); ++i)
    CHECK(Pages[i], Expected[i]);
  CHECK(raw(Close, fd, 0, 0, 0), 0);
  return 0;
}
U64 files_unlink_pending(U64 mode) {
  const char *name = mode == 0   ? Path
                     : mode == 1 ? "/fixture"
                     : mode == 2 ? "fixture/absent"
                     : mode == 3 ? "/absent"
                     : mode == 4 ? "/links/raw"
                                 : "/fixture/../absent";
  return raw(UnlinkAt, (U64)-1, (U64)name, mode == 3 ? 512 : 0, 0);
}

U64 files_sequence(void) {
  U64 F = raw(OpenAt, 0x77889900ffffffffUL, (U64)Path,
              0x1122334400000000UL | LargeFile | 02000000, 0777);
  CHECK(F, 3);
  U64 Other = raw(OpenAt, (U64)-100, (U64)Path, 0, 0);
  CHECK(Other, 4);
  CHECK(raw(Read, F | 0x1234567800000000UL, (U64)Pages, 2, 0), 2);
  CHECK(raw(Read, F, (U64)Pages + 2, 100, 0), 4);
  for (unsigned I = 0; I < sizeof(Expected); ++I)
    CHECK(((volatile unsigned char *)Pages)[I], Expected[I]);
  CHECK(raw(Read, F, 1, 2, 0), 0); // EOF does not probe payload mappings.
  CHECK(raw(Read, Other, (U64)Pages + 8, 1, 0), 1);
  CHECK(Pages[8], 0);
  CHECK(raw(Seek, F, (U64)-2, 0x1234567800000002UL, 0), 4);
  CHECK(raw(Read, F, (U64)Pages + 10, 2, 0), 2);
  CHECK(Pages[10], 0x80);
  CHECK(Pages[11], 0x5a);
  CHECK(raw(Seek, F, (U64)-7, 0, 0), (U64)-22);
  CHECK(raw(Seek, F, 0, 1, 0), 6);
  CHECK(raw(Seek, F, 0x7fffffffffffffffUL, 0, 0), 0x7fffffffffffffffUL);
  CHECK(raw(Seek, F, 1, 1, 0), (U64)-22);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), (U64)-22);
  CHECK(raw(Seek, F, 0, 1, 0), 0x7fffffffffffffffUL);
  CHECK(raw(Read, F, UserLimit, 0, 0), 0);
  CHECK(raw(Seek, F, (U64)-1, 1, 0), 0x7ffffffffffffffeUL);
  CHECK(raw(Read, F, 1, 1, 0), 0);
  CHECK(raw(Read, F, 1, 2, 0), (U64)-22);
  CHECK(raw(Seek, F, 0, 1, 0), 0x7ffffffffffffffeUL);
  // Validate the original count before transfer clamping or EOF.
  CHECK(raw(Seek, F, 0x7fffffff7fffffffUL, 0, 0), 0x7fffffff7fffffffUL);
  CHECK(raw(Read, F, 1, 0x100000000UL, 0), (U64)-22);
  CHECK(raw(Read, F, (U64)-1, 0x100000000UL, 0), (U64)-14);
  CHECK(raw(Seek, F, 0, 1, 0), 0x7fffffff7fffffffUL);
  CHECK(raw(Write, F, 1, 1, 0), (U64)-9);
  CHECK(raw(Close, F | 0x1234567800000000UL, 0, 0, 0), 0);
  CHECK(raw(Close, F, 0, 0, 0), (U64)-9);
  CHECK(raw(Read, F, (U64)-1, (U64)-1, 0), (U64)-9);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 0, 0), 3);
  CHECK(raw(Close, 1, 0, 0, 0), 0);
  CHECK(raw(Write, 1, 1, 0, 0), (U64)-9);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 0, 0), 1);
  CHECK(raw(Write, 1, (U64)Path, 1, 0), (U64)-9);
  CHECK(raw(Read, 1, (U64)Pages + 12, 3, 0), 3);
  CHECK(Pages[14], 0x41);
  CHECK(raw(Close, 0, 0, 0, 0), 0);
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, 0, 0), 0);
#if defined(__x86_64__)
  CHECK(raw(2, (U64)Path, 0, 0, 0), 5);
#endif
  return 0;
}

U64 files_open_observed_errors(void) {
  const char *Missing = "/fixture/missing";
  CHECK(raw(OpenAt, (U64)-100, (U64)Path, DirectoryOnly, 0), (U64)-20);
  CHECK(raw(OpenAt, 0, (U64)Path, DirectoryOnly | DirectIO | 02000000, 0777),
        (U64)-20);
  CHECK(raw(OpenAt, 0, (U64)Missing,
            0x1234567800000000UL | DirectIO | LargeFile | 02000000, 0),
        (U64)-2);
  CHECK(raw(OpenAt, 0, (U64)Missing, DirectoryOnly, 0), (U64)-2);
  CHECK(raw(OpenAt, 0, (U64) "/fixture/data/child", DirectIO, 0), (U64)-20);
  CHECK(raw(OpenAt, 0, (U64) "/fixture/data///", DirectIO, 0), (U64)-20);
  CHECK(raw(OpenAt, 0, 1, DirectoryOnly, 0), (U64)-14);
  CHECK(raw(OpenAt, 0, 1, DirectIO, 0), (U64)-14);
#if defined(__x86_64__)
  CHECK(raw(2, (U64)Path, DirectoryOnly, 0777, 0), (U64)-20);
  CHECK(raw(2, (U64)Missing, DirectIO | 02000000, 0, 0), (U64)-2);
#endif
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], 0);
  CHECK(raw(OpenAt, 0, (U64)Missing, DirectIO, 0), (U64)-24);
  CHECK(raw(OpenAt, 0, (U64)Path, DirectoryOnly, 0), (U64)-24);
  CHECK(raw(OpenAt, 0, (U64)Path, DirectIO, 0), (U64)-24);
  CHECK(raw(OpenAt, 0, 1, DirectIO, 0), (U64)-14);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], 0xff);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(OpenAt, 0, (U64)Path, DirectoryOnly, 0), (U64)-20);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), 3);
  CHECK(raw(Close, 3, 0, 0, 0), 0);
  return 0;
}
U64 files_open_unobserved(U64 Mode) {
  const char *Name = Mode == 0 ? Path : "/fixture///";
  U64 Flags = Mode == 2 ? DirectoryOnly : DirectIO;
  U64 F = raw(OpenAt, 0, (U64)Name, Flags, 0);
  if (Mode == 2) {
    CHECK(F, 3);
    CHECK(raw(Read, F, (U64)Pages, 1, 0), (U64)-21);
    CHECK(raw(Seek, F, 0, 1, 0), 0);
    CHECK(raw(Close, F, 0, 0, 0), 0);
    return 0;
  }
  return 99;
}

U64 files_xattr_errors(U64 NameFirst) {
  const char *Missing = "/fixture/missing";
  const char *Name = "user.fixture";
  char LongName[257];
  for (unsigned I = 0; I != 256; ++I)
    LongName[I] = 'x';
  LongName[256] = 0;
  for (unsigned I = 0; I != 64; ++I)
    Pages[I] = 0xa5;
  for (unsigned Route = 0; Route != 2; ++Route) {
    U64 Call = Route ? LGetXAttr : GetXAttr;
    CHECK(raw(Call, (U64)Missing, (U64)Name, 1, (U64)-1), (U64)-2);
    CHECK(raw(Call, (U64)Missing, 1, (U64)Pages, 64),
          (U64) - (NameFirst ? 14 : 2));
    CHECK(raw(Call, (U64)Missing, (U64) "", (U64)Pages, 64),
          (U64) - (NameFirst ? 34 : 2));
    CHECK(raw(Call, 1, (U64) "", 1, 0), (U64) - (NameFirst ? 34 : 14));
    CHECK(raw(Call, (U64)Path, 1, 1, 0), (U64)-14);
    CHECK(raw(Call, (U64)Path, (U64) "", 1, 0), (U64)-34);
    CHECK(raw(Call, (U64) "/fixture///", (U64) "", 1, 0), (U64)-34);
    CHECK(raw(Call, (U64)Missing, (U64)LongName, 1, 0),
          (U64) - (NameFirst ? 34 : 2));
    CHECK(raw(Call, (U64)Path, (U64)LongName, 1, 0), (U64)-34);
    CHECK(raw(Call, (U64) "/fixture/data/child", (U64) "", 1, 0),
          (U64) - (NameFirst ? 34 : 20));
    CHECK(raw(Call, (U64) "/fixture/data///", (U64)Name, 1, 0), (U64)-20);
    LongName[255] = 0;
    CHECK(raw(Call, (U64)Missing, (U64)LongName, 1, 0), (U64)-2);
    LongName[255] = 'x';
  }
  CHECK(raw(FGetXAttr, (U64)-1, (U64)Name, 1, (U64)-1), (U64)-9);
  CHECK(raw(FGetXAttr, (U64)-1, (U64) "", 1, 0), (U64) - (NameFirst ? 34 : 9));
  CHECK(raw(FGetXAttr, (U64)-1, 1, 1, 0), (U64) - (NameFirst ? 14 : 9));
  U64 F = raw(OpenAt, (U64)-100, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)(Pages + 128), 1, 0), 1);
  CHECK(Pages[128], 0);
  CHECK(raw(FGetXAttr, 0x1234567800000000UL | F, (U64) "", 1, 0), (U64)-34);
  CHECK(raw(FGetXAttr, F, 1, 1, 0), (U64)-14);
  CHECK(raw(Read, F, (U64)(Pages + 128), 1, 0), 1);
  CHECK(Pages[128], 0xff);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(FGetXAttr, F, (U64)Name, 1, 0), (U64)-9);
  CHECK(raw(FGetXAttr, 1, (U64) "", 1, 0), (U64)-34);
  for (unsigned I = 0; I != 64; ++I)
    CHECK(Pages[I], 0xa5);
  Pages[4094] = 'x';
  Pages[4095] = 0;
  CHECK(raw(Protect, (U64)(Pages + 4096), 4096, 0, 0), 0);
  CHECK(raw(LGetXAttr, (U64)Missing, (U64)(Pages + 4094), 1, 0), (U64)-2);
  CHECK(raw(LGetXAttr, (U64)Missing, (U64)(Pages + 4096), 1, 0),
        (U64) - (NameFirst ? 14 : 2));
  CHECK(raw(Protect, (U64)(Pages + 4096), 4096, 3, 0), 0);
  return 0;
}
U64 files_xattr_unobserved(U64 Mode) {
  const char *Name = "user.fixture";
  if (Mode == 0)
    raw(GetXAttr, (U64)Path, (U64)Name, 1, 0);
  else if (Mode == 1)
    raw(LGetXAttr, (U64) "/fixture///", (U64)Name, 1, 0);
  else if (Mode == 2) {
    U64 F = raw(OpenAt, (U64)-100, (U64)Path, 0, 0);
    raw(FGetXAttr, F, (U64)Name, 1, 0);
  } else
    raw(FGetXAttr, 1, (U64)Name, 1, 0);
  return 99;
}

U64 files_faults(void) {
  CHECK(raw(OpenAt, 0, (U64) "", 0, 0), (U64)-2);
  CHECK(raw(OpenAt, 0, 1, 0, 0), (U64)-14);
  CHECK(raw(OpenAt, 0, (U64) "/fixture/missing", 0, 0), (U64)-2);
  CHECK(raw(OpenAt, 0, (U64) "/fixture/data/child", 0, 0), (U64)-20);
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, 1, 0, 0), 0);
  CHECK(raw(Read, F, UserLimit, 0, 0), 0);
  CHECK(raw(Read, F, UserLimit - 1, 0, 0), 0);
  CHECK(raw(Read, F, UserLimit + 1, 0, 0), (U64)-14);
  CHECK(raw(Read, F, UserLimit, 1, 0), (U64)-14);
  CHECK(raw(Read, F, (U64)-1, 0, 0), (U64)-14);
  CHECK(raw(Read, F, (U64)Pages, (U64)-1, 0), (U64)-14);
  CHECK(raw(Read, 1, (U64)-1, 0, 0), (U64)-9);
  CHECK(raw(Seek, F, 0, 99, 0), (U64)-22);
  CHECK(raw(Seek, 1, 0, 0, 0), (U64)-29);
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 1, 0), 0);
  CHECK(raw(Read, F, (U64)Pages + 4094, 6, 0), 2);
  CHECK(Pages[4094], 0);
  CHECK(Pages[4095], 0xff);
  CHECK(raw(Read, F, (U64)Pages + 4096, 2, 0), (U64)-14);
  CHECK(raw(Seek, F, 0, 1, 0), 2);
  CHECK(raw(Read, F, (U64)Pages, 6, 0), 4);
  CHECK(Pages[0], 0x41);
  CHECK(Pages[3], 0x5a);
  CHECK(raw(Read, F, (U64)Pages + 4096, 1, 0), 0);
  // A terminator at the last readable byte must not probe the next page.
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 0, 0), 0);
  for (unsigned I = 0; I < sizeof(Path); ++I)
    Pages[4096 - sizeof(Path) + I] = Path[I];
  CHECK(raw(OpenAt, 0, (U64)Pages + 4096 - sizeof(Path), 0, 0), 4);
  Pages[4095] = 'x';
  CHECK(raw(OpenAt, 0, (U64)Pages + 4096 - sizeof(Path), 0, 0), (U64)-14);
  return 0;
}

U64 files_capacity(void) {
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), 3);
  CHECK(raw(OpenAt, 0, (U64) "/absent", 0, 0), (U64)-24);
  CHECK(raw(OpenAt, 0, 1, 0, 0), (U64)-14);
  CHECK(raw(Close, 3, 0, 0, 0), 0);
  CHECK(raw(OpenAt, 0, (U64) "/absent", 0, 0), (U64)-2);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), 3);
  return 0;
}

U64 files_access(void) {
  CHECK(raw(FaccessAt, (U64)-1, (U64)Path, 0x1234567800000000UL, 99), 0);
  CHECK(raw(FaccessAt, (U64)-100, (U64) "/", 0, 0), 0);
  CHECK(raw(FaccessAt, 0, (U64) "/fixture", 0, 0), 0);
  CHECK(raw(FaccessAt, 0, (U64) "/fixture/missing", 0, 0), (U64)-2);
  CHECK(raw(FaccessAt, 0, (U64) "/absent", 0, 0), (U64)-2);
  CHECK(raw(FaccessAt, 0, (U64) "/fixture/data/child", 0, 0), (U64)-20);
  CHECK(raw(FaccessAt, 0, (U64) "/absent", 7, 0), (U64)-2);
  CHECK(raw(FaccessAt, 0, (U64) "/fixture/data/child", 4, 0), (U64)-20);
  // Invalid mode precedes pathname access; only its low int bits participate.
  CHECK(raw(FaccessAt, 0, 1, 8, 0), (U64)-22);
  CHECK(raw(FaccessAt, 0, 1, 9, 0), (U64)-22);
  CHECK(raw(FaccessAt, 0, 1, (U64)-1, 0), (U64)-22);
#if defined(__x86_64__)
  CHECK(raw(21, (U64)Path, 0, 0, 0), 0);
  CHECK(raw(21, (U64) "/absent", 0, 0, 0), (U64)-2);
  CHECK(raw(21, 1, 8, 0, 0), (U64)-22);
#endif
  // Existence checks neither allocate a descriptor nor change open cursors.
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), (U64)-24);
  CHECK(raw(FaccessAt, 0, (U64)Path, 0, 0), 0);
  CHECK(raw(FaccessAt, 0, (U64) "/absent", 0, 0), (U64)-2);
  CHECK(raw(Seek, F, 0, 1, 0), 1);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], 0xff);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), F);
  return 0;
}

U64 files_access_faults(void) {
  static U64 UnterminatedPath[512];
  CHECK(raw(FaccessAt, 0, (U64) "", 0, 0), (U64)-2);
  CHECK(raw(FaccessAt, 0, 1, 0, 0), (U64)-14);
  CHECK(raw(FaccessAt, 0, UserLimit, 0, 0), (U64)-14);
  CHECK(raw(FaccessAt, 0, (U64)-1, 0, 0), (U64)-14);
  for (unsigned I = 0; I < sizeof(UnterminatedPath) / sizeof(U64); ++I)
    UnterminatedPath[I] = 0x7878787878787878UL;
  CHECK(raw(FaccessAt, 0, (U64)UnterminatedPath, 0, 0), (U64)-36);
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 0, 0), 0);
  for (unsigned I = 0; I < sizeof(Path); ++I)
    Pages[4096 - sizeof(Path) + I] = Path[I];
  CHECK(raw(FaccessAt, 0, (U64)Pages + 4096 - sizeof(Path), 0, 0), 0);
  Pages[4095] = 'x';
  CHECK(raw(FaccessAt, 0, (U64)Pages + 4096 - sizeof(Path), 0, 0), (U64)-14);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), 3);
  return 0;
}

U64 files_access_empty(void) {
  CHECK(raw(FaccessAt, 0, (U64) "/", 0, 0), 0);
  CHECK(raw(FaccessAt, 0, (U64) "///", 0, 0), 0);
  CHECK(raw(FaccessAt, 0, (U64) "/fixture", 0, 0), (U64)-2);
  CHECK(raw(FaccessAt, 0, (U64) "/fixture/", 0, 0), (U64)-2);
  CHECK(raw(FaccessAt, 0, (U64)Path, 0, 0), (U64)-2);
  return 0;
}

U64 files_access_unsupported(U64 Mode) {
  static const char *const Paths[] = {Path, "fixture/data", "/fixture//data",
                                      "/fixture/../data"};
  const char *Name = Mode < 4 ? Paths[Mode] : Path;
  U64 Permission = Mode < 4 ? 0 : Mode < 7 ? 1UL << (Mode - 4) : 7;
  return raw(FaccessAt, (U64)-100, (U64)Name, Permission, 0);
}

U64 files_repeated_slashes(void) {
  static const char *const Aliases[] = {"//fixture/data", "/fixture///data",
                                        "///fixture////data"};
  for (unsigned I = 0; I < sizeof(Aliases) / sizeof(Aliases[0]); ++I) {
    U64 Name = (U64)Aliases[I];
    U64 F = raw(OpenAt, (U64)-1, Name, 0, 0);
    CHECK(F, 3);
    CHECK(raw(Read, F, (U64)Pages, sizeof(Expected), 0), sizeof(Expected));
    for (unsigned J = 0; J < sizeof(Expected); ++J)
      CHECK(Pages[J], Expected[J]);
    CHECK(raw(FaccessAt, (U64)-1, Name, 0, 0), 0);
    CHECK(raw(Close, F, 0, 0, 0), 0);
  }
  CHECK(raw(OpenAt, 0, (U64) "//fixture//missing", 0, 0), (U64)-2);
  CHECK(raw(FaccessAt, 0, (U64) "//fixture//data///", 0, 0), (U64)-20);
  CHECK(raw(FstatAt, 0, (U64) "//fixture//data//child", 1, 0), (U64)-20);
  CHECK(raw(MkdirAt, 0, (U64) "//fixture//data///", 0777, 0), (U64)-17);
  return 0;
}

U64 files_directory_errors(void) {
  CHECK(raw(MkdirAt, (U64)-1, (U64)Path, (U64)-1, 99), (U64)-17);
  CHECK(raw(MkdirAt, 0, (U64) "/", 0, 0), (U64)-17);
  CHECK(raw(MkdirAt, 0, (U64) "/fixture", 0777, 0), (U64)-17);
  CHECK(raw(MkdirAt, 0, (U64) "/missing/child", 0777, 0), (U64)-2);
  CHECK(raw(MkdirAt, 0, (U64) "/missing/child/deep", 0, 0), (U64)-2);
  CHECK(raw(MkdirAt, 0, (U64) "/fixture/data/child", 0777, 0), (U64)-20);
  CHECK(raw(MkdirAt, 0, (U64) "/fixture/data/child/deep", 0, 0), (U64)-20);
  CHECK(raw(MkdirAt, 0, (U64) "", (U64)-1, 0), (U64)-2);
  CHECK(raw(MkdirAt, 0, 1, (U64)-1, 0), (U64)-14);
  CHECK(raw(MkdirAt, 0, UserLimit, 0, 0), (U64)-14);
#if defined(__x86_64__)
  CHECK(raw(83, (U64)Path, 0777, 0, 0), (U64)-17);
  CHECK(raw(83, (U64) "/missing/child", 0777, 0, 0), (U64)-2);
#endif
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), (U64)-24);
  CHECK(raw(MkdirAt, 0, (U64)Path, 0777, 0), (U64)-17);
  CHECK(raw(MkdirAt, 0, (U64) "/missing/child", 0777, 0), (U64)-2);
  CHECK(raw(FaccessAt, 0, (U64) "/missing", 0, 0), (U64)-2);
  CHECK(raw(Seek, F, 0, 1, 0), 1);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), F);
  return 0;
}

U64 files_directory_unsupported(U64 Mode) {
  static const char *const Paths[] = {"/missing/child", "/new", "/fixture/new",
                                      "relative", "/fixture/../new"};
  return raw(MkdirAt, (U64)-1, (U64)Paths[Mode], 0777, 0);
}

U64 files_trailing_paths(void) {
  static const struct {
    const char *Name;
    I64 Query, Create;
  } Cases[] = {{"/fixture/data/", -20, -17},
               {"/fixture/data////", -20, -17},
               {"/fixture/data/child/", -20, -20},
               {"/missing/child/", -2, -2},
               {"/missing/child/deep///", -2, -2}};
  for (unsigned I = 0; I < sizeof(Cases) / sizeof(Cases[0]); ++I) {
    U64 Name = (U64)Cases[I].Name;
    CHECK(raw(OpenAt, (U64)-1, Name, 0, 0), Cases[I].Query);
    CHECK(raw(FaccessAt, 0, Name, 0, 0), Cases[I].Query);
    CHECK(raw(FstatAt, 0, Name, 1, 0), Cases[I].Query);
    CHECK(raw(MkdirAt, 0, Name, 0777, 0), Cases[I].Create);
  }
  for (unsigned I = 0; I < 2; ++I) {
    const char *Directory = I ? "////" : "/fixture///";
    CHECK(raw(FaccessAt, 0, (U64)Directory, 0, 0), 0);
    CHECK(raw(MkdirAt, 0, (U64)Directory, 0777, 0), (U64)-17);
  }
  // A trailing separator does not alter flag or descriptor-limit priority.
  CHECK(raw(FstatAt, 0, (U64) "/fixture/data/", 1, 0x40000000), (U64)-22);
  CHECK(raw(FaccessAt, 0, (U64) "/fixture/data/", 8, 0), (U64)-22);
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(raw(OpenAt, 0, (U64) "/fixture/data/", 0, 0), (U64)-24);
  CHECK(raw(FaccessAt, 0, (U64) "/fixture/data/", 0, 0), (U64)-20);
  CHECK(raw(FstatAt, 0, (U64) "/missing/", 1, 0), (U64)-2);
  CHECK(raw(Seek, F, 0, 1, 0), 1);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), F);
  // Import stops at the first NUL even when the next byte is inaccessible.
  static const char Edge[] = "/fixture/data///";
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 0, 0), 0);
  for (unsigned I = 0; I < sizeof(Edge); ++I)
    Pages[4096 - sizeof(Edge) + I] = Edge[I];
  U64 Name = (U64)Pages + 4096 - sizeof(Edge);
  CHECK(raw(FstatAt, 0, Name, 1, 0), (U64)-20);
  CHECK(raw(MkdirAt, 0, Name, 0777, 0), (U64)-17);
  Pages[4095] = '/';
  CHECK(raw(FstatAt, 0, Name, 1, 0), (U64)-14);
  return 0;
}

U64 files_trailing_unsupported(U64 Mode) {
  switch (Mode) {
  case 0:
    return raw(OpenAt, 0, (U64) "/fixture/", 0, 0);
  case 1:
    return raw(FstatAt, 0, (U64) "/fixture/", (U64)Pages, 0);
  case 2:
    return raw(MkdirAt, 0, (U64) "/fixture/new///", 0777, 0);
  case 3:
    return raw(FaccessAt, 0, (U64) "/fixture/", 4, 0);
  case 4:
    return raw(FaccessAt, 0, (U64) "/fixture//data/", 0, 0);
  case 5:
    return raw(FaccessAt, 0, (U64) "fixture/data/", 0, 0);
  case 6:
    return raw(OpenAt, 0, (U64) "///", 0, 0);
  case 7:
    return raw(FstatAt, 0, (U64) "///", (U64)Pages, 0);
  case 8:
    return raw(MkdirAt, 0, (U64) "/new///", 0777, 0);
  default:
    return raw(FaccessAt, 0, (U64) "/fixture/../data/", 0, 0);
  }
}

struct FileTime {
  I64 Seconds;
  U64 Nanoseconds;
};
struct FileStatus {
  U64 Device, Inode;
#if defined(__aarch64__)
  U32 Mode, Links, UID, GID;
  U64 RDevice, Pad1;
  I64 Size;
  int BlockSize, Pad2;
#else
  U64 Links;
  U32 Mode, UID, GID, Pad1;
  U64 RDevice;
  I64 Size, BlockSize;
#endif
  I64 Blocks;
  struct FileTime Access, Modification, Change;
#if defined(__aarch64__)
  U32 Reserved[2];
#else
  U64 Reserved[3];
#endif
};
#if defined(__aarch64__)
_Static_assert(sizeof(struct FileStatus) == 128, "AArch64 stat ABI");
#else
_Static_assert(sizeof(struct FileStatus) == 144, "x64 stat ABI");
#endif
_Static_assert(__builtin_offsetof(struct FileStatus, BlockSize) == 56,
               "stat block-size offset");

static const char RenamedPath[] = "/fixture/moved";
static U64 rename_prepare(struct FileStatus *Before) {
  U64 fd = raw(OpenAt, -1UL, (U64)Path, 2, 0);
  CHECK(fd, 3);
  CHECK(raw(Read, fd, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], Expected[0]);
  CHECK(raw(Fstat, fd, (U64)Before, 0, 0), 0);
  return fd;
}
static U64 rename_verify(U64 fd, struct FileStatus *Before) {
  struct FileStatus After;
  Before->Change.Seconds = 100;
  Before->Change.Nanoseconds = 123;
  CHECK(raw(Fstat, fd, (U64)&After, 0, 0), 0);
  for (unsigned i = 0; i < sizeof(After); ++i)
    CHECK(((unsigned char *)&After)[i], ((unsigned char *)Before)[i]);
  CHECK(raw(FstatAt, -1UL, (U64)RenamedPath, (U64)&After, 0), 0);
  for (unsigned i = 0; i < sizeof(After); ++i)
    CHECK(((unsigned char *)&After)[i], ((unsigned char *)Before)[i]);
  CHECK(raw(FaccessAt, -1UL, (U64)Path, 0, 0), (U64)-2);
  CHECK(raw(FaccessAt, -1UL, (U64)RenamedPath, 0, 0), 0);
  CHECK(raw(Read, fd, (U64)Pages, 2, 0), 2);
  CHECK(Pages[0], Expected[1]);
  CHECK(Pages[1], Expected[2]);
  U64 second = raw(OpenAt, -1UL, (U64)RenamedPath, 2, 0);
  CHECK(second, 4);
  Pages[0] = 0x7e;
  CHECK(raw(Write, second, (U64)Pages, 1, 0), 1);
  CHECK(raw(Close, second, 0, 0, 0), 0);
  CHECK(raw(Seek, fd, 0, 0, 0), 0);
  CHECK(raw(Read, fd, (U64)Pages, sizeof(Expected), 0), sizeof(Expected));
  CHECK(Pages[0], 0x7e);
  for (unsigned i = 1; i < sizeof(Expected); ++i)
    CHECK(Pages[i], Expected[i]);
  CHECK(raw(ReadlinkAt, -1UL, (U64) "/proc/self/fd/3", (U64)Pages, 64),
        sizeof(RenamedPath) - 1);
  for (unsigned i = 0; i < sizeof(RenamedPath) - 1; ++i)
    CHECK(Pages[i], RenamedPath[i]);
  return 0;
}
U64 files_rename_raw(void) {
  struct FileStatus Before;
  U64 fd = rename_prepare(&Before);
  CHECK(fd, 3);
  CHECK(raw(RenameAt, -1UL, (U64)Path, -1UL, (U64)RenamedPath), 0);
  CHECK(rename_verify(fd, &Before), 0);
#if defined(__x86_64__)
  CHECK(raw(82, (U64)RenamedPath, (U64)Path, 0, 0), 0);
#else
  CHECK(raw(RenameAt, -1UL, (U64)RenamedPath, -1UL, (U64)Path), 0);
#endif
  CHECK(raw(FaccessAt, -1UL, (U64)Path, 0, 0), 0);
  CHECK(raw(FaccessAt, -1UL, (U64)RenamedPath, 0, 0), (U64)-2);
  CHECK(raw(Close, fd, 0, 0, 0), 0);
  return 0;
}
U64 files_rename_reuse(U64 *out) {
  struct FileStatus Before, Directory, File;
  U64 fd = rename_prepare(&Before);
  CHECK(fd, 3);
  CHECK(raw(RenameAt, -1UL, (U64)Path, -1UL, (U64)RenamedPath), 0);
  CHECK(raw(MkdirAt, -1UL, (U64)Path, 0755, 0), 0);
  CHECK(raw(FstatAt, -1UL, (U64)Path, (U64)&Directory, 0), 0);
  if (out) {
    out[0] = Directory.Inode;
    out[1] = Before.Inode;
  }
  CHECK(Directory.Inode != Before.Inode, 1);
  CHECK(Directory.Mode & 0170000, 0040000);
  CHECK(raw(Fstat, fd, (U64)&File, 0, 0), 0);
  CHECK(File.Inode, Before.Inode);
  CHECK(File.Mode & 0170000, 0100000);
  CHECK(raw(FstatAt, -1UL, (U64)RenamedPath, (U64)&File, 0), 0);
  CHECK(File.Inode, Before.Inode);
  CHECK(raw(Read, fd, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], Expected[1]);
  CHECK(raw(ReadlinkAt, -1UL, (U64) "/proc/self/fd/3", (U64)Pages, 64),
        sizeof(RenamedPath) - 1);
  for (unsigned i = 0; i < sizeof(RenamedPath) - 1; ++i)
    CHECK(Pages[i], RenamedPath[i]);
  CHECK(raw(Close, fd, 0, 0, 0), 0);
  return 0;
}
U64 files_rename_fdinfo(unsigned char *out) {
  struct FileStatus Before, File, Directory;
  U64 fd = rename_prepare(&Before);
  CHECK(fd, 3);
  U64 info = raw(OpenAt, -1UL, (U64) "/proc/self/fdinfo/3", 0, 0);
  CHECK(info, 4);
  unsigned char *observed = out ? out : Pages;
  for (unsigned phase = 0; phase != 3; ++phase) {
    if (phase == 1)
      CHECK(raw(RenameAt, -1UL, (U64)Path, -1UL, (U64)RenamedPath), 0);
    if (phase == 2)
      CHECK(raw(MkdirAt, -1UL, (U64)Path, 0755, 0), 0);
    CHECK(raw(Seek, info, 0, 0, 0), 0);
    U64 count = raw(Read, info, (U64)(observed + phase * 256 + 16), 128, 0);
    CHECK(count <= 128, 1);
    *(U64 *)(observed + phase * 256) = count;
  }
  CHECK(raw(Fstat, fd, (U64)&File, 0, 0), 0);
  CHECK(raw(FstatAt, -1UL, (U64)Path, (U64)&Directory, 0), 0);
  *(U64 *)(observed + 768) = File.Inode;
  *(U64 *)(observed + 776) = Directory.Inode;
#if defined(__aarch64__)
  static const char expected[] =
      "pos:\t1\nflags:\t0400002\nmnt_id:\t1\nino:\t42\n";
#else
  static const char expected[] =
      "pos:\t1\nflags:\t0100002\nmnt_id:\t1\nino:\t42\n";
#endif
  for (unsigned phase = 0; phase != 3; ++phase) {
    CHECK(*(U64 *)(observed + phase * 256), sizeof(expected) - 1);
    for (unsigned i = 0; i != sizeof(expected) - 1; ++i)
      CHECK(observed[phase * 256 + 16 + i], expected[i]);
  }
  CHECK(File.Inode, 42);
  CHECK(Directory.Inode != File.Inode, 1);
  CHECK(raw(Read, fd, (U64)Pages + 1024, 1, 0), 1);
  CHECK(Pages[1024], Expected[1]);
  CHECK(raw(Close, info, 0, 0, 0), 0);
  CHECK(raw(Close, fd, 0, 0, 0), 0);
  return 0;
}
U64 files_rename_pending(U64 mode) {
  const char *source = mode == 0   ? "/fixture/absent"
                       : mode == 1 ? "fixture/data"
                                   : Path;
  const char *target = mode == 2   ? Path
                       : mode == 3 ? "/other/moved"
                       : mode == 4 ? "/fixture/moved/"
                                   : RenamedPath;
  return raw(RenameAt, -1UL, (U64)source, -1UL, mode == 5 ? 0 : (U64)target);
}

static U64 creation_prepare(struct FileStatus *Before) {
  U64 fd = rename_prepare(Before);
  CHECK(fd, 3);
  CHECK(raw(RenameAt, -1UL, (U64)Path, -1UL, (U64)RenamedPath), 0);
  Before->Change.Seconds = 100;
  Before->Change.Nanoseconds = 123;
  return fd;
}

static U64 creation_verify(U64 old, U64 created,
                           const struct FileStatus *Before) {
  struct FileStatus Original, New, ByName, Parent;
  CHECK(created, 4);
  CHECK(raw(Fstat, old, (U64)&Original, 0, 0), 0);
  for (unsigned i = 0; i != sizeof(Original); ++i)
    CHECK(((unsigned char *)&Original)[i], ((const unsigned char *)Before)[i]);
  CHECK(raw(Fstat, created, (U64)&New, 0, 0), 0);
  CHECK(New.Inode != 0 && New.Inode != Original.Inode && New.Inode != 91, 1);
  CHECK(New.Device, 7);
  CHECK(New.Mode, 0100644);
  CHECK(New.Links, 1);
  CHECK(New.UID, 1000);
  CHECK(New.GID, 1000);
  CHECK(New.Size, 0);
  CHECK(New.BlockSize, 4096);
  CHECK(New.Blocks, 0);
  CHECK(New.Access.Seconds, 100);
  CHECK(New.Access.Nanoseconds, 123);
  CHECK(New.Modification.Seconds, 100);
  CHECK(New.Modification.Nanoseconds, 123);
  CHECK(New.Change.Seconds, 100);
  CHECK(New.Change.Nanoseconds, 123);
  CHECK(raw(FstatAt, -1UL, (U64)Path, (U64)&ByName, 0), 0);
  for (unsigned i = 0; i != sizeof(New); ++i)
    CHECK(((unsigned char *)&New)[i], ((unsigned char *)&ByName)[i]);
  CHECK(raw(FstatAt, -1UL, (U64) "/fixture", (U64)&Parent, 0), 0);
  CHECK(Parent.Size, 80);
  CHECK(Parent.Links, 2);
  CHECK(Parent.Modification.Seconds, 100);
  CHECK(Parent.Modification.Nanoseconds, 123);
  CHECK(raw(Write, created, (U64) "new", 3, 0), 3);
  CHECK(raw(Seek, created, 0, 0, 0), 0);
  CHECK(raw(Read, created, (U64)Pages, 3, 0), 3);
  CHECK(Pages[0], 'n');
  CHECK(Pages[1], 'e');
  CHECK(Pages[2], 'w');
  CHECK(raw(Read, old, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], Expected[1]);
  CHECK(raw(ReadlinkAt, -1UL, (U64) "/proc/self/fd/4", (U64)Pages, 128),
        sizeof(Path) - 1);
  for (unsigned i = 0; i != sizeof(Path) - 1; ++i)
    CHECK(Pages[i], Path[i]);
  CHECK(raw(Close, created, 0, 0, 0), 0);
  // The old descriptor has the same initial pathname but a different object.
  CHECK(raw(UnlinkAt, -1UL, (U64)Path, 0, 0), 0);
  CHECK(raw(FaccessAt, -1UL, (U64)Path, 0, 0), (U64)-2);
  CHECK(raw(FstatAt, -1UL, (U64)RenamedPath, (U64)&Original, 0), 0);
  for (unsigned i = 0; i != sizeof(Original); ++i)
    CHECK(((unsigned char *)&Original)[i], ((const unsigned char *)Before)[i]);
  CHECK(raw(Seek, old, 0, 0, 0), 0);
  CHECK(raw(Read, old, (U64)Pages, sizeof(Expected), 0), sizeof(Expected));
  for (unsigned i = 0; i != sizeof(Expected); ++i)
    CHECK(Pages[i], Expected[i]);
  CHECK(raw(Close, old, 0, 0, 0), 0);
  return 0;
}

U64 files_creation_raw(void) {
  struct FileStatus Before;
  U64 old = creation_prepare(&Before);
  CHECK(old, 3);
  U64 created =
      raw(OpenAt, -1UL, (U64)Path, 0x1234567800000000UL | 0100 | 01000 | 2,
          0xabcdef0000000000UL | 0666);
  return creation_verify(old, created, &Before);
}

static U64 counted_raw_open(U64 Mode) {
  (void)Mode;
  return raw(OpenAt, -1UL, (U64)Path, 0102, 0666);
}

static U64 creation_counted(U64 (*Create)(U64), U64 Mode) {
  struct FileStatus Before, Original, New, ByName, Parent;
  U64 old = creation_prepare(&Before);
  CHECK(old, 3);
  U64 previous = Before.Inode;
  for (unsigned generation = 0; generation != 2; ++generation) {
    U64 created = Create(Mode);
    CHECK(created, 4);
    CHECK(raw(Fstat, created, (U64)&New, 0, 0), 0);
    CHECK(New.Inode != 0 && New.Inode != previous && New.Inode != Before.Inode,
          1);
    previous = New.Inode;
    CHECK(New.Device, 7);
    CHECK(New.Mode, 0100644);
    CHECK(New.Links, 1);
    CHECK(New.UID, 1000);
    CHECK(New.GID, 1000);
    CHECK(New.Size, 0);
    CHECK(New.BlockSize, 4096);
    CHECK(New.Blocks, 0);
    CHECK(New.Access.Seconds, 100);
    CHECK(New.Access.Nanoseconds, 123);
    CHECK(New.Modification.Seconds, 100);
    CHECK(New.Modification.Nanoseconds, 123);
    CHECK(New.Change.Seconds, 100);
    CHECK(New.Change.Nanoseconds, 123);
    CHECK(raw(FstatAt, -1UL, (U64)Path, (U64)&ByName, 0), 0);
    for (unsigned i = 0; i != sizeof(New); ++i)
      CHECK(((unsigned char *)&New)[i], ((unsigned char *)&ByName)[i]);
    const char *value = generation ? "two" : "one";
    CHECK(raw(Write, created, (U64)value, 3, 0), 3);
    CHECK(raw(Seek, created, 0, 0, 0), 0);
    CHECK(raw(Read, created, (U64)Pages, 3, 0), 3);
    for (unsigned i = 0; i != 3; ++i)
      CHECK(Pages[i], value[i]);
    // An existing-name open does not consume the next creation's reservation.
    CHECK(raw(OpenAt, -1UL, (U64)Path, 0102, 0666), 5);
    CHECK(raw(Close, 5, 0, 0, 0), 0);
    CHECK(raw(Read, old, (U64)Pages, 1, 0), 1);
    CHECK(Pages[0], Expected[generation + 1]);
    CHECK(raw(FstatAt, -1UL, (U64) "/fixture", (U64)&Parent, 0), 0);
    CHECK(Parent.Size, 80);
    CHECK(raw(Close, created, 0, 0, 0), 0);
    CHECK(raw(UnlinkAt, -1UL, (U64)Path, 0, 0), 0);
    CHECK(raw(FaccessAt, -1UL, (U64)Path, 0, 0), (U64)-2);
    CHECK(raw(FstatAt, -1UL, (U64) "/fixture", (U64)&Parent, 0), 0);
    CHECK(Parent.Size, 60);
    CHECK(raw(Fstat, old, (U64)&Original, 0, 0), 0);
    for (unsigned i = 0; i != sizeof(Original); ++i)
      CHECK(((unsigned char *)&Original)[i], ((unsigned char *)&Before)[i]);
  }
  CHECK(raw(Seek, old, 0, 0, 0), 0);
  CHECK(raw(Read, old, (U64)Pages, sizeof(Expected), 0), sizeof(Expected));
  for (unsigned i = 0; i != sizeof(Expected); ++i)
    CHECK(Pages[i], Expected[i]);
  CHECK(raw(Close, old, 0, 0, 0), 0);
  return 0;
}

U64 files_creation_counted_raw(void) {
  return creation_counted(counted_raw_open, 0);
}

// Keep independent field observations scalar under both optimization levels.
static U64 verify_status(const volatile struct FileStatus *S) {
  CHECK(S->Device, 0xfe12cd34UL);
  CHECK(S->Inode, 0xfedcba9876543210UL);
  CHECK(S->Mode, 0100644);
  CHECK(S->Links, 0x89abcdefUL);
  CHECK(S->UID, 0x87654321UL);
  CHECK(S->GID, 0xfedcba98UL);
  CHECK(S->RDevice, 0);
  CHECK(S->Pad1, 0);
#if defined(__aarch64__)
  CHECK(S->Pad2, 0);
#endif
  CHECK(S->Size, 0); // Explicit metadata is independent of readable bytes.
  CHECK(S->BlockSize, 16384);
  CHECK(S->Blocks, 0x1234567890UL);
  CHECK(S->Access.Seconds, -0x7fffffffffffffffL);
  CHECK(S->Access.Nanoseconds, 123456789);
  CHECK(S->Modification.Seconds, 4294967297L);
  CHECK(S->Modification.Nanoseconds, 987654321);
  CHECK(S->Change.Seconds, 0x7fffffffffffffffL);
  CHECK(S->Change.Nanoseconds, 999999999);
  for (unsigned I = 0; I < sizeof(S->Reserved) / sizeof(S->Reserved[0]); ++I)
    CHECK(S->Reserved[I], 0);
  return 0;
}

U64 files_status(void) {
  struct {
    U64 Before;
    struct FileStatus Status;
    U64 After;
  } Out;
  volatile unsigned char *Bytes = (volatile unsigned char *)&Out;
  for (unsigned I = 0; I < sizeof(Out); ++I)
    Bytes[I] = 0xa5;
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(raw(Fstat, F | 0xabcdef0000000000UL, (U64)&Out.Status, 0, 0), 0);
  CHECK(verify_status(&Out.Status), 0);
  CHECK(Out.Before, 0xa5a5a5a5a5a5a5a5UL);
  CHECK(Out.After, 0xa5a5a5a5a5a5a5a5UL);
  CHECK(raw(Seek, F, 0, 1, 0), 1);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], 0xff);
  CHECK(raw(Fstat, F, (U64)Pages + 1, 0, 0), 0);
  for (unsigned I = 0; I < sizeof(Out.Status); ++I)
    CHECK(((volatile unsigned char *)Pages)[I + 1],
          ((volatile unsigned char *)&Out.Status)[I]);
  CHECK(raw(Fstat, F, 1, 0, 0), (U64)-14);
  CHECK(raw(Fstat, F, UserLimit - sizeof(Out.Status) + 1, 0, 0), (U64)-14);
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 1, 0), 0);
  CHECK(raw(Fstat, F, (U64)Pages + 4096, 0, 0), (U64)-14);
  CHECK(raw(Seek, F, 0, 1, 0), 2);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(Fstat, F, (U64)-1, 0, 0), (U64)-9);
  CHECK(raw(Close, 1, 0, 0, 0), 0);
  CHECK(raw(OpenAt, 0, (U64)Path, 0, 0), 1);
  CHECK(raw(Fstat, 1, (U64)&Out.Status, 0, 0), 0);
  CHECK(verify_status(&Out.Status), 0);
  return 0;
}

U64 files_filesystem_status(void) {
  unsigned char Output[256];
  for (unsigned I = 0; I < sizeof(Output); ++I)
    Output[I] = 0xa5;
  static const struct {
    const char *Name;
    U64 Error;
  } Cases[] = {{"/missing", 2},
               {"/missing/child///", 2},
               {"/fixture/data/", 20},
               {"/fixture/data/child", 20},
               {"", 2}};
  U64 F = raw(OpenAt, -1UL, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  for (unsigned I = 0; I < sizeof(Cases) / sizeof(Cases[0]); ++I) {
    CHECK(raw(StatFS, (U64)Cases[I].Name, (U64)Output, 0, 0), -Cases[I].Error);
    CHECK(raw(StatFS, (U64)Cases[I].Name, 1, 0, 0), -Cases[I].Error);
  }
  CHECK(raw(StatFS, 1, 1, 0, 0), (U64)-14);
  CHECK(raw(FstatFS, 0x12345678ffffffffUL, (U64)Output, 0, 0), (U64)-9);
  CHECK(raw(Seek, F, 0, 1, 0), 1);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(FstatFS, F | 0x1234567800000000UL, 1, 0, 0), (U64)-9);
  CHECK(raw(OpenAt, -1UL, (U64)Path, 0, 0), F);
  CHECK(raw(Seek, F, 0, 1, 0), 0);
  CHECK(raw(Close, 0, 0, 0, 0), 0);
  CHECK(raw(FstatFS, 0xabcdef1200000000UL, 1, 0, 0), (U64)-9);
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 0, 0), 0);
  static const char Missing[] = "/missing/";
  for (unsigned I = 0; I < sizeof(Missing); ++I)
    Pages[4096 - sizeof(Missing) + I] = Missing[I];
  U64 Name = (U64)Pages + 4096 - sizeof(Missing);
  CHECK(raw(StatFS, Name, (U64)Output, 0, 0), (U64)-2);
  Pages[4095] = 'x';
  CHECK(raw(StatFS, Name, 1, 0, 0), (U64)-14);
  for (unsigned I = 0; I < sizeof(Output); ++I)
    CHECK(Output[I], 0xa5);
  return 0;
}

U64 files_status_at(void) {
  struct {
    U64 Before;
    struct FileStatus Status;
    U64 After;
  } Out;
  for (unsigned I = 0; I < sizeof(Out); ++I)
    ((volatile unsigned char *)&Out)[I] = 0xa5;
  // Absolute paths ignore dirfd and use only the int-width flag bits.
  CHECK(raw(FstatAt, 0x12345678ffffffffUL, (U64)Path, (U64)&Out.Status,
            0xabcdef0000000900UL),
        0);
  CHECK(verify_status(&Out.Status), 0);
  CHECK(Out.Before, 0xa5a5a5a5a5a5a5a5UL);
  CHECK(Out.After, 0xa5a5a5a5a5a5a5a5UL);
  U64 F = raw(OpenAt, (U64)-100, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(raw(FstatAt, F | 0x9876543200000000UL, (U64) "", (U64)&Out.Status,
            0x1000),
        0);
  CHECK(verify_status(&Out.Status), 0);
  CHECK(raw(Seek, F, 0, 1, 0), 1);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(Pages[0], 0xff);
  // Import the complete path before writing overlapping status bytes.
  for (unsigned I = 0; I < sizeof(Path); ++I)
    Pages[I + 1] = Path[I];
  CHECK(raw(FstatAt, F, (U64)Pages + 1, (U64)Pages + 1, 0), 0);
  for (unsigned I = 0; I < sizeof(Out.Status); ++I)
    CHECK(Pages[I + 1], ((unsigned char *)&Out.Status)[I]);
  CHECK(raw(FstatAt, F, 1, (U64)&Out.Status, 0x40000000), (U64)-22);
  CHECK(raw(FstatAt, F, 1, (U64)&Out.Status, 0), (U64)-14);
  CHECK(raw(FstatAt, F, (U64) "/missing", 1, 0), (U64)-2);
  CHECK(raw(FstatAt, F, (U64) "/fixture/data/child", 1, 0), (U64)-20);
  CHECK(raw(FstatAt, F, (U64) "", 1, 0), (U64)-2);
  CHECK(raw(FstatAt, F, (U64)Path, 1, 0), (U64)-14);
  CHECK(raw(Close, F, 0, 0, 0), 0);
  CHECK(raw(FstatAt, F, (U64) "", 1, 0x1000), (U64)-9);
  CHECK(verify_status(&Out.Status), 0);
  // The first NUL is the last readable byte, followed by an inaccessible page.
  for (unsigned I = 0; I < sizeof(Path); ++I)
    Pages[4096 - sizeof(Path) + I] = Path[I];
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 0, 0), 0);
  CHECK(raw(FstatAt, (U64)-100, (U64)Pages + 4096 - sizeof(Path),
            (U64)&Out.Status, 0),
        0);
  CHECK(verify_status(&Out.Status), 0);
  CHECK(raw(FstatAt, F, (U64)Path, (U64)Pages + 4096, 0), (U64)-14);
  Pages[4095] = 'x';
  CHECK(raw(FstatAt, F, (U64)Pages + 4095, 1, 0), (U64)-14);
  CHECK(Out.Before, 0xa5a5a5a5a5a5a5a5UL);
  CHECK(Out.After, 0xa5a5a5a5a5a5a5a5UL);
  return 0;
}

U64 files_status_unsupported(U64 Mode, U64 Buffer) {
  if (Mode == 1)
    return raw(Fstat, 1, Buffer, 0, 0);
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  if (Mode == 0)
    return raw(Fstat, F, Buffer, 0, 0);
  CHECK(raw(Protect, Buffer + (Mode == 2 ? 4096 : 0), 4096, 1, 0), 0);
  return raw(Fstat, F, Buffer + 4096 - 64, 0, 0);
}

U64 files_unsupported(U64 Mode) {
  if (Mode == 0)
    return raw(OpenAt, (U64)-100, (U64)Path, 0, 0);
  if (Mode == 1)
    return raw(OpenAt, (U64)-100, (U64)Path, 0100, 0);
  if (Mode == 2)
    return raw(OpenAt, (U64)-100, (U64) "fixture/data", 0, 0);
  if (Mode == 3)
    return raw(OpenAt, (U64)-100, (U64) "/fixture/../fixture/data", 0, 0);
  if (Mode == 4)
    return raw(Read, 0, (U64)Pages, 1, 0);
  if (Mode == 5)
    return raw(OpenAt, 0, (U64) "/fixture", 0, 0);
  if (Mode == 7)
    return raw(OpenAt, 0, (U64) "/fixture/new", 0100, 0);
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  return raw(Seek, F, 0, 3, 0);
}

// Targets are supplied as raw bytes: /x, a non-ASCII byte, then y.
static const char LinkPath[] = "/links/raw";
U64 files_links(void) {
  U64 F = raw(OpenAt, 0, (U64)Path, 0, 0);
  CHECK(F, 3);
  CHECK(raw(Read, F, (U64)Pages + 16, 1, 0), 1);
  for (unsigned I = 0; I < 16; ++I)
    Pages[I] = 0xa5;
  CHECK(raw(ReadlinkAt, (U64)-1, (U64)LinkPath, (U64)Pages, 16), 4);
  CHECK(Pages[0], '/');
  CHECK(Pages[1], 'x');
  CHECK(Pages[2], 0xff);
  CHECK(Pages[3], 'y');
  CHECK(Pages[4], 0xa5);
  CHECK(
      raw(ReadlinkAt, 99, (U64)LinkPath, (U64)Pages + 8, 0x1234567800000002UL),
      2);
  CHECK(Pages[8], '/');
  CHECK(Pages[9], 'x');
  CHECK(Pages[10], 0xa5);
  CHECK(raw(ReadlinkAt, 99, 1, 1, 0x1234567800000000UL), (U64)-22);
  CHECK(raw(ReadlinkAt, 99, 1, 1, 0x80000000), (U64)-22);
  CHECK(raw(ReadlinkAt, 99, 1, 1, (U64)-1), (U64)-22);
  CHECK(raw(ReadlinkAt, 99, 1, 1, 1), (U64)-14);
  CHECK(raw(ReadlinkAt, 99, (U64) "/absent", 1, 1), (U64)-2);
  CHECK(raw(ReadlinkAt, 99, (U64)Path, 1, 1), (U64)-22);
  CHECK(raw(ReadlinkAt, 99, (U64) "/links/", 1, 1), (U64)-22);
  CHECK(raw(ReadlinkAt, 99, (U64) "/fixture/data/child", 1, 1), (U64)-20);
  CHECK(raw(ReadlinkAt, 99, (U64) "", 1, 1), (U64)-9);
  CHECK(raw(ReadlinkAt, 0x1234567800000000UL | F, (U64) "", 1, 1), (U64)-2);
  CHECK(raw(ReadlinkAt, (U64)-100, (U64) "", 1, 1), (U64)-2);
  CHECK(raw(ReadlinkAt, 99, (U64)LinkPath, 1, 1), (U64)-14);
  CHECK(raw(FaccessAt, 99, (U64) "/links/", 0, 0), 0);
  CHECK(raw(MkdirAt, 99, (U64)LinkPath, 0777, 0), (U64)-17);
  CHECK(raw(Read, F, (U64)Pages + 16, 1, 0), 1);
  CHECK(Pages[16], 0xff);
  CHECK(raw(Close, F, 0, 0, 0), 0);
#if !defined(__aarch64__)
  CHECK(raw(89, (U64)LinkPath, (U64)Pages, 4, 0), 4);
  CHECK(raw(89, (U64) "", 1, 1, 0), (U64)-2);
#endif
  // Import the whole pathname before writing an overlapping output buffer.
  for (unsigned I = 0; I < sizeof(LinkPath); ++I)
    Pages[I] = LinkPath[I];
  CHECK(raw(ReadlinkAt, 99, (U64)Pages, (U64)Pages, 4), 4);
  CHECK(Pages[0], '/');
  CHECK(Pages[1], 'x');
  CHECK(Pages[2], 0xff);
  CHECK(Pages[3], 'y');
  CHECK(Pages[4], 'k');
  // Only the copied prefix must be writable, even for a much larger size.
  for (unsigned I = 0; I < sizeof(LinkPath); ++I)
    Pages[4096 - sizeof(LinkPath) + I] = LinkPath[I];
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 0, 0), 0);
  CHECK(raw(ReadlinkAt, 99, (U64)Pages + 4096 - sizeof(LinkPath),
            (U64)Pages + 4092, 0x7fffffff),
        4);
  CHECK(Pages[4095], 'y');
  Pages[4095] = '/';
  CHECK(raw(ReadlinkAt, 99, (U64)Pages + 4095, 1, 1), (U64)-14);
  CHECK(raw(Protect, (U64)Pages + 4096, 4096, 3, 0), 0);
  for (unsigned I = 0; I < 4096; ++I)
    Pages[I] = 'x';
  CHECK(raw(ReadlinkAt, 99, (U64)Pages, 1, 1), (U64)-36);
  return 0;
}

U64 files_links_pending(U64 Mode, U64 Output) {
  if (Mode == 0)
    return raw(ReadlinkAt, 99, (U64) "/links/raw/", Output, 4);
  if (Mode == 1)
    return raw(ReadlinkAt, 99, (U64) "/links/raw/child", Output, 4);
  if (Mode == 2)
    return raw(ReadlinkAt, 99, (U64) "relative", Output, 4);
  if (Mode == 3)
    return raw(FaccessAt, 99, (U64)LinkPath, 0, 0);
  if (Mode == 4)
    return raw(OpenAt, 99, (U64)LinkPath, 0, 0);
  if (Mode == 5 || Mode == 6)
    return raw(FstatAt, 99, (U64)LinkPath, Output, Mode == 5 ? 0 : 0x100);
  if (Mode == 7)
    return raw(StatFS, (U64)LinkPath, Output, 0, 0);
  if (Mode == 8)
    return raw(MkdirAt, 99, (U64) "/links/raw/child", 0777, 0);
  if (Mode == 9)
    return raw(ReadlinkAt, 0, (U64) "", Output, 4);
  if (Mode == 10) {
    CHECK(raw(Protect, Output + 4096, 4096, 0, 0), 0);
    return raw(ReadlinkAt, 99, (U64)LinkPath, Output + 4094, 4);
  }
  return raw(Mode == 11 ? GetXAttr : LGetXAttr, (U64)LinkPath,
             (U64) "user.fixture", Output, 4);
}

#if defined(__ANDROID__)
extern int open(const char *, int, ...);
extern int open64(const char *, int, ...);
extern int openat(int, const char *, int, ...);
extern int openat64(int, const char *, int, ...);
extern I64 read(int, void *, U64);
extern I64 __read_chk(int, void *, U64, U64);
extern int close(int);
extern I64 lseek(int, I64, int);
extern I64 lseek64(int, I64, int);
extern int fstat(int, struct FileStatus *);
extern int fstat64(int, struct FileStatus *);
extern int fstatat(int, const char *, struct FileStatus *, int);
extern int fstatat64(int, const char *, struct FileStatus *, int);
extern int stat(const char *, struct FileStatus *);
extern int stat64(const char *, struct FileStatus *);
extern int lstat(const char *, struct FileStatus *);
extern int lstat64(const char *, struct FileStatus *);
extern int access(const char *, int);
extern int faccessat(int, const char *, int, int);
extern int mkdir(const char *, U32);
extern int mkdirat(int, const char *, U32);
extern I64 syscall(I64, ...);
extern int *__errno(void);
extern I64 readlink(const char *, void *, U64);
extern I64 readlinkat(int, const char *, void *, U64);
U64 files_repeated_slashes_bionic(void) {
  *__errno() = 73;
  int F = open("///fixture////data", 0);
  CHECK(F, 3);
  CHECK(*__errno(), 73);
  CHECK(read(F, Pages, sizeof(Expected)), sizeof(Expected));
  for (unsigned I = 0; I < sizeof(Expected); ++I)
    CHECK(Pages[I], Expected[I]);
  CHECK(access("//fixture//data", 0), 0);
  CHECK(faccessat(-1, "/fixture///data", 0, 0), 0);
  CHECK(*__errno(), 73);
  CHECK(close(F), 0);
  CHECK(openat(-1, "//fixture//missing", 0), -1);
  CHECK(*__errno(), 2);
  CHECK(access("//fixture//data///", 0), -1);
  CHECK(*__errno(), 20);
  CHECK(mkdir("//fixture//data///", 0777), -1);
  CHECK(*__errno(), 17);
  return 0;
}

U64 files_create_directories(U64 Mode) {
  struct FileStatus Parent, Child, Again;
  int F = open("/made", 0);
  CHECK(F, 3);
  *__errno() = 73;
  U64 R = Mode == 0   ? (U64)mkdir("//made//child///", 07777)
          : Mode == 1 ? (U64)syscall(MkdirAt, -1UL, "/made/child", 07777UL)
                      : raw(MkdirAt, -1UL, (U64) "/made/child", 07777, 0);
  CHECK(R, 0);
  CHECK(*__errno(), 73);
  CHECK(fstat(F, &Parent), 0);
  CHECK(Parent.Links, 3);
  CHECK(Parent.Size, 60);
  CHECK(Parent.Modification.Seconds, 100);
  CHECK(Parent.Change.Seconds, 100);
  CHECK(Parent.Access.Seconds, 3);
  int D = open("/made/child", 0);
  CHECK(D, 4);
  CHECK(fstat(D, &Child), 0);
  CHECK(Child.Mode, 0043755);
  CHECK(Child.UID, 1000);
  CHECK(Child.GID, 1234);
  CHECK(Child.Links, 2);
  CHECK(Child.Size, 40);
  CHECK(Child.Blocks, 0);
  CHECK(Child.BlockSize, 4096);
  CHECK(Child.Access.Seconds, 100);
  CHECK(Child.Access.Nanoseconds, 123);
  CHECK(mkdirat(-1, "/made/child", 0), -1);
  CHECK(*__errno(), 17);
  CHECK(fstat(D, &Again), 0);
  for (unsigned I = 0; I < sizeof(Child); ++I)
    CHECK(((unsigned char *)&Child)[I], ((unsigned char *)&Again)[I]);
  CHECK(mkdirat(-1, "/made/child/deep", 0700), 0);
  CHECK(fstat(D, &Child), 0);
  CHECK(Child.Links, 3);
  CHECK(Child.Size, 60);
  CHECK(close(D), 0);
  CHECK(close(F), 0);
  CHECK(access("/made/child/deep", 0), 0);
  return 0;
}

extern char *getcwd(char *, U64);
extern void free(void *);
static int cwd_text(const char *p) {
  static const char Text[] = "/fixture";
  if (!p)
    return 0;
  for (unsigned I = 0; I < sizeof(Text); ++I)
    if (p[I] != Text[I])
      return 0;
  return 1;
}
U64 files_cwd_bionic(void) {
  char Text[24];
  for (unsigned I = 0; I < sizeof(Text); ++I)
    Text[I] = 'X';
  *__errno() = 73;
  CHECK(getcwd(Text, sizeof(Text)), Text);
  CHECK(cwd_text(Text), 1);
  CHECK(Text[9], 'X');
  CHECK(*__errno(), 73);
  CHECK(getcwd(Text, 0), 0);
  CHECK(*__errno(), 22);
  CHECK(getcwd((void *)1, 8), 0);
  CHECK(*__errno(), 34);
  CHECK(getcwd((void *)1, 9), 0);
  CHECK(*__errno(), 14);
  CHECK(getcwd((void *)0, 8), 0);
  CHECK(*__errno(), 34);
  *__errno() = 79;
  char *Allocated = getcwd((void *)0, 24);
  CHECK(cwd_text(Allocated), 1);
  CHECK(*__errno(), 79);
  free(Allocated);
  Allocated = getcwd((void *)0, 0);
  CHECK(cwd_text(Allocated), 1);
  CHECK(*__errno(), 79);
  free(Allocated);
  CHECK(getcwd((void *)0, ~0UL), 0);
  CHECK(*__errno(), 12);
  *__errno() = 83;
  CHECK(raw(17, (U64)Text, 9, 0, 0), 9);
  CHECK(*__errno(), 83);
  CHECK(syscall(17, Text, 8UL), -1);
  CHECK(*__errno(), 34);
  return 0;
}
U64 files_cwd_unknown(void) {
  return (U64)getcwd((char *)Pages, sizeof(Pages));
}

U64 files_links_bionic(void) {
  for (unsigned Route = 0; Route != 3; ++Route) {
    *__errno() = 73;
    Pages[4] = 0xa5;
    I64 R = Route == 0   ? readlink(LinkPath, Pages, 16)
            : Route == 1 ? readlinkat(-1, LinkPath, Pages, 16)
                         : syscall(ReadlinkAt, -1UL, LinkPath, Pages, 16UL);
    CHECK(R, 4);
    CHECK(*__errno(), 73);
    CHECK(Pages[4], 0xa5);
    CHECK(Pages[0], '/');
    CHECK(Pages[1], 'x');
    CHECK(Pages[2], 0xff);
    CHECK(Pages[3], 'y');
    R = Route == 0   ? readlink((void *)1, (void *)1, 0)
        : Route == 1 ? readlinkat(-1, (void *)1, (void *)1, 0)
                     : syscall(ReadlinkAt, -1UL, 1UL, 1UL, 0UL);
    CHECK(R, -1);
    CHECK(*__errno(), 22);
    CHECK(readlink("/missing", (void *)1, 1), -1);
    CHECK(*__errno(), 2);
  }
  return 0;
}

U64 files_open_observed_errors_bionic(void) {
  for (unsigned Route = 0; Route != 4; ++Route) {
    *__errno() = 77;
    I64 Result = Route == 0   ? open(Path, DirectoryOnly | 02000000)
                 : Route == 1 ? open64(Path, DirectoryOnly)
                 : Route == 2 ? openat(-1, Path, DirectoryOnly | DirectIO)
                              : openat64(-1, Path, DirectoryOnly);
    CHECK(Result, -1);
    CHECK(*__errno(), 20);
    Result = Route == 0   ? open("/fixture/missing", DirectIO | 02000000)
             : Route == 1 ? open64("/fixture/missing", DirectIO)
             : Route == 2 ? openat(-1, "/fixture/missing", DirectIO)
                          : openat64(-1, "/fixture/missing", DirectIO);
    CHECK(Result, -1);
    CHECK(*__errno(), 2);
    CHECK(raw(OpenAt, 0, 1, DirectoryOnly, 0), (U64)-14);
    CHECK(*__errno(), 2);
    int F = openat(-1, Path, 0);
    CHECK(F, 3);
    CHECK(*__errno(), 2);
    CHECK(open("/fixture/missing", DirectoryOnly), -1);
    CHECK(*__errno(), 24);
    CHECK(open(Path, DirectIO), -1);
    CHECK(*__errno(), 24);
    CHECK(close(F), 0);
    CHECK(*__errno(), 24);
  }
  return 0;
}
extern I64 getxattr(const char *, const char *, void *, U64);
extern I64 lgetxattr(const char *, const char *, void *, U64);
extern I64 fgetxattr(int, const char *, void *, U64);
U64 files_xattr_errors_bionic(U64 NameFirst) {
  for (unsigned Route = 0; Route != 2; ++Route) {
    *__errno() = 91;
    CHECK(raw(Route ? LGetXAttr : GetXAttr, (U64) "/fixture/missing",
              (U64) "user.fixture", 1, (U64)-1),
          (U64)-2);
    CHECK(*__errno(), 91);
    I64 R = Route ? lgetxattr("/fixture/missing", "user.fixture", (void *)1, -1)
                  : getxattr("/fixture/missing", "user.fixture", (void *)1, -1);
    CHECK(R, -1);
    CHECK(*__errno(), 2);
    R = Route ? lgetxattr("/fixture/missing", "", (void *)1, 0)
              : getxattr("/fixture/missing", "", (void *)1, 0);
    CHECK(R, -1);
    CHECK(*__errno(), NameFirst ? 34 : 2);
    R = Route ? lgetxattr((void *)1, "", (void *)1, 0)
              : getxattr((void *)1, "", (void *)1, 0);
    CHECK(R, -1);
    CHECK(*__errno(), NameFirst ? 34 : 14);
    R = Route ? lgetxattr(Path, "", (void *)1, 0)
              : getxattr(Path, "", (void *)1, 0);
    CHECK(R, -1);
    CHECK(*__errno(), 34);
  }
  CHECK(fgetxattr(-1, "", (void *)1, 0), -1);
  CHECK(*__errno(), NameFirst ? 34 : 9);
  CHECK(fgetxattr(-1, "user.fixture", (void *)1, 0), -1);
  CHECK(*__errno(), 9);
  int F = open(Path, 0);
  CHECK(F, 3);
  CHECK(*__errno(), 9);
  CHECK(fgetxattr(F, "", (void *)1, 0), -1);
  CHECK(*__errno(), 34);
  CHECK(close(F), 0);
  CHECK(*__errno(), 34);
  return 0;
}
U64 files_xattr_unobserved_bionic(U64 Mode) {
  if (Mode == 0)
    getxattr(Path, "user.fixture", (void *)1, 0);
  else if (Mode == 1)
    lgetxattr("/fixture///", "user.fixture", (void *)1, 0);
  else
    fgetxattr(1, "user.fixture", (void *)1, 0);
  return 99;
}
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

U64 files_read_chk_sequence(U64 Mode) {
  I64 (*Checked)(int, void *, U64, U64) = __read_chk;
  void *Library = 0;
  if (Mode) {
    Library = dlopen("libfiles.so", 2);
    CHECK(Library != 0, 1);
    Checked = dlsym(Library, "__read_chk");
    CHECK(Checked != 0, 1);
    if (Mode == 2)
      CHECK(dlclose(Library), 0);
  }
  int FD = open(Path, 0);
  CHECK(FD, 3);
  *__errno() = 77;
  CHECK(Checked(FD, (void *)1, 0, 0), 0);
  CHECK(*__errno(), 77);
  CHECK(Checked(FD, (void *)1, 1, 1), -1);
  CHECK(*__errno(), 14);
  *__errno() = 77;
  CHECK(Checked(FD, Pages, 2, 2), 2);
  CHECK(raw(Read, FD, (U64)(Pages + 2), 2, 0), 2);
  CHECK(Checked(FD, Pages + 4, 4096, ~0UL), 2);
  for (unsigned I = 0; I != sizeof(Expected); ++I)
    CHECK(Pages[I], Expected[I]);
  CHECK(Checked(FD, Pages, sizeof(Expected), sizeof(Expected)), 0);
  CHECK(*__errno(), 77);
  CHECK(close(FD), 0);
  CHECK(Checked(FD, Pages, 1, 1), -1);
  CHECK(*__errno(), 9);
  if (Mode)
    CHECK(dlclose(Library), 0);
  return 0;
}

U64 files_read_chk_bounds(void *Buffer, U64 Count, U64 Capacity) {
  *__errno() = 77;
  return __read_chk(99, Buffer, Count, Capacity);
}

static void *reader(void *Arg) {
  U64 FD = (U64)Arg;
  *__errno() = 82;
  U64 R = read(FD, Pages + 8, 2);
  return (void *)(R == 2 && *__errno() == 82 ? 0UL : 1UL);
}

U64 files_bionic(void) {
  *__errno() = 73;
  int (*volatile Open)(const char *, int, ...) = open;
  int F = Open(Path, 0, 0777);
  CHECK(F, 3);
  CHECK(*__errno(), 73);
  CHECK(raw(Read, F, (U64)Pages, 1, 0), 1);
  CHECK(syscall(63L, (U64)F, Pages + 1, 1UL), 1);
  U64 Thread;
  void *Value = (void *)1;
  CHECK(pthread_create(&Thread, 0, reader, (void *)(U64)F), 0);
  CHECK(pthread_join(Thread, &Value), 0);
  CHECK((U64)Value, 0);
  CHECK(Pages[8], 0x41);
  CHECK(Pages[9], 0x0a);
  CHECK(*__errno(), 73);
  CHECK(lseek(F, 0, 1), 4);
  CHECK(lseek64(F, -1, 2), 5);
  CHECK(read(F, Pages + 2, 8), 1);
  CHECK(Pages[0], 0);
  CHECK(Pages[1], 0xff);
  CHECK(Pages[2], 0x5a);
  CHECK(lseek64(F, 0x7fffffffffffffffL, 0), 0x7fffffffffffffffL);
  CHECK(read(F, Pages, 1), -1);
  CHECK(*__errno(), 22);
  CHECK(read(F, (void *)UserLimit, 0), 0);
  CHECK(*__errno(), 22);
  CHECK(read(F, (void *)UserLimit, 1), -1);
  CHECK(*__errno(), 14);
  CHECK(lseek64(F, 0, 1), 0x7fffffffffffffffL);
  CHECK(close(F), 0);
  CHECK(read(F, Pages, 1), -1);
  CHECK(*__errno(), 9);
  CHECK(open64(Path, 0), 3);
  CHECK(openat(-1, Path, 0), 4);
  CHECK(openat64(-1, Path, 0), 5);
  CHECK(open("/absent", 0), -1);
  CHECK(*__errno(), 2);
  return 0;
}

static void *status_reader(void *Arg) {
  struct FileStatus Status;
  *__errno() = 82;
  if (fstat64((U64)Arg, &Status) || verify_status(&Status) ||
      *__errno() != 82 || fstat(-1, &Status) != -1 || *__errno() != 9)
    return (void *)1;
  return 0;
}

U64 files_status_bionic(void) {
  struct FileStatus Status;
  *__errno() = 73;
  int F = open(Path, 0);
  CHECK(F, 3);
  CHECK(read(F, Pages, 1), 1);
  CHECK(fstat(F, &Status), 0);
  CHECK(verify_status(&Status), 0);
  CHECK(*__errno(), 73);
  CHECK(syscall(80L, (U64)F | 0x1234567800000000UL, &Status), 0);
  CHECK(verify_status(&Status), 0);
  CHECK(*__errno(), 73);
  U64 Thread;
  void *Value = (void *)1;
  CHECK(pthread_create(&Thread, 0, status_reader, (void *)(U64)F), 0);
  CHECK(pthread_join(Thread, &Value), 0);
  CHECK((U64)Value, 0);
  CHECK(*__errno(), 73);
  CHECK(lseek(F, 0, 1), 1);
  CHECK(fstat64(F, (struct FileStatus *)1), -1);
  CHECK(*__errno(), 14);
  CHECK(fstat(F, &Status), 0);
  CHECK(*__errno(), 14);
  CHECK(verify_status(&Status), 0);
  CHECK(close(F), 0);
  CHECK(fstat(F, (struct FileStatus *)1), -1);
  CHECK(*__errno(), 9);
  CHECK(open(Path, 0), F);
  CHECK(fstat64(F, &Status), 0);
  CHECK(verify_status(&Status), 0);
  return 0;
}

extern int statfs(const char *, void *);
extern int statfs64(const char *, void *);
extern int fstatfs(int, void *);
extern int fstatfs64(int, void *);

U64 files_filesystem_status_call(U64 Route, U64 Descriptor, U64 Input,
                                 U64 Output, U64 ObservedErrno) {
  *__errno() = 83;
  I64 Result;
  switch (Route) {
  case 0:
    Result = Descriptor ? fstatfs(Input, (void *)Output)
                        : statfs((const char *)Input, (void *)Output);
    break;
  case 1:
    Result = Descriptor ? fstatfs64(Input, (void *)Output)
                        : statfs64((const char *)Input, (void *)Output);
    break;
  case 2:
    Result = syscall(Descriptor ? FstatFS : StatFS, Input, Output);
    break;
  default:
    Result = raw(Descriptor ? FstatFS : StatFS, Input, Output, 0, 0);
    break;
  }
  *(U64 *)ObservedErrno = *__errno();
  return Result;
}

U64 files_filesystem_status_bionic(void) {
  U64 Output = 0x123456789abcdef0UL, ObservedErrno;
  for (U64 Route = 0; Route < 4; ++Route) {
    CHECK(files_filesystem_status_call(Route, 0, (U64) "/missing/",
                                       (U64)&Output, (U64)&ObservedErrno),
          Route == 3 ? (U64)-2 : -1UL);
    CHECK(ObservedErrno, Route == 3 ? 83 : 2);
    CHECK(files_filesystem_status_call(Route, 1, 0x87654321ffffffffUL,
                                       (U64)&Output, (U64)&ObservedErrno),
          Route == 3 ? (U64)-9 : -1UL);
    CHECK(ObservedErrno, Route == 3 ? 83 : 9);
    CHECK(Output, 0x123456789abcdef0UL);
  }
  return 0;
}

U64 files_filesystem_status_live(U64 Route, U64 Output, U64 ObservedErrno) {
  U64 F = open(Path, 0);
  CHECK(F, 3);
  return files_filesystem_status_call(Route, 1, F | 0xabcdef1200000000UL,
                                      Output, ObservedErrno);
}

U64 files_filesystem_status_dynamic(U64 Closed, U64 Alias, U64 Descriptor) {
  void *Library = dlopen("libfiles.so", 2);
  CHECK(Library != 0, 1);
  const char *Name = Descriptor ? (Alias ? "fstatfs64" : "fstatfs")
                                : (Alias ? "statfs64" : "statfs");
  void *Call = dlsym(Library, Name);
  CHECK(Call != 0, 1);
  if (Closed)
    CHECK(dlclose(Library), 0);
  U64 Output = 0x123456789abcdef0UL;
  int Result =
      Descriptor ? ((int (*)(int, void *))Call)(-1, &Output)
                 : ((int (*)(const char *, void *))Call)("/missing/", &Output);
  CHECK(Result, -1);
  CHECK(*__errno(), Descriptor ? 9 : 2);
  CHECK(Output, 0x123456789abcdef0UL);
  return 0;
}

U64 files_open_at_call(U64 Route, U64 BasePath, U64 Name, U64 Flags, U64 Mode,
                       U64 ObservedErrno) {
  I64 Directory = BasePath ? open((const char *)BasePath, 0) : -1;
  if (BasePath)
    CHECK(Directory, 3);
  *__errno() = 83;
  I64 Result;
  switch (Route) {
  case 0:
    Result = openat(Directory, (const char *)Name, Flags, Mode);
    break;
  case 1:
    Result = openat64(Directory, (const char *)Name, Flags, Mode);
    break;
  case 2:
    Result = syscall(OpenAt, Directory, Name, Flags, Mode);
    break;
  default:
    Result = raw(OpenAt, Directory, Name, Flags, Mode);
    break;
  }
  *(U64 *)ObservedErrno = *__errno();
  return Result;
}

U64 files_status_at_call(U64 Route, U64 Directory, U64 Name, U64 Output,
                         U64 Flags, U64 ObservedErrno) {
  *__errno() = 83;
  I64 Result;
  switch (Route) {
  case 0:
    Result = fstatat(Directory, (const char *)Name, (struct FileStatus *)Output,
                     Flags);
    break;
  case 1:
    Result = fstatat64(Directory, (const char *)Name,
                       (struct FileStatus *)Output, Flags);
    break;
  case 2:
    Result = syscall(FstatAt, Directory, Name, Output, Flags);
    break;
  default:
    Result = raw(FstatAt, Directory, Name, Output, Flags);
    break;
  }
  *(U64 *)ObservedErrno = *__errno();
  return Result;
}

U64 files_path_status_call(U64 Alias, U64 Name, U64 Output, U64 ObservedErrno,
                           U64 Dynamic) {
  void *Library = 0;
  int (*Function)(const char *, struct FileStatus *) = 0;
  if (Dynamic) {
    const char *Names[] = {"stat", "stat64", "lstat", "lstat64"};
    Library = dlopen("libfiles.so", 2);
    CHECK(Library != 0, 1);
    Function = dlsym(Library, Names[Alias]);
    CHECK(Function != 0, 1);
    if (Dynamic == 2)
      CHECK(dlclose(Library), 0);
  }
  *__errno() = 83;
  I64 Result;
  if (Dynamic) {
    Result = Function((const char *)Name, (struct FileStatus *)Output);
  } else {
    switch (Alias) {
    case 0:
      Result = stat((const char *)Name, (struct FileStatus *)Output);
      break;
    case 1:
      Result = stat64((const char *)Name, (struct FileStatus *)Output);
      break;
    case 2:
      Result = lstat((const char *)Name, (struct FileStatus *)Output);
      break;
    default:
      Result = lstat64((const char *)Name, (struct FileStatus *)Output);
      break;
    }
  }
  *(U64 *)ObservedErrno = *__errno();
  if (Dynamic == 1)
    CHECK(dlclose(Library), 0);
  return Result;
}

U64 files_status_at_bionic(void) {
  struct FileStatus Status;
  U64 ObservedErrno;
  for (U64 Route = 0; Route < 4; ++Route) {
    CHECK(files_status_at_call(Route, (U64)-100, (U64)Path, (U64)&Status, 0,
                               (U64)&ObservedErrno),
          0);
    CHECK(ObservedErrno, 83);
    CHECK(verify_status(&Status), 0);
    CHECK(files_status_at_call(Route, (U64)-100, (U64) "/missing", (U64)&Status,
                               0, (U64)&ObservedErrno),
          Route == 3 ? (U64)-2 : (U64)-1);
    CHECK(ObservedErrno, Route == 3 ? 83 : 2);
    CHECK(verify_status(&Status), 0);
  }
  return 0;
}

U64 files_status_at_dynamic(U64 Closed, U64 Alias) {
  void *Library = dlopen("libfiles.so", 2);
  CHECK(Library != 0, 1);
  int (*Call)(int, const char *, struct FileStatus *, int) =
      dlsym(Library, Alias ? "fstatat64" : "fstatat");
  CHECK(Call != 0, 1);
  if (Closed)
    CHECK(dlclose(Library), 0);
  struct FileStatus Status;
  CHECK(Call(-1, Path, &Status, 0), 0);
  return verify_status(&Status);
}

U64 files_status_dynamic(U64 Closed) {
  struct FileStatus Status;
  int F = open(Path, 0);
  CHECK(F, 3);
  void *Library = dlopen("libfiles.so", 2);
  if (!Library)
    return 99;
  int (*StatusCall)(int, struct FileStatus *) = dlsym(Library, "fstat64");
  if (!StatusCall)
    return 98;
  if (Closed)
    CHECK(dlclose(Library), 0);
  CHECK(StatusCall(F, &Status), 0);
  return verify_status(&Status);
}

U64 files_dynamic(U64 Closed) {
  void *Library = dlopen("libfiles.so", 2);
  if (!Library)
    return 99;
  int (*Open)(const char *, int, ...) = dlsym(Library, "open");
  if (!Open)
    return 98;
  if (Closed)
    CHECK(dlclose(Library), 0);
  return Open(Path, 0);
}

static void *access_reader(void *Arg) {
  *__errno() = 82;
  if (access(Arg, 0) || *__errno() != 82 ||
      faccessat(-1, "/absent", 0, 0) != -1 || *__errno() != 2)
    return (void *)1;
  return 0;
}

U64 files_access_bionic(void) {
  *__errno() = 73;
  CHECK(access(Path, 0), 0);
  CHECK(*__errno(), 73);
  CHECK(faccessat(-1, Path, 0, 0), 0);
  CHECK(*__errno(), 73);
  CHECK(syscall(48L, -1UL, Path, 0x1234567800000000UL, 99UL), 0);
  CHECK(*__errno(), 73);
  U64 Thread;
  void *Value = (void *)1;
  CHECK(pthread_create(&Thread, 0, access_reader, (void *)Path), 0);
  CHECK(pthread_join(Thread, &Value), 0);
  CHECK((U64)Value, 0);
  CHECK(*__errno(), 73);
  CHECK(access("/absent", 0), -1);
  CHECK(*__errno(), 2);
  CHECK(faccessat(-1, (const char *)1, 0, 0x100), -1);
  CHECK(*__errno(), 22);
  CHECK(faccessat(-1, (const char *)1, 0, 0), -1);
  CHECK(*__errno(), 14);
  CHECK(access((const char *)1, 9), -1);
  CHECK(*__errno(), 22);
  CHECK(access(Path, 0), 0);
  CHECK(*__errno(), 22);
  return 0;
}

U64 files_access_dynamic(U64 Closed, U64 At) {
  void *Library = dlopen("libfiles.so", 2);
  if (!Library)
    return 99;
  void *Call = dlsym(Library, At ? "faccessat" : "access");
  if (!Call)
    return 98;
  if (Closed)
    CHECK(dlclose(Library), 0);
  if (At)
    return ((int (*)(int, const char *, int, int))Call)(-1, Path, 0, 0);
  return ((int (*)(const char *, int))Call)(Path, 0);
}
static void *directory_reader(void *Arg) {
  *__errno() = 82;
  if (mkdir(Arg, 0777) != -1 || *__errno() != 17 ||
      mkdirat(-1, "/missing/child", 0777) != -1 || *__errno() != 2)
    return (void *)1;
  return 0;
}

U64 files_directory_bionic(void) {
  *__errno() = 73;
  U64 Thread;
  void *Value = (void *)1;
  CHECK(pthread_create(&Thread, 0, directory_reader, (void *)Path), 0);
  CHECK(pthread_join(Thread, &Value), 0);
  CHECK((U64)Value, 0);
  CHECK(*__errno(), 73);
  CHECK(mkdir(Path, 0777), -1);
  CHECK(*__errno(), 17);
  CHECK(mkdirat(-1, "/missing/child", 0777), -1);
  CHECK(*__errno(), 2);
  CHECK(syscall(34L, -1UL, "/fixture/data/child", 0777UL), -1);
  CHECK(*__errno(), 20);
  CHECK(mkdir((const char *)1, (U32)-1), -1);
  CHECK(*__errno(), 14);
  CHECK(access("/fixture", 0), 0);
  CHECK(*__errno(), 14);
  return 0;
}

U64 files_trailing_paths_bionic(void) {
  const char *Name = "/fixture/data///";
  *__errno() = 83;
  CHECK(open(Name, 0), -1);
  CHECK(*__errno(), 20);
  CHECK(open64(Name, 0), -1);
  CHECK(*__errno(), 20);
  CHECK(openat(-1, Name, 0), -1);
  CHECK(*__errno(), 20);
  CHECK(openat64(-1, Name, 0), -1);
  CHECK(*__errno(), 20);
  CHECK(access(Name, 0), -1);
  CHECK(*__errno(), 20);
  CHECK(faccessat(-1, Name, 0, 0), -1);
  CHECK(*__errno(), 20);
  CHECK(fstatat(-1, Name, (struct FileStatus *)1, 0), -1);
  CHECK(*__errno(), 20);
  CHECK(fstatat64(-1, "/missing/", (struct FileStatus *)1, 0), -1);
  CHECK(*__errno(), 2);
  CHECK(mkdir(Name, 0777), -1);
  CHECK(*__errno(), 17);
  CHECK(mkdirat(-1, Name, 0777), -1);
  CHECK(*__errno(), 17);
  CHECK(syscall(FstatAt, -1UL, "/missing/", 1UL, 0UL), -1);
  CHECK(*__errno(), 2);
  CHECK(syscall(MkdirAt, -1UL, Name, 0777UL), -1);
  CHECK(*__errno(), 17);
  CHECK(access("/fixture/", 0), 0);
  CHECK(*__errno(), 17);
  CHECK(open(Path, 0), 3);
  CHECK(*__errno(), 17);
  return 0;
}

U64 files_links_dynamic(U64 Closed, U64 At) {
  void *Library = dlopen("libfiles.so", 2);
  if (!Library)
    return 99;
  void *Call = dlsym(Library, At ? "readlinkat" : "readlink");
  if (!Call)
    return 98;
  if (Closed)
    CHECK(dlclose(Library), 0);
  I64 N = At ? ((I64 (*)(int, const char *, void *, U64))Call)(-1, LinkPath,
                                                               Pages, 4)
             : ((I64 (*)(const char *, void *, U64))Call)(LinkPath, Pages, 4);
  CHECK(N, 4);
  CHECK(Pages[2], 0xff);
  return 0;
}

U64 files_directory_dynamic(U64 Closed, U64 At) {
  void *Library = dlopen("libfiles.so", 2);
  if (!Library)
    return 99;
  void *Call = dlsym(Library, At ? "mkdirat" : "mkdir");
  if (!Call)
    return 98;
  if (Closed)
    CHECK(dlclose(Library), 0);
  if (At)
    CHECK(((int (*)(int, const char *, U32))Call)(-1, Path, 0777), -1);
  else
    CHECK(((int (*)(const char *, U32))Call)(Path, 0777), -1);
  CHECK(*__errno(), 17);
  return 0;
}

extern int rename(const char *, const char *);
extern int renameat(int, const char *, int, const char *);
U64 files_rename_bionic(U64 mode) {
  void *library = 0, *function = 0;
  if (mode >= 3) {
    library = dlopen("libfiles.so", 2);
    CHECK(library != 0, 1);
    function = dlsym(library, (mode == 4 || mode == 6) ? "renameat" : "rename");
    CHECK(function != 0, 1);
    if (mode >= 5)
      CHECK(dlclose(library), 0);
  }
  struct FileStatus Before;
  U64 fd = rename_prepare(&Before);
  CHECK(fd, 3);
  *__errno() = 77;
  I64 result =
      mode == 0   ? rename(Path, RenamedPath)
      : mode == 1 ? renameat(-1, Path, -1, RenamedPath)
      : mode == 2 ? syscall(RenameAt, -1UL, Path, -1UL, RenamedPath)
      : (mode == 4 || mode == 6)
          ? ((int (*)(int, const char *, int, const char *))function)(
                -1, Path, -1, RenamedPath)
          : ((int (*)(const char *, const char *))function)(Path, RenamedPath);
  CHECK(result, 0);
  CHECK(*__errno(), 77);
  CHECK(rename_verify(fd, &Before), 0);
  CHECK(raw(Close, fd, 0, 0, 0), 0);
  if (library)
    CHECK(dlclose(library), 0);
  return 0;
}

extern int unlink(const char *);
extern int unlinkat(int, const char *, int);
U64 files_unlink_bionic(U64 mode, U64 *out) {
  void *library = 0, *function = 0;
  if (mode >= 3) {
    library = dlopen("libfiles.so", 2);
    CHECK(library != 0, 1);
    function = dlsym(library, (mode == 4 || mode == 6) ? "unlinkat" : "unlink");
    CHECK(function != 0, 1);
    if (mode >= 5)
      CHECK(dlclose(library), 0);
  }
  const char *names[] = {"/absent", "", "/fixture/data/child", (const char *)0};
  const int errors[] = {2, 2, 20, 14};
  for (unsigned i = 0; i != 4; ++i) {
    *__errno() = 77;
    I64 result =
        mode == 0   ? unlink(names[i])
        : mode == 1 ? unlinkat(-1, names[i], 0)
        : mode == 2 ? syscall(UnlinkAt, -1UL, names[i], 0UL)
        : (mode == 4 || mode == 6)
            ? ((int (*)(int, const char *, int))function)(-1, names[i], 0)
            : ((int (*)(const char *))function)(names[i]);
    out[2 * i] = result;
    out[2 * i + 1] = *__errno();
    CHECK(result, -1);
    CHECK(*__errno(), errors[i]);
  }
  CHECK(access(Path, 0), 0);
  CHECK(*__errno(), 14);
  if (library)
    CHECK(dlclose(library), 0);
  return 0;
}

U64 files_unlink_closed_bionic(U64 mode) {
  void *library = 0, *function = 0;
  if (mode >= 3) {
    library = dlopen("libfiles.so", 2);
    CHECK(library != 0, 1);
    function = dlsym(library, (mode == 4 || mode == 6) ? "unlinkat" : "unlink");
    CHECK(function != 0, 1);
    if (mode >= 5)
      CHECK(dlclose(library), 0);
  }
  *__errno() = 77;
  I64 result = mode == 0   ? unlink(Path)
               : mode == 1 ? unlinkat(-1, Path, 0)
               : mode == 2 ? syscall(UnlinkAt, -1UL, Path, 0UL)
               : (mode == 4 || mode == 6)
                   ? ((int (*)(int, const char *, int))function)(-1, Path, 0)
                   : ((int (*)(const char *))function)(Path);
  CHECK(result, 0);
  CHECK(*__errno(), 77);
  CHECK(access(Path, 0), -1);
  CHECK(*__errno(), 2);
  CHECK(access("/fixture", 0), 0);
  CHECK(*__errno(), 2);
  CHECK(unlink(Path), -1);
  CHECK(*__errno(), 2);
  if (library)
    CHECK(dlclose(library), 0);
  return 0;
}

extern void *fopen(const char *, const char *);
extern int fclose(void *);

static U64 counted_bionic_open(U64 Mode) {
  if (Mode == 0)
    return open(Path, 0102, 0666);
  if (Mode == 1)
    return openat(-1, Path, 0102, 0666);
  if (Mode == 2)
    return syscall(OpenAt, -1UL, Path, 0102UL, 0666UL);
  if (Mode == 3) {
    void *stream = fopen(Path, "w+");
    CHECK(stream != 0, 1);
    CHECK(fclose(stream), 0);
    return open(Path, 2);
  }
  void *library = dlopen("libfiles.so", 2);
  CHECK(library != 0, 1);
  int (*create)(int, const char *, int, ...) = dlsym(library, "openat");
  CHECK(create != 0, 1);
  I64 result = create(-1, Path, 0102, 0666);
  CHECK(dlclose(library), 0);
  return result;
}

U64 files_creation_counted_bionic(U64 Mode) {
  *__errno() = 77;
  CHECK(creation_counted(counted_bionic_open, Mode), 0);
  CHECK(*__errno(), 77);
  return 0;
}

U64 files_creation_bionic(U64 mode) {
  void *library = 0, *function = 0;
  if (mode >= 3 && mode <= 6) {
    library = dlopen("libfiles.so", 2);
    CHECK(library != 0, 1);
    function = dlsym(library, (mode == 4 || mode == 6) ? "openat" : "open");
    CHECK(function != 0, 1);
    if (mode >= 5)
      CHECK(dlclose(library), 0);
  }
  struct FileStatus Before;
  U64 old = creation_prepare(&Before);
  CHECK(old, 3);
  *__errno() = 77;
  I64 created;
  if (mode == 7) {
    void *stream = fopen(Path, "w+");
    CHECK(stream != 0, 1);
    CHECK(*__errno(), 77);
    CHECK(fclose(stream), 0);
    CHECK(*__errno(), 77);
    created = open(Path, 2);
  } else {
    const int flags = 0100 | 01000 | 2;
    created =
        mode == 0   ? open(Path, flags, 0666)
        : mode == 1 ? openat(-1, Path, flags, 0666)
        : mode == 2 ? syscall(OpenAt, -1UL, Path, (U64)flags, 0666UL)
        : (mode == 4 || mode == 6)
            ? ((int (*)(int, const char *, int, ...))function)(-1, Path, flags,
                                                               0666)
            : ((int (*)(const char *, int, ...))function)(Path, flags, 0666);
  }
  CHECK(created, 4);
  CHECK(*__errno(), 77);
  CHECK(creation_verify(old, created, &Before), 0);
  if (library)
    CHECK(dlclose(library), 0);
  return 0;
}

#else
void process_main(U64 *Stack) {
  if (Stack[0] != 2)
    __builtin_trap();
  char Mode = ((const char **)(Stack + 1))[1][0];
  U64 Status = Mode == 'M'   ? files_creation_counted_raw()
               : Mode == 'N' ? files_creation_raw()
               : Mode == 'Q' ? files_rename_raw()
               : Mode == 'T' ? files_rename_reuse(0)
               : Mode == '1' ? files_rename_fdinfo(0)
               : Mode == 'e' ? files_unlink_errors()
               : Mode == 'E' ? files_unlink_pending(0)
               : Mode == 'F' ? files_unlink_pending(1)
               : Mode == 'G' ? files_unlink_pending(2)
               : Mode == 'H' ? files_unlink_pending(3)
               : Mode == 'I' ? files_unlink_pending(4)
               : Mode == 'K' ? files_unlink_pending(5)
               : Mode == 'l' ? files_links()
               : Mode == 's' ? files_sequence()
               : Mode == 'f' ? files_faults()
               : Mode == 'c' ? files_capacity()
               : Mode == 't' ? files_status()
               : Mode == 'n' ? files_status_at()
               : Mode == 'a' ? files_access()
               : Mode == 'p' ? files_access_faults()
               : Mode == 'd' ? files_directory_errors()
               : Mode == 'q' ? files_trailing_paths()
               : Mode == 'v' ? files_filesystem_status()
               : Mode == 'X' ? files_xattr_errors(0)
               : Mode == 'Y' ? files_xattr_errors(1)
               : Mode == 'U' ? files_xattr_unobserved(0)
               : Mode == 'V' ? files_xattr_unobserved(1)
               : Mode == 'W' ? files_xattr_unobserved(2)
               : Mode == 'J' ? files_xattr_unobserved(3)
               : Mode == 'o' ? files_open_observed_errors()
               : Mode == 'R' ? files_open_unobserved(0)
               : Mode == 'D' ? files_open_unobserved(1)
               : Mode == 'B' ? files_open_unobserved(2)
                             : files_unsupported(Mode - '0');
  raw(Exit, Status, 0, 0, 0);
  __builtin_trap();
}
#endif
