//===- LibCNames.h - libc/POSIX registry and traits ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCNAMES_H
#define NEVERD_LIBC_LIBCNAMES_H

// Shared libc/POSIX symbol tables for C emission, MIR stub resolution, etc.
// Per-header name lists live beside this file; registry lookup and call-trait
// APIs are implemented in LibCNames.cpp and LibCCallTraits.cpp respectively.

#include <optional>
#include <string_view>

namespace neverd::libc {

/// Integer/pointer and floating-point argument counts of a non-variadic libc
/// function with a fixed, well-known signature.
struct LibCArity {
  int IntArgs = 0;        ///< Integer/pointer arguments (register + stack).
  int FpArgs = 0;         ///< Floating-point (float/double) arguments.
  bool FpIsFloat = false; ///< FP args/return are 32-bit float (the `f` math
                          ///< variants), else 64-bit double.
  bool FpFirst = false;   ///< Mixed int+FP libm whose real signature is the FP
                        ///< args first then the int/pointer args (ldexp/frexp/
                        ///< modf/scalbn): the FP arg(s) precede the int/ptr,
                        ///< and the return is FP.  Lets the emitter reorder the
                        ///< (int-then-FP modelled) recovered arguments back to
                        ///< the real positional order.
  bool FpRet = false; ///< Return value is floating-point (width per FpIsFloat)
                      ///< even when there are NO FP arguments — the
                      ///< integer/pointer-arg, FP-return forms (atof/strtod/
                      ///< strtof/difftime/nan).  Without it the emitter
                      ///< declares an i64 return and reads the result from the
                      ///< integer return register instead of d0/xmm0/s0, so a
                      ///< patched `atof("3.14")` yields 0.
  bool FpRetLongDouble =
      false; ///< Return is `long double`.  Only ABI-equivalent
             ///< to `double` where long double is 64-bit (Apple
             ///< AArch64); the emitter models it as a double return
             ///< only there and leaves other targets (x87 80-bit /
             ///< binary128) to the conservative fallback.
  bool FpRetComplex =
      false; ///< Return is `_Complex float`/`_Complex double` — a
             ///< homogeneous FP aggregate of two elements returned in
             ///< two FP registers (d0/d1, s0/s1, xmm0/xmm1) on the
             ///< 64-bit ABIs (csqrt/cexp/clog/cpow/...).  The FP-arg
             ///< count still counts each complex element separately
             ///< (one complex arg == 2 FP args).  The emitter declares
             ///< the callee with a `{fp,fp}` struct return so the
             ///< backend reads both result registers; without it the
             ///< conservative `(...)->i64` fallback reads only the
             ///< integer return register, so the real part is garbage
             ///< and the imaginary part is 0.
};

/// A single (name, arity) row of a per-header arity table (kStdioArity,
/// kStringArity, ...).  These tables live beside the function-name lists in the
/// libc_*.h headers; libcArity assembles them into one lookup map.
struct LibCArityEntry {
  std::string_view Name;
  LibCArity Arity;
};

/// The fixed argument arity of a known NON-variadic libc function (e.g. fputs
/// -> {2,0}, sqrt -> {0,1}), used to bound the heuristic argument recovery for
/// an external call whose true signature is otherwise unknown.  Returns nullopt
/// for an unknown function, a variadic function (use varArgFixedCount instead),
/// or a function whose arity is intentionally not modelled (mixed int/FP forms
/// like ldexp/frexp).  Name must have its leading underscores already stripped.
std::optional<LibCArity> libcArity(std::string_view Name);

/// The fixed arity for a symbol name as it appears in an object or executable.
/// This preserves platform-decorated spellings whose leading underscores are
/// semantically significant before falling back to the canonical registry.
std::optional<LibCArity> libcArityForSymbol(std::string_view Name);

/// True if Name is a known libc/POSIX library function.
bool isKnownFunction(std::string_view Name);

/// Returns e.g. "stdlib.h", or nullptr if not a known libc function.
const char *headerFor(std::string_view Name);

/// If Name is a known variadic C function (printf, fprintf, etc.),
/// returns the number of fixed parameters before the '...'. Returns 0
/// if the function is not variadic.
unsigned varArgFixedCount(std::string_view Name);

/// ABI category of one fixed parameter in a known variadic function.  Pointer
/// parameters must be symbolized before code generation; integer parameters
/// must remain scalar values even when a small constant happens to overlap a
/// low-address data segment in a relocatable image.  Unknown preserves the
/// recovered call-site type (used for user functions and selector arguments
/// whose source-level type is not encoded in the symbol name).
enum class VarArgFixedParamKind {
  Unknown,
  Integer,
  Pointer,
};

/// Return the ABI category of fixed parameter \p Index for a known variadic
/// libc/POSIX function.  Names may retain platform-leading underscores.
VarArgFixedParamKind varArgFixedParamKind(std::string_view Name,
                                          unsigned Index);

/// True if Name is a known va_list-consuming function: the v-prefixed printf /
/// scanf family (vprintf, vfprintf, vsnprintf, vscanf, ...) and their fortified
/// __v*_chk variants, plus vsyslog/verr/vwarn.  These take a trailing `va_list`
/// argument rather than `...`; a function that forwards its own varargs into
/// one of them (the classic `void log(const char*fmt,...){ ...;
/// vfprintf(f,fmt,ap); }` wrapper) is therefore itself variadic.  Name must
/// have leading underscores already stripped.
bool isVaListConsumer(std::string_view Name);

/// True if Name is a libc/POSIX function that never returns to its caller
/// (abort / exit / _exit / _Exit / quick_exit, longjmp / siglongjmp,
/// pthread_exit / thrd_exit, the err / errx family, and the internal assert /
/// stack-check / C++ unwind failure handlers).  A direct call to one is a
/// control-flow terminator: at -O2 the compiler emits nothing after such a
/// call, so the bytes that follow belong to the NEXT function — the CFG builder
/// must stop exploring rather than fall through and swallow them (a leaf `bl
/// _longjmp` would otherwise absorb the whole function laid out after it).
/// Leading platform underscores (the Mach-O `_` prefix) are stripped
/// internally.
bool isNoReturnFunction(std::string_view Name);

/// True if Name is a setjmp-family function that may return more than once
/// (setjmp / _setjmp / sigsetjmp): control re-enters the call site when a
/// matching longjmp restores the saved context.  The emitter marks such a
/// callee `returns_twice` so a value live across the call is reloaded after it
/// instead of being stranded in a caller-saved register longjmp does not
/// restore (which would make the longjmp-return path read garbage).  Leading
/// platform underscores are stripped internally.
bool isReturnsTwiceFunction(std::string_view Name);

/// True if \p Name names a memory-copy libc routine (memcpy/memmove and their
/// fortified forms).  Leading platform underscores are stripped internally.
bool isMemCopyName(std::string_view Name);

/// True if \p Name names a memory-set libc routine (memset and its fortified
/// form).  Leading platform underscores are stripped internally.
bool isMemSetName(std::string_view Name);

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCNAMES_H
