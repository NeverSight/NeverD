// A private DLL deliberately reuses a WDK export name with an open prototype.
// Regenerate the import library and x64 caller from this directory with
//   printf 'LIBRARY fixture.dll\nEXPORTS\nKeGetCurrentThread\n' > fixture.def
//   lld-link /lib /machine:x64 /def:fixture.def /out:fixture.lib
//   clang --target=x86_64-pc-windows-msvc -O1 -fno-optimize-sibling-calls
//     -c local_variadic_import.c -o caller.obj
//   lld-link /subsystem:console /entry:Probe /nodefaultlib /timestamp:0
//     /out:local_variadic_import.exe caller.obj fixture.lib

typedef unsigned long long Word;
__declspec(dllimport) Word KeGetCurrentThread(Word First, ...);

Word Probe(Word Value) {
  return KeGetCurrentThread(Value, (Word)11, (Word)13);
}
