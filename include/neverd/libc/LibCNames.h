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

#include "neverd/Common.h"
#include "neverd/libc/LibCVariant.h"

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace neverd {
struct BinaryImage;
}

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

/// The bytes of the scalar float a routine of \p Arity returns: the tables
/// list a routine with only floating arguments, or its floating arguments
/// first, by its floating return, and mark one without (FpRet); a long
/// double or complex return is no scalar in the vector register.
constexpr uint16_t floatReturnBytes(const LibCArity &Arity) {
  if (Arity.FpRetLongDouble || Arity.FpRetComplex ||
      !(Arity.FpRet || Arity.FpFirst ||
        (Arity.FpArgs > 0 && Arity.IntArgs == 0)))
    return 0;
  return Arity.FpIsFloat ? sizeof(float) : sizeof(double);
}

/// A single (name, arity) row of a per-header arity table (kStdioArity,
/// kStringArity, ...).  These tables live beside the function-name lists in the
/// libc_*.h headers; libcArity assembles them into one lookup map.
struct LibCArityEntry {
  std::string_view Name;
  LibCArity Arity;
};

/// The C declaration of a routine no standard header declares: one the start
/// files enter a program through (LibCStartup.h), a C++ runtime entry point
/// (LibCExceptionRuntime.h), a fortified or ISO-alias C library routine
/// (LibCFortify.h) or a Windows C runtime one (WindowsCRT.h).  A
/// non-variadic one's arity is derived from it.
struct LibCPrototype {
  /// The C name the routine links by.
  std::string_view Name;
  std::string_view Return;
  /// Each parameter's C type.  kWinapiMarker in one names the Windows API
  /// calling convention of the function pointer it spells.
  std::array<std::string_view, 8> Params{};
  uint8_t ParamCount = 0;
  /// Further arguments follow the parameters (`...`).
  bool Variadic = false;
  /// The header declaring a type the declaration names (`FILE`, `size_t`),
  /// or empty.
  std::string_view Header{};
  /// The object format whose C runtime provides the routine, or Unknown for
  /// every format's.
  BinaryFormat Format = BinaryFormat::Unknown;
  /// The routine uses the Windows API calling convention: stdcall on 32-bit
  /// x86.
  bool Winapi = false;
};

/// The prototype `Return Name(Params)`, whose types \p Header declares where
/// C does not; a last parameter `...` makes it variadic.
constexpr LibCPrototype
makeLibCPrototype(std::string_view Name, std::string_view Return,
                  std::initializer_list<std::string_view> Params,
                  std::string_view Header = {}) {
  LibCPrototype Prototype{Name, Return};
  for (std::string_view Param : Params) {
    if (Param == "...") {
      Prototype.Variadic = true;
      break;
    }
    Prototype.Params[Prototype.ParamCount++] = Param;
  }
  Prototype.Header = Header;
  return Prototype;
}

/// Whether C type \p Type is a pointer: to an object or to a function, or to
/// a pointer to one.
constexpr bool isPointerType(std::string_view Type) {
  return !Type.empty() &&
         (Type.back() == '*' || Type.find("*)") != std::string_view::npos);
}

/// Whether C type \p Type is a real floating type, which the calling
/// conventions pass in a vector register.
constexpr bool isFloatingType(std::string_view Type) {
  return Type == "float" || Type == "double" || Type == "long double";
}

/// The bytes of the scalar float a routine \p Prototype declares returns in
/// the vector return register: 4 for `float`, 8 for `double`, else 0.
constexpr uint16_t floatReturnBytes(const LibCPrototype &Prototype) {
  return Prototype.Return == "double"  ? sizeof(double)
         : Prototype.Return == "float" ? sizeof(float)
                                       : 0;
}

/// Whether a parameter of C type \p Type takes a narrow string literal as it
/// is: C converts its `char *` to a character or untyped pointer.
constexpr bool takesStringLiteral(std::string_view Type) {
  return Type == "char *" || Type == "const char *" || Type == "void *" ||
         Type == "const void *";
}

