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
extern void __android_log_assert(const char *, const char *, const char *, ...);
extern void abort(void);
extern void _exit(int);
extern void _Exit(int);
extern int gettid(void);
extern int getpid(void);
extern int kill(int, int);
extern int close(int);
extern int dup2(int, int);
extern int open(const char *, int, ...);
extern int sigaction64(int, const void *, void *);
extern int sigprocmask64(int, const U64 *, U64 *);

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

// Checked entry names are called directly as well as through dlsym, so both
// ELF binding routes carry real function relocations.
extern void *__memcpy_chk(void *, const void *, U64, U64);
extern void *__memmove_chk(void *, const void *, U64, U64);
extern void *__memset_chk(void *, int, U64, U64);
extern void *__memchr_chk(const void *, int, U64, U64);
extern void __FD_SET_chk(int, void *, U64);
extern void __FD_CLR_chk(int, void *, U64);
extern int __FD_ISSET_chk(int, const void *, U64);
extern U64 __strlen_chk(const char *, U64);
extern char *__strcpy_chk(char *, const char *, U64);
extern char *__strncpy_chk(char *, const char *, U64, U64);
extern char *__strncpy_chk2(char *, const char *, U64, U64, U64);
extern U64 __strlcpy_chk(char *, const char *, U64, U64);
extern char *__strcat_chk(char *, const char *, U64);
extern char *__strncat_chk(char *, const char *, U64, U64);
extern char *__strchr_chk(const char *, int, U64);
extern char *__strrchr_chk(const char *, int, U64);
extern long __read_chk(int, void *, U64, U64);
extern char *__fgets_chk(char *, int, void *, U64);
extern U64 __fread_chk(void *, U64, U64, void *, U64);
extern int __snprintf_chk(char *, U64, int, U64, const char *, ...);
extern int __sprintf_chk(char *, int, U64, const char *, ...);
extern int __vsnprintf_chk(char *, U64, int, U64, const char *,
                           __builtin_va_list);
