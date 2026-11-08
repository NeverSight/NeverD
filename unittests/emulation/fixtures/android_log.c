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
extern int __android_log_write(int, const char *, const char *);
extern int __android_log_buf_write(int, int, const char *, const char *);
extern void android_set_abort_message(const char *);
extern void *mmap(void *, U64, int, int, int, long);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);

extern int __android_log_print(int, const char *, const char *, ...);
extern int __android_log_buf_print(int, int, const char *, const char *, ...);
extern int __android_log_vprint(int, const char *, const char *,
                                __builtin_va_list);
typedef int (*LogPrint)(int, const char *, const char *, ...);
typedef int (*LogBufferPrint)(int, int, const char *, const char *, ...);
typedef int (*LogVPrint)(int, const char *, const char *, __builtin_va_list);

static __attribute__((noinline)) int
variadic_text_log(LogVPrint Fn, int Priority, const char *Tag,
                  const char *Format, ...) {
  __builtin_va_list Args;
  __builtin_va_start(Args, Format);
  unsigned char Before[sizeof(Args)];
  memcpy(Before, &Args, sizeof(Args));
  int Result = Fn ? Fn(Priority, Tag, Format, Args)
                  : __android_log_vprint(Priority, Tag, Format, Args);
  for (U64 I = 0; I != sizeof(Args); ++I)
    if (Before[I] != ((const unsigned char *)&Args)[I])
      return -999;
  __builtin_va_end(Args);
  return Result;
}

// Each form reaches the same liblog write with real compiler-generated
// AAPCS64 arguments, including integer overflow slots and a borrowed va_list.
U64 log_formatted(U64 *Out, U64 Form, U64 Mode, const char *Format,
                  const char *String, U64 ID, U64 Priority, const char *Tag) {
  void *Library = Form >= 3 ? dlopen("liblog.so", 2) : (void *)0;
  LogPrint Print =
      Form == 3 ? (LogPrint)dlsym(Library, "__android_log_print") : (void *)0;
  LogBufferPrint BufferPrint =
      Form == 4 ? (LogBufferPrint)dlsym(Library, "__android_log_buf_print")
                : (void *)0;
  LogVPrint VPrint =
      Form == 5 ? (LogVPrint)dlsym(Library, "__android_log_vprint") : (void *)0;
  if (Form > 5 || (Form == 3 && !Print) || (Form == 4 && !BufferPrint) ||
      (Form == 5 && !VPrint))
    return 1;
#define EMIT(...)                                                              \
  (Form == 0   ? __android_log_print(Priority, Tag, __VA_ARGS__)               \
   : Form == 1 ? __android_log_buf_print(ID, Priority, Tag, __VA_ARGS__)       \
   : Form == 3 ? Print(Priority, Tag, __VA_ARGS__)                             \
   : Form == 4 ? BufferPrint(ID, Priority, Tag, __VA_ARGS__)                   \
               : variadic_text_log(VPrint, Priority, Tag, __VA_ARGS__))
  for (U64 I = 0; I != 2; ++I) {
    *__errno() = 73;
    int Result;
    if (Mode == 0)
      Result =
          EMIT(Format, -123, 0xfedcba9876543210UL, String, 7, 8, 9, 10, 11, 12);
    else if (Mode == 1)
      Result = EMIT(Format, 4, String);
    else if (Mode == 2)
      Result = EMIT(Format, 0);
    else if (Mode == 3)
      Result = EMIT(Format, 1.5);
    else
      Result = EMIT("%*s", (int)(Mode - 10), "");
    Out[2 * I] = (long)Result;
    Out[2 * I + 1] = *__errno();
  }
#undef EMIT
  return Library ? dlclose(Library) : 0;
}

static __attribute__((noinline)) int
direct_text_write(int Priority, const char *Tag, const char *Message) {
  return __android_log_write(Priority, Tag, Message);
}

