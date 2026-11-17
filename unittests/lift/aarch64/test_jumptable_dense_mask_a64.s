.text
.p2align 2

// ubfx w, #1, #2 is a dense mask of two bits. Nothing compares that lane.
// The compact halfword load addresses it with the widened register. The
// domain is the mask, including when earlier guards only test other
// registers. The halfword after the four proved slots is not an entry.
.globl a64_ubfx_dense_half
.type a64_ubfx_dense_half, %function
a64_ubfx_dense_half:
  cbz x1, .Ldense_default
  ldr x2, [x1]
  cbz x2, .Ldense_default
  mov w24, w0
  ubfx w9, w24, #1, #2
  adrp x12, .Ldense_table
  add x12, x12, :lo12:.Ldense_table
  adr x10, .Ldense_anchor
  ldrh w11, [x12, x9, lsl #1]
  add x10, x10, x11, lsl #2
  br x10
.Ldense_anchor:
.Ldense_c0:
  mov w0, #10
  ret
.Ldense_c1:
  mov w0, #11
  ret
.Ldense_c2:
  mov w0, #12
  ret
.Ldense_c3:
  mov w0, #13
  ret
.Ldense_default:
  mov w0, #-1
  ret
.Ldense_poison:
  mov w0, #499
  ret
.size a64_ubfx_dense_half, .-a64_ubfx_dense_half

// The same mask is wider than the compare of its own lane. The compare
// owns the domain: slots 2 and 3 stay out of the switch.
.globl a64_ubfx_dense_half_tighter
.type a64_ubfx_dense_half_tighter, %function
a64_ubfx_dense_half_tighter:
  ubfx w9, w0, #1, #2
  cmp w9, #1
  b.hi .Ltight_default
  adrp x12, .Ltight_table
  add x12, x12, :lo12:.Ltight_table
  adr x10, .Ltight_anchor
  ldrh w11, [x12, x9, lsl #1]
  add x10, x10, x11, lsl #2
  br x10
.Ltight_anchor:
.Ltight_c0:
  mov w0, #20
  ret
.Ltight_c1:
  mov w0, #21
  ret
.Ltight_c2:
  mov w0, #498
  ret
.Ltight_c3:
  mov w0, #499
  ret
.Ltight_default:
  mov w0, #-1
  ret
.size a64_ubfx_dense_half_tighter, .-a64_ubfx_dense_half_tighter

// A later write of the masked lane is the value that reaches the load.
.globl a64_ubfx_dense_half_clobber
.type a64_ubfx_dense_half_clobber, %function
a64_ubfx_dense_half_clobber:
  ubfx w9, w0, #1, #2
  mov w9, w1
  adrp x12, .Lclobber_table
  add x12, x12, :lo12:.Lclobber_table
  adr x10, .Lclobber_anchor
  ldrh w11, [x12, x9, lsl #1]
  add x10, x10, x11, lsl #2
  br x10
.Lclobber_anchor:
.Lclobber_c0:
  mov w0, #30
  ret
.Lclobber_c1:
  mov w0, #31
  ret
.Lclobber_c2:
  mov w0, #32
  ret
.Lclobber_c3:
  mov w0, #33
  ret
.size a64_ubfx_dense_half_clobber, .-a64_ubfx_dense_half_clobber

// A call between the mask and the load can replace the caller-saved lane.
.globl a64_ubfx_dense_half_call
.type a64_ubfx_dense_half_call, %function
a64_ubfx_dense_half_call:
  stp x29, x30, [sp, #-16]!
  ubfx w9, w0, #1, #2
  bl a64_dense_mask_helper
  adrp x12, .Lcall_table
  add x12, x12, :lo12:.Lcall_table
  adr x10, .Lcall_anchor
  ldrh w11, [x12, x9, lsl #1]
  add x10, x10, x11, lsl #2
  ldp x29, x30, [sp], #16
  br x10
.Lcall_anchor:
.Lcall_c0:
  mov w0, #40
  ret
.Lcall_c1:
  mov w0, #41
  ret
.Lcall_c2:
  mov w0, #42
  ret
.Lcall_c3:
  mov w0, #43
  ret
.size a64_ubfx_dense_half_call, .-a64_ubfx_dense_half_call

.globl a64_dense_mask_helper
.type a64_dense_mask_helper, %function
a64_dense_mask_helper:
  ret
.size a64_dense_mask_helper, .-a64_dense_mask_helper

// cmp w0, #100 does not read the masked lane. and #7 is the tighter domain,
// so the eight proved slots publish and the following halfwords do not.
.globl a64_and_clamps_wide_compare
.type a64_and_clamps_wide_compare, %function
a64_and_clamps_wide_compare:
  cmp w0, #100
  b.hi .Lclamp_default
  and w9, w0, #7
  adrp x12, .Lclamp_table
  add x12, x12, :lo12:.Lclamp_table
  adr x10, .Lclamp_anchor
  ldrh w11, [x12, x9, lsl #1]
  add x10, x10, x11, lsl #2
  br x10
.Lclamp_anchor:
.Lclamp_c0: mov w0, #50; ret
.Lclamp_c1: mov w0, #51; ret
.Lclamp_c2: mov w0, #52; ret
.Lclamp_c3: mov w0, #53; ret
.Lclamp_c4: mov w0, #54; ret
.Lclamp_c5: mov w0, #55; ret
.Lclamp_c6: mov w0, #56; ret
.Lclamp_c7: mov w0, #57; ret
.Lclamp_poison:
  mov w0, #499
  ret
.Lclamp_default:
  mov w0, #-1
  ret
.size a64_and_clamps_wide_compare, .-a64_and_clamps_wide_compare

.section .rodata.a64_ubfx_dense_half,"a",%progbits
.p2align 1
.Ldense_table:
  .hword (.Ldense_c0 - .Ldense_anchor) / 4
  .hword (.Ldense_c1 - .Ldense_anchor) / 4
  .hword (.Ldense_c2 - .Ldense_anchor) / 4
  .hword (.Ldense_c3 - .Ldense_anchor) / 4
  .hword (.Ldense_poison - .Ldense_anchor) / 4

.section .rodata.a64_ubfx_dense_half_tighter,"a",%progbits
.p2align 1
.Ltight_table:
  .hword (.Ltight_c0 - .Ltight_anchor) / 4
  .hword (.Ltight_c1 - .Ltight_anchor) / 4
  .hword (.Ltight_c2 - .Ltight_anchor) / 4
  .hword (.Ltight_c3 - .Ltight_anchor) / 4

.section .rodata.a64_ubfx_dense_half_clobber,"a",%progbits
.p2align 1
.Lclobber_table:
  .hword (.Lclobber_c0 - .Lclobber_anchor) / 4
  .hword (.Lclobber_c1 - .Lclobber_anchor) / 4
  .hword (.Lclobber_c2 - .Lclobber_anchor) / 4
  .hword (.Lclobber_c3 - .Lclobber_anchor) / 4

.section .rodata.a64_ubfx_dense_half_call,"a",%progbits
.p2align 1
.Lcall_table:
  .hword (.Lcall_c0 - .Lcall_anchor) / 4
  .hword (.Lcall_c1 - .Lcall_anchor) / 4
  .hword (.Lcall_c2 - .Lcall_anchor) / 4
  .hword (.Lcall_c3 - .Lcall_anchor) / 4

.section .rodata.a64_and_clamps_wide_compare,"a",%progbits
.p2align 1
.Lclamp_table:
  .hword (.Lclamp_c0 - .Lclamp_anchor) / 4
  .hword (.Lclamp_c1 - .Lclamp_anchor) / 4
  .hword (.Lclamp_c2 - .Lclamp_anchor) / 4
  .hword (.Lclamp_c3 - .Lclamp_anchor) / 4
  .hword (.Lclamp_c4 - .Lclamp_anchor) / 4
  .hword (.Lclamp_c5 - .Lclamp_anchor) / 4
  .hword (.Lclamp_c6 - .Lclamp_anchor) / 4
  .hword (.Lclamp_c7 - .Lclamp_anchor) / 4
  .hword (.Lclamp_poison - .Lclamp_anchor) / 4
  .hword (.Lclamp_poison - .Lclamp_anchor) / 4
  .hword (.Lclamp_poison - .Lclamp_anchor) / 4
  .hword (.Lclamp_poison - .Lclamp_anchor) / 4
