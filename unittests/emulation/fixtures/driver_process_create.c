// Original guest callbacks consume explicit PS_CREATE_NOTIFY_INFO observations.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef struct {
  U16 Length, MaximumLength;
  const U16 *Buffer;
} US;
typedef struct {
  U64 Size;
  U32 Flags;
  U64 Parent, Creator, Thread;
  void *File;
  US *Image, *Command;
  U32 Status;
} INFO;
typedef struct {
  U8 Prefix[0x68];
  void (*Unload)(void *);
} DRIVER;
_Static_assert(sizeof(INFO) == 72, "WDK x64 create info");
_Static_assert(__builtin_offsetof(INFO, Status) == 64, "creation status");
#define API __declspec(dllimport)
API U32 PsSetCreateProcessNotifyRoutine(void (*)(U64, U64, U8), U8);
API U32 PsSetCreateProcessNotifyRoutineEx(void (*)(U64, U64, INFO *), U8);
API U64 PsGetCurrentProcessId(void);
API U64 PsGetCurrentThreadId(void);
API U64 PsGetProcessId(U64);
API U8 KeAreApcsDisabled(void);
API U8 KeAreAllApcsDisabled(void);
API U8 ExGetPreviousMode(void);
static U32 Created, Exited, Bad;
static INFO *Borrowed;
static U64 CreatorThread;
static void context(U64 PID) {
  if (PsGetCurrentProcessId() != PID || !KeAreApcsDisabled() ||
      KeAreAllApcsDisabled() || ExGetPreviousMode())
    ++Bad;
}
static void legacy(U64 Parent, U64 PID, U8 Create) {
  if (Parent != 4 || PID != 100)
    ++Bad;
  context(Create ? 4 : 100);
  if (Create) {
    if (Created)
      ++Bad;
    ++Created;
  } else {
    if (Exited)
      ++Bad;
    ++Exited;
  }
}
static void first(U64 Process, U64 PID, INFO *Info) {
  if (PID != 100 || PsGetProcessId(Process) != 100)
    ++Bad;
  context(Info ? 4 : 100);
  if (Info) {
    static const U16 Name[] = u"\\??\\C:\\NeverDCreate.exe";
    if (Created != 1 || Info->Size != 72 || Info->Flags != 1 ||
        Info->Parent != 4 || Info->Creator != 4 ||
        Info->Thread != CreatorThread || !Info->File || !Info->Image ||
        Info->Image->Length != sizeof(Name) - 2 || !Info->Command ||
        Info->Status)
      ++Bad;
    if (Info->Image)
      for (U32 I = 0; I < (sizeof(Name) - 2) / 2; ++I)
        if (Info->Image->Buffer[I] != Name[I])
          ++Bad;
    Borrowed = Info;
    ++Created;
#ifdef CREATE_VETO
    Info->Status = 0xc0000022;
#endif
  } else {
    if (Exited != 1)
      ++Bad;
    ++Exited;
  }
}
static void second(U64 Process, U64 PID, INFO *Info) {
  if (PID != 100 || PsGetProcessId(Process) != 100)
    ++Bad;
  context(Info ? 4 : 100);
  if (Info) {
    if (Created != 2 || Info != Borrowed || Info->Status)
      ++Bad;
    ++Created;
  } else {
    if (Exited != 2)
      ++Bad;
    ++Exited;
  }
}
static void unload(void *D) {
  (void)D;
#ifdef CREATE_BORROW
  Bad += *(volatile U64 *)&Borrowed->Size;
#endif
#ifdef CREATE_VETO
  if (Created != 2 || Exited)
    ++Bad;
#else
  if (Created != 3 || Exited != 3)
    ++Bad;
#endif
  if (Bad || PsSetCreateProcessNotifyRoutineEx(second, 1) ||
      PsSetCreateProcessNotifyRoutineEx(first, 1) ||
      PsSetCreateProcessNotifyRoutine(legacy, 1))
    *(volatile U32 *)0xdead0000 = Bad + 1;
}
U32 DriverEntry(DRIVER *D, void *Path) {
  (void)Path;
  D->Unload = unload;
  CreatorThread = PsGetCurrentThreadId();
  if (PsSetCreateProcessNotifyRoutine(legacy, 0) ||
      PsSetCreateProcessNotifyRoutineEx(first, 0) ||
      PsSetCreateProcessNotifyRoutineEx(second, 0))
    return 0xc000000d;
  return 0;
}