/// The marker a prototype's function-pointer type carries for the Windows
/// API calling convention, which is stdcall on 32-bit x86.
inline constexpr std::string_view kWinapiMarker = "WINAPI ";

/// The prototype of the routine C name \p Name links to in an image of object
/// format \p Format, or null.
const LibCPrototype *libcPrototype(std::string_view Name, BinaryFormat Format);

/// Whether the routine C name \p Name links to in a program of C library
/// \p Variant returns a value, as its C declaration says: the prototype
/// above or, for a routine a C library header declares, the declared result
/// (LibCReturnKinds.inc, LibCVariants.def).  Unknown for a routine neither
/// declares.
std::optional<bool> libcReturnsValue(std::string_view Name,
                                     LibCVariant Variant);

/// The C library header that declares the data object C name \p Name
/// (LibCDataObjects.def): "stdio.h" for `stdout`; null for any other name.
const char *headerOfDataObject(std::string_view Name);

/// The C type the header that declares data object \p Name declares it with
/// (LibCDataObjects.def): "FILE *" for `stdout`; null for any other name.
const char *typeOfDataObject(std::string_view Name);

/// Whether the compiler runtime routine C name \p Name returns a double-word
/// integer in a register pair on a target of \p WordBytes registers
/// (CompilerRuntimeDoubleWord.def): `__udivdi3` on a 32-bit target, and
/// `__udivti3` on a 64-bit one.
bool returnsDoubleWord(std::string_view Name, unsigned WordBytes);

/// The fixed argument arity of a known NON-variadic libc function (e.g. fputs
/// -> {2,0}, sqrt -> {0,1}), used to bound the heuristic argument recovery for
/// an external call whose true signature is otherwise unknown.  Returns nullopt
/// for an unknown function, a variadic function (use varArgFixedCount instead),
/// or a function whose arity is intentionally not modelled (mixed int/FP forms
/// like ldexp/frexp).  Name must have its leading underscores already stripped.
/// \p Variant is the C library whose declaration is meant (LibCVariants.def).
std::optional<LibCArity> libcArity(std::string_view Name, LibCVariant Variant);

/// A Windows kernel routine's documented parameters
/// (WindowsKernelRoutines.inc): the bytes the Win64 ABI passes for each, in
/// order.
struct WindowsKernelPrototype {
  std::string_view Name;
  std::array<uint8_t, 16> ArgWidths{};
  uint8_t ArgCount = 0;
};

/// The WDK prototype of \p Name, or null.  Leading underscores are ignored.
const WindowsKernelPrototype *windowsKernelPrototype(std::string_view Name);

/// True for an indirect-call dispatcher such as MSVC Control Flow Guard's
/// `_guard_dispatch_icall`, which jumps to RAX with the caller's arguments.
bool isIndirectCallDispatchThunk(std::string_view Name);

/// The fixed arity for a symbol name as it appears in an object or executable.
/// This preserves platform-decorated spellings whose leading underscores are
/// semantically significant before falling back to the canonical registry.
std::optional<LibCArity> libcArityForSymbol(std::string_view Name,
                                            LibCVariant Variant);

/// True if Name is a known libc/POSIX library function.
bool isKnownFunction(std::string_view Name);

/// Returns e.g. "stdlib.h", or nullptr if not a known libc function.
/// The routine the GNU C Library's large-file routine \p Name is the
/// 64-bit-offset variant of (`open64` of `open`), which takes and returns
/// alike (LibCLargeFile.def); empty for any other name.
std::string_view largeFileRoutine(std::string_view Name);
const char *headerFor(std::string_view Name);

/// If Name is a known variadic C function (printf, fprintf, etc.) of C
/// library \p Variant, returns the number of fixed parameters before the
/// '...'. Returns 0 if the function is not variadic.
unsigned varArgFixedCount(std::string_view Name, LibCVariant Variant);

/// A printf-family routine (LibCPrintfFormats.inc): its return type and the
/// C types of its fixed parameters, the last of which is the format whose
/// conversions name the further arguments.
struct LibCPrintfFormat {
  std::string_view Name;
  std::string_view Return;
  std::array<std::string_view, 6> Params{};
  uint8_t ParamCount = 0;
};

