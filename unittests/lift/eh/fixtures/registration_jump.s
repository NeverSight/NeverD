// Native SEH capture, nonlocal finally unwind, then another exception.
#if REGISTRATION_EH4_JUMP
#define JUMP_SENTINEL -2
#define JUMP_PERSONALITY __except_handler4
#else
#define JUMP_SENTINEL -1
#define JUMP_PERSONALITY __except_handler3
#endif
#if REGISTRATION_CASE == 16 || REGISTRATION_CASE == 17
#define FILTER_JUMP 1
#endif
#if REGISTRATION_CASE == 18 || REGISTRATION_CASE == 19
#define RETIRED_JUMP 1
#endif
.intel_syntax noprefix
.text
.globl _registration_entry
_registration_entry:
  push ebp
  mov ebp,esp
  push JUMP_SENTINEL
  .byte 0x68
  .long .Lscope_table
  .byte 0x68
  .long JUMP_PERSONALITY
  mov eax,fs:[0]
  push eax
#if REGISTRATION_EH4_JUMP
  sub esp,96
  mov eax,[___security_cookie]
  xor dword ptr [ebp-8],eax
  xor eax,ebp
  mov [ebp-28],eax
#if REGISTRATION_EXPECT_GS
  mov [ebp-32],eax
#endif
  lea eax,[ebp-16]
  mov fs:[0],eax
#else
  mov fs:[0],esp
  sub esp,96
#endif
  mov [ebp-24],esp
  mov [ebp-104],ebx
  mov ebx,6
  mov dword ptr [ebp-100],0
#if REGISTRATION_CASE == 25
  mov dword ptr [ebp-112],0
#endif
#if REGISTRATION_CASE == 24
  mov dword ptr [ebp-112],0xdeadbeef
  push 0
  push 0
  lea eax,[ebp-112]
  push eax
  call dword ptr [__imp___controlfp_s]
  add esp,12
  test eax,eax
  jne .Lbad
  cmp dword ptr [ebp-112],0xdeadbeef
  je .Lbad
#endif
#if RETIRED_JUMP
  mov dword ptr [ebp-108],0
#endif
#if REGISTRATION_EH4_JUMP
  .byte 0x68
  .long ___security_cookie
#endif
#if REGISTRATION_CASE == 19
  mov dword ptr [ebp-4],1
  push 1
#elif FILTER_JUMP
  mov dword ptr [ebp-4],JUMP_SENTINEL
  push JUMP_SENTINEL
#else
  mov dword ptr [ebp-4],0
  push 0
#endif
  .byte 0x68
#if REGISTRATION_EH4_JUMP
  .long __seh_longjmp_unwind4
  push 3
#else
  .long __seh_longjmp_unwind
  push 2
#endif
  lea eax,[ebp-96]
  push eax
  call __setjmp3
#if REGISTRATION_EH4_JUMP
  add esp,20
#else
  add esp,16
#endif
  test eax,eax
  jnz .Lresumed
#if REGISTRATION_CASE == 50
  call _registration_no_raise
#elif REGISTRATION_CASE == 51
  call _registration_private_graph
#endif
#if RETIRED_JUMP
  mov dword ptr [ebp-108],1
#if REGISTRATION_CASE == 19
  mov dword ptr [ebp-4],0
#else
  mov dword ptr [ebp-4],JUMP_SENTINEL
#endif
  mov ebx,9
#if REGISTRATION_CASE == 19
  mov dword ptr [ebp-100],2
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#else
  call .Lfinally
  jmp .Lbad
#endif
#elif FILTER_JUMP
  mov dword ptr [ebp-4],0
  mov ebx,9
.Lfault:
  mov eax,dword ptr [0]
  jmp .Lfault
#elif REGISTRATION_CASE == 20 || REGISTRATION_CASE == 21 || REGISTRATION_CASE == 22
  mov dword ptr [ebp-4],0
  mov dword ptr [ebp-100],0
  mov ebx,9
  sub esp,160