extern int __vsprintf_chk(char *, int, U64, const char *, __builtin_va_list);
#define FORTIFY_CALL(Name, ...)                                                \
  (Library ? ((__typeof__(&Name))dlsym(Library, #Name))(__VA_ARGS__)           \
           : Name(__VA_ARGS__))
static int fortify_va(U64 Function, void *Library, const U64 *A, ...) {
  __builtin_va_list Args;
  __builtin_va_start(Args, A);
  int Result = Function == 20
                   ? FORTIFY_CALL(__vsnprintf_chk, (char *)A[0], A[2], 0, A[3],
                                  (const char *)A[1], Args)
                   : FORTIFY_CALL(__vsprintf_chk, (char *)A[0], 0, A[3],
                                  (const char *)A[1], Args);
  __builtin_va_end(Args);
  return Result;
}
static void fortify_dispatch(U64 Function, void *Library, const U64 *A) {
  switch (Function) {
  case 0:
    FORTIFY_CALL(__memcpy_chk, (void *)A[0], (void *)A[1], A[2], A[3]);
    break;
  case 1:
    FORTIFY_CALL(__memmove_chk, (void *)A[0], (void *)A[1], A[2], A[3]);
    break;
  case 2:
    FORTIFY_CALL(__memset_chk, (void *)A[0], (int)A[1], A[2], A[3]);
    break;
  case 3:
    FORTIFY_CALL(__memchr_chk, (void *)A[0], (int)A[1], A[2], A[3]);
    break;
  case 4:
    FORTIFY_CALL(__FD_SET_chk, (int)A[0], (void *)A[1], A[2]);
    break;
  case 5:
    FORTIFY_CALL(__FD_CLR_chk, (int)A[0], (void *)A[1], A[2]);
    break;
  case 6:
    FORTIFY_CALL(__FD_ISSET_chk, (int)A[0], (void *)A[1], A[2]);
    break;
  case 7:
    FORTIFY_CALL(__strlen_chk, (void *)A[0], A[1]);
    break;
  case 8:
    FORTIFY_CALL(__strcpy_chk, (void *)A[0], (void *)A[1], A[2]);
    break;
  case 9:
    FORTIFY_CALL(__strncpy_chk, (void *)A[0], (void *)A[1], A[2], A[3]);
    break;
  case 10:
    FORTIFY_CALL(__strncpy_chk2, (void *)A[0], (void *)A[1], A[2], A[3], A[4]);
    break;
  case 11:
    FORTIFY_CALL(__strlcpy_chk, (void *)A[0], (void *)A[1], A[2], A[3]);
    break;
  case 12:
    FORTIFY_CALL(__strcat_chk, (void *)A[0], (void *)A[1], A[2]);
    break;
  case 13:
    FORTIFY_CALL(__strncat_chk, (void *)A[0], (void *)A[1], A[2], A[3]);
    break;
  case 14:
    FORTIFY_CALL(__strchr_chk, (void *)A[0], (int)A[1], A[2]);
    break;
  case 15:
    FORTIFY_CALL(__strrchr_chk, (void *)A[0], (int)A[1], A[2]);
    break;
  case 16:
    FORTIFY_CALL(__read_chk, (int)A[0], (void *)A[1], A[2], A[3]);
    break;
  case 17:
    FORTIFY_CALL(__fgets_chk, (void *)A[0], (int)A[1], (void *)A[2], A[3]);
    break;
  case 18:
    FORTIFY_CALL(__fread_chk, (void *)A[0], A[1], A[2], (void *)A[3], A[4]);
    break;
  case 19:
    FORTIFY_CALL(__snprintf_chk, (void *)A[0], A[2], 0, A[3], (void *)A[1],
                 (void *)A[4]);
    break;
  case 21:
    FORTIFY_CALL(__sprintf_chk, (void *)A[0], 0, A[3], (void *)A[1],
                 (void *)A[4]);
    break;
  default:
    fortify_va(Function, Library, A, (void *)A[4]);
    break;
  }
}
#undef FORTIFY_CALL

struct AssertAction {
  int Flags;
  void (*Handler)(int, void *, void *);
  U64 Mask;
  void (*Restorer)(void);
};
struct AssertCall {
  U64 *Out, *Slot;
  U64 Form, Mode;
  const char *Condition, *Tag, *Format, *Message;
  U64 FortifyFunction;
  const U64 *FortifyArguments;
};
static struct AssertCall *ActiveAssert;

static void assert_handler(int Signal, void *Info, void *Context) {
  struct AssertCall *Call = ActiveAssert;
  U64 *Out = Call->Out;
  ++Out[0];
  Out[1] = Signal;
  Out[2] = (long)((int *)Info)[2];
  Out[17 + Out[0]] = Out[2];
  Out[3] = ((unsigned *)Info)[4];
  Out[4] = ((unsigned *)Info)[5];
  Out[5] = *__errno();
  sigprocmask64(0, (void *)0, Out + 6);
  Out[7] = *Call->Slot;
  Out[8] = gettid();
  if (Out[7])
    memcpy((char *)Out + 24000, (void *)Out[7], 4096);
  const U64 Handling = (Call->Mode >> 8) & 255;
  if (Handling == 3)
    _exit(23);
  if (Handling == 7)
    _Exit(23);
  if (Handling == 4) {
    // Deliberately bypass the modeled import's return boundary via ucontext.
    U64 *Registers = (void *)((char *)Context + 184);
    *(U64 *)((char *)Context + 440) = Registers[30];
  }
  if (Handling == 5 && Out[0] == 1)
    abort();
  if (Handling == 6) {
    struct AssertAction Ignore = {0, (void *)1, 0, 0};
    sigaction64(6, &Ignore, (void *)0);
  }
  *__errno() = 77;
  Out[9] = 1;
}

static void *assert_body(void *Arg) {
  struct AssertCall *Call = Arg;
  const U64 Handling = (Call->Mode >> 8) & 255;
  if (Handling) {
    struct AssertAction Action = {4, Handling == 1 ? (void *)1 : assert_handler,
                                  0, 0};
    if (sigaction64(6, &Action, (void *)0))
      return (void *)91;
  }
  const U64 Descriptor = (Call->Mode >> 16) & 255;
  if (Descriptor == 1 && close(2))
    return (void *)92;
  if ((Descriptor == 2 || Descriptor == 3) &&
      dup2(Descriptor == 2 ? 1 : 0, 2) != 2)
    return (void *)93;
  if (Descriptor == 4) {
    int FD = open("/assert-output", 2, 0);
    if (FD < 0 || dup2(FD, 2) != 2)
      return (void *)94;
  }
  void *Library = Call->Form & 1
                      ? dlopen(Call->Form == 1 ? "liblog.so" : "libc.so", 2)
                      : (void *)0;
  void (*Assert)(const char *, const char *, const char *, ...) =
      Call->Form == 1 ? (void *)dlsym(Library, "__android_log_assert")
                      : (void *)0;
  void (*Abort)(void) =
      Call->Form == 3 ? (void *)dlsym(Library, "abort") : (void *)0;
  if ((Call->Form == 1 && !Assert) || (Call->Form == 3 && !Abort))
    return (void *)95;
  Call->Out[12] = (U64)__errno();
  Call->Out[13] = gettid();
  if ((Call->Mode & (1UL << 25)) && kill(getpid(), 6))
    return (void *)97;
  *__errno() = 55;
#define ASSERT_LOG(...)                                                        \
  (Call->Form == 1                                                             \
       ? Assert(Call->Condition, Call->Tag, Call->Format, __VA_ARGS__)         \
       : __android_log_assert(Call->Condition, Call->Tag, Call->Format,        \
                              __VA_ARGS__))
  if (Call->Form >= 4) {
    fortify_dispatch(Call->FortifyFunction, Library, Call->FortifyArguments);
  } else if (Call->Form >= 2) {
    if (Call->Form == 3)
      Abort();
    else
      abort();
  } else if ((Call->Mode & 255) == 1) {
    ASSERT_LOG(-123, 0xfedcba9876543210UL, Call->Message, 7, 8, 9, 10, 11, 12);
  } else if ((Call->Mode & 255) == 2) {
    ASSERT_LOG(0, Call->Message);
  } else {
    ASSERT_LOG(Call->Message);
  }
#undef ASSERT_LOG
  Call->Out[17] = 99;
  return (void *)99;
}

U64 log_fortify(U64 *Out, U64 *Slot, U64 Function, U64 Mode, const U64 *Args) {
  for (unsigned I = 0; I != 32; ++I)
    Out[I] = 0;
  struct AssertCall Call = {Out, Slot, 4 + (Mode & 1), Mode, 0, 0,
                            0,   0,    Function,       Args};
  ActiveAssert = &Call;
  if (Mode & (1UL << 24)) {
    U64 Thread;
    void *Value;
    if (pthread_create(&Thread, (void *)0, assert_body, &Call) ||
        pthread_join(Thread, &Value))
      return 96;
    return (U64)Value;
  }
  return (U64)assert_body(&Call);
}

U64 log_assertion(U64 *Out, U64 *Slot, U64 Form, U64 Mode,
                  const char *Condition, const char *Tag, const char *Format,
                  const char *Message) {
  for (unsigned I = 0; I != 32; ++I)
    Out[I] = 0;
  struct AssertCall Call = {Out, Slot,   Form,    Mode, Condition,
                            Tag, Format, Message, 0,    (void *)0};
  ActiveAssert = &Call;
  if (Mode & (1UL << 24)) {
    U64 Thread;
    void *Value;
    if (pthread_create(&Thread, (void *)0, assert_body, &Call) ||
        pthread_join(Thread, &Value))
      return 96;
    return (U64)Value;
  }
  return (U64)assert_body(&Call);
}
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
