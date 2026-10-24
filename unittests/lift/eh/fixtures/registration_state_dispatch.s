// A shared search filter overwrites one byte of two distinct complete levels.
// The low path writes 0; the high path writes 256 and must visit the lower
// finally as well. Four alternating calls observe one or two cleanup visits,
// the restored EBX/EAX, the final level and the unchanged registration chain.
.intel_syntax noprefix
.text
.globl _registration_entry
_registration_entry:
  push ebp
  mov ebp,esp
  push -1
  .byte 0x68
  .long .Lscope_table
  .byte 0x68
  .long __except_handler3
  mov eax,fs:[0]
  push eax
  mov fs:[0],esp
  sub esp,96
  mov [ebp-24],esp
  mov [ebp-104],ebx
  mov ebx,6
  mov dword ptr [ebp-100],0
  // Give the intermediate sibling rows independent ordinary reachability.
  // Their depth is unrelated to this test's shared filter result domain.
  .set .Lseed_level,3
  .rept 254
  mov dword ptr [ebp-4],.Lseed_level
  .set .Lseed_level,.Lseed_level+1
  .endr
#if REGISTRATION_STATE_WRITE == 1
  mov ecx,0
  nop
  mov dword ptr [ebp-4],ecx
#elif REGISTRATION_STATE_WRITE == 2
  and dword ptr [ebp-4],0
#else
  mov dword ptr [ebp-4],0
#endif
  push 0
  .byte 0x68
  .long __seh_longjmp_unwind
  push 2
  lea eax,[ebp-96]
  push eax
  call __setjmp3
  add esp,16
  test eax,eax
  jnz .Lresumed
  mov ebx,9
  mov eax,[_registration_iteration]
  test eax,1
  jnz .Lhigh
  mov dword ptr [ebp-4],2
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
.Lhigh:
  mov dword ptr [ebp-4],258
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
.Lresumed:
  cmp ebx,6
  jne .Lbad
  cmp eax,42
  jne .Lbad
  cmp dword ptr [ebp-4],0
  jne .Lbad
  mov eax,[_registration_iteration]
  and eax,1
  add eax,1
  cmp [ebp-100],eax
  jne .Lbad
  mov [_registration_trace],eax
  mov dword ptr [ebp-4],-1
  call _registration_no_raise
  mov eax,7
  jmp .Lfinish
.Lbad:
  mov dword ptr [ebp-4],-1
  mov eax,91
  jmp .Lfinish
.Lfilter:
  mov byte ptr [ebp-4],0
  mov eax,-1
  ret
.Louter_filter:
  mov eax,-1
  ret
.Lfinally:
  mov eax,[ebp-100]
  add eax,1
  mov [ebp-100],eax
  push 0
  push 0
  push 0
  push 0xe0420042
  call dword ptr [__imp__RaiseException@16]
#if REGISTRATION_CASE != 44
  mov eax,0
#endif
  ret
.Louter_handler:
  mov dword ptr [ebp-4],-1
  mov eax,96
  jmp .Lfinish
.Llow_handler:
  mov dword ptr [ebp-4],-1
  mov eax,97
  jmp .Lfinish
.Lhigh_handler:
  mov dword ptr [ebp-4],-1
  mov eax,98
  jmp .Lfinish
.Lunused_filter:
  mov eax,-1
  ret
.Lunused_handler:
  mov dword ptr [ebp-4],-1
  mov eax,99
  jmp .Lfinish
.Lfilter256:
  mov eax,-1
  ret
.Lhandler256:
  mov dword ptr [ebp-4],-1
  mov eax,99
  jmp .Lfinish
.Lfinish:
  mov ebx,[ebp-104]
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
.safeseh __except_handler3
.section .rdata,"dr"
.p2align 2
.Lscope_table:
  .long -1,.Louter_filter,.Louter_handler
  .long 0,.Lfilter,.Llow_handler
  .long 1,0,.Lfinally
  .rept 253
  .long 0,.Lunused_filter,.Lunused_handler
  .endr
  .long 2,.Lfilter256,.Lhandler256
  .long 256,.Lfilter,.Lhigh_handler
  .long 257,0,.Lfinally
  .long 0,0,0
.globl __load_config_used
__load_config_used:
  .long 92
  .zero 60
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
  .long 0,0,0,0,0x400
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1
