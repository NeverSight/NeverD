//===- android_log.c - Independent liblog ABI callers --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Freestanding callers check aggregate log_time arguments, whole output
/// buffers, independent cursors, duplicate opens and opaque handle lifetime.
typedef unsigned long U64;
typedef unsigned int U32;
struct LogTime {
  U32 Seconds, Nanoseconds;
} __attribute__((packed));
struct LogMessage {
  unsigned char Bytes[5121];
} __attribute__((aligned(4)));
extern void *android_logger_list_alloc(int, U32, int);
extern void *android_logger_list_alloc_time(int, struct LogTime, int);
extern void *android_logger_open(void *, int);
extern int android_logger_list_read(void *, struct LogMessage *);
extern void android_logger_list_free(void *);
extern void *memcpy(void *, const void *, U64);
extern void *memset(void *, int, U64);
extern int *__errno(void);
extern int mprotect(void *, U64, int);
extern int munmap(void *, U64);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int __android_log_bwrite(int, const void *, U64);

static __attribute__((noinline)) int direct_event_write(const void *Payload,
                                                        U64 Size) {
  return __android_log_bwrite(-7, Payload, Size);
}

U64 log_event_write(U64 *Out, const void *Payload, U64 Size, U64 Count,
                    U64 Dynamic) {
  if (Count > 4)
    return 1;
  void *Library = Dynamic ? dlopen("liblog.so", 2) : (void *)0;
  int (*Write)(int, const void *, U64) =
      Dynamic ? (int (*)(int, const void *, U64))dlsym(Library,
                                                       "__android_log_bwrite")
              : (void *)0;
  if (Dynamic && !Write)
    return 2;
  for (U64 I = 0; I != Count; ++I) {
    *__errno() = 73;
    Out[2 * I] = (long)(Dynamic ? Write(-7, Payload, Size)
                                : direct_event_write(Payload, Size));
    Out[2 * I + 1] = *__errno();
  }
  if (Dynamic && dlclose(Library))
    return 3;
  return 0;
}

static void capture(unsigned char *Out, int Result,
                    const struct LogMessage *M) {
  ((U64 *)Out)[0] = (long)Result;
  ((U64 *)Out)[1] = *__errno();
  memcpy(Out + 16, M, sizeof(*M));
}

U64 log_records(unsigned char *Out, U64 Mode, U64 Tail, U64 PID, U64 Start,
                U64 Mask, U64 Reads) {
  struct LogTime T = {(U32)Start, (U32)(Start >> 32)};
  void *List = Start == ~(U64)0 ? android_logger_list_alloc(Mode, Tail, PID)
                                : android_logger_list_alloc_time(Mode, T, PID);
  if (!List || Reads > 4)
    return 1;
  for (int ID = 0; ID < 6; ++ID)
    if ((Mask & (1u << ID)) && !android_logger_open(List, ID))
      return 2;
  for (U64 I = 0; I < Reads; ++I) {
    struct LogMessage M;
    memset(&M, 0x6b, sizeof(M));
    *__errno() = 73;
    int R = android_logger_list_read(List, &M);
    capture(Out + I * 8192, R, &M);
  }
  android_logger_list_free(List);
  android_logger_list_free((void *)0);
  return 0;
}

U64 log_lifecycle(unsigned char *Out) {
  void *A = android_logger_list_alloc(0x800, 0, 0);
  void *B = android_logger_list_alloc(0x800, 0, 0);
  void *First = android_logger_open(A, 0);
  if (!A || !B || !First || First != android_logger_open(A, 0) ||
      !android_logger_open(B, 0))
    return 1;
  for (unsigned I = 0; I < 4; ++I) {
    struct LogMessage M;
    if (I == 2 && !android_logger_open(A, 1))
      return 2;
    if (I == 3 && android_logger_open(A, 0) != First)
      return 3;
    *__errno() = 73;
    int R = android_logger_list_read(I == 1 ? B : A, &M);
    capture(Out + I * 8192, R, &M);
  }
  android_logger_list_free(A);
  android_logger_list_free(B);
  return 0;
}

U64 log_invalid(unsigned char *Out, U64 Which) {
  void *List = android_logger_list_alloc(0x800, 0, 0);
  *__errno() = 73;
  if (Which == 0) {
    int R = android_logger_list_read((void *)0, (void *)Out);
    return R == -22 && *__errno() == 73 ? 0 : 1;
  }
  if (Which == 1) {
    int R = android_logger_list_read(List, (void *)Out);
    android_logger_list_free(List);
    return R == -22 && *__errno() == 73 ? 0 : 2;
  }
  if (Which == 2) {
    void *R = android_logger_open(List, -1);
    android_logger_list_free(List);
    return !R && *__errno() == 73 ? 0 : 3;
  }
  void *Logger = android_logger_open(List, 0);
  if (Which == 3)
    android_logger_list_free(List);
  if (Which == 4)
    Out = (void *)1;
  if (Which == 5) {
    if (mprotect(List, 4096, 3))
      return 4;
  }
  if (Which == 6 && munmap(List, 4096))
    return 5;
  if (Which == 7 && mprotect(Logger, 4096, 3))
    return 6;
  if (Which == 8 && munmap(Logger, 4096))
    return 7;
  return android_logger_list_read(List, (void *)Out);
}

U64 log_dynamic(unsigned char *Out) {
  typedef void *(*Alloc)(int, U32, int);
  typedef void *(*AllocTime)(int, struct LogTime, int);
  typedef void *(*Open)(void *, int);
  typedef int (*Read)(void *, struct LogMessage *);
  typedef void (*Free)(void *);
  void *Library = dlopen("liblog.so", 2);
  Alloc A = (Alloc)dlsym(Library, "android_logger_list_alloc");
  AllocTime T = (AllocTime)dlsym(Library, "android_logger_list_alloc_time");
  Open O = (Open)dlsym(Library, "android_logger_open");
  Read R = (Read)dlsym(Library, "android_logger_list_read");
  Free F = (Free)dlsym(Library, "android_logger_list_free");
  if (!Library || !A || !T || !O || !R || !F)
    return 1;
  void *Empty = A(0x800, 0, 0);
  if (!Empty)
    return 2;
  F(Empty);
  struct LogTime Start = {20, 6};
  void *List = T(0x800, Start, 42);
  if (!List || !O(List, 1))
    return 3;
  struct LogMessage Message;
  *__errno() = 73;
  int Returned = R(List, &Message);
  capture(Out, Returned, &Message);
  F(List);
  return dlclose(Library);
}
