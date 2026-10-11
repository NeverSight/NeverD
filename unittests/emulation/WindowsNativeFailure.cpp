//===- WindowsNativeFailure.cpp - Bounded native failure diagnostics -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsNativeFailure.h"

#if defined(_WIN32) && defined(_M_X64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stdio.h>
#include <windows.h>

// Diagnostic replay only: the ordinary, undebugged execution remains the
// oracle. First-chance application exceptions retain their normal dispatch.
// https://learn.microsoft.com/windows/win32/api/debugapi/nf-debugapi-waitfordebugevent
void diagnoseNativeFailure(const wchar_t *Path) {
  STARTUPINFOW Startup = {0};
  Startup.cb = sizeof(Startup);
  PROCESS_INFORMATION Child = {0};
  if (!CreateProcessW(Path, NULL, NULL, NULL, FALSE,
                      DEBUG_ONLY_THIS_PROCESS | CREATE_DEFAULT_ERROR_MODE, NULL,
                      NULL, &Startup, &Child)) {
    fprintf(stderr, "native diagnostic launch error=%lu\n", GetLastError());
    return;
  }
  const ULONGLONG Deadline = GetTickCount64() + 10000;
  int InitialBreakpoint = 1, Exited = 0;
  for (unsigned Count = 0; Count < 256; ++Count) {
    const ULONGLONG Now = GetTickCount64();
    DEBUG_EVENT Event = {0};
    if (Now >= Deadline || !WaitForDebugEvent(&Event, (DWORD)(Deadline - Now)))
      break;
    DWORD Continue = DBG_CONTINUE;
    HANDLE File = NULL;
    void *Base = NULL;
    if (Event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
      File = Event.u.CreateProcessInfo.hFile;
      Base = Event.u.CreateProcessInfo.lpBaseOfImage;
    } else if (Event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
      File = Event.u.LoadDll.hFile;
      Base = Event.u.LoadDll.lpBaseOfDll;
    }
    if (File && File != INVALID_HANDLE_VALUE) {
      wchar_t Name[1024] = {0};
      const DWORD N = GetFinalPathNameByHandleW(File, Name, 1024, 0);
      fprintf(stderr, "native module base=%p path=%ls\n", Base,
              N && N < 1024 ? Name : L"unavailable");
      CloseHandle(File);
    }
    if (Event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
      const EXCEPTION_DEBUG_INFO *Info = &Event.u.Exception;
      const EXCEPTION_RECORD *Exception = &Info->ExceptionRecord;
      if (InitialBreakpoint && Info->dwFirstChance &&
          Exception->ExceptionCode == EXCEPTION_BREAKPOINT) {
        InitialBreakpoint = 0;
      } else {
        Continue = DBG_EXCEPTION_NOT_HANDLED;
        fprintf(stderr, "native exception code=%08lx first=%lu pc=%p",
                Exception->ExceptionCode, Info->dwFirstChance,
                Exception->ExceptionAddress);
        for (DWORD I = 0; I < Exception->NumberParameters && I < 15; ++I)
          fprintf(stderr, " arg%lu=%llx", I,
                  (unsigned long long)Exception->ExceptionInformation[I]);
        fprintf(stderr, "\n");
        HANDLE Thread = OpenThread(THREAD_GET_CONTEXT, FALSE, Event.dwThreadId);
        CONTEXT Context = {0};
        Context.ContextFlags = CONTEXT_FULL;
        if (Thread && GetThreadContext(Thread, &Context)) {
          fprintf(stderr,
                  "native registers rip=%llx rsp=%llx rax=%llx rbx=%llx "
                  "rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx\n",
                  Context.Rip, Context.Rsp, Context.Rax, Context.Rbx,
                  Context.Rcx, Context.Rdx, Context.Rsi, Context.Rdi,
                  Context.R8, Context.R9);
          for (unsigned I = 0; I < 2; ++I) {
            const void *Address = (void *)(I ? Context.Rsp : Context.Rip);
            unsigned char Bytes[64];
            SIZE_T N = 0;
            ReadProcessMemory(Child.hProcess, Address, Bytes, sizeof(Bytes),
                              &N);
            fprintf(stderr, "native %s bytes=", I ? "stack" : "instruction");
            for (SIZE_T J = 0; J < N; ++J)
              fprintf(stderr, "%02x", Bytes[J]);
            fprintf(stderr, "\n");
          }
        }
        if (Thread)
          CloseHandle(Thread);
      }
    } else if (Event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
      fprintf(stderr, "native diagnostic exit=%08lx\n",
              Event.u.ExitProcess.dwExitCode);
      Exited = 1;
    }
    if (!ContinueDebugEvent(Event.dwProcessId, Event.dwThreadId, Continue) ||
        Exited)
      break;
  }
  if (!Exited) {
    fprintf(stderr, "native diagnostic incomplete\n");
    TerminateProcess(Child.hProcess, 0xe04e00ff);
    DebugActiveProcessStop(Child.dwProcessId);
    WaitForSingleObject(Child.hProcess, 2000);
  }
  CloseHandle(Child.hThread);
  CloseHandle(Child.hProcess);
}
#endif