#if REGISTRATION_CASE == 22
  push 42
#endif
  lea eax,[ebp-100]
  push eax
  lea eax,[ebp-96]
  push eax
  call _registration_jump_callee
#if REGISTRATION_CASE == 22
  add esp,172
#else
  add esp,168
#endif
#if REGISTRATION_CASE == 21
#if REGISTRATION_OBSERVE_CALLEE_RESULT
  // Observe the actual helper EAX through the subsequent longjmp value.
  add eax,5
  push eax
#else
  push 42
#endif
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#endif
  jmp .Lbad
#else
#if REGISTRATION_CASE == 28 || REGISTRATION_CASE == 29
  mov dword ptr [ebp-4],2
#endif
  mov dword ptr [ebp-4],1
  mov ebx,9
#if REGISTRATION_CASE == 13 || REGISTRATION_CASE == 15 || REGISTRATION_CASE == 27 || REGISTRATION_CASE == 28
  mov dword ptr [ebp-4],0
  call .Lfinally
  jmp .Lbad
#else
#if REGISTRATION_CASE == 10 || REGISTRATION_CASE == 15
  push 0
#else
  push 42
#endif
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#endif
#endif
.Lresumed:
#if REGISTRATION_CASE == 50 || REGISTRATION_CASE == 51
  cmp dword ptr [_registration_helper_visits],1
  jne .Lbad
#endif
#if REGISTRATION_CASE == 12
  cmp dword ptr [ebp+8],7
  jne .Lbad
#endif
#if FILTER_JUMP || REGISTRATION_CASE == 18
  cmp dword ptr [ebp-4],JUMP_SENTINEL
#else
  cmp dword ptr [ebp-4],0
#endif
#if REGISTRATION_CASE == 26 || REGISTRATION_CASE == 27
  jne .Lbad_level
#else
  jne .Lbad
#endif
  cmp ebx,6
#if REGISTRATION_CASE == 26 || REGISTRATION_CASE == 27
  jne .Lbad_register
#else
  jne .Lbad
#endif
#if REGISTRATION_CASE == 10 || REGISTRATION_CASE == 15 || REGISTRATION_CASE == 17
  cmp eax,1
#else
  cmp eax,42
#endif
#if REGISTRATION_CASE == 26 || REGISTRATION_CASE == 27
  jne .Lbad_value
#else
  jne .Lbad
#endif
#if FILTER_JUMP || REGISTRATION_CASE == 18 || REGISTRATION_CASE == 26 || REGISTRATION_CASE == 27
#if RETIRED_JUMP
  mov dword ptr [ebp-4],JUMP_SENTINEL
#endif
  call _registration_no_raise
#if REGISTRATION_CASE == 26 || REGISTRATION_CASE == 27
  mov dword ptr [ebp-4],JUMP_SENTINEL
#endif
  mov eax,7
  jmp .Lfinish
#else
#if REGISTRATION_CASE == 19
  mov dword ptr [ebp-4],0
#endif
#if REGISTRATION_CASE == 23 || REGISTRATION_CASE == 25
  call _registration_no_raise
  push 0
  push 0
#if REGISTRATION_CASE == 25
  push 0
#else
  push 1
#endif
  push 0xe0420042
  call dword ptr [__imp__RaiseException@16]
#if REGISTRATION_CASE == 25
  mov dword ptr [esp],7
  cmp dword ptr [ebp-112],7
  jne .Lbad
  mov dword ptr [ebp-4],JUMP_SENTINEL
  mov eax,7
  jmp .Lfinish
#endif
#else
  call _registration_raise
#endif
#endif
.Lbad:
  mov dword ptr [ebp-4],JUMP_SENTINEL
  mov eax,91
  jmp .Lfinish
#if REGISTRATION_CASE == 26 || REGISTRATION_CASE == 27
.Lbad_level:
  mov dword ptr [ebp-4],JUMP_SENTINEL
  mov eax,92
  jmp .Lfinish
