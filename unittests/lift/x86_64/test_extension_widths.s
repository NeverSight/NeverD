// Integer-extension width regressions for issue #11. Every function targets a
// formerly equal-width extension site; the final function keeps one genuine
// widening conversion in the fixture so the parser cannot pass vacuously.

        .text

        .globl  test_setcc_memory_width
        .type   test_setcc_memory_width,@function
test_setcc_memory_width:
        cmpl    %esi, %edx
        sete    (%rdi)
        retq
        .size   test_setcc_memory_width, .-test_setcc_memory_width

        .globl  test_adc_byte_width
        .type   test_adc_byte_width,@function
test_adc_byte_width:
        stc
        adcb    %sil, (%rdi)
        retq
        .size   test_adc_byte_width, .-test_adc_byte_width

        .globl  test_sbb_byte_width
        .type   test_sbb_byte_width,@function
test_sbb_byte_width:
        stc
        sbbb    %sil, (%rdi)
        retq
        .size   test_sbb_byte_width, .-test_sbb_byte_width

        .globl  test_rcl_byte_width
        .type   test_rcl_byte_width,@function
test_rcl_byte_width:
        rclb    %cl, (%rdi)
        retq
        .size   test_rcl_byte_width, .-test_rcl_byte_width

        .globl  test_rcr_byte_width
        .type   test_rcr_byte_width,@function
test_rcr_byte_width:
        rcrb    %cl, (%rdi)
        retq
        .size   test_rcr_byte_width, .-test_rcr_byte_width

        .globl  test_lahf_width
        .type   test_lahf_width,@function
test_lahf_width:
        lahf
        retq
        .size   test_lahf_width, .-test_lahf_width

        .globl  test_genuine_zext_width
        .type   test_genuine_zext_width,@function
test_genuine_zext_width:
        movzbl  %dil, %eax
        retq
        .size   test_genuine_zext_width, .-test_genuine_zext_width

        .section .note.GNU-stack,"",@progbits
