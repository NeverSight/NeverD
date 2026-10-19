// Run the existing original registry lifecycle through an actual guest filter.
#define DriverEntry RegistryLifecycleEntry
#define Unload RegistryLifecycleUnload
#include "driver_registry.c"
#undef DriverEntry
#undef Unload

__declspec(dllimport) NTSTATUS CmRegisterCallback(NTSTATUS (*)(void *, void *,
                                                               void *),
                                                  void *, U64 *);
__declspec(dllimport) NTSTATUS CmUnRegisterCallback(U64);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) U64 PsGetCurrentThreadId(void);
__declspec(dllimport) U64 PsGetCurrentProcessId(void);
__declspec(dllimport) U8 ExGetPreviousMode(void);
__declspec(dllimport) U8 KeAreApcsDisabled(void);
__declspec(dllimport) U8 KeAreAllApcsDisabled(void);

typedef struct {
  UNICODE_STRING *CompleteName;
  void *RootObject, *ObjectType;
  U32 Options;
  UNICODE_STRING *Class;
  void *SecurityDescriptor, *SecurityQuality;
  U32 DesiredAccess, GrantedAccess;
  U32 *Disposition;
  void **ResultObject, *CallContext, *RootContext, *Transaction;
  U64 Version;
  UNICODE_STRING *RemainingName;
  U32 Wow64Flags, Attributes;
  U8 CheckAccessMode;
} OPEN_INFO;
typedef struct {
  void *Object;
  NTSTATUS Status;
  void *PreInformation;
  NTSTATUS ReturnStatus;
  void *CallContext, *ObjectContext, *Reserved;
} POST_INFO;
_Static_assert(sizeof(OPEN_INFO) == 136, "WDK V1 x64 layout");
_Static_assert(__builtin_offsetof(OPEN_INFO, CallContext) == 80,
               "call context");
_Static_assert(__builtin_offsetof(POST_INFO, CallContext) == 32,
               "post context");

static U64 Cookie, Thread, Process;
static U32 Calls, PreClass, Errors;
static void *PrePointer, *ServiceObject, *ChildObject;
static U32 ContextMarker;
static NTSTATUS Filter(void *Context, void *ClassPointer, void *Information) {
  U32 Class = (U32)(U64)ClassPointer;
  if (Context != &ContextMarker || KeGetCurrentIrql() ||
      PsGetCurrentThreadId() != Thread || PsGetCurrentProcessId() != Process ||
      ExGetPreviousMode() || !KeAreApcsDisabled() || KeAreAllApcsDisabled())
    ++Errors;
  ++Calls;
  if (Class == 0 || Class == 1 || Class == 2 || Class == 8 || Class == 14 ||
      Class == 26 || Class == 28) {
    if (!(Calls & 1))
      ++Errors;
    PreClass = Class;
    PrePointer = Information;
    U32 Offset = Class == 1 ? 40 : Class == 2 ? 16 : Class == 8 ? 48 : 8;
    if (Class == 26 || Class == 28) {
      OPEN_INFO *Open = Information;
      Offset = 80;
      if (Open->Version != 1 || !Open->RootObject || Open->CheckAccessMode ||
          Open->Wow64Flags || !Open->CompleteName || !Open->RemainingName ||
          !Open->RemainingName->Length)
        ++Errors;
      if (Class == 26 && Open->RootObject != ServiceObject)
        ++Errors;
    } else {
      void *Object = *(void **)Information;
      if (!Object || (Object != ServiceObject && Object != ChildObject))
        ++Errors;
    }
    *(void **)((U8 *)Information + Offset) = &ContextMarker;
  } else {
    POST_INFO *Post = Information;
    U32 Expected = PreClass == 0    ? 15
                   : PreClass == 1  ? 16
                   : PreClass == 2  ? 17
                   : PreClass == 8  ? 23
                   : PreClass == 14 ? 25
                                    : PreClass + 1;
    if ((Calls & 1) || Class != Expected ||
        Post->PreInformation != PrePointer ||
        Post->CallContext != &ContextMarker ||
        Post->ReturnStatus != Post->Status)
      ++Errors;
    if (!Post->Status) {
      if (PreClass == 28) {
        ServiceObject = Post->Object;
        OPEN_INFO *Open = Post->PreInformation;
        if (*Open->ResultObject != Post->Object)
          ++Errors;
      } else if (PreClass == 26)
        ChildObject = Post->Object;
      else if (Post->Object != *(void **)PrePointer)
        ++Errors;
    }
  }
  return 0;
}
static void Unload(void *Driver) {
  RegistryLifecycleUnload(Driver);
  if (Calls != 20 || Errors)
    *(volatile U32 *)0xdead0000 = Errors + 1;
  if (CmUnRegisterCallback(Cookie))
    *(volatile U32 *)0xdead0000 = 2;
}
NTSTATUS DriverEntry(DRIVER_OBJECT *Driver,
                     const UNICODE_STRING *RegistryPath) {
  Thread = PsGetCurrentThreadId();
  Process = PsGetCurrentProcessId();
  NTSTATUS Status = CmRegisterCallback(Filter, &ContextMarker, &Cookie);
  if (Status)
    return Status;
  Status = RegistryLifecycleEntry(Driver, RegistryPath);
  if (Status || Calls != 18 || Errors)
    return (NTSTATUS)0xc000000d;
  Driver->DriverUnload = Unload;
  return 0;
}
