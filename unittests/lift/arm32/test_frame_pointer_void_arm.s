// Clang 20+ keeps an ARM frame pointer at -O0 even for this leaf shape.
// The saved link register is restored into pc and must not be mistaken for a
// value returned through r0 by LLVM-C void-return inference.

    .syntax unified
    .text
    .arm

    .globl test_frame_pointer_void_arm
    .type test_frame_pointer_void_arm, %function
test_frame_pointer_void_arm:
    push {r11, lr}
    mov r11, sp
    nop
    pop {r11, pc}
    .size test_frame_pointer_void_arm, .-test_frame_pointer_void_arm

// The same restored return address is a genuine result when explicitly loaded
// into r0.  This guards the void fix from classifying by value provenance alone.
    .globl test_frame_pointer_return_address_arm
    .type test_frame_pointer_return_address_arm, %function
test_frame_pointer_return_address_arm:
    push {r11, lr}
    mov r11, sp
    sub sp, sp, #8
    str lr, [sp]
    ldr r0, [sp]
    mov sp, r11
    pop {r11, pc}
    .size test_frame_pointer_return_address_arm, .-test_frame_pointer_return_address_arm
