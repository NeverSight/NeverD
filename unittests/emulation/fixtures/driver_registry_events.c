// Actual guest filter driven by external scenario registry operations.
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  U8 Prefix[0x68];
  void (*Unload)(void *);
} DRIVER;
typedef struct {
  void *Object, *ValueName;
  U32 TitleIndex, Type;
  const U32 *Data;
  U32 DataSize;
  void *CallContext, *ObjectContext, *Reserved;
} SET;
typedef struct {
  void *Object;
  U32 Status;
  void *Pre;
  U32 ReturnStatus;
  void *CallContext, *ObjectContext, *Reserved;
} POST;
_Static_assert(__builtin_offsetof(SET, CallContext) == 40, "set context");
_Static_assert(__builtin_offsetof(POST, ReturnStatus) == 24, "post override");
__declspec(dllimport) NTSTATUS CmRegisterCallback(NTSTATUS (*)(void *, void *,
                                                               void *),
                                                  void *, U64 *);
__declspec(dllimport) NTSTATUS CmUnRegisterCallback(U64);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) U8 KeAreApcsDisabled(void);
__declspec(dllimport) U8 ExGetPreviousMode(void);
__declspec(dllimport) U64 PsGetCurrentProcessId(void);
static U64 Cookie;
static U32 Errors, Calls, Marker;
#ifdef EVENTS_USER_ACTOR
static void *OpenedRoot;
#endif
#ifdef EVENTS_RETAIN_INPUT
static const U32 *Borrowed;
#endif
static NTSTATUS Filter(void *Context, void *ClassPointer, void *Info) {
  U32 Class = (U32)(U64)ClassPointer;
  ++Calls;
  if (Context != &Marker || KeGetCurrentIrql() || !KeAreApcsDisabled() ||
      ExGetPreviousMode() ||
      PsGetCurrentProcessId() !=
#ifdef EVENTS_USER_ACTOR
          100
#else
          4
#endif
  )
    ++Errors;
  if (Class == 1) {
    SET *Set = Info;
    if (Set->Type == 4 && Set->DataSize == 4) {
#ifdef EVENTS_RETAIN_INPUT
      Borrowed = Set->Data;
#endif
      if (*Set->Data == 0xbaad)
        return (NTSTATUS)0xc0000022;
      if (*Set->Data == 0xbeef)
        return (NTSTATUS)0xc0000503;
      if (*Set->Data == 0xcafe)
        Set->CallContext = &Marker;
    }
  } else if (Class == 16) {
    POST *Post = Info;
    if (Post->CallContext == &Marker) {
      Post->ReturnStatus = 0xc0000022;
      return (NTSTATUS)0xc0000503;
    }
  }
#ifdef EVENTS_USER_ACTOR
  if (Class == 28) {
    U8 *Open = Info;
    U8 *Name = *(U8 **)Open;
    U16 *Text = *(U16 **)(Name + 8);
    if (Text[0] != '\\' && (*(void **)(Open + 8) != OpenedRoot || !OpenedRoot))
      ++Errors;
  } else if (Class == 29 && !OpenedRoot)
    OpenedRoot = ((POST *)Info)->Object;
#endif
  return 0;
}
static void Unload(void *Object) {
  (void)Object;
#ifdef EVENTS_RETAIN_INPUT
  Marker = *(volatile const U32 *)Borrowed;
#endif
#ifndef EVENTS_NO_FILTER
  if (Errors || !Calls || CmUnRegisterCallback(Cookie))
    *(volatile U32 *)0xdead0000 = Errors + 1;
#endif
}
NTSTATUS DriverEntry(DRIVER *Driver, void *Path) {
  (void)Path;
  Driver->Unload = Unload;
#ifndef EVENTS_NO_FILTER
  return CmRegisterCallback(Filter, &Marker, &Cookie);
#else
  return 0;
#endif
}
