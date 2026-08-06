// Memory-set libc symbols lowered to llvm.memset in MedLLVMCall.
// Included with LIBC_MEM_SET_SYMBOL defined; entries are compared after
// stripLeadingUnderscores().

LIBC_MEM_SET_SYMBOL("memset")
LIBC_MEM_SET_SYMBOL("memset_chk")
