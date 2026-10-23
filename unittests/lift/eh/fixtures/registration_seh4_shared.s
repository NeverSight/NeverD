// Shared SEH4 installation/restoration with independent runtime observers.
.intel_syntax noprefix
.text
.globl _registration_entry
_registration_entry:
#if REGISTRATION_STARTUP_INFO
  push 92
#else
  push 32
#endif
  .byte 0x68
  .long .Lscope_table
  call __seh_prolog4
#define REGISTRATION_SENTINEL -2
#define REGISTRATION_SHARED_SEH4 1
#include "registration_seh_body.inc"

// Keep the physical helper protocol explicit, including transient PC writes.
.p2align 4,0x90
__seh_prolog4:
  .byte 0x68
  .long __except_handler4
  .byte 0x64,0xff,0x35
  .long 0
  .byte 0x8b,0x44,0x24,0x10,0x89,0x6c,0x24,0x10
  .byte 0x8d,0x6c,0x24,0x10,0x2b,0xe0,0x53,0x56,0x57,0xa1
  .long ___security_cookie
  .byte 0x31,0x45,0xfc,0x33,0xc5,0x50,0x89,0x65,0xe8
  .byte 0xff,0x75,0xf8,0x8b,0x45,0xfc
  .byte 0xc7,0x45,0xfc,0xfe,0xff,0xff,0xff
  .byte 0x89,0x45,0xf8,0x8d,0x45,0xf0,0x64,0xa3
  .long 0
  ret
__seh_epilog4:
  .byte 0x8b,0x4d,0xf0,0x64,0x89,0x0d
  .long 0
  .byte 0x59,0x5f,0x5f,0x5e,0x5b,0x8b,0xe5,0x5d,0x51,0xc3

#include "registration_eh4_personality.inc"
.safeseh __except_handler4
.section .rdata,"dr"
.p2align 2
.Lscope_table:
#if REGISTRATION_STARTUP_INFO
  .long -2,0,-124,0
#else
  .long -2,0,-64,0
#endif
#include "registration_seh_scopes.inc"
.globl __load_config_used
__load_config_used:
  .long 92
  .zero 56
  .long ___security_cookie
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
  .long 0,0,0,0,0x400
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1
#include "registration_cookie_check.inc"
