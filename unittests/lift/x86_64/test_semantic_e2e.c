/* End-to-end semantic verification for x86-64.
   Each function uses inline asm to perform a known computation,
   then verifies the result matches expected value. */

int test_add_semantics(void) {
    int result;
    __asm__ volatile (
        "movl $42, %%eax\n\t"
        "addl $8, %%eax\n\t"
        "movl %%eax, %0"
        : "=r"(result) :: "eax"
    );
    return result; /* expected: 50 */
}

int test_sub_semantics(void) {
    int result;
    __asm__ volatile (
        "movl $100, %%eax\n\t"
        "subl $37, %%eax\n\t"
        "movl %%eax, %0"
        : "=r"(result) :: "eax"
    );
    return result; /* expected: 63 */
}

int test_imul_semantics(void) {
    int result;
    __asm__ volatile (
        "movl $7, %%eax\n\t"
        "imull $6, %%eax, %%eax\n\t"
        "movl %%eax, %0"
        : "=r"(result) :: "eax"
    );
    return result; /* expected: 42 */
}

int test_shift_semantics(void) {
    int result;
    __asm__ volatile (
        "movl $0xFF00, %%eax\n\t"
        "shrl $8, %%eax\n\t"
        "movl %%eax, %0"
        : "=r"(result) :: "eax"
    );
    return result; /* expected: 0xFF = 255 */
}

int test_and_or_xor_chain(void) {
    int result;
    __asm__ volatile (
        "movl $0xFF, %%eax\n\t"
        "andl $0x0F, %%eax\n\t"
        "orl  $0x30, %%eax\n\t"
        "xorl $0x05, %%eax\n\t"
        "movl %%eax, %0"
        : "=r"(result) :: "eax"
    );
    return result; /* expected: (0xFF & 0x0F) | 0x30 ^ 0x05 = 0x0F | 0x30 ^ 0x05 = 0x3F ^ 0x05 = 0x3A = 58 */
}

int test_neg_semantics(void) {
    int result;
    __asm__ volatile (
        "movl $42, %%eax\n\t"
        "negl %%eax\n\t"
        "movl %%eax, %0"
        : "=r"(result) :: "eax"
    );
    return result; /* expected: -42 */
}

int test_lea_complex(void) {
    int result;
    __asm__ volatile (
        "movl $10, %%eax\n\t"
        "movl $3, %%ecx\n\t"
        "leal 5(%%eax, %%ecx, 4), %0"
        : "=r"(result) :: "eax", "ecx"
    );
    return result; /* expected: 10 + 3*4 + 5 = 27 */
}

long long test_sext_semantics(void) {
    long long result;
    __asm__ volatile (
        "movl $0xFFFFFF80, %%eax\n\t"
        "cdqe\n\t"
        "movq %%rax, %0"
        : "=r"(result) :: "rax"
    );
    return result; /* expected: 0xFFFFFFFFFFFFFF80 = -128 */
}

int test_rol_semantics(void) {
    unsigned int result;
    __asm__ volatile (
        "movl $0x80000001, %%eax\n\t"
        "roll $1, %%eax\n\t"
        "movl %%eax, %0"
        : "=r"(result) :: "eax"
    );
    return result; /* expected: 0x00000003 */
}

int test_bswap_semantics(void) {
    unsigned int result;
    __asm__ volatile (
        "movl $0x01020304, %%eax\n\t"
        "bswapl %%eax\n\t"
        "movl %%eax, %0"
        : "=r"(result) :: "eax"
    );
    return result; /* expected: 0x04030201 */
}

void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60), "D"(0));
}
