# SEH3 companion to registration_cleanup_shared.s. The C++ fixture supplies
# the load-config table, cookie and observer. Both parent bodies are replaced.
.intel_syntax noprefix
.text
.p2align 4, 0x90
.globl _mixed_seh
_mixed_seh:
  push ebp
  mov ebp,esp
  push -1
  .byte 0x68
  .long mixed_scopes
  .byte 0x68
  .long __except_handler3
  mov eax,fs:[0]
  push eax
  mov fs:[0],esp
  sub esp,16
  # The native dispatcher enters the handler with volatile register state.
  # Preserve the caller's nonvolatile registers across that transfer.
  push ebx
  push esi
  push edi
  mov [ebp-24],esp
  mov dword ptr [ebp-4],0
  push 0
  push 0
  push 0
  push 0xe0421189
  call dword ptr [__imp__RaiseException@16]
  mov dword ptr [ebp-4],-1
  mov eax,9
  jmp mixed_finish
mixed_filter:
  mov eax,1
  ret
mixed_handler:
  mov eax,31
mixed_finish:
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  lea esp,[ebp-44]
  pop edi
  pop esi
  pop ebx
  mov esp,ebp
  pop ebp
  ret
.safeseh __except_handler3
.section .rdata,"dr"
.p2align 2
mixed_scopes:
  .long -1,mixed_filter,mixed_handler
  .long 0,0,0
.def @feat.00; .scl 3; .type 0; .endef
.set @feat.00,1
