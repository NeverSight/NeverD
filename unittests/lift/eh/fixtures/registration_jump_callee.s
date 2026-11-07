// A separately registered helper borrows its caller's buffer and scalar local.
.intel_syntax noprefix
.text
.globl _registration_jump_callee
_registration_jump_callee:
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
  sub esp,32
  mov [ebp-24],esp
  // Mutations replace this same-size window with frame-observing branches.
  .space 8,0x90
  mov ecx,[ebp+12]
  mov dword ptr [ecx],2
  mov dword ptr [ebp-4],-1
#if REGISTRATION_CASE == 21
#if REGISTRATION_OBSERVE_CALLEE_RESULT
  mov eax,37
#endif
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
#else
#if REGISTRATION_CASE == 22
  push dword ptr [ebp+16]
#else
  push 42
#endif
  push dword ptr [ebp+8]
  call __longjmpex
  ud2
#endif
.Lfilter:
  mov eax,1
  ret
.Lhandler:
  mov eax,95
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
.safeseh __except_handler3
.section .rdata,"dr"
.p2align 2
.Lscope_table:
  .long -1,.Lfilter,.Lhandler
  .long 0,0,0
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1
