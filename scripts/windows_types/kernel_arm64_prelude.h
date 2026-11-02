//===- kernel_arm64_prelude.h - ReactOS/MinGW annotation compatibility
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Analysis annotations do not change the C ABI. MinGW's SAL lacks these
// spellings used by the pinned ReactOS core DDK. Real noreturn and alignment
// attributes remain intact; no target architecture macro is overridden.

#ifndef NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_ARM64_PRELUDE_H
#define NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_ARM64_PRELUDE_H

#define _Enum_is_bitflag_
#define _Interlocked_operand_
#define __inner_callback
#define __analysis_noreturn
#define __in_data_source(x)
#define __kernel_entry

#endif // NEVERD_SCRIPTS_WINDOWS_TYPES_KERNEL_ARM64_PRELUDE_H
