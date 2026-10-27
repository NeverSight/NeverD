// clang-format off: this include contains x86 assembly.
// Shared semantic cases for the SEH3 and EH4 runtime probes.
.macro registration_set_state level
#if REGISTRATION_STATE_WRITE == 1
  mov ecx,\level
  nop
  mov dword ptr [ebp-4],ecx
#elif REGISTRATION_STATE_WRITE == 2
  .if \level == 0
    and dword ptr [ebp-4],0
  .elseif \level == -1
    or dword ptr [ebp-4],-1
  .else
    mov dword ptr [ebp-4],\level
  .endif
#else
  mov dword ptr [ebp-4],\level
#endif
.endm
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 2 || REGISTRATION_CASE == 4
  registration_set_state 1
#else
  registration_set_state 0
#endif
#if REGISTRATION_STARTUP_INFO
#include "registration_startup_info.inc"
#endif
#if REGISTRATION_HEAP_TERMINATION
  push 0
  push 0
  push 1
  push 0
  call dword ptr ["__imp__HeapSetInformation@16"]
#endif
#if REGISTRATION_CASE == 4
  call _registration_no_raise
#else
  call _registration_raise
#endif
  registration_set_state REGISTRATION_SENTINEL
#if REGISTRATION_CASE == 4
  call .Lfinally
#endif
#if REGISTRATION_CASE == 3 || REGISTRATION_CASE == 4
  mov eax,7
#else
  mov eax,9
#endif
  jmp .Lfinish
.Lfilter:
#if REGISTRATION_CASE == 7 || REGISTRATION_CASE == 8
  mov eax,[ebp-20]
  mov eax,[eax]
#if REGISTRATION_CASE == 8
  mov eax,[eax+4]
  cmp eax,1
#else
  mov eax,[eax]
  cmp eax,0xe0420042
#endif
  jne .Lwrong_exception
  mov dword ptr [_registration_trace],7
  mov eax,1
  ret
.Lwrong_exception:
  xor eax,eax
#elif REGISTRATION_CASE == 1
  mov eax,[ebp-4]
  mov [_registration_trace],eax
  mov eax,1
#elif REGISTRATION_CASE == 2
  mov eax,[_registration_trace]
  imul eax,eax,10
  add eax,2
  mov [_registration_trace],eax
  mov eax,1
#elif REGISTRATION_CASE == 5 || REGISTRATION_CASE == 6
  mov eax,[ebp+8]
  mov [_registration_trace],eax
  mov eax,1
#elif REGISTRATION_CASE == 3
  mov eax,[ebp-4]
  add eax,1
  mov [_registration_trace],eax
  mov eax,-1
#else
  mov eax,[ebp-4]
  add eax,1
#endif
  ret
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 4
.Lfinally:
#if REGISTRATION_CASE == 4
  mov eax,[ebp-4]
#if REGISTRATION_SENTINEL == -2
  add eax,4
#else
  add eax,3
#endif
#else
  mov eax,[_registration_trace]
  imul eax,eax,10
  add eax,2
#endif
  mov [_registration_trace],eax
  ret
#elif REGISTRATION_CASE == 2
.Linnerfilter:
  mov eax,[ebp-4]
  mov [_registration_trace],eax
  xor eax,eax
  ret
.Linnerhandler:
  registration_set_state REGISTRATION_SENTINEL
  mov eax,91
  jmp .Lfinish
#endif
.Lhandler:
#if REGISTRATION_SHARED_SEH4
  mov esp,[ebp-24]
#endif
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 2
  mov eax,[_registration_trace]
  imul eax,eax,10
  add eax,3
  mov [_registration_trace],eax
#endif
#if REGISTRATION_CASE == 3 || REGISTRATION_CASE == 4
  mov eax,97
#elif REGISTRATION_CASE == 5
  mov eax,[ebp+8]
#elif REGISTRATION_CASE == 6
  add dword ptr [ebp+8],10
  mov eax,[ebp+8]
  sub eax,10
#else
  mov eax,7
#endif
.Lfinish:
#if REGISTRATION_SHARED_SEH4
  registration_set_state REGISTRATION_SENTINEL
  call __seh_epilog4
#else
#if REGISTRATION_EXPECT_GS
  mov ecx, dword ptr [ebp - 32]
  xor ecx, ebp
  call "@__security_check_cookie@4"
#endif
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
#endif
#if REGISTRATION_PARENT_POP
  ret REGISTRATION_PARENT_POP
#else
  ret
#endif
#if REGISTRATION_CASE == 6
.globl _registration_call_entry
_registration_call_entry:
  push 7
  call _registration_entry
  mov edx,[esp]
  mov [_registration_parameter_after],edx
  add esp,4
  ret
#endif