/// The printf-family routine C name \p Name links to, or null.  Its format is
/// its last fixed parameter.
const LibCPrintfFormat *libcPrintfFormat(std::string_view Name);

/// How C passes every argument a function receives on to a variadic routine
/// (LibCVariadicForwards.inc), as a stub that jumps to the routine does.
struct LibCVariadicForward {
  enum class Kind : uint8_t {
    /// Target takes the variadic arguments as a va_list after the fixed
    /// parameters, the last of which is the format.
    VaList,
    /// The routine, Target, reads at most Count variadic arguments of Type.
    Bounded,
    /// The variadic arguments are pointers up to a null one, which Target
    /// takes as an array after the first fixed parameter, followed by an
    /// environment array when Environment.
    Sentinel,
  };
  Kind TheKind = Kind::VaList;
  std::string_view Name;
  std::string_view Return;
  /// The header declaring the types the fixed parameters name, or empty.
  std::string_view Header;
  std::string_view Target;
  /// The C types of the fixed parameters.
  std::array<std::string_view, 6> Fixed{};
  uint8_t FixedCount = 0;
  uint8_t Count = 0;
  std::string_view Type;
  bool Environment = false;
  /// The only object format whose C libraries declare the routine
  /// variadic, or Unknown for every format's: for ELF, Linux's (variadicIn).
  BinaryFormat Format = BinaryFormat::Unknown;
};

/// How a function passes its arguments on to the variadic routine C name
/// \p Name links to, or null when C cannot.
const LibCVariadicForward *libcVariadicForward(std::string_view Name);

/// The position of the va_list parameter of the routine C name \p Name links
/// to, where it is the va_list form of a variadic one: 2 for
/// `vfprintf(FILE *, const char *, va_list)`.  Empty for any other routine.
/// Same-named Windows legacy/ISO forms require the actual declaration.
std::optional<unsigned> vaListParameter(std::string_view Name,
                                        LibCVariant Variant);

/// Whether a program of C library \p Variant declares \p Forward's routine
/// variadic: every C library does where the row names no format, and where
/// it names ELF, Linux's do (GNU, musl and bionic), as Linux's ptrace
/// takes its address and datum variadic and FreeBSD's fixed.
bool variadicIn(const LibCVariadicForward &Forward, LibCVariant Variant);

/// The C type of fixed parameter \p Index of \p Forward's routine as the
/// headers of C library \p Variant declare it (LibCVariants.def): the GNU C
/// Library's ptrace takes its request as an enumeration, bionic's and musl's
/// as an int.
std::string_view forwardFixedType(const LibCVariadicForward &Forward,
                                  size_t Index, LibCVariant Variant);

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
/// libc/POSIX function of C library \p Variant.  Names may retain
/// platform-leading underscores.
VarArgFixedParamKind varArgFixedParamKind(std::string_view Name, unsigned Index,
                                          LibCVariant Variant);

/// True if Name is a known va_list-consuming function: the v-prefixed printf /
/// scanf family (vprintf, vfprintf, vsnprintf, vscanf, ...) and their fortified
/// __v*_chk variants, plus vsyslog/verr/vwarn.  These take a trailing `va_list`
/// argument rather than `...`; a function that forwards its own varargs into
/// one of them (the classic `void log(const char*fmt,...){ ...;
/// vfprintf(f,fmt,ap); }` wrapper) is therefore itself variadic.  Name must
/// have leading underscores already stripped.
bool isVaListConsumer(std::string_view Name);

/// True if parameter \p Index of the standard C or POSIX function \p Name is
/// a narrow, NUL-terminated `char` string.  Leading underscores are ignored.
bool isCStringParameter(std::string_view Name, unsigned Index);

/// The C type of parameter \p Index of the standard C or POSIX function
/// \p Name of C library \p Variant when it takes a function (`qsort`'s
/// comparison), or nullopt.  Leading underscores are ignored.
std::optional<std::string_view> functionPointerParameter(std::string_view Name,
                                                         unsigned Index,
                                                         LibCVariant Variant);

