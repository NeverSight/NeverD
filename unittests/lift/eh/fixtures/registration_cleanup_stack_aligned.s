# Authored MSVC-style aligned ESP parent. Entry EBP and the aligned
# runtime establisher remain separate; the C++ fixture owns throw and catch.
.intel_syntax noprefix
.text
.p2align 4, 0x90
.globl _cleanup_only_parent
_cleanup_only_parent:
  push ebp
  .byte 0x8b, 0xec
  and esp, -16
  push -1
  .byte 0x68
  .long stack_handler
  mov eax, dword ptr fs:[0]
  push eax
  sub esp, 32
  push ebx
  push esi
  push edi
  lea eax, [esp+44]
  mov dword ptr fs:[0], eax
  mov eax, dword ptr [_cleanup_only_enabled]
  test eax, eax
  jne stack_throw
  xor eax, eax
  mov ecx, dword ptr [esp+44]
  mov dword ptr fs:[0], ecx
  pop edi
  pop esi
  pop ebx
  mov esp, ebp
  pop ebp
  ret
stack_throw:
  mov dword ptr [esp+16], 1
  mov dword ptr [esp+20], 2
  mov dword ptr [esp+52], 0
  call _cleanup_only_throw
  int3
.p2align 4, 0x90
stack_cleanup:
  push ebp
  add ebp, 0
  lea ecx, [ebp-36]
  call "??1CleanupOnlyGuard@@QAE@XZ"
  lea ecx, [ebp-40]
  call "??1CleanupOnlyGuard@@QAE@XZ"
  pop ebp
  ret
.p2align 4, 0x90
stack_handler:
  mov eax, offset stack_info
  jmp ___CxxFrameHandler3
.safeseh stack_handler
.section .rdata,"dr"
.p2align 2
stack_info:
  .long 0x19930522, 1, stack_unwind, 0, 0, 0, 0, 0, 1
stack_unwind:
  .long -1, stack_cleanup
.globl __load_config_used
__load_config_used:
  .long 72
  .zero 56
  .long ___security_cookie
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
.def @feat.00; .scl 3; .type 0; .endef
.set @feat.00, 1
.data
.p2align 2
___security_cookie: .long 0xbb40e64e
