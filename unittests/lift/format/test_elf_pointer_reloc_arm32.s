        .syntax unified
        .arm

        .text
        .globl  neverd_arm_pointer_relocation_fixture_entry
        .type   neverd_arm_pointer_relocation_fixture_entry,%function
neverd_arm_pointer_relocation_fixture_entry:
        bx      lr
        .size   neverd_arm_pointer_relocation_fixture_entry, .-neverd_arm_pointer_relocation_fixture_entry

        .type   neverd_arm_pointer_relocation_fixture_target_alpha,%function
neverd_arm_pointer_relocation_fixture_target_alpha:
        bx      lr
        .size   neverd_arm_pointer_relocation_fixture_target_alpha, .-neverd_arm_pointer_relocation_fixture_target_alpha

        .type   neverd_arm_pointer_relocation_fixture_target_beta,%function
neverd_arm_pointer_relocation_fixture_target_beta:
        bx      lr
        .size   neverd_arm_pointer_relocation_fixture_target_beta, .-neverd_arm_pointer_relocation_fixture_target_beta

        .type   neverd_arm_pointer_relocation_fixture_target_gamma,%function
neverd_arm_pointer_relocation_fixture_target_gamma:
        bx      lr
        .size   neverd_arm_pointer_relocation_fixture_target_gamma, .-neverd_arm_pointer_relocation_fixture_target_gamma

        .section .data.rel.ro,"aw",%progbits
        .p2align 2
        .globl  neverd_arm_pointer_relocation_fixture_table
        .type   neverd_arm_pointer_relocation_fixture_table,%object
neverd_arm_pointer_relocation_fixture_table:
        .word   neverd_arm_pointer_relocation_fixture_target_alpha
        .word   neverd_arm_pointer_relocation_fixture_target_beta
        .word   neverd_arm_pointer_relocation_fixture_target_gamma
        .size   neverd_arm_pointer_relocation_fixture_table, .-neverd_arm_pointer_relocation_fixture_table