/// Whether parameter \p Index of the standard C or POSIX function \p Name
/// of C library \p Variant takes an object pointer (`memcpy`'s destination,
/// `fputs`'s stream), which C does not convert an integer to.  Leading
/// underscores are ignored.
bool isObjectPointerParameter(std::string_view Name, unsigned Index,
                              LibCVariant Variant);

/// What a compiler stack-probe helper does (StackProbeRoutines.inc).
enum class StackProbeEffect : uint8_t {
  /// Touches the frame's pages and changes nothing the program observes.
  Probe,
  /// Also moves the stack pointer by the size it receives.
  Allocate,
};

/// The effect of the stack-probe helper whose entry is \p Target in \p Img,
/// or nullopt when \p Target is not one for the image's format and
/// architecture.  The helper is known by a name the binary or its debug
/// information states, never by one an analysis guessed.
std::optional<StackProbeEffect> stackProbeEffect(const BinaryImage &Img,
                                                 va_t Target);

/// True if Name is a libc/POSIX function that never returns to its caller
/// (abort / exit / _exit / _Exit / quick_exit, longjmp / siglongjmp,
/// pthread_exit / thrd_exit, the err / errx family, and the internal assert /
/// stack-check / C++ unwind failure handlers).  A direct call to one is a
/// control-flow terminator: at -O2 the compiler emits nothing after such a
/// call, so the bytes that follow belong to the NEXT function — the CFG builder
/// must stop exploring rather than fall through and swallow them (a leaf `bl
/// _longjmp` would otherwise absorb the whole function laid out after it).
/// Leading platform underscores (the Mach-O `_` prefix) are stripped
/// internally.  Rust standard library routines declared to return `!` (a
/// panic, abort or exit) are known by their demangled path.
bool isNoReturnFunction(std::string_view Name);

/// True if \p Imp never returns: by its name (isNoReturnFunction), or as the
/// routine its module exports under a name a program may also define for
/// itself, as the Universal CRT exports `terminate`.
bool isNoReturnImport(const Import &Imp);

/// True if \p Target resolves through \p Img to a known no-return import
/// veneer, or to a function the image exports or a symbol names so.  This is
/// the image-aware counterpart of isNoReturnFunction() shared by discovery
/// and CFG recovery.
bool isNoReturnTarget(const BinaryImage &Img, va_t Target);

/// Exact no-return name lookup for one operation on an unchanged image.
/// The owner must outlive its readers; a different image uses the live lookup.
class NoReturnTargetIndex {
public:
  /// Names a function address that a restricted (`--func`) load has not
  /// ingested, such as a PDB public loaded on demand.
  using NameResolver = std::function<std::optional<std::string>(va_t)>;
  /// Whether the routine of a name never returns by a declaration the
  /// tables do not hold, such as a header library's `_Noreturn` prototype.
  using DeclaredNoReturn = std::function<bool(llvm::StringRef)>;

  explicit NoReturnTargetIndex(const BinaryImage &Img,
                               NameResolver ResolveName = {},
                               DeclaredNoReturn Declared = {});
  bool contains(const BinaryImage &Img, va_t Target) const;
  /// Drop demand-loaded name answers between sequential runs of the same
  /// immutable image. All querying workers must have joined first.
  void clearMemoizedNames();

private:
  const BinaryImage *Image;
  std::set<va_t> Targets;
  std::set<va_t> ImportAddresses;
  /// A restricted load skips the whole-image symbol walk.  Its targets are
  /// answered one at a time, the same way, and remembered: CFG workers
  /// share one index.
  bool SymbolsIndexed = true;
  NameResolver ResolveName;
  DeclaredNoReturn Declared;
  mutable std::mutex LazyMutex;
  mutable std::map<va_t, bool> LazyAnswers;
};

/// isNoReturnTarget() answered through \p Index when one is available, so
/// every layer that asks about one image gets the same answer.
bool isNoReturnTarget(const BinaryImage &Img, va_t Target,
                      const NoReturnTargetIndex *Index);

/// True if Name requires returns-twice register semantics
/// (setjmp / _setjmp / sigsetjmp / vfork). For setjmp, control re-enters when a
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
