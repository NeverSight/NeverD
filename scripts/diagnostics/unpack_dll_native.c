#include <stdio.h>
#include <windows.h>

static LONG CALLBACK report(EXCEPTION_POINTERS *P) {
  const EXCEPTION_RECORD *E = P->ExceptionRecord;
  if (E->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  MEMORY_BASIC_INFORMATION M = {0};
  VirtualQuery(E->ExceptionAddress, &M, sizeof(M));
  printf(
      "AV code=%08lx pc=%p allocation=%p type=%lu access=%llx address=%llx\n",
      E->ExceptionCode, E->ExceptionAddress, M.AllocationBase, M.Protect,
      E->NumberParameters > 0 ? E->ExceptionInformation[0] : 0,
      E->NumberParameters > 1 ? E->ExceptionInformation[1] : 0);
  printf(
      "RAX=%llx RCX=%llx RDX=%llx R8=%llx R9=%llx R10=%llx R11=%llx RSP=%llx\n",
      P->ContextRecord->Rax, P->ContextRecord->Rcx, P->ContextRecord->Rdx,
      P->ContextRecord->R8, P->ContextRecord->R9, P->ContextRecord->R10,
      P->ContextRecord->R11, P->ContextRecord->Rsp);
  fflush(stdout);
  return EXCEPTION_CONTINUE_SEARCH;
}

int main(void) {
  AddVectoredExceptionHandler(1, report);
  puts("loading");
  fflush(stdout);
  HMODULE Module = LoadLibraryA("input.dll");
  printf("loaded=%p error=%lu\n", Module, GetLastError());
  fflush(stdout);
  if (!Module)
    return 1;
  typedef unsigned (*QUERY)(void);
  QUERY Query = (QUERY)GetProcAddress(Module, "Query");
  printf("query=%p\n", Query);
  fflush(stdout);
  printf("value=%u\n", Query());
  fflush(stdout);
  printf("unloaded=%d\n", FreeLibrary(Module));
  return 0;
}
