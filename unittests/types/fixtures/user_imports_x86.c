// A 32-bit program calling routines kernel32, user32 and ntdll export, which
// NeverD's windows-user-x86 library declares the prototypes of, __stdcall
// as WINAPI declares them, and windows-kernel-x86 the native routines the
// user mode headers do not: their calls pass what the prototypes take, a
// 64-bit argument in two stack slots, and ExitProcess returns to no caller.
// Regenerate user_imports_x86.exe with:
//   printf 'LIBRARY kernel32.dll\nEXPORTS\n%s\n%s\n%s\n%s\n%s\n' \
//     CreateFileW@28 CloseHandle@4 GetLastError@0 ExitProcess@4 \
//     SetFilePointerEx@20 > kernel32.def
//   printf 'LIBRARY user32.dll\nEXPORTS\nMessageBoxW@16\n' > user32.def
//   printf 'LIBRARY ntdll.dll\nEXPORTS\nZwClose@4\n' > ntdll.def
//   for Dll in kernel32 user32 ntdll; do
//     llvm-dlltool -m i386 -k -d $Dll.def -l $Dll.lib; done
//   clang-cl --target=i686-pc-windows-msvc /O1 /GS- /c user_imports_x86.c \
//     /Fouser_imports_x86.obj
//   lld-link /ENTRY:Start /NODEFAULTLIB /SUBSYSTEM:CONSOLE \
//     /OUT:user_imports_x86.exe user_imports_x86.obj kernel32.lib \
//     user32.lib ntdll.lib

#define WINAPI __stdcall

__declspec(dllimport) void *WINAPI
CreateFileW(const unsigned short *Name, unsigned long Access,
            unsigned long Share, void *Security, unsigned long Disposition,
            unsigned long Flags, void *Template);
__declspec(dllimport) int WINAPI CloseHandle(void *Handle);
__declspec(dllimport) unsigned long WINAPI GetLastError(void);
__declspec(dllimport) __declspec(noreturn) void WINAPI
ExitProcess(unsigned Code);
__declspec(dllimport) int WINAPI MessageBoxW(void *Window,
                                             const unsigned short *Text,
                                             const unsigned short *Caption,
                                             unsigned Type);
__declspec(dllimport) long WINAPI ZwClose(void *Handle);
__declspec(dllimport) int WINAPI SetFilePointerEx(void *File,
                                                  long long Distance,
                                                  long long *Position,
                                                  unsigned long Method);

__declspec(dllexport) void *Open(const unsigned short *Name) {
  // GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING.
  return CreateFileW(Name, 0x80000000, 1, 0, 3, 0, 0);
}

__declspec(dllexport) int Probe(void *Handle) {
  if (GetLastError() == 5)
    return -1;
  return CloseHandle(Handle) + (int)ZwClose(Handle);
}

__declspec(dllexport) int Finish(int Code) {
  if (Code > 3)
    ExitProcess(Code);
  return Code + 1;
}

__declspec(dllexport) int Seek(void *Handle, long long Offset) {
  // Position lies on the stack above the five slots the call passes.
  long long Position = 0;
  if (!SetFilePointerEx(Handle, Offset, &Position, 0))
    return -1;
  return (int)Position;
}

int Start(void) {
  void *Handle = Open(L"input");
  MessageBoxW(0, L"text", L"caption", 0);
  return Finish(Probe(Handle));
}
