/* Owned x86 source-grounded function-splitting fixture. Never executed. */
        .text
        .globl original
        .type original,@function
original:
        mov %edi,%eax
        add $3,%eax
        xor $23,%eax
        imul $7,%eax,%eax
        add $9,%eax
#ifdef REGION_SPLIT
        ret
        .size original, .-original
        .globl extracted
        .type extracted,@function
extracted:
#else
        jmp .Lsecond
.Lsecond:
#endif
        mov %esi,%edx
        add $19,%edx
        xor $93,%edx
        imul $11,%edx,%edx
        add $27,%edx
        ret
#ifdef REGION_SPLIT
        .size extracted, .-extracted
#else
        .size original, .-original
#endif
        .section .note.GNU-stack,"",@progbits
