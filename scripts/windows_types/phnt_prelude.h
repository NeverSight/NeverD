// What phnt takes from Microsoft's SDK that mingw-w64's headers spell
// otherwise or lack, for scripts/generate_windows_types.py: macros that
// mark declarations for the compiler or for code analysis, which declare
// nothing.
#define EXTERN_C_START
#define EXTERN_C_END
#define static_assert _Static_assert
#define UFIELD_OFFSET(type, field) ((ULONG)__builtin_offsetof(type, field))
#define DECLSPEC_ALLOCATOR
#define DECLSPEC_NOINITALL
#define DECLSPEC_RESTRICT
#define __callback
#define _Analysis_noreturn_
#define _Enum_is_bitflag_
#define _Deref_post_count_(size)
#define _Deref_post_opt_count_(size)
