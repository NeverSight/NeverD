// An entry Q register read by AESE, or by EOR, is that register. A
// parameter, a write, a 64-bit half, and a store of the same register are
// not. An entry GPR is a pointer-authentication modifier only at that
// operand; used as an address it stays unknown.

        .text
        .globl  entry_q_is_the_aes_key
        .type   entry_q_is_the_aes_key,%function
entry_q_is_the_aes_key:
        aese    v0.16b, v17.16b
        str     q0, [x0]
        ret
        .size   entry_q_is_the_aes_key, .-entry_q_is_the_aes_key

        .globl  abi_vectors_stay_parameters
        .type   abi_vectors_stay_parameters,%function
abi_vectors_stay_parameters:
        aese    v0.16b, v1.16b
        str     q0, [x0]
        ret
        .size   abi_vectors_stay_parameters, .-abi_vectors_stay_parameters

        .globl  written_q_is_not_the_entry_register
        .type   written_q_is_not_the_entry_register,%function
written_q_is_not_the_entry_register:
        movi    v17.16b, #1
        aese    v0.16b, v17.16b
        str     q0, [x0]
        ret
        .size   written_q_is_not_the_entry_register, .-written_q_is_not_the_entry_register

        .globl  d_half_stays_unknown
        .type   d_half_stays_unknown,%function
d_half_stays_unknown:
        fadd    d0, d0, d17
        str     d0, [x0]
        ret
        .size   d_half_stays_unknown, .-d_half_stays_unknown

        .globl  entry_q_xor_is_the_register
        .type   entry_q_xor_is_the_register,%function
entry_q_xor_is_the_register:
        eor     v2.16b, v2.16b, v31.16b
        str     q2, [x0]
        ret
        .size   entry_q_xor_is_the_register, .-entry_q_xor_is_the_register

        .globl  stored_q_stays_unknown
        .type   stored_q_stays_unknown,%function
stored_q_stays_unknown:
        str     q17, [x0]
        ret
        .size   stored_q_stays_unknown, .-stored_q_stays_unknown

        .globl  pacia_names_entry_modifier
        .type   pacia_names_entry_modifier,%function
pacia_names_entry_modifier:
        pacia   x0, x16
        ret
        .size   pacia_names_entry_modifier, .-pacia_names_entry_modifier

        .globl  pacia_parameter_modifier_stays_argument
        .type   pacia_parameter_modifier_stays_argument,%function
pacia_parameter_modifier_stays_argument:
        pacia   x0, x1
        ret
        .size   pacia_parameter_modifier_stays_argument, .-pacia_parameter_modifier_stays_argument

        .globl  address_use_stays_unknown
        .type   address_use_stays_unknown,%function
address_use_stays_unknown:
        ldr     x0, [x16]
        ret
        .size   address_use_stays_unknown, .-address_use_stays_unknown