.Lbad_register:
  mov dword ptr [ebp-4],JUMP_SENTINEL
  mov eax,93
  jmp .Lfinish
.Lbad_value:
  mov dword ptr [ebp-4],JUMP_SENTINEL
  mov eax,94
  jmp .Lfinish
#endif
.Lfilter:
#if FILTER_JUMP
  mov dword ptr [_registration_trace],23
#if REGISTRATION_CASE == 17
  push 0
#else
  push 42
#endif
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#else
  mov eax,[ebp-100]
  imul eax,eax,10
#if REGISTRATION_CASE == 12
  add eax,[ebp+8]
  sub eax,4
#else
  add eax,3
#endif
  mov [_registration_trace],eax
#if REGISTRATION_CASE == 25 || REGISTRATION_CASE == 26 || REGISTRATION_CASE == 27
  mov eax,-1
#else
  mov eax,1
#endif
  ret
#endif
.Lfinally:
#if RETIRED_JUMP
  cmp dword ptr [ebp-108],0
  je .Lfinally_return
  mov dword ptr [_registration_trace],23
#endif
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,2
  mov [ebp-100],eax
#if REGISTRATION_CASE == 28 || REGISTRATION_CASE == 29
  push 7
  call .Linner_finally
  pop edx
  add [ebp-100],edx
  sub dword ptr [ebp-100],7
#endif
#if REGISTRATION_CASE == 26 || REGISTRATION_CASE == 27
  push 0
  push 0
  push 0
  push 0xe0420042
  call dword ptr [__imp__RaiseException@16]
#endif
#if REGISTRATION_CASE >= 13
#if REGISTRATION_CASE == 15
  push 0
#else
  push 42
#endif
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#else
  ret
#endif
#if RETIRED_JUMP
.Lfinally_return:
  ret
#endif
#if REGISTRATION_CASE == 19
.Linner_handler:
  mov dword ptr [ebp-4],JUMP_SENTINEL
  mov eax,94
  jmp .Lfinish
#endif
#if REGISTRATION_CASE == 28 || REGISTRATION_CASE == 29
.Linner_finally:
  push 9
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,4
  pop edx
  add eax,edx
  sub eax,9
  mov [ebp-100],eax
  ret
#endif
.Lhandler:
  mov eax,7
.Lfinish:
#if REGISTRATION_EXPECT_GS
  mov ecx,[ebp-32]
  xor ecx,ebp
  call "@__security_check_cookie@4"
#endif
  mov ebx,[ebp-104]
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
#if REGISTRATION_EH4_JUMP
#include "registration_eh4_personality.inc"
#endif
.safeseh JUMP_PERSONALITY
.section .rdata,"dr"
.p2align 2
.Lscope_table:
#if REGISTRATION_EH4_JUMP
#if REGISTRATION_EXPECT_GS
  .long -32,0,-28,0
#else
  .long -2,0,-28,0
#endif
#endif
#if REGISTRATION_CASE == 18
  .long JUMP_SENTINEL,0,.Lfinally
#else
  .long JUMP_SENTINEL,.Lfilter,.Lhandler
#if !FILTER_JUMP && REGISTRATION_CASE != 20 && REGISTRATION_CASE != 21 && REGISTRATION_CASE != 22
#if REGISTRATION_CASE == 19
  .long 0,.Lfilter,.Linner_handler
#elif REGISTRATION_CASE == 11
  .long 0,.Lfilter,.Lbad
#else
  .long 0,0,.Lfinally
#endif
#endif
#endif
#if REGISTRATION_CASE == 28 || REGISTRATION_CASE == 29
  .long 1,0,.Linner_finally
#endif
  .long 0,0,0
.globl __load_config_used
__load_config_used:
  .long 92
#if REGISTRATION_EH4_JUMP
  .zero 56
  .long ___security_cookie
#else
  .zero 60
#endif
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
  .long 0,0,0,0,0x400
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1

#if REGISTRATION_EH4_JUMP
#include "registration_cookie_check.inc"
#endif
