#ifndef NEVERD_LIBC_LIBCCOMPLEX_H
#define NEVERD_LIBC_LIBCCOMPLEX_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C complex.h — complex arithmetic
inline constexpr std::string_view kComplexHeader = "complex.h";

inline constexpr std::array kComplexFunctions = {
    // imaginary part
    "cimag",
    "cimagf",
    "cimagl",
    // real part
    "creal",
    "crealf",
    "creall",
    // conjugate
    "conj",
    "conjf",
    "conjl",
    // projection
    "cproj",
    "cprojf",
    "cprojl",
    // absolute value
    "cabs",
    "cabsf",
    "cabsl",
    // argument (phase angle)
    "carg",
    "cargf",
    "cargl",
    // exponential / log
    "cexp",
    "cexpf",
    "cexpl",
    "clog",
    "clogf",
    "clogl",
    // power / sqrt
    "cpow",
    "cpowf",
    "cpowl",
    "csqrt",
    "csqrtf",
    "csqrtl",
    // trigonometric
    "ccos",
    "ccosf",
    "ccosl",
    "csin",
    "csinf",
    "csinl",
    "ctan",
    "ctanf",
    "ctanl",
    // inverse trigonometric
    "cacos",
    "cacosf",
    "cacosl",
    "casin",
    "casinf",
    "casinl",
    "catan",
    "catanf",
    "catanl",
    // hyperbolic
    "ccosh",
    "ccoshf",
    "ccoshl",
    "csinh",
    "csinhf",
    "csinhl",
    "ctanh",
    "ctanhf",
    "ctanhl",
    // inverse hyperbolic
    "cacosh",
    "cacoshf",
    "cacoshl",
    "casinh",
    "casinhf",
    "casinhl",
    "catanh",
    "catanhf",
    "catanhl",
};

/// Fixed arity of the complex.h functions.  A `_Complex double` argument is a
/// homogeneous FP aggregate passed in two FP registers, so it is modelled as
/// two FP args (each element counted separately).  The scalar-returning forms
/// (cabs/carg/creal/cimag) return a plain double/float; the others return a
/// `_Complex` value — a 2-element FP aggregate in two FP registers
/// (FpRetComplex). {IntArgs, FpArgs, FpIsFloat, FpFirst, FpRet,
/// FpRetLongDouble, FpRetComplex}.
inline constexpr auto kComplexArity = std::to_array<LibCArityEntry>({
    {"cabs", {0, 2}},
    {"cabsf", {0, 2, true}},
    {"carg", {0, 2}},
    {"cargf", {0, 2, true}},
    {"creal", {0, 2}},
    {"crealf", {0, 2, true}},
    {"cimag", {0, 2}},
    {"cimagf", {0, 2, true}},
    {"cexp", {0, 2, false, false, false, false, true}},
    {"cexpf", {0, 2, true, false, false, false, true}},
    {"clog", {0, 2, false, false, false, false, true}},
    {"clogf", {0, 2, true, false, false, false, true}},
    {"csqrt", {0, 2, false, false, false, false, true}},
    {"csqrtf", {0, 2, true, false, false, false, true}},
    {"csin", {0, 2, false, false, false, false, true}},
    {"csinf", {0, 2, true, false, false, false, true}},
    {"ccos", {0, 2, false, false, false, false, true}},
    {"ccosf", {0, 2, true, false, false, false, true}},
    {"ctan", {0, 2, false, false, false, false, true}},
    {"ctanf", {0, 2, true, false, false, false, true}},
    {"casin", {0, 2, false, false, false, false, true}},
    {"casinf", {0, 2, true, false, false, false, true}},
    {"cacos", {0, 2, false, false, false, false, true}},
    {"cacosf", {0, 2, true, false, false, false, true}},
    {"catan", {0, 2, false, false, false, false, true}},
    {"catanf", {0, 2, true, false, false, false, true}},
    {"csinh", {0, 2, false, false, false, false, true}},
    {"csinhf", {0, 2, true, false, false, false, true}},
    {"ccosh", {0, 2, false, false, false, false, true}},
    {"ccoshf", {0, 2, true, false, false, false, true}},
    {"ctanh", {0, 2, false, false, false, false, true}},
    {"ctanhf", {0, 2, true, false, false, false, true}},
    {"casinh", {0, 2, false, false, false, false, true}},
    {"casinhf", {0, 2, true, false, false, false, true}},
    {"cacosh", {0, 2, false, false, false, false, true}},
    {"cacoshf", {0, 2, true, false, false, false, true}},
    {"catanh", {0, 2, false, false, false, false, true}},
    {"catanhf", {0, 2, true, false, false, false, true}},
    {"conj", {0, 2, false, false, false, false, true}},
    {"conjf", {0, 2, true, false, false, false, true}},
    {"cproj", {0, 2, false, false, false, false, true}},
    {"cprojf", {0, 2, true, false, false, false, true}},
    // cpow/cpowf take TWO `_Complex` arguments (4 FP elements: d0-d3 / s0-s3).
    {"cpow", {0, 4, false, false, false, false, true}},
    {"cpowf", {0, 4, true, false, false, false, true}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCCOMPLEX_H
