// An ARM64 program calling routines kernel32, user32 and ntdll export, which
// NeverD's windows-user-arm64 library declares the prototypes of: their
// calls pass what the prototypes take, a register the caller passes on
// untouched included and a register an earlier call clobbered not, and
// ExitProcess returns to no caller.  ZwClose's comes from phnt.
// Regenerate user_imports_arm64.exe with:
//   printf 'LIBRARY kernel32.dll\nEXPORTS\n%s\n%s\n%s\n%s\n' CreateFileW \
//     CloseHandle GetLastError ExitProcess > kernel32.def
//   printf 'LIBRARY user32.dll\nEXPORTS\n%s\n%s\n' MessageBoxW \
//     CreateWindowExW > user32.def
//   printf 'LIBRARY ntdll.dll\nEXPORTS\nZwClose\n' > ntdll.def
//   for Dll in kernel32 user32 ntdll; do
//     llvm-dlltool -m arm64 -d $Dll.def -l $Dll.lib; done
//   clang-cl --target=aarch64-pc-windows-msvc /O1 /GS- /c \
//     user_imports_arm64.c /Fouser_imports_arm64.obj
//   lld-link /ENTRY:Start /NODEFAULTLIB /SUBSYSTEM:CONSOLE \
//     /OUT:user_imports_arm64.exe user_imports_arm64.obj kernel32.lib \
//     user32.lib ntdll.lib

__declspec(dllimport) void *CreateFileW(const unsigned short *Name,
                                        unsigned long Access,
                                        unsigned long Share, void *Security,
                                        unsigned long Disposition,
                                        unsigned long Flags, void *Template);
__declspec(dllimport) int CloseHandle(void *Handle);
__declspec(dllimport) unsigned long GetLastError(void);
__declspec(dllimport) __declspec(noreturn) void ExitProcess(unsigned Code);
__declspec(dllimport) int MessageBoxW(void *Window, const unsigned short *Text,
                                      const unsigned short *Caption,
                                      unsigned Type);
__declspec(dllimport) long ZwClose(void *Handle);
__declspec(dllimport) void *
CreateWindowExW(unsigned long ExStyle, const unsigned short *Class,
                const unsigned short *Title, unsigned long Style, int X, int Y,
                int Width, int Height, void *Parent, void *Menu, void *Instance,
                void *Parameter);

__declspec(dllexport) void *Open(const unsigned short *Name) {
  // GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING.
  return CreateFileW(Name, 0x80000000, 1, 0, 3, 0, 0);
}

__declspec(dllexport) int Probe(void *Handle) {
  // GetLastError clobbered every argument register the calls after it read.
  if (GetLastError() == 5)
    return -1;
  return CloseHandle(Handle) + (int)ZwClose(Handle);
}

// Exported under kernel32's name, which takes the table's size: the
// definition reads no parameter, and shows the one its prototype declares.
__declspec(dllexport) int InitAtomTable(unsigned long Size) {
  (void)Size;
  return 1;
}

// CreateWindowExW takes twelve arguments, the last four on the stack.
__declspec(dllexport) void *Window(void *Parent, void *Instance) {
  // WS_VISIBLE.
  return CreateWindowExW(0, L"class", L"title", 0x10000000, 1, 2, 3, 4, Parent,
                         0, Instance, 0);
}

__declspec(dllexport) int Finish(int Code) {
  if (Code > 3)
    ExitProcess(Code);
  return Code + 1;
}

int Start(void) {
  void *Handle = Open(L"input");
  MessageBoxW(0, L"text", L"caption", 0);
  return Finish(Probe(Handle));
}
