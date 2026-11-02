# A shared-prologue counterpart to registration_cleanup_only.cpp. The C++
# translation unit supplies the real throwing helper, destructor and caller
# catch; this parent owns its independently encoded physical frame and table.
.intel_syntax noprefix
.text
.p2align 4, 0x90
.ifdef NEVERD_NESTED_CXX_PARENT
.globl _cleanup_only_inner
_cleanup_only_inner:
.else
.globl _cleanup_only_parent
_cleanup_only_parent:
.endif
  push 32
  mov eax, offset _shared_handler
  call _shared_prologue
  mov eax, dword ptr [_cleanup_only_enabled]
  test eax, eax
  jne shared_throw
  xor eax, eax
  call _shared_epilogue
  ret
shared_throw:
  mov dword ptr [ebp-24], 1
  mov dword ptr [ebp-20], 2
  mov dword ptr [ebp-4], 0
  call _cleanup_only_throw
  int3
.p2align 4, 0x90
shared_cleanup:
  push ebp
  add ebp, 0
  lea ecx, [ebp-20]
  call "??1CleanupOnlyGuard@@QAE@XZ"
  lea ecx, [ebp-24]
  call "??1CleanupOnlyGuard@@QAE@XZ"
  pop ebp
  ret
.p2align 4, 0x90
_shared_prologue:
  push eax
  push dword ptr fs:[0]
  lea eax, [esp+12]
  sub esp, dword ptr [esp+12]
  push ebx
  push esi
  push edi
  mov dword ptr [eax], ebp
  .byte 0x8b, 0xe8
  mov eax, dword ptr [___security_cookie]
  .byte 0x33, 0xc5
  push eax
  mov dword ptr [ebp-16], esp
  push dword ptr [ebp-4]
  mov dword ptr [ebp-4], -1
  lea eax, [ebp-12]
  mov dword ptr fs:[0], eax
  ret
.p2align 4, 0x90
_shared_epilogue:
  mov ecx, dword ptr [ebp-12]
  mov dword ptr fs:[0], ecx
  pop ecx
  pop edi
  pop edi
  pop esi
  pop ebx
  .byte 0x8b, 0xe5
  pop ebp
  push ecx
  ret
.p2align 4, 0x90
_shared_handler:
  mov eax, offset shared_info
  jmp ___CxxFrameHandler3
.safeseh _shared_handler
.section .rdata,"dr"
.p2align 2
shared_info:
  .long 0x19930522, 1, shared_unwind, 0, 0, 0, 0, 0, 1
shared_unwind:
  .long -1, shared_cleanup
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

.ifdef NEVERD_NESTED_CXX_PARENT
.section .drectve,"yn"
.ascii " /EXPORT:_cleanup_only_inner"
.endif