U64 log_text_write(U64 *Out, U64 ID, U64 Priority, const char *Tag,
                   const char *Message, U64 Count, U64 Form) {
  if (Count > 4)
    return 1;
  void *Library = Form >= 2 ? dlopen("liblog.so", 2) : (void *)0;
  int (*Write)(int, const char *, const char *) =
      Form == 2 ? (int (*)(int, const char *, const char *))dlsym(
                      Library, "__android_log_write")
                : (void *)0;
  int (*BufferWrite)(int, int, const char *, const char *) =
      Form == 3 ? (int (*)(int, int, const char *, const char *))dlsym(
                      Library, "__android_log_buf_write")
                : (void *)0;
  if ((Form == 2 && !Write) || (Form == 3 && !BufferWrite))
    return 2;
  for (U64 I = 0; I != Count; ++I) {
    *__errno() = 73;
    Out[2 * I] =
        (long)(Form == 1   ? __android_log_buf_write(ID, Priority, Tag, Message)
               : Form == 2 ? Write(Priority, Tag, Message)
               : Form == 3 ? BufferWrite(ID, Priority, Tag, Message)
                           : direct_text_write(Priority, Tag, Message));
    Out[2 * I + 1] = *__errno();
  }
  if (Library && dlclose(Library))
    return 3;
  return 0;
}

struct AbortCall {
  const char *Message;
  U64 Error;
};
static void *abort_in_thread(void *Arg) {
  struct AbortCall *Call = Arg;
  *__errno() = 73;
  android_set_abort_message(Call->Message);
  Call->Error = *__errno();
  return (void *)0;
}

U64 log_abort_message(U64 *Out, U64 *Slot, const char *First,
                      const char *Second, U64 CopySize, U64 Form, U64 Reset) {
  if (Reset == 2 && mprotect((void *)((U64)Slot & ~4095UL), 4096, 1))
    return 4;
  void *Library = Form == 1 ? dlopen("libc.so", 2) : (void *)0;
  void (*Set)(const char *) =
      Form == 1
          ? (void (*)(const char *))dlsym(Library, "android_set_abort_message")
          : android_set_abort_message;
  if (!Set)
    return 1;
  for (U64 I = 0; I != 2; ++I) {
    if (I && Reset == 1)
      *Slot = 0;
    const char *Message = I ? Second : First;
    if (Form == I + 2) {
      struct AbortCall Call = {Message, 0};
      U64 Thread;
      if (pthread_create(&Thread, (void *)0, abort_in_thread, &Call) ||
          pthread_join(Thread, (void *)0))
        return 2;
      Out[2 * I] = Call.Error;
    } else {
      *__errno() = 73;
      Set(Message);
      Out[2 * I] = *__errno();
    }
    Out[2 * I + 1] = *Slot;
  }
  if (CopySize) {
    memcpy((char *)Out + 10000, (void *)Out[1], CopySize);
    memcpy((char *)Out + 24000, (void *)Out[3], CopySize);
  }
  if (Library && dlclose(Library))
    return 3;
  return 0;
}

U64 log_fatal_write(U64 *Out, U64 *Slot, const char *Tag, const char *Message,
                    U64 Form, U64 CopySize) {
  U64 Result = log_text_write(Out, 3, 7, Tag, Message, 2, Form);
  if (!Result && CopySize)
    memcpy((char *)Out + 24000, (void *)*Slot, CopySize);
  return Result;
}

U64 log_fatal_formatted(U64 *Out, U64 *Slot, U64 Form, U64 Mode,
                        const char *Format, const char *Message,
                        const char *Tag, U64 CopySize) {
  U64 Result = log_formatted(Out, Form, Mode, Format, Message, 3, 7, Tag);
  if (!Result && CopySize)
    memcpy((char *)Out + 24000, (void *)*Slot, CopySize);
  return Result;
}

U64 log_abort_exhaust(U64 *Out, U64 *Slot, const char *Message, U64 Form,
                      U64 CopySize) {
  void *Last = (void *)0;
  for (U64 Size = 4 * 1024 * 1024; Size >= 4096; Size >>= 1) {
    void *P;
    while ((P = mmap((void *)0, Size, 3, 0x22, -1, 0)) != (void *)-1)
      Last = P;
  }
  if (!Last)
    return 1;
  for (U64 I = 0; I != 2; ++I) {
    if (I && munmap(Last, 4096))
      return 2;
    *__errno() = 73;
    if (Form)
      Out[4 + I] = (long)__android_log_write(7, "tag", Message);
    else {
      android_set_abort_message(Message);
      Out[4 + I] = 0;
    }
    Out[2 * I] = *__errno();
    Out[2 * I + 1] = *Slot;
  }
  memcpy((char *)Out + 24000, (void *)*Slot, CopySize);
  return 0;
}

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
