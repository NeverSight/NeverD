// clang-format off: this include contains x86 assembly.
// The complete PE32 STARTUPINFOW is disjoint from registration administration.
// Poison every word before each call; observe writes and preserved fields.
.irp offset,0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64
  mov dword ptr [ebp-108+\offset],0xa5a5a5a5
.endr
  lea eax,[ebp-108]
  push eax
  call dword ptr ["__imp__GetStartupInfoW@4"]
.irp offset,0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64
  mov eax,[ebp-108+\offset]
  mov [_registration_startup_snapshot+\offset],eax
.endr
